/*
 * ytcui-we — session helper for ytcui-dl
 *
 * "we" for web session. It opens your own default browser so you can sign in
 * to YouTube normally, then lifts that session out of the browser's cookie
 * store into a Netscape cookies.txt that ytcui-dl's existing --cookies flag
 * reads. Nothing is embedded, nothing is scraped, no password is ever seen by
 * this program: the browser does the login, and the OS keeps the keys.
 *
 * WHAT A SESSION ACTUALLY BUYS YOU, measured rather than hoped for:
 *
 *   It unlocks   age-gated, members-only and private videos, and gives search
 *                and the bootstrap a logged-in WEB session.
 *
 *   It does NOT  fix "made for kids" videos, and it does NOT get you past the
 *                ~60 s byte cap on ANDROID/IOS media URLs. Both of those need
 *                a GVS PO Token, and a browser session is not one. A logged-in
 *                web client does hand back an HLS manifest for a kids video --
 *                the only thing that does -- but every segment of it 403s,
 *                with or without a real PO Token spliced in. That route was
 *                tested to exhaustion; see "Known constraints" in the README
 *                so nobody spends another evening on it.
 *
 * So this is an auth helper, not a second playback engine. It is useful, and
 * it is narrower than it first looks.
 *
 * Platforms: Linux, macOS, FreeBSD/OpenBSD/NetBSD. POSIX only, like the rest
 * of this project.
 */

#include "ytfast.h"
#include "yt_browser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

using namespace ytfast;

static const char* SIGNIN_URL =
    "https://accounts.google.com/ServiceLogin?service=youtube"
    "&continue=https%3A%2F%2Fwww.youtube.com%2F";

// ---------------------------------------------------------------------------
// Where the cookie jar lives. Config, not cache: a cache is something you can
// delete without consequence, and this is a credential.
// ---------------------------------------------------------------------------
static std::string default_jar() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        return std::string(x) + "/ytcui-dl/cookies.txt";
    const std::string h = browser::home();
    if (h.empty()) return "cookies.txt";
    return h + "/.config/ytcui-dl/cookies.txt";
}

static void ensure_parent(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return;
    const std::string dir = path.substr(0, slash);
    // One level at a time, 0700: the jar's directory should not be readable
    // by other users either.
    for (size_t i = 1; i < dir.size(); ++i)
        if (dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0700);
    ::mkdir(dir.c_str(), 0700);
}

static void usage() {
    std::printf(
"ytcui-we %s — browser session helper for ytcui-dl\n"
"\n"
"Opens your own default browser to sign in to YouTube, then exports that\n"
"session as a cookies.txt that ytcui-dl can use.\n"
"\n"
"usage: ytcui-we <command> [options]\n"
"\n"
"commands:\n"
"  login              open the default browser at YouTube's sign-in page\n"
"  export             read the browser's cookies into a cookies.txt\n"
"  check [<url>]      verify the exported session against YouTube\n"
"  list               show the browser profiles that were found\n"
"  play <url> [...]   export if needed, then run ytcui-dl -p with the session\n"
"\n"
"options:\n"
"  -o, --output <f>   cookie jar path (default: %s)\n"
"      --browser <b>  firefox|chrome|chromium|brave|edge|vivaldi|opera\n"
"      --profile <d>  use this cookie database directly\n"
"      --print-command  show how the browser would be opened, don't open it\n"
"  -h, --help / -V, --version\n"
"\n"
"what a session unlocks: age-gated, members-only and private videos, plus a\n"
"logged-in session for search. What it does NOT do: \"made for kids\" videos\n"
"and the ~60s ANDROID/IOS byte cap both need a GVS PO Token, which a browser\n"
"session is not. See \"Known constraints\" in the README.\n"
"\n"
"Safari is not supported: its cookie store is a different binary format, not\n"
"SQLite. Sign in with Firefox or any Chromium-family browser instead.\n",
        YTFAST_VERSION, default_jar().c_str());
}

// ---------------------------------------------------------------------------
// Profile selection
// ---------------------------------------------------------------------------
static bool pick_profile(const std::string& want_browser,
                         const std::string& want_profile,
                         browser::Profile& out) {
    if (!want_profile.empty()) {
        if (!browser::is_file(want_profile)) {
            std::fprintf(stderr, "error: no cookie database at %s\n",
                         want_profile.c_str());
            return false;
        }
        out.cookie_db = want_profile;
        // Firefox's file is named cookies.sqlite; Chromium's is "Cookies".
        const bool ff = want_profile.find("cookies.sqlite") != std::string::npos;
        out.engine  = ff ? browser::Engine::Firefox : browser::Engine::Chromium;
        out.browser = ff ? "firefox" : (want_browser.empty() ? "chrome" : want_browser);
        out.label   = out.browser + " / (explicit path)";
        for (const auto& f : browser::CHROMIUM_FAMILIES)
            if (out.browser == f.name) {
                out.keyring_app   = f.keyring_app;
                out.keychain_svc  = std::string(f.keychain) + " Safe Storage";
                out.keychain_acct = f.keychain;
            }
        return true;
    }

    std::vector<browser::Profile> found = browser::discover();
    if (found.empty()) {
        std::fprintf(stderr,
            "error: no browser cookie store found.\n"
            "       Looked for Firefox and the Chromium family under your home\n"
            "       directory (including flatpak and snap locations).\n"
            "       Sign in once with 'ytcui-we login', or point at the file\n"
            "       directly with --profile <path to cookies.sqlite|Cookies>.\n");
        return false;
    }
    if (!want_browser.empty()) {
        for (const auto& p : found)
            if (p.browser == want_browser) { out = p; return true; }
        std::fprintf(stderr, "error: no profile found for '%s'. Try 'ytcui-we list'.\n",
                     want_browser.c_str());
        return false;
    }
    out = found.front();          // most recently written wins
    return true;
}

static int cmd_list() {
    const std::vector<browser::Profile> found = browser::discover();
    if (found.empty()) {
        std::printf("no browser profiles found\n");
        return 1;
    }
    std::printf("browser profiles (most recently used first)\n");
    for (const auto& p : found) {
        std::printf("  %-28s %s\n", p.label.c_str(), p.cookie_db.c_str());
    }
    std::printf("\nauto-pick would use: %s\n", found.front().label.c_str());
    return 0;
}

static int cmd_login(bool print_only) {
    const std::vector<std::string> cmd = browser::browser_command(SIGNIN_URL);
    if (cmd.empty()) {
        std::fprintf(stderr,
            "error: could not find a way to open a browser.\n"
            "       Set $BROWSER, or install xdg-utils, then retry. Or just\n"
            "       open this URL yourself:\n         %s\n", SIGNIN_URL);
        return 1;
    }
    if (print_only) {
        for (size_t i = 0; i < cmd.size(); ++i)
            std::printf("%s%s", cmd[i].c_str(), i + 1 < cmd.size() ? " " : "\n");
        return 0;
    }
    std::printf("opening %s\n", cmd[0].c_str());
    if (!browser::open_url(SIGNIN_URL)) {
        std::fprintf(stderr, "error: failed to launch the browser\n");
        return 1;
    }
    std::printf(
        "\nSign in there, then come back and run:\n"
        "    ytcui-we export\n"
        "\nUse a throwaway account if you would rather not point your main one\n"
        "at a command line tool -- YouTube can flag automated access.\n"
        "Close the browser before exporting, or recent cookies may still be\n"
        "sitting in its write-ahead log where this cannot see them.\n");
    return 0;
}

static int cmd_export(const std::string& jar, const std::string& want_browser,
                      const std::string& want_profile, bool quiet) {
    browser::Profile p;
    if (!pick_profile(want_browser, want_profile, p)) return 1;

    std::vector<browser::Cookie> cookies;
    const browser::ExtractResult r = browser::read_cookies(p, cookies);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    if (cookies.empty()) {
        std::fprintf(stderr,
            "error: %s has no youtube.com/google.com cookies.\n"
            "       Sign in first:  ytcui-we login\n", p.label.c_str());
        if (r.undecryptable)
            std::fprintf(stderr,
                "       (%d cookie(s) were present but could not be decrypted --\n"
                "        on Linux an unlocked login keyring is usually the fix)\n",
                r.undecryptable);
        return 1;
    }

    ensure_parent(jar);
    std::string err;
    if (!browser::write_netscape(jar, cookies, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    const bool authed = browser::has_auth(cookies);
    if (!quiet) {
        std::printf("exported %d cookie(s) from %s\n  -> %s (mode 0600)\n",
                    r.total, p.label.c_str(), jar.c_str());
        std::printf("signed in: %s\n", authed ? "yes" :
                    "NO — these are anonymous cookies, sign in first");
        if (r.undecryptable)
            std::printf("warning: %d cookie(s) could not be decrypted and were skipped\n",
                        r.undecryptable);
        if (r.wal_warning)
            std::printf("warning: the browser looks like it is still running; its newest\n"
                        "         cookies may not be in the file yet. Close it and re-run\n"
                        "         if the session does not work.\n");
        if (authed)
            std::printf("\nuse it:  ytcui-dl --cookies %s -p <url>\n", jar.c_str());
    }
    return authed ? 0 : 2;
}

static int cmd_check(const std::string& jar, const std::string& target) {
    if (!browser::is_file(jar)) {
        std::fprintf(stderr, "error: no cookie jar at %s — run 'ytcui-we export'\n",
                     jar.c_str());
        return 1;
    }
    CurlGlobalInit init;
    auto& yt = InnertubeClient::get_instance();
    struct Cleanup { InnertubeClient& c; ~Cleanup() { c.shutdown(); } } cleanup{yt};

    if (!yt.set_cookies_file(jar)) {
        std::fprintf(stderr,
            "error: %s has no usable youtube/google cookies\n", jar.c_str());
        return 1;
    }
    std::printf("cookie jar: %s\n", jar.c_str());

    const std::string id = target.empty() ? std::string("dQw4w9WgXcQ")
                                          : extract_video_id(target);
    std::printf("resolving %s with the session...\n", id.c_str());
    VideoInfo info = yt.get_stream_formats(id);
    if (info.formats.empty() && info.hls_url.empty()) {
        std::fprintf(stderr,
            "  FAILED — no playable formats. The session may be stale; re-run\n"
            "  'ytcui-we login' and 'ytcui-we export'.\n");
        return 1;
    }
    std::printf("  ok: %s\n", info.title.c_str());
    std::printf("  client %s, %zu format(s)%s\n",
                info.client_name ? info.client_name : "?",
                info.formats.size(),
                info.hls_url.empty() ? "" : ", +HLS manifest");
    std::printf(
        "\nThe session is working. Remember what it does and does not cover:\n"
        "  unlocks  age-gated / members-only / private videos\n"
        "  does not fix \"made for kids\" videos or the ANDROID/IOS byte cap —\n"
        "           those need a GVS PO Token, which this is not.\n");
    return 0;
}

// Convenience: hand straight off to ytcui-dl with the session attached.
static int cmd_play(const std::string& jar, int argc, char** argv, int first) {
    if (!browser::is_file(jar)) {
        std::fprintf(stderr, "error: no cookie jar at %s — run 'ytcui-we export'\n",
                     jar.c_str());
        return 1;
    }
    std::vector<std::string> args = {"ytcui-dl", "--cookies", jar, "-p"};
    for (int i = first; i < argc; ++i) args.push_back(argv[i]);

    std::vector<char*> cargv;
    for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    // Prefer a sibling binary over $PATH, so a build directory works without
    // installing anything.
    std::string self = argv[0];
    const size_t slash = self.rfind('/');
    if (slash != std::string::npos) {
        const std::string sib = self.substr(0, slash + 1) + "ytcui-dl";
        if (browser::is_file(sib)) {
            cargv[0] = const_cast<char*>(sib.c_str());
            ::execv(sib.c_str(), cargv.data());
        }
    }
    ::execvp("ytcui-dl", cargv.data());
    std::fprintf(stderr, "error: could not run ytcui-dl (not in PATH?)\n");
    return 127;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    std::string cmd, jar = default_jar(), want_browser, want_profile;
    bool print_only = false, quiet = false;
    int rest = argc;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")        { usage(); return 0; }
        else if (a == "-V" || a == "--version"){ std::printf("%s\n", YTFAST_VERSION); return 0; }
        else if (a == "-o" || a == "--output") jar = next("--output");
        else if (a == "--browser")             want_browser = next("--browser");
        else if (a == "--profile")             want_profile = next("--profile");
        else if (a == "--print-command")       print_only = true;
        else if (a == "-q" || a == "--quiet")  quiet = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
        else if (cmd.empty())                  cmd = a;
        else { rest = i; break; }
    }

    if (cmd == "login")  return cmd_login(print_only);
    if (cmd == "list")   return cmd_list();
    if (cmd == "export") return cmd_export(jar, want_browser, want_profile, quiet);
    if (cmd == "check")  return cmd_check(jar, rest < argc ? argv[rest] : std::string());
    if (cmd == "play") {
        if (rest >= argc) { std::fprintf(stderr, "play needs a url\n"); return 2; }
        return cmd_play(jar, argc, argv, rest);
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    usage();
    return 2;
}
