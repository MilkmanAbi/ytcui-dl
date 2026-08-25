// Selection engine: three modes, resolution ladder, graceful switching.
// Runs entirely on captured fixtures — no network.
#include "ytfast.h"
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

using namespace ytfast;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++failures; }
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "test/fixtures";
    VideoInfo info;
    InnertubeClient::parse_player_into(slurp(dir + "/player_full.json"), "aqz-KE-bpKQ", info);
    check(!info.formats.empty(), "fixture parsed");
    if (info.formats.empty()) return 1;
    const auto& F = info.formats;

    // ------------------------------------------------------------ the ladder
    auto ladder = Selector::ladder(F);
    std::printf("resolution ladder (%zu rungs):\n", ladder.size());
    for (const auto& r : ladder)
        std::printf("   %-10s itag %-4d %-6.*s %-5.*s %8s\n", r.label().c_str(), r.itag,
                    (int)r.codec.size(), r.codec.data(),
                    (int)r.container.size(), r.container.data(),
                    human_bytes(r.bytes).c_str());
    check(!ladder.empty(), "ladder non-empty");
    check(ladder.front().height >= ladder.back().height, "ladder sorted high to low");
    // one rung per height+fps+hdr, no duplicates
    std::set<std::tuple<int,int,bool>> seen;
    for (const auto& r : ladder)
        check(seen.insert({r.height, r.fps > 30 ? r.fps : 30, r.hdr}).second,
              "no duplicate rungs");

    auto aud = Selector::audio_ladder(F);
    std::printf("\naudio ladder (%zu):\n", aud.size());
    for (auto* a : aud)
        std::printf("   itag %-4d %7lld bps  %dch  %-10.*s drc=%d\n", a->itag,
                    (long long)a->effective_bitrate(), a->audio_channels,
                    (int)a->audio_codec.size(), a->audio_codec.data(), (int)a->is_drc);
    check(!aud.empty(), "audio ladder non-empty");
    for (auto* a : aud) check(!a->is_drc, "no DRC track survives into the ladder");

    // ------------------------------------------------------------- the modes
    std::printf("\nmodes:\n");
    auto ao = Selector::select(F, Mode::AudioOnly);
    auto vo = Selector::select(F, Mode::VideoOnly);
    auto av = Selector::select(F, Mode::AudioVideo);
    std::printf("   AudioOnly  : %s\n", ao.describe().c_str());
    std::printf("   VideoOnly  : %s\n", vo.describe().c_str());
    std::printf("   AudioVideo : %s\n", av.describe().c_str());

    check(ao.ok() && ao.audio && !ao.video, "AudioOnly gives an audio track and no video");
    check(ao.audio->is_audio_only(), "AudioOnly picks an adaptive audio stream");
    check(vo.ok() && vo.video && !vo.audio, "VideoOnly gives a video track and no audio");
    check(vo.video->is_video_only(), "VideoOnly picks an adaptive video stream");
    check(av.ok() && av.video && av.audio, "AudioVideo gives both");
    check(!av.muxed, "AudioVideo prefers separate adaptive streams over muxed");
    check(av.video->height >= 2160, "AudioVideo reaches 4K");
    check(av.audio->effective_bitrate() > 300000, "AudioVideo reaches the 5.1 track");
    check(av.total_bitrate() > av.video->effective_bitrate(), "total bitrate sums both tracks");
    check(av.total_bytes() > 0, "total bytes known");

    // --------------------------------------------------- targeting a height
    std::printf("\ntargeted heights:\n");
    for (int h : {144, 240, 360, 480, 720, 1080, 1440, 2160, 4320}) {
        auto s = Selector::select(F, Mode::AudioVideo, Quality::at(h));
        std::printf("   ask %-5d -> %-9s (itag %d)\n", h, s.describe().c_str(),
                    s.video ? s.video->itag : 0);
        check(s.ok(), "targeted selection resolves");
        // Never exceed what was asked for, unless nothing at or below exists.
        bool any_at_or_below = false;
        for (const auto& f : F)
            if (f.is_video_only() && f.height <= h) { any_at_or_below = true; break; }
        if (any_at_or_below) check(s.video->height <= h, "target height respected as a ceiling");
    }

    // A ceiling and a target are different things.
    auto cap = Selector::select(F, Mode::VideoOnly, Quality::upto(720));
    check(cap.video && cap.video->height <= 720, "max_height honoured");
    std::printf("   upto(720) -> %s\n", cap.describe().c_str());

    auto low = Selector::select(F, Mode::AudioVideo, Quality::lowest());
    std::printf("   lowest()  -> %s\n", low.describe().c_str());
    check(low.ok(), "lowest() resolves");
    check(low.audio && low.audio->audio_channels <= 2, "lowest() stays stereo");
    check(low.video && low.video->height == ladder.back().height,
          "lowest() actually picks the lowest rung");
    for (const auto& f : F)
        if (f.is_audio_only() && f.audio_channels <= 2)
            check(low.audio->effective_bitrate() <= f.effective_bitrate(),
                  "lowest() picks the lowest-bitrate stereo track");
    // Asking below everything available must land on the nearest rung ABOVE,
    // not on the best stream in the list.
    {
        std::vector<StreamFormat> hi;
        for (const auto& f : F) if (f.is_video_only() && f.height >= 1080) hi.push_back(f);
        for (const auto& f : F) if (f.is_audio_only()) hi.push_back(f);
        auto s2 = Selector::select(hi, Mode::AudioVideo, Quality::at(240));
        std::printf("   ask 240 of a 1080p+ list -> %s\n", s2.describe().c_str());
        check(s2.ok() && s2.video->height == 1080,
              "unreachable target lands on the nearest rung above, not the highest");
    }

    // ------------------------------------------------- codec / container hints
    std::printf("\nhints:\n");
    struct { const char* v; const char* a; const char* c; } hints[] = {
        {"av01", nullptr, nullptr}, {"vp9", nullptr, nullptr}, {"avc1", nullptr, nullptr},
        {"h264", nullptr, nullptr}, {nullptr, "opus", nullptr}, {nullptr, "mp4a", nullptr},
        {nullptr, "aac", nullptr},  {nullptr, nullptr, "webm"}, {nullptr, nullptr, "mp4"},
        {"nonsense", "nonsense", "nonsense"},
    };
    for (auto& h : hints) {
        Quality q;
        if (h.v) q.vcodec = h.v;
        if (h.a) q.acodec = h.a;
        if (h.c) q.container = h.c;
        auto s = Selector::select(F, Mode::AudioVideo, q);
        std::printf("   v=%-9s a=%-9s c=%-9s -> %s\n",
                    h.v ? h.v : "-", h.a ? h.a : "-", h.c ? h.c : "-", s.describe().c_str());
        check(s.ok(), "hinted selection still resolves");
        if (h.v && std::string(h.v) == "av01")
            check(s.video->video_codec.rfind("av01", 0) == 0, "av01 hint honoured");
        if (h.v && std::string(h.v) == "h264")
            check(s.video->video_codec.rfind("avc1", 0) == 0, "h264 alias honoured");
        if (h.a && std::string(h.a) == "opus")
            check(s.audio->audio_codec == "opus", "opus hint honoured");
        if (h.a && std::string(h.a) == "aac")
            check(s.audio->audio_codec.rfind("mp4a", 0) == 0, "aac alias honoured");
    }
    // The nonsense row is the important one: an unsatisfiable hint must
    // degrade to a working pick rather than returning nothing.
    {
        Quality q; q.vcodec = "nonsense"; q.acodec = "nonsense"; q.container = "nonsense";
        auto s = Selector::select(F, Mode::AudioVideo, q);
        check(s.ok() && s.video && s.audio, "unsatisfiable hints degrade instead of failing");
    }

    // --------------------------------------------------- graceful switching
    std::printf("\ngraceful switching (audio must not change):\n");
    auto cur = Selector::select(F, Mode::AudioVideo, Quality::at(1080));
    const StreamFormat* audio_before = cur.audio;
    std::printf("   start        %s\n", cur.describe().c_str());
    for (int h : {2160, 720, 360, 1440}) {
        auto next = Selector::switch_video(F, cur, h);
        std::printf("   switch %-5d %-28s audio same: %s\n", h, next.describe().c_str(),
                    next.audio == audio_before ? "yes" : "NO");
        check(next.audio == audio_before, "switching resolution keeps the same audio track");
        check(next.ok(), "switched selection is usable");
        cur = next;
    }

    // Stepping the ladder settles at the ends instead of wrapping.
    std::printf("\nstepping:\n");
    int h = ladder.back().height;
    std::printf("   from %d up: ", h);
    for (int i = 0; i < 12; ++i) { h = Selector::step_height(F, h, +1); std::printf("%d ", h); }
    std::printf("\n");
    check(h == ladder.front().height, "stepping up reaches the top and stays");
    std::printf("   from %d down: ", h);
    for (int i = 0; i < 12; ++i) { h = Selector::step_height(F, h, -1); std::printf("%d ", h); }
    std::printf("\n");
    check(h == ladder.back().height, "stepping down reaches the bottom and stays");
    check(Selector::step_height(F, 99999, -1) > 0, "stepping from off-ladder lands on a rung");

    // --------------------------------------------------------- muxed policy
    {
        Quality q; q.allow_muxed = false;
        auto s = Selector::select(F, Mode::AudioVideo, q);
        check(s.ok() && !s.muxed, "allow_muxed=false still resolves via adaptive");
    }
    // A format list containing ONLY a muxed stream must still work in all
    // three modes — this is the SABR-degraded case.
    {
        std::vector<StreamFormat> only_muxed;
        for (const auto& f : F) if (f.is_muxed()) only_muxed.push_back(f);
        check(!only_muxed.empty(), "fixture has a muxed stream to test with");
        for (Mode m : {Mode::AudioOnly, Mode::VideoOnly, Mode::AudioVideo}) {
            auto s = Selector::select(only_muxed, m);
            check(s.ok(), "muxed-only list still resolves");
            check(s.muxed, "muxed-only list reports muxed");
            if (m == Mode::AudioVideo)
                check(s.audio == nullptr, "muxed selection exposes no separate audio");
        }
        std::printf("\nmuxed-only fallback: %s\n",
                    Selector::select(only_muxed, Mode::AudioVideo).describe().c_str());
    }

    // -------------------------------------------------------------- degenerate
    {
        std::vector<StreamFormat> empty;
        for (Mode m : {Mode::AudioOnly, Mode::VideoOnly, Mode::AudioVideo}) {
            auto s = Selector::select(empty, m);
            check(!s.ok(), "empty list yields an unusable selection");
            check(s.describe() == "nothing", "empty selection describes itself");
            check(s.total_bytes() == 0 && s.total_bitrate() == 0, "empty totals are zero");
        }
        check(Selector::ladder(empty).empty(), "empty ladder");
        check(Selector::step_height(empty, 720, 1) == 720, "stepping an empty ladder is a no-op");
        auto s = Selector::switch_video(empty, Selector::select(empty, Mode::AudioVideo), 720);
        check(!s.ok(), "switching on an empty list stays unusable");
    }
    // Audio-only list in video mode, and vice versa.
    {
        std::vector<StreamFormat> only_audio, only_video;
        for (const auto& f : F) {
            if (f.is_audio_only()) only_audio.push_back(f);
            if (f.is_video_only()) only_video.push_back(f);
        }
        check(!Selector::select(only_audio, Mode::VideoOnly).ok(), "no video in an audio-only list");
        check(Selector::select(only_audio, Mode::AudioOnly).ok(), "audio-only list serves audio");
        check(!Selector::select(only_video, Mode::AudioOnly).ok(), "no audio in a video-only list");
        check(Selector::select(only_video, Mode::VideoOnly).ok(), "video-only list serves video");
        auto s = Selector::select(only_video, Mode::AudioVideo);
        check(!s.ok(), "AudioVideo needs audio and says so when there is none");
    }

    // ------------------------------------------------------- SABR fixture
    {
        std::string sb = slurp(dir + "/player_sabr.json");
        if (!sb.empty()) {
            VideoInfo si;
            InnertubeClient::parse_player_into(sb, "aqz-KE-bpKQ", si);
            check(si.sabr_only, "SABR fixture flagged");
            for (Mode m : {Mode::AudioOnly, Mode::VideoOnly, Mode::AudioVideo})
                check(!Selector::select(si.formats, m).ok(), "SABR yields nothing selectable");
        }
    }

    // ------------------------------------------------------------- filenames
    std::printf("\nfilenames:\n");
    {
        VideoInfo v = info;
        v.title = "Big Buck Bunny 4K/60 \"test\": a*b?c|d";
        std::printf("   %s\n", Downloader::suggest_filename(v, *av.video).c_str());
        std::string n = Downloader::suggest_filename(v, *av.video);
        check(n.find('/') == std::string::npos, "no slash in filename");
        check(n.find('"') == std::string::npos, "no quote in filename");
        check(n.find(v.id) != std::string::npos, "filename carries the id");
        VideoInfo blank;
        std::string bn = Downloader::suggest_filename(blank, *av.audio);
        check(!bn.empty(), "blank metadata still yields a filename");
        std::printf("   audio ext: %s   blank: %s\n",
                    Downloader::ext_for(*av.audio).c_str(), bn.c_str());
        check(Downloader::ext_for(*av.audio) == "m4a" ||
              Downloader::ext_for(*av.audio) == "opus", "audio extension sensible");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
