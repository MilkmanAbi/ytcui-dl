// test_sqlite.cpp - the minimal SQLite reader and the browser cookie layer.
//
// These two are the riskiest new code in the project: a hand-written parser for
// a binary file format, and a decryption path whose failure mode is "silently
// wrong value" rather than a crash. So they are tested against a real SQLite
// file (test/fixtures/browser_cookies.sqlite, built by a generator that used
// the actual sqlite3 library) rather than against anything this code wrote
// itself -- a parser tested only against its own output proves nothing.
//
// The fixture deliberately contains:
//   * a Firefox-shaped moz_cookies whose column order is NOT the obvious one,
//     so position-based lookups would fail
//   * a 9000-byte value, which cannot fit in one 4 KiB page and therefore
//     exercises the overflow-page chain
//   * multi-byte UTF-8 and emoji
//   * a Chromium-shaped table with real AES-128-CBC 'v10' blobs
//   * one blob carrying Chrome 130+'s 32-byte domain-hash plaintext prefix
//   * cookies for an unrelated domain, which must be filtered out

#include "yt_sqlite.h"
#include "yt_browser.h"
#include "yt_innertube.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

using namespace ytfast;

static std::string fixture_dir(int argc, char** argv) {
    return argc > 1 ? argv[1] : "test/fixtures";
}

// ---------------------------------------------------------------------------
static void test_reader(const std::string& path) {
    printf("\nsqlite reader\n");
    sqlite::Db db;
    CHECK(db.open(path), "cannot open fixture %s", path.c_str());
    if (!db.open(path)) return;

    const std::vector<std::string> mc = db.columns("moz_cookies");
    CHECK(!mc.empty(), "no moz_cookies columns");
    // Looked up by name on purpose: the fixture's order is id, originAttributes,
    // name, value, host, ... so anything assuming host-first breaks here.
    int i_host = -1, i_name = -1, i_value = -1;
    for (size_t i = 0; i < mc.size(); ++i) {
        if (mc[i] == "host")  i_host = (int)i;
        if (mc[i] == "name")  i_name = (int)i;
        if (mc[i] == "value") i_value = (int)i;
    }
    CHECK(i_host == 4, "host column index %d (expected 4)", i_host);
    CHECK(i_name == 2, "name column index %d (expected 2)", i_name);
    printf("   moz_cookies columns: %zu, host at %d\n", mc.size(), i_host);

    int rows = 0;
    std::string big, uni;
    bool saw_sapisid = false;
    db.scan("moz_cookies", [&](const std::vector<sqlite::Value>& r) {
        ++rows;
        const std::string n = r[i_name].as_str();
        if (n == "SAPISID")   saw_sapisid = true;
        if (n == "BIGCOOKIE") big = r[i_value].as_str();
        if (n == "UNICODE")   uni = r[i_value].as_str();
        return true;
    });
    CHECK(rows == 7, "moz_cookies rows = %d (expected 7)", rows);
    CHECK(saw_sapisid, "SAPISID row missing");

    // The overflow path: 9000 bytes cannot live in a 4 KiB page.
    CHECK(big.size() == 9000, "overflow value is %zu bytes (expected 9000)", big.size());
    bool all_x = !big.empty();
    for (char c : big) if (c != 'x') { all_x = false; break; }
    CHECK(all_x, "overflow value is corrupted (wrong bytes)");
    printf("   overflow row: %zu bytes, intact: %s\n", big.size(), all_x ? "yes" : "NO");

    // Asserted structurally rather than against one hand-typed byte sequence:
    // "ünïcødé-値-🎵" is 20 UTF-8 bytes, and the two interesting ones are the
    // 3-byte CJK codepoint and the 4-byte emoji. Spelling the whole literal out
    // by hand is how you write a test that fails on a correct reader.
    CHECK(uni.size() == 20, "unicode value is %zu bytes (expected 20): '%s'",
          uni.size(), uni.c_str());
    CHECK(uni.find("\xe5\x80\xa4") != std::string::npos,
          "3-byte CJK codepoint lost: '%s'", uni.c_str());
    CHECK(uni.find("\xf0\x9f\x8e\xb5") != std::string::npos,
          "4-byte emoji lost: '%s'", uni.c_str());

    // A table that isn't there must fail cleanly, not crash.
    CHECK(!db.scan("no_such_table", [](const std::vector<sqlite::Value>&) { return true; }),
          "scan of a missing table should return false");
    CHECK(db.columns("no_such_table").empty(), "columns of a missing table");

    // Garbage input must be rejected, not parsed.
    sqlite::Db bad;
    CHECK(!bad.open(path + ".does-not-exist"), "opening a missing file should fail");
}

// ---------------------------------------------------------------------------
static void test_firefox(const std::string& path) {
    printf("\nfirefox extraction\n");
    browser::Profile p;
    p.engine  = browser::Engine::Firefox;
    p.browser = "firefox";
    p.label   = "firefox / fixture";
    p.cookie_db = path;

    std::vector<browser::Cookie> v;
    const browser::ExtractResult r = browser::read_cookies(p, v);
    CHECK(r.ok, "firefox extraction failed: %s", r.error.c_str());
    // 7 rows, one of which is example.com and must be filtered away.
    CHECK(v.size() == 6, "kept %zu cookies (expected 6 after domain filter)", v.size());
    for (const auto& c : v)
        CHECK(c.domain.find("example.com") == std::string::npos,
              "unrelated domain leaked through: %s", c.domain.c_str());
    CHECK(browser::has_auth(v), "SAPISID should count as signed in");

    bool found_secure = false, found_session = false;
    for (const auto& c : v) {
        if (c.name == "SAPISID") {
            CHECK(c.value == "ff-sapisid-value", "SAPISID value '%s'", c.value.c_str());
            CHECK(c.secure && c.http_only, "SAPISID flags lost");
            found_secure = true;
        }
        if (c.name == "SESSIONONLY") { CHECK(c.expiry == 0, "session cookie expiry"); found_session = true; }
    }
    CHECK(found_secure, "SAPISID not found");
    CHECK(found_session, "session cookie not found");
    printf("   kept %zu youtube/google cookies, auth detected: %s\n",
           v.size(), browser::has_auth(v) ? "yes" : "no");
}

// ---------------------------------------------------------------------------
static void test_chromium(const std::string& path) {
    printf("\nchromium extraction + decryption\n");
    browser::Profile p;
    p.engine        = browser::Engine::Chromium;
    p.browser       = "chromium";
    p.label         = "chromium / fixture";
    p.cookie_db     = path;
    p.keyring_app   = "chromium";
    p.keychain_svc  = "Chromium Safe Storage";
    p.keychain_acct = "Chromium";

#ifdef YTFAST_MACOS
    // The key would come from the Keychain, which a test must not prompt for.
    printf("   skipped on macOS: the key lives in the Keychain\n");
    (void)p;
    return;
#else
    std::vector<browser::Cookie> v;
    const browser::ExtractResult r = browser::read_cookies(p, v);
    CHECK(r.ok, "chromium extraction failed: %s", r.error.c_str());
    CHECK(r.undecryptable == 0, "%d cookie(s) failed to decrypt", r.undecryptable);
    CHECK(v.size() == 4, "kept %zu cookies (expected 4 after domain filter)", v.size());

    bool sap = false, prefixed = false;
    for (const auto& c : v) {
        if (c.name == "SAPISID") {
            CHECK(c.value == "chrome-sapisid-value",
                  "decrypted SAPISID = '%s'", c.value.c_str());
            sap = true;
            // 1900000000 unix, stored as microseconds since 1601.
            CHECK(c.expiry == 1900000000, "expiry converted to %lld", (long long)c.expiry);
        }
        if (c.name == "HASHPREFIXED") {
            // Chrome 130+ puts a 32-byte SHA-256 of the domain in front of the
            // value; it must be stripped, not handed back as part of it.
            CHECK(c.value == "prefixed-value",
                  "hash-prefixed value = '%s' (prefix not stripped?)", c.value.c_str());
            prefixed = true;
        }
    }
    CHECK(sap, "SAPISID not decrypted");
    CHECK(prefixed, "hash-prefixed cookie missing");
    CHECK(browser::has_auth(v), "auth not detected for chromium set");
    printf("   decrypted %zu cookies, 32-byte prefix handled: %s\n",
           v.size(), prefixed ? "yes" : "no");

    // A wrong key must be reported, never guessed at.
    std::string out;
    CHECK(!browser::decrypt_chromium("v10not-really-ciphertext-at-all!!", "wrong", out),
          "garbage ciphertext should fail");
    CHECK(!browser::decrypt_chromium("plaintext", "peanuts", out),
          "a value with no v10/v11 prefix should fail");
#endif
}

// ---------------------------------------------------------------------------
// The round trip that actually matters: what ytcui-dl session writes must be what
// ytcui-dl reads. These are two different parsers in two different files, and
// a mismatch (a missing #HttpOnly_ prefix, a dropped tab) would silently cost
// the session its auth.
static void test_roundtrip(const std::string& path) {
    printf("\nnetscape round trip (ytcui-dl session writes -> ytcui-dl reads)\n");
    browser::Profile p;
    p.engine = browser::Engine::Firefox;
    p.cookie_db = path;
    std::vector<browser::Cookie> v;
    browser::read_cookies(p, v);
    CHECK(!v.empty(), "no cookies to round trip");

    const std::string tmp = "/tmp/ytcui-dl-session-test-cookies.txt";
    std::string err;
    CHECK(browser::write_netscape(tmp, v, &err), "write failed: %s", err.c_str());

    struct stat st{};
    CHECK(::stat(tmp.c_str(), &st) == 0, "written file missing");
    CHECK((st.st_mode & 0777) == 0600, "jar mode is %o (must be 0600)",
          st.st_mode & 0777);

    // The real consumer.
    auto& yt = InnertubeClient::get_instance();
    CHECK(yt.set_cookies_file(tmp), "ytcui-dl could not read what ytcui-dl session wrote");

    // An httponly cookie is written with the #HttpOnly_ prefix; if ytcui-dl's
    // reader did not strip it, SAPISID would vanish and auth would silently
    // degrade. Assert the prefix is actually present in the file, so this test
    // keeps meaning something.
    FILE* f = std::fopen(tmp.c_str(), "rb");
    std::string all;
    if (f) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) all.append(b, n); std::fclose(f); }
    CHECK(all.find("#HttpOnly_.youtube.com") != std::string::npos,
          "httponly prefix not written");
    CHECK(all.find("\tSAPISID\tff-sapisid-value") != std::string::npos,
          "SAPISID line not written as expected");
    printf("   wrote %zu cookies, mode 0600, ytcui-dl parsed it back\n", v.size());
    ::remove(tmp.c_str());
}

// ---------------------------------------------------------------------------
// The browser-launch side has no GUI in CI, so what is testable is the command
// construction: $BROWSER wins, %s is substituted, a stale entry is skipped.
static void test_browser_command() {
    printf("\ndefault-browser command construction\n");
    const char* saved = std::getenv("BROWSER");
    const std::string url = "https://www.youtube.com/";

    ::setenv("BROWSER", "/bin/echo", 1);
    auto c = browser::browser_command(url);
    CHECK(c.size() == 2 && c[0] == "/bin/echo" && c[1] == url,
          "explicit $BROWSER path not honoured");

    ::setenv("BROWSER", "/bin/echo --open %s --done", 1);
    c = browser::browser_command(url);
    CHECK(c.size() == 3 && c[0] == "/bin/sh" && c[1] == "-c",
          "%%s form should go through a shell");
    CHECK(c.size() == 3 && c[2].find(url) != std::string::npos,
          "url not substituted into %%s form");

    // A $BROWSER naming something that isn't installed must fall through to the
    // platform opener rather than dead-ending.
    ::setenv("BROWSER", "/nonexistent/browser-xyz", 1);
    c = browser::browser_command(url);
    CHECK(c.empty() || c[0] != "/nonexistent/browser-xyz",
          "a stale $BROWSER should be skipped");

    if (saved) ::setenv("BROWSER", saved, 1); else ::unsetenv("BROWSER");
    printf("   $BROWSER handling ok\n");
}

int main(int argc, char** argv) {
    const std::string dir = fixture_dir(argc, argv);
    const std::string fx = dir + "/browser_cookies.sqlite";

    printf("sqlite reader / browser session tests\n   fixture: %s\n", fx.c_str());
    sqlite::Db probe;
    if (!probe.open(fx)) {
        printf("\nFAILED: fixture not found or unreadable: %s\n", fx.c_str());
        return 1;
    }

    test_reader(fx);
    test_firefox(fx);
    test_chromium(fx);
    test_roundtrip(fx);
    test_browser_command();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
