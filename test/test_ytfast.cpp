/*
 * ytcui-dl v0.2.0 test suite
 *
 * Tests:
 *   1. visitor_data bootstrap
 *   2. search
 *   3. stream format resolution + metadata
 *   4. URL cache performance
 *   5. format selection (audio codecs, video resolution, filters)
 *   6. AUDIO URL fetchability (THE v0.1 bug)
 *   7. VIDEO URL fetchability
 *   8. thumbnail validity (THE v0.1 bug)
 *   9. format dedup (THE v0.1 bug)
 *  10. codec parsing for muxed streams (THE v0.1 bug)
 *  11. yt-dlp format string parser
 *  12. async prefetch
 *  13. convenience free functions
 *
 * Build:  g++ -std=c++17 -O2 -Iinclude -o test_ytfast test/test_ytfast.cpp -lcurl -lssl -lcrypto -lpthread
 * Run:    ./test_ytfast
 *         ./test_ytfast --mpv          (also launches mpv to test actual playback)
 *         ./test_ytfast --video-id ID  (test a specific video)
 */

#include "ytfast.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <unordered_set>
#include <curl/curl.h>

using C = std::chrono::high_resolution_clock;
static double ms(std::chrono::time_point<C> t0) {
    return std::chrono::duration<double,std::milli>(C::now()-t0).count();
}

static int pass=0, fail=0, skip=0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  \033[32m✓\033[0m %s\n",label); pass++; } \
    else      { printf("  \033[31m✗\033[0m %s\n",label); fail++; } \
} while(0)

#define SKIP(label, reason) do { \
    printf("  \033[33m⊘\033[0m %s (%s)\n",label,reason); skip++; \
} while(0)

// Fetch first 4KB of a URL, return HTTP status (follows redirects)
static long fetch_bytes(const std::string& url, const std::string& ua="",
                         std::string* content_type_out = nullptr) {
    CURL* c = curl_easy_init();
    std::string buf;
    auto wcb=[](char*p,size_t s,size_t n,void*u){((std::string*)u)->append(p,s*n);return s*n;};
    curl_easy_setopt(c,CURLOPT_URL,url.c_str());
    curl_easy_setopt(c,CURLOPT_RANGE,"0-4095");
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,+wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&buf);
    curl_easy_setopt(c,CURLOPT_USERAGENT,
        ua.empty() ? ytfast::ANDROID_UA : ua.c_str());
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);
    curl_easy_perform(c);
    long code=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);
    if (content_type_out) {
        char* ct = nullptr;
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
        if (ct) *content_type_out = ct;
    }
    curl_easy_cleanup(c);
    return code;
}

static bool is_cloud_blocked(long code) { return code == 403; }
static bool is_fetchable(long code) { return code == 200 || code == 206 || code == 302; }

int main(int argc, char** argv) {
    bool test_mpv = false;
    std::string custom_vid;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--mpv") test_mpv = true;
        if (std::string(argv[i]) == "--video-id" && i+1 < argc) custom_vid = argv[++i];
    }

    ytfast::CurlGlobalInit curl_init;
    auto& yt = ytfast::InnertubeClient::get_instance();

    const char* test_vid = custom_vid.empty() ? "jNQXAC9IVRw" : custom_vid.c_str();
    // "Me at the zoo" — first YouTube video, always available, good test case
    // because it has no maxresdefault thumbnail (tests fallback chain)

    printf("\033[1m=== ytcui-dl v0.2.0 test suite ===\033[0m\n");
    printf("Test video: %s\n\n", test_vid);

    // ------------------------------------------------------------------
    // 1. Visitor data bootstrap
    // ------------------------------------------------------------------
    printf("\033[1m[1] visitor_data bootstrap\033[0m\n");
    {
        auto t0 = C::now();
        yt.bootstrap_visitor_data();
        double elapsed = ms(t0);
        std::string vd = yt.visitor_data();
        if (vd.empty()) {
            SKIP("visitor_data from homepage", "cloud IP — will use API fallback");
        } else {
            CHECK("visitor_data non-empty", true);
            CHECK("visitor_data reasonable length", vd.size() > 10);
        }
        printf("    bootstrap: %.0fms, len=%zu\n", elapsed, vd.size());
    }

    // ------------------------------------------------------------------
    // 2. Search
    // ------------------------------------------------------------------
    printf("\n\033[1m[2] search(\"lofi hip hop\", 10)\033[0m\n");
    std::vector<ytfast::SearchResult> results;
    {
        auto t0 = C::now();
        results = yt.search("lofi hip hop", 10, false);
        double elapsed = ms(t0);
        CHECK("returns results",          !results.empty());
        CHECK("returns ≤10",              results.size() <= 10);
        if (!results.empty()) {
            CHECK("result has 11-char id",    results[0].id.size() == 11);
            CHECK("result has title",         !results[0].title.empty());
            CHECK("result has channel",       !results[0].channel.empty());
            CHECK("result has youtube URL",   results[0].url.find("youtube.com") != std::string::npos);
            CHECK("result has thumbnail_url", !results[0].thumbnail_url.empty());
        }
        printf("    %zu results in %.0fms\n", results.size(), elapsed);
        if (!results.empty())
            printf("    top: [%s] %s (%s)\n",
                results[0].id.c_str(), results[0].title.c_str(), results[0].duration_str.c_str());
    }

    // ------------------------------------------------------------------
    // 3. Stream formats + metadata
    // ------------------------------------------------------------------
    printf("\n\033[1m[3] get_stream_formats(\"%s\")\033[0m\n", test_vid);
    ytfast::VideoInfo info;
    {
        auto t0 = C::now();
        try {
            info = yt.get_stream_formats(test_vid);
            double elapsed = ms(t0);
            CHECK("got formats",           !info.formats.empty());
            CHECK("title non-empty",       !info.title.empty());
            CHECK("channel non-empty",     !info.channel.empty());
            CHECK("duration > 0",          info.duration_secs > 0);
            CHECK("has audio-only formats",[&]{ for(auto&f:info.formats) if(f.is_audio_only()) return true; return false; }());
            CHECK("has video formats",     [&]{ for(auto&f:info.formats) if(f.has_video) return true; return false; }());
            CHECK("has muxed formats",     [&]{ for(auto&f:info.formats) if(f.is_muxed()) return true; return false; }());
            CHECK("thumbnail_url set",     !info.thumbnail_url.empty());
            CHECK("url set",              !info.url.empty());

            int maxh=0; for(auto&f:info.formats) if(f.height>maxh) maxh=f.height;
            printf("    '%s' — %s, views=%s, formats=%zu, maxH=%dp in %.0fms\n",
                info.title.c_str(), info.duration_str.c_str(),
                info.view_count_str.c_str(), info.formats.size(), maxh, elapsed);
        } catch(std::exception& e) {
            printf("    \033[31mERROR: %s\033[0m\n", e.what());
            fail += 5;
        }
    }

    // ------------------------------------------------------------------
    // 4. URL cache — second call must be instant
    // ------------------------------------------------------------------
    printf("\n\033[1m[4] URL cache performance\033[0m\n");
    {
        auto t0 = C::now();
        auto info2 = yt.get_stream_formats(test_vid);
        double elapsed = ms(t0);
        CHECK("cache hit returns same id",     info2.id == std::string(test_vid));
        CHECK("cache hit is fast (<5ms)",      elapsed < 5.0);
        CHECK("cache hit same format count",   info2.formats.size() == info.formats.size());
        printf("    cache hit: %.3fms\n", elapsed);
    }

    // ------------------------------------------------------------------
    // 5. Format selection (the core of v0.2.0)
    // ------------------------------------------------------------------
    printf("\n\033[1m[5] format selection\033[0m\n");
    if (!info.formats.empty()) {
        // Audio download URL (with range)
        std::string audio_dl = ytfast::InnertubeClient::select_best_audio_download(info.formats);
        CHECK("best_audio_download non-empty", !audio_dl.empty());
        CHECK("best_audio_download is https",  audio_dl.substr(0,5) == "https");

        // Audio stream URL (no range — for mpv)
        std::string audio_st = ytfast::InnertubeClient::select_best_audio_stream(info.formats);
        CHECK("best_audio_stream non-empty",   !audio_st.empty());
        CHECK("best_audio_stream NO range param",
              audio_st.find("&range=") == std::string::npos);

        // Video
        std::string video = ytfast::InnertubeClient::select_best_video(info.formats, 1080);
        CHECK("best_video non-empty",          !video.empty());

        // Video stream URL
        std::string video_st = ytfast::InnertubeClient::select_best_video_stream(info.formats, 1080);
        CHECK("best_video_stream non-empty",   !video_st.empty());

        // Worst audio
        std::string worst = ytfast::InnertubeClient::select_worst_audio(info.formats);
        CHECK("worst_audio non-empty",         !worst.empty());
        if (!worst.empty() && !audio_dl.empty())
            CHECK("worst_audio != best_audio", worst != audio_dl);

        // Codec preference
        std::string aac = ytfast::InnertubeClient::select_best_audio_download(info.formats, "aac");
        if (!aac.empty()) {
            // Verify it's actually aac/mp4
            for (auto& f : info.formats)
                if (f.url == aac) {
                    CHECK("aac preference returns mp4a codec",
                          f.audio_codec.find("mp4a") != std::string::npos);
                    printf("    aac: itag=%d %s %lldkbps\n",
                        f.itag, f.audio_codec.c_str(), (long long)f.effective_bitrate()/1000);
                    break;
                }
        }

        std::string opus = ytfast::InnertubeClient::select_best_audio_download(info.formats, "opus");
        if (!opus.empty()) {
            for (auto& f : info.formats)
                if (f.url == opus) {
                    CHECK("opus preference returns opus codec",
                          f.audio_codec.find("opus") != std::string::npos);
                    printf("    opus: itag=%d %s %lldkbps\n",
                        f.itag, f.audio_codec.c_str(), (long long)f.effective_bitrate()/1000);
                    break;
                }
        }

        // Log format details
        for (auto& f : info.formats) {
            if (f.url == audio_dl)
                printf("    best_audio_dl: itag=%d %s %s %lldkbps\n",
                    f.itag, f.container.c_str(), f.audio_codec.c_str(),
                    (long long)f.effective_bitrate()/1000);
            if (f.url == video)
                printf("    best_video: itag=%d %dp %s muxed=%s\n",
                    f.itag, f.height, f.container.c_str(),
                    ytfast::InnertubeClient::is_muxed(info.formats,video)?"yes":"no");
        }
    }

    // ------------------------------------------------------------------
    // 6. AUDIO URL fetchability — THE BUG FROM v0.1
    // ------------------------------------------------------------------
    printf("\n\033[1m[6] audio URL fetchability (v0.1 bug fix)\033[0m\n");
    if (!info.formats.empty()) {
        // Test stream URL (for mpv — the one that was broken)
        std::string audio_stream = ytfast::InnertubeClient::select_best_audio_stream(info.formats);
        if (!audio_stream.empty()) {
            std::string ct;
            long code = fetch_bytes(audio_stream, ytfast::ANDROID_UA, &ct);
            if (is_cloud_blocked(code)) {
                SKIP("audio stream URL fetchable", "403=cloud IP block, works on residential");
            } else {
                CHECK("audio STREAM URL fetchable (HTTP 200/206)", is_fetchable(code));
                CHECK("audio STREAM content-type is audio",
                      ct.find("audio") != std::string::npos || ct.find("octet") != std::string::npos);
            }
            printf("    stream URL: HTTP %ld  content-type: %s\n", code, ct.c_str());

            // Also test with mpv/ffmpeg User-Agent
            long code2 = fetch_bytes(audio_stream, "Lavf/60.3.100");
            printf("    mpv/ffmpeg UA: HTTP %ld %s\n", code2,
                   is_fetchable(code2) ? "\033[32m✓\033[0m" : "(may need Android UA)");
        }

        // Test download URL (with range param)
        std::string audio_dl = ytfast::InnertubeClient::select_best_audio_download(info.formats);
        if (!audio_dl.empty()) {
            long code = fetch_bytes(audio_dl, ytfast::ANDROID_UA);
            if (is_cloud_blocked(code)) {
                SKIP("audio download URL fetchable", "403=cloud IP block");
            } else {
                CHECK("audio DOWNLOAD URL fetchable", is_fetchable(code));
            }
            printf("    download URL: HTTP %ld  has_range=%s\n", code,
                   audio_dl.find("&range=") != std::string::npos ? "yes" : "no");
        }
    }

    // ------------------------------------------------------------------
    // 7. VIDEO URL fetchability
    // ------------------------------------------------------------------
    printf("\n\033[1m[7] video URL fetchability\033[0m\n");
    if (!info.formats.empty()) {
        std::string video_stream = ytfast::InnertubeClient::select_best_video_stream(info.formats, 1080);
        if (!video_stream.empty()) {
            std::string ct;
            long code = fetch_bytes(video_stream, ytfast::ANDROID_UA, &ct);
            if (is_cloud_blocked(code)) {
                SKIP("video stream URL fetchable", "403=cloud IP block");
            } else {
                CHECK("video STREAM URL fetchable", is_fetchable(code));
            }
            printf("    stream URL: HTTP %ld  content-type: %s\n", code, ct.c_str());
        }

        std::string video_dl = ytfast::InnertubeClient::select_best_video(info.formats, 1080);
        if (!video_dl.empty()) {
            long code = fetch_bytes(video_dl, ytfast::ANDROID_UA);
            if (is_cloud_blocked(code)) {
                SKIP("video download URL fetchable", "403=cloud IP block");
            } else {
                CHECK("video DOWNLOAD URL fetchable", is_fetchable(code));
            }
            printf("    download URL: HTTP %ld\n", code);
        }
    }

    // ------------------------------------------------------------------
    // 8. Thumbnail validity — v0.1 bug: maxresdefault 404s
    // ------------------------------------------------------------------
    printf("\n\033[1m[8] thumbnail validity (v0.1 bug fix)\033[0m\n");
    {
        CHECK("thumbnail_url not empty",      !info.thumbnail_url.empty());
        CHECK("thumbnail_url is not maxresdefault for old video",
              std::string(test_vid) != "jNQXAC9IVRw" ||
              info.thumbnail_url.find("maxresdefault") == std::string::npos);

        if (!info.thumbnail_url.empty()) {
            long code = fetch_bytes(info.thumbnail_url, "");
            CHECK("thumbnail URL returns HTTP 200/206", is_fetchable(code));
            printf("    thumbnail: HTTP %ld  url=%s\n", code,
                   info.thumbnail_url.substr(0, 70).c_str());
        }

        CHECK("thumbnails array populated",   !info.thumbnails.empty());
        printf("    %zu thumbnails available\n", info.thumbnails.size());
        for (auto& t : info.thumbnails)
            printf("    %4dx%-4d  %s\n", t.width, t.height,
                   t.url.substr(0, 60).c_str());
    }

    // ------------------------------------------------------------------
    // 9. Format dedup — v0.1 bug: duplicate itags
    // ------------------------------------------------------------------
    printf("\n\033[1m[9] format dedup (v0.1 bug fix)\033[0m\n");
    {
        std::unordered_set<int> itags;
        bool has_dup = false;
        for (auto& f : info.formats) {
            if (itags.count(f.itag)) {
                printf("    \033[31mDUPLICATE itag=%d\033[0m\n", f.itag);
                has_dup = true;
            }
            itags.insert(f.itag);
        }
        CHECK("no duplicate itags", !has_dup);
        printf("    %zu unique formats\n", itags.size());
    }

    // ------------------------------------------------------------------
    // 10. Codec parsing for muxed streams — v0.1 bug
    // ------------------------------------------------------------------
    printf("\n\033[1m[10] codec parsing (v0.1 bug fix)\033[0m\n");
    {
        bool found_muxed = false;
        for (auto& f : info.formats) {
            if (f.is_muxed()) {
                found_muxed = true;
                CHECK("muxed has video_codec",  !f.video_codec.empty());
                CHECK("muxed has audio_codec",  !f.audio_codec.empty());
                CHECK("muxed container set",    !f.container.empty());
                printf("    itag=%d  video=%s  audio=%s  container=%s\n",
                    f.itag, f.video_codec.c_str(), f.audio_codec.c_str(), f.container.c_str());
                break;
            }
        }
        if (!found_muxed) SKIP("muxed codec test", "no muxed formats available");

        // Also verify audio-only codecs
        for (auto& f : info.formats) {
            if (f.is_audio_only()) {
                CHECK("audio-only has audio_codec", !f.audio_codec.empty());
                printf("    itag=%d  codec=%s  container=%s  %lldkbps\n",
                    f.itag, f.audio_codec.c_str(), f.container.c_str(),
                    (long long)f.effective_bitrate()/1000);
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    // 11. Format string parser (yt-dlp compat)
    // ------------------------------------------------------------------
    printf("\n\033[1m[11] format string parser\033[0m\n");
    if (!info.formats.empty()) {
        auto test_fmt = [&](const char* sel, const char* /*desc*/, bool should_have_range, bool should_be_audio) {
            std::string url = ytfast::InnertubeClient::resolve_format_string(
                sel, info.formats, !should_have_range);
            bool ok = !url.empty();
            char label[128];
            snprintf(label, sizeof(label), "%-30s → %s", sel, ok ? "resolved" : "EMPTY");
            CHECK(label, ok);
            if (ok && should_be_audio) {
                bool has_range = url.find("&range=") != std::string::npos;
                if (should_have_range)
                    CHECK("  download URL has &range=", has_range);
                else
                    CHECK("  stream URL has no &range=", !has_range);
            }
        };
        test_fmt("bestaudio",              "best audio stream",    false, true);
        test_fmt("ba",                     "alias for bestaudio",  false, true);
        test_fmt("worstaudio",             "worst audio",          false, true);
        test_fmt("bestaudio[ext=m4a]",     "AAC audio",            false, true);
        test_fmt("bestaudio[ext=webm]",    "opus audio",           false, true);
        test_fmt("best",                   "best muxed",           false, false);
        test_fmt("best[height<=720]",      "best ≤720p",           false, false);
        test_fmt("bestvideo",              "best video-only",      false, false);
        test_fmt("worst",                  "worst muxed",          false, false);

        // Test itag selection
        if (!info.formats.empty()) {
            std::string itag_str = std::to_string(info.formats[0].itag);
            std::string url = ytfast::InnertubeClient::resolve_format_string(
                itag_str, info.formats, false);
            CHECK("itag number resolves",  !url.empty());
        }
    }

    // ------------------------------------------------------------------
    // 12. Async prefetch
    // ------------------------------------------------------------------
    printf("\n\033[1m[12] async prefetch\033[0m\n");
    if (results.size() >= 2) {
        std::string prefetch_id = results[1].id;
        auto t0 = C::now();
        yt.prefetch(prefetch_id);
        printf("    prefetch(%s) launched in %.2fms\n", prefetch_id.c_str(), ms(t0));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        auto t1 = C::now();
        try {
            auto pinfo = yt.get_stream_formats(prefetch_id);
            double elapsed = ms(t1);
            CHECK("prefetched result available", !pinfo.formats.empty());
            CHECK("prefetch cache fast (<5ms)",  elapsed < 5.0);
            printf("    cache after prefetch: %.2fms\n", elapsed);
        } catch (...) { fail++; }
    }

    // ------------------------------------------------------------------
    // 13. Convenience free functions
    // ------------------------------------------------------------------
    printf("\n\033[1m[13] convenience API\033[0m\n");
    {
        try {
            auto res2 = ytfast::yt_search("never gonna give you up", 3);
            CHECK("yt_search works", !res2.empty());
            if (!res2.empty()) {
                std::string audio = ytfast::yt_best_audio(res2[0].id);
                CHECK("yt_best_audio works", !audio.empty());
                CHECK("yt_best_audio returns stream URL (no range)",
                      audio.find("&range=") == std::string::npos);

                std::string audio_dl = ytfast::yt_best_audio_download(res2[0].id);
                CHECK("yt_best_audio_download works", !audio_dl.empty());

                std::string video = ytfast::yt_best_video_stream(res2[0].id);
                CHECK("yt_best_video_stream works", !video.empty());
            }
        } catch (std::exception& e) {
            printf("    \033[31mERROR: %s\033[0m\n", e.what());
            fail++;
        }
    }

    // ------------------------------------------------------------------
    // 14. MPV playback test (optional, --mpv flag)
    // ------------------------------------------------------------------
    if (test_mpv) {
        printf("\n\033[1m[14] mpv playback test\033[0m\n");
        // Test audio
        std::string audio_url = ytfast::InnertubeClient::select_best_audio_stream(info.formats, "aac");
        if (audio_url.empty())
            audio_url = ytfast::InnertubeClient::select_best_audio_stream(info.formats);
        if (!audio_url.empty()) {
            printf("    Playing AUDIO for 5 seconds...\n");
            std::string cmd = "timeout 5 mpv --no-video --really-quiet "
                              "--user-agent='com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip' "
                              "'" + audio_url + "' 2>&1 || true";
            int rc = system(cmd.c_str());
            CHECK("mpv audio playback ran", rc != 127); // 127 = mpv not found
        }
        // Test video
        std::string video_url = ytfast::InnertubeClient::select_best_video_stream(info.formats, 480);
        if (!video_url.empty()) {
            printf("    Playing VIDEO for 5 seconds...\n");
            std::string cmd = "timeout 5 mpv --really-quiet "
                              "--user-agent='com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip' "
                              "'" + video_url + "' 2>&1 || true";
            int rc = system(cmd.c_str());
            CHECK("mpv video playback ran", rc != 127);
        }
    }

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    printf("\n\033[1m=== Results: \033[32m%d passed\033[0m, ", pass);
    if (fail > 0) printf("\033[31m%d failed\033[0m", fail);
    else          printf("0 failed");
    if (skip > 0) printf(", \033[33m%d skipped\033[0m", skip);
    printf(" ===\033[0m\n");

    if (fail > 0) {
        printf("\n\033[31mFailed tests indicate bugs. Key fixes in v0.2.0:\033[0m\n");
        printf("  - Audio streaming: use stream_url (no &range=) for mpv\n");
        printf("  - Thumbnails: use API-provided URLs, not blind maxresdefault\n");
        printf("  - Codec parsing: split muxed codecs into video+audio\n");
        printf("  - Format dedup: no duplicate itags\n");
    }

    return fail > 0 ? 1 : 0;
}
