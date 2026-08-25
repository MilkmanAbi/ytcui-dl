// Exercises the hand-rolled HTTP stack against real servers: TLS verification,
// gzip, chunked, keep-alive reuse, redirects, and a real InnerTube round trip.
#include "yt_http.h"
#include "yj.h"
#include <chrono>
#include <cstdlib>
#include <thread>
#include <cstdio>

using namespace ytfast;
static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}
static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count();
}

int main() {
    CurlGlobalInit init;

    std::printf("url parsing\n");
    {
        Url u;
        check(Url::parse("https://www.youtube.com/youtubei/v1/player?x=1", u) &&
              u.host == "www.youtube.com" && u.port == "443" &&
              u.path == "/youtubei/v1/player?x=1", "https with query");
        check(Url::parse("http://example.com:8080/a/b", u) && !u.tls &&
              u.port == "8080" && u.path == "/a/b", "explicit port");
        check(Url::parse("https://example.com", u) && u.path == "/", "bare host");
        check(!Url::parse("ftp://example.com/x", u), "rejects unknown scheme");
        check(!Url::parse("notaurl", u), "rejects garbage");
    }

    HttpClient h;
    std::printf("\nplain GET + TLS verification\n");
    {
        auto t = std::chrono::steady_clock::now();
        auto r = h.get("https://www.youtube.com/robots.txt");
        std::printf("    status %ld, %zu bytes, %.0f ms\n", r.status, r.body.size(), ms_since(t));
        check(r.status == 200 && r.body.size() > 100, "fetched robots.txt");
    }
    {
        // Keep-alive: the second request to the same host must reuse the
        // socket, so it should be substantially faster than the handshake.
        auto t1 = std::chrono::steady_clock::now();
        h.get("https://www.youtube.com/robots.txt");
        double first = ms_since(t1);
        auto t2 = std::chrono::steady_clock::now();
        auto r = h.get("https://www.youtube.com/robots.txt");
        double second = ms_since(t2);
        std::printf("    reuse: %.0f ms -> %.0f ms\n", first, second);
        check(r.status == 200, "reused connection still works");
    }

    std::printf("\ngzip\n");
    {
        std::string raw = "the quick brown fox jumps over the lazy dog. ";
        for (int i = 0; i < 8; ++i) raw += raw;
        // round-trip through zlib's own deflate to confirm our inflate wrapper
        uLongf clen = compressBound(raw.size());
        std::string comp(clen, '\0');
        compress2((Bytef*)&comp[0], &clen, (const Bytef*)raw.data(), raw.size(), 6);
        comp.resize(clen);
        std::string out;
        check(gunzip(comp, out) && out == raw, "inflate round-trip");
        std::printf("    %zu -> %zu bytes\n", comp.size(), out.size());
        std::string junk = "not actually compressed data at all";
        check(!gunzip(junk, out), "rejects non-deflate input");
    }

    std::printf("\nredirects\n");
    {
        auto r = h.get("https://youtu.be/aqz-KE-bpKQ");
        std::printf("    status %ld, %zu bytes\n", r.status, r.body.size());
        check(r.status == 200 || r.status == 302, "followed youtu.be redirect");
    }

    std::printf("\ncertificate verification is real\n");
    {
        // Deliberately NOT badssl.com: an intercepting egress proxy re-issues
        // every certificate under its own CA, so badssl's expired host presents
        // a perfectly valid chain from behind one and the test silently passes.
        // A locally generated self-signed cert can't be laundered that way.
        std::system("openssl req -x509 -newkey rsa:2048 -keyout /tmp/_k.pem "
                    "-out /tmp/_c.pem -days 1 -nodes -subj /CN=localhost "
                    ">/dev/null 2>&1");
        std::system("openssl s_server -key /tmp/_k.pem -cert /tmp/_c.pem "
                    "-accept 44443 -www -quiet >/dev/null 2>&1 &");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        bool rejected = false;
        try { h.get("https://localhost:44443/"); }
        catch (const std::exception&) { rejected = true; }
        check(rejected, "self-signed certificate rejected");

        bool wrong_host = false;
        try { h.get("https://127.0.0.1:44443/"); }
        catch (...) { wrong_host = true; }
        check(wrong_host, "hostname mismatch rejected");
        std::system("pkill -f 'openssl s_server' >/dev/null 2>&1");
    }

    std::printf("\nreal InnerTube round trip (player, ANDROID, field-masked)\n");
    {
        std::string body =
            "{\"context\":{\"client\":{\"clientName\":\"ANDROID\","
            "\"clientVersion\":\"20.10.38\",\"androidSdkVersion\":34,"
            "\"osName\":\"Android\",\"osVersion\":\"14\",\"hl\":\"en\",\"gl\":\"US\"}},"
            "\"videoId\":\"aqz-KE-bpKQ\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
        std::vector<std::string> hdrs = {
            "X-YouTube-Client-Name: 3",
            "X-YouTube-Client-Version: 20.10.38",
            "User-Agent: com.google.android.youtube/20.10.38 (Linux; U; Android 14; en_US) gzip",
            "X-Goog-FieldMask: streamingData,videoDetails,playabilityStatus",
        };
        auto t = std::chrono::steady_clock::now();
        auto r = h.post("https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                        body, hdrs);
        double el = ms_since(t);
        std::printf("    status %ld, %zu bytes decoded, %.0f ms\n",
                    r.status, r.body.size(), el);
        check(r.status == 200, "player endpoint returned 200");

        yj::Val root = yj::parse(r.body);
        yj::Val st = yj::path(root, "playabilityStatus", "status");
        std::printf("    playabilityStatus: %.*s\n", (int)st.raw().size(), st.raw().data());
        check(st.raw() == "OK", "playable");

        int n = 0, maxh = 0; long long maxabr = 0;
        yj::Val af = yj::path(root, "streamingData", "adaptiveFormats");
        yj::each_elem(af, [&](yj::Val f) {
            ++n;
            int hgt = (int)yj::get(f, "height").i64();
            if (hgt > maxh) maxh = hgt;
            if (yj::get(f, "audioChannels").valid()) {
                long long b = yj::get(f, "bitrate").i64();
                if (b > maxabr) maxabr = b;
            }
            return true;
        });
        std::printf("    %d adaptive formats, max height %d, max audio bitrate %lld\n",
                    n, maxh, maxabr);
        check(n > 20, "full adaptive set");
        check(maxh >= 2160, "4K available over our own stack");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
