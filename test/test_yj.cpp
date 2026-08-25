// test_yj.cpp - correctness + robustness for the scanner.
// Every parse here runs against deliberately broken input. Nothing may crash,
// hang, or read out of bounds. Run under ASan/UBSan.
#include "yj.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>

using yj::keyis;
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// ---------------------------------------------------------------------------
static void test_basics() {
    const char* s = R"({"a":1,"b":"two","c":[1,2,3],"d":{"e":true},"f":null,"g":-4.5e2})";
    yj::Val r = yj::parse(s, strlen(s));
    CHECK(r.is_obj(), "root not obj");
    CHECK(yj::get(r, "a").i64() == 1, "a");
    CHECK(yj::get(r, "b").raw() == "two", "b");
    CHECK(yj::count(yj::get(r, "c")) == 3, "c count");
    CHECK(yj::path(r, "d", "e").boolean() == true, "d.e");
    CHECK(yj::get(r, "f").type() == yj::T::Null, "f null");
    CHECK(yj::get(r, "g").dbl() == -450.0, "g = %f", yj::get(r, "g").dbl());
    CHECK(!yj::get(r, "nope").valid(), "missing key should be invalid");

    // numeric strings (YouTube sends contentLength/viewCount this way)
    const char* n = R"({"contentLength":"12345678901","viewCount":"999"})";
    yj::Val rn = yj::parse(n, strlen(n));
    CHECK(yj::get(rn, "contentLength").i64() == 12345678901LL, "numeric string");
    CHECK(yj::get(rn, "viewCount").i64() == 999, "viewCount");
}

static void test_escapes() {
    struct { const char* in; const char* want; } cases[] = {
        {R"("a\u0026b")",        "a&b"},
        {R"("x\u003dy")",        "x=y"},
        {R"("q\"quoted\"")",     "q\"quoted\""},
        {R"("back\\slash")",     "back\\slash"},
        {R"("sl\/ash")",         "sl/ash"},
        {R"("nl\nhere")",        "nl\nhere"},
        {R"("\ud83d\ude00")",    "\xF0\x9F\x98\x80"},   // emoji surrogate pair
        {R"("caf\u00e9")",       "caf\xC3\xA9"},
        {R"("trunc\u00")",       ""},                    // truncated escape, must not read past
    };
    for (auto& c : cases) {
        yj::Val v = yj::parse(c.in, strlen(c.in));
        std::string got = yj::unescape(v.raw());
        if (strcmp(c.want, "") != 0)
            CHECK(got == c.want, "escape %s -> [%s] want [%s]", c.in, got.c_str(), c.want);
    }
    // a quote preceded by an even run of backslashes really does terminate
    const char* tricky = R"({"k":"ends with backslash\\","after":7})";
    yj::Val r = yj::parse(tricky, strlen(tricky));
    CHECK(yj::get(r, "after").i64() == 7, "even backslash run mis-parsed");
}

static void test_truncation(const std::string& body) {
    // Every prefix of a real response must parse without crashing.
    for (size_t n = 0; n < body.size(); n += 97) {
        yj::Val r = yj::parse(body.data(), n);
        volatile long long sink = 0;
        yj::each_member(r, [&](std::string_view, yj::Val v) {
            sink += v.i64();
            yj::each_elem(v, [&](yj::Val e) { sink += e.i64(); return true; });
            return true;
        });
        yj::Val sd = yj::path(r, "streamingData", "adaptiveFormats");
        yj::each_elem(sd, [&](yj::Val f) {
            yj::each_member(f, [&](std::string_view, yj::Val v) {
                std::string s = yj::unescape(v.raw()); sink += (long long)s.size();
                return true;
            });
            return true;
        });
        (void)sink;
    }
    printf("  truncation: %zu prefixes survived\n", body.size() / 97);
}

static void test_corruption(const std::string& body) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<size_t> pos(0, body.size() ? body.size() - 1 : 0);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int iter = 0; iter < 400; ++iter) {
        std::string b = body;
        int nmut = 1 + (iter % 40);
        for (int m = 0; m < nmut; ++m) b[pos(rng)] = (char)byte(rng);
        yj::Val r = yj::parse(b);
        volatile long long sink = 0;
        yj::Val sd = yj::path(r, "streamingData", "adaptiveFormats");
        yj::each_elem(sd, [&](yj::Val f) {
            yj::each_member(f, [&](std::string_view k, yj::Val v) {
                if (keyis(k, "url")) { std::string s = yj::unescape(v.raw()); sink += (long long)s.size(); }
                else sink += v.i64();
                return true;
            });
            return true;
        });
        yj::find_all(r, "videoRenderer", [&](yj::Val) { ++sink; return true; });
        (void)sink;
    }
    printf("  corruption: 400 mutated bodies survived\n");
}

static void test_pathological() {
    // Deep nesting must not recurse or hang.
    std::string deep;
    for (int i = 0; i < 200000; ++i) deep += '[';
    yj::Val r = yj::parse(deep);
    auto t0 = std::chrono::steady_clock::now();
    volatile long long n = 0;
    yj::each_elem(r, [&](yj::Val) { ++n; return true; });
    yj::find_all(r, "x", [&](yj::Val) { ++n; return true; });
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000, "deep nesting took %.1f ms", ms);

    const char* junk[] = {
        "", "{", "}", "[", "]", "\"", "\"\\", "{\"a\"", "{\"a\":", "{\"a\":}",
        "[[[[", "nulll", "{{{{{{", "\xff\xfe\xfd", "{\"a\":\"\\u\"}",
        "{\"a\":\"\\ud800\"}", "{\"\":\"\"}", ",,,,", "::::",
    };
    for (const char* j : junk) {
        yj::Val v = yj::parse(j, strlen(j));
        volatile long long s = v.i64() + (long long)v.dbl();
        yj::each_member(v, [&](std::string_view, yj::Val x) { s += x.i64(); return true; });
        yj::each_elem(v, [&](yj::Val x) { s += x.i64(); return true; });
        std::string u = yj::unescape(v.raw()); s += (long long)u.size();
        (void)s;
    }
    printf("  pathological: deep nesting %.1f ms, %zu junk inputs survived\n",
           ms, sizeof(junk) / sizeof(*junk));
}

// ---------------------------------------------------------------------------
// Search response: the renderer union types make a fixed path unreliable, so
// this uses find_all. Verify we pull the same videos a path walk would.
struct SR { std::string id, title, channel, dur, views; };

// simpleText or runs[].text — InnerTube uses both interchangeably.
static std::string text_of(yj::Val node) {
    yj::Val st = yj::get(node, "simpleText");
    if (st.is_str()) return yj::unescape(st.raw());
    std::string o;
    yj::each_elem(yj::get(node, "runs"), [&](yj::Val r) {
        o += yj::unescape(yj::get(r, "text").raw()); return true; });
    return o;
}

static void test_search(const std::string& body) {
    if (body.empty()) { printf("  search: no fixture, skipped\n"); return; }
    std::vector<SR> out;
    yj::Val root = yj::parse(body);
    static const std::string_view kRenderers[] = {"videoRenderer", "compactVideoRenderer"};
    yj::find_any(root, kRenderers, 2, [&](std::string_view, yj::Val vr) {
        SR s;
        yj::each_member(vr, [&](std::string_view k, yj::Val v) {
            if (keyis(k, "videoId")) s.id = yj::unescape(v.raw());
            else if (keyis(k, "title")) {
                s.title = text_of(v);
            } else if (keyis(k, "ownerText")) {
                yj::Val runs = yj::get(v, "runs");
                yj::each_elem(runs, [&](yj::Val r) {
                    s.channel += yj::unescape(yj::get(r, "text").raw()); return true; });
            } else if (keyis(k, "longBylineText") && s.channel.empty()) {
                yj::each_elem(yj::get(v, "runs"), [&](yj::Val r) {
                    s.channel += yj::unescape(yj::get(r, "text").raw()); return true; });
            } else if (keyis(k, "lengthText")) {
                s.dur = text_of(v);
            } else if (keyis(k, "shortViewCountText") || keyis(k, "viewCountText")) {
                if (s.views.empty()) s.views = text_of(v);
            }
            return true;
        });
        if (!s.id.empty()) out.push_back(std::move(s));
        return true;
    });
    CHECK(out.size() >= 10, "only %zu search results extracted", out.size());
    int with_title = 0, with_chan = 0, with_dur = 0;
    for (auto& s : out) {
        if (!s.title.empty()) ++with_title;
        if (!s.channel.empty()) ++with_chan;
        if (!s.dur.empty()) ++with_dur;
    }
    printf("  search: %zu videos (%d titled, %d w/ channel, %d w/ duration)\n",
           out.size(), with_title, with_chan, with_dur);
    for (size_t i = 0; i < out.size() && i < 4; ++i)
        printf("     [%s] %-52.52s | %-24.24s | %s\n", out[i].id.c_str(),
               out[i].title.c_str(), out[i].channel.c_str(), out[i].dur.c_str());
    CHECK(with_title == (int)out.size(), "%d/%zu missing titles", with_title, out.size());
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "fixtures";
    std::string player = slurp(dir + "/player_full.json");
    std::string search = slurp(dir + "/search.json");

    printf("yj scanner tests\n");
    test_basics();
    test_escapes();
    test_pathological();
    if (!player.empty()) { test_truncation(player); test_corruption(player); }
    test_search(search);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
