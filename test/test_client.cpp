// Validates the yj-based client against the captured live responses.
// Checks the things that were actually broken: DRC dedup, adaptive-first
// selection, high-res/high-bitrate availability, and search field extraction.
#include "yt_innertube.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace ytfast;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++failures; }
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

static const char* sv(std::string_view s) {
    static char buf[8][128]; static int i = 0;
    i = (i + 1) % 8;
    size_t n = s.size() < 127 ? s.size() : 127;
    memcpy(buf[i], s.data(), n); buf[i][n] = 0;
    return buf[i];
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "fixtures";

    // ---------------------------------------------------------------- player
    std::string body = slurp(dir + "/player_full.json");
    VideoInfo info;
    InnertubeClient::parse_player_into(body, "aqz-KE-bpKQ", info);

    std::printf("player_full.json -> %zu formats\n", info.formats.size());
    check(!info.formats.empty(), "formats extracted");
    check(!info.title.empty(),   "title extracted");
    check(info.duration_secs > 0,"duration extracted");

    int max_h = 0, max_fps = 0; int64_t max_abr = 0;
    int n_video_only = 0, n_audio_only = 0, n_muxed = 0, n_drc = 0;
    for (auto& f : info.formats) {
        if (f.height > max_h) max_h = f.height;
        if (f.fps > max_fps) max_fps = f.fps;
        if (f.is_video_only()) ++n_video_only;
        if (f.is_audio_only()) { ++n_audio_only; if (f.effective_bitrate() > max_abr) max_abr = f.effective_bitrate(); }
        if (f.is_muxed()) ++n_muxed;
        if (f.is_drc) ++n_drc;
        check(!f.url.empty(), "every format has a url");
        check(f.has_video || f.has_audio, "every format has a track");
    }
    std::printf("  video-only %d  audio-only %d  muxed %d  drc-kept %d\n",
                n_video_only, n_audio_only, n_muxed, n_drc);
    std::printf("  max height %d  max fps %d  max audio bitrate %lld\n",
                max_h, max_fps, (long long)max_abr);
    check(max_h >= 2160, "4K present");
    check(max_fps >= 60, "60fps present");
    check(n_drc == 0, "all DRC duplicates dropped in favour of clean audio");

    // no duplicate itags after dedup
    for (size_t i = 0; i < info.formats.size(); ++i)
        for (size_t j = i + 1; j < info.formats.size(); ++j)
            check(info.formats[i].itag != info.formats[j].itag, "no duplicate itags");

    // ------------------------------------------------------------- selection
    auto* a = InnertubeClient::pick_audio(info.formats);
    auto* v = InnertubeClient::pick_video(info.formats);
    auto* v1080 = InnertubeClient::pick_video(info.formats, 1080);
    auto* mux = InnertubeClient::pick_muxed(info.formats);

    std::printf("\nselection:\n");
    if (a) std::printf("  audio   itag %-4d %-9s %-6s %lld bps  %dch  drc=%d\n",
                       a->itag, sv(a->quality_label), sv(a->container),
                       (long long)a->effective_bitrate(), a->audio_channels, a->is_drc);
    if (v) std::printf("  video   itag %-4d %-9s %-6s %lld bps  %dx%d@%d\n",
                       v->itag, sv(v->quality_label), sv(v->container),
                       (long long)v->effective_bitrate(), v->width, v->height, v->fps);
    if (v1080) std::printf("  <=1080p itag %-4d %-9s %-6s %dx%d@%d\n",
                       v1080->itag, sv(v1080->quality_label), sv(v1080->container),
                       v1080->width, v1080->height, v1080->fps);
    if (mux) std::printf("  muxed   itag %-4d %-9s %dx%d  (what the old code shipped)\n",
                       mux->itag, sv(mux->quality_label), mux->width, mux->height);

    check(a && !a->is_drc, "selected audio is not DRC");
    check(a && a->effective_bitrate() >= 300000, "selected audio is the 5.1 track");
    check(v && v->height >= 2160, "selected video is 4K");
    check(v && v->fps >= 60, "selected video is 60fps");
    check(v1080 && v1080->height == 1080, "height cap respected");
    check(mux && mux->height <= 360, "muxed really is only 360p here");

    auto* aac = InnertubeClient::pick_audio(info.formats, "aac");
    auto* opus = InnertubeClient::pick_audio(info.formats, "opus");
    auto* stereo = InnertubeClient::pick_audio(info.formats, {}, false);
    if (aac)    std::printf("  aac     itag %-4d %lld bps\n", aac->itag, (long long)aac->effective_bitrate());
    if (opus)   std::printf("  opus    itag %-4d %lld bps\n", opus->itag, (long long)opus->effective_bitrate());
    if (stereo) std::printf("  stereo  itag %-4d %lld bps %dch\n", stereo->itag,
                            (long long)stereo->effective_bitrate(), stereo->audio_channels);
    check(aac && aac->audio_codec.rfind("mp4a", 0) == 0, "aac preference honoured");
    check(opus && opus->audio_codec == "opus", "opus preference honoured");
    check(stereo && stereo->audio_channels == 2, "surround exclusion honoured");

    auto* av1 = InnertubeClient::pick_video(info.formats, 0, "av1");
    auto* h264 = InnertubeClient::pick_video(info.formats, 0, "h264");
    check(av1 && av1->video_codec.rfind("av01", 0) == 0, "av1 preference honoured");
    check(h264 && h264->video_codec.rfind("avc1", 0) == 0, "h264 preference honoured");

    // range_url is built on demand and only when a length is known
    if (a) {
        std::string ru = a->range_url();
        check(a->content_length <= 0 || ru.find("&range=0-") != std::string::npos,
              "range_url appends a range");
    }

    // ------------------------------------------------- masked == full parity
    std::string masked = slurp(dir + "/player_masked.json");
    VideoInfo mi;
    InnertubeClient::parse_player_into(masked, "aqz-KE-bpKQ", mi);
    std::printf("\nfield-masked response -> %zu formats (full: %zu)\n",
                mi.formats.size(), info.formats.size());
    check(mi.formats.size() == info.formats.size(), "field mask loses no formats");
    int mmax_h = 0; int64_t mmax_abr = 0;
    for (auto& f : mi.formats) {
        if (f.height > mmax_h) mmax_h = f.height;
        if (f.is_audio_only() && f.effective_bitrate() > mmax_abr) mmax_abr = f.effective_bitrate();
    }
    check(mmax_h == max_h, "field mask loses no resolution");
    check(mmax_abr == max_abr, "field mask loses no audio bitrate");

    // ------------------------------------------------------------ SABR gate
    // Real capture from ANDROID 21.02.35: status OK, full adaptiveFormats,
    // not one url among them. Must be rejected outright, not silently
    // downgraded to the single 360p muxed format that does have one.
    {
        std::string sb = slurp(dir + "/player_sabr.json");
        if (!sb.empty()) {
            VideoInfo si;
            InnertubeClient::parse_player_into(sb, "aqz-KE-bpKQ", si);
            std::printf("\nSABR response -> %zu formats, sabr_only=%d\n",
                        si.formats.size(), (int)si.sabr_only);
            check(si.sabr_only, "SABR response flagged");
            check(si.formats.empty(), "SABR response yields no usable formats");
        }
    }

    // ---------------------------------------------------------------- search
    std::string sbody = slurp(dir + "/search.json");
    std::vector<SearchResult> res;
    InnertubeClient::parse_search_into(sbody, 15, res);
    std::printf("\nsearch.json -> %zu results (capped at 15)\n", res.size());
    check(res.size() == 15, "max_results respected");
    int titled = 0, chan = 0, dur = 0, live = 0;
    for (auto& r : res) {
        if (!r.title.empty()) ++titled;
        if (!r.channel.empty()) ++chan;
        if (r.duration_secs > 0) ++dur;
        if (r.is_live) ++live;
        check(!r.id.empty(), "result has an id");
        check(r.url.find(r.id) != std::string::npos, "result url contains the id");
    }
    std::printf("  titled %d  channel %d  duration %d  live %d\n", titled, chan, dur, live);
    check(titled == (int)res.size(), "all results titled");
    check(chan == (int)res.size(), "all results have a channel");
    for (int i = 0; i < 3 && i < (int)res.size(); ++i)
        std::printf("  [%s] %-52s | %-22s | %s\n", res[i].id.c_str(),
                    res[i].title.substr(0, 52).c_str(),
                    res[i].channel.substr(0, 22).c_str(),
                    res[i].duration_str.c_str());

    // unlimited
    std::vector<SearchResult> all;
    InnertubeClient::parse_search_into(sbody, 0, all);
    check(all.size() == 20, "uncapped search returns everything");

    // ---------------------------------------------------------- itag table
    std::printf("\nitag table:\n");
    check(itag_lookup(140) && itag_lookup(140)->container == "mp4", "itag 140 = m4a");
    check(itag_lookup(251) && itag_lookup(251)->acodec == "opus", "itag 251 = opus");
    check(itag_lookup(315) && itag_lookup(315)->height == 2160 &&
          itag_lookup(315)->fps == 60, "itag 315 = 2160p60");
    check(itag_lookup(18) && itag_lookup(18)->muxed(), "itag 18 is muxed");
    check(itag_lookup(999999) == nullptr, "unknown itag returns null");
    // binary search must agree with a linear scan over the whole table
    for (size_t i = 0; i < detail::kItagCount; ++i) {
        const ItagInfo& row = detail::kItags[i];
        check(itag_lookup(row.itag) == &row, "binary search finds every row");
        if (i) check(detail::kItags[i-1].itag < row.itag, "table is sorted and unique");
    }
    std::printf("  %zu rows, all reachable, sorted\n", detail::kItagCount);

    // -------------------------------------------------------------- garbage
    for (std::string junk : {"", "{", "null", "[]", "{\"streamingData\":{}}",
                             "{\"streamingData\":{\"adaptiveFormats\":[{}]}}",
                             "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\"}}"}) {
        VideoInfo g;
        InnertubeClient::parse_player_into(junk, "x", g);
        check(g.formats.empty(), "junk yields no formats");
        std::vector<SearchResult> gs;
        InnertubeClient::parse_search_into(junk, 10, gs);
        check(gs.empty(), "junk yields no search results");
    }
    // truncation sweep over the real body
    for (size_t n = 0; n < body.size(); n += 997) {
        VideoInfo g;
        InnertubeClient::parse_player_into(std::string_view(body).substr(0, n), "x", g);
        for (auto& f : g.formats) check(!f.url.empty(), "truncated: no empty urls");
    }
    std::printf("  survived %zu truncated player bodies\n", body.size() / 997);

    std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
