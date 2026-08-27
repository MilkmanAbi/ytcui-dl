/*
 * ytcui-dl — command line front end
 *
 * Three playback modes (audio / video / audio+video), a resolution ladder with
 * graceful switching, parallel downloads, and a diagnostic mode that says
 * exactly which layer is failing when something does not work.
 */

#include "ytfast.h"
#include "yjw.h"
#include "ytdlp_compat.h"

#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

using namespace ytfast;

#ifndef YTFAST_VERSION
#define YTFAST_VERSION "0.4.0"
#endif

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static bool g_quiet = false;

static void info_msg(const char* fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static std::string sv2s(std::string_view v) { return std::string(v); }

// Accepts a bare id, a watch URL, youtu.be, shorts, embed, or a live URL.
static std::string extract_id(const std::string& s) {
    if (s.size() == 11 && s.find('/') == std::string::npos &&
        s.find('.') == std::string::npos) return s;
    static const char* keys[] = {"v=", "/shorts/", "/embed/", "/live/", "youtu.be/"};
    for (const char* k : keys) {
        size_t p = s.find(k);
        if (p == std::string::npos) continue;
        p += std::strlen(k);
        size_t e = s.find_first_of("&?/#", p);
        std::string id = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (id.size() >= 11) return id.substr(0, 11);
    }
    return s;
}

// A format's signed URL only works with the exact User-Agent of the client
// that signed it. VideoInfo::client_ua carries that from whichever client in
// PLAYER_CHAIN actually resolved this video (see yt_innertube.h); the
// fallback here only matters for code paths with no VideoInfo at hand yet.
static const char* ua_for(const VideoInfo& info) {
    return info.client_ua ? info.client_ua : VISIONOS_CLIENT.ua;
}

static bool have_cmd(const char* c) {
    std::string probe = std::string(c) + " --version >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------
static void print_formats(const VideoInfo& info) {
    std::printf("%-6s %-5s %-13s %-22s %9s %10s %5s %s\n",
                "itag", "ext", "resolution", "codec", "bitrate", "size", "ch", "note");
    std::printf("%s\n", std::string(96, '-').c_str());
    for (const auto& f : info.formats) {
        std::string res;
        if (f.has_video) {
            res = std::to_string(f.width) + "x" + std::to_string(f.height);
            if (f.fps > 30) res += "@" + std::to_string(f.fps);
        } else {
            res = "audio only";
        }
        std::string codec = f.has_video ? sv2s(f.video_codec) : sv2s(f.audio_codec);
        std::string note;
        if (f.is_muxed())        note += "muxed ";
        if (f.is_drc)            note += "DRC ";
        if (Selector::is_hdr(f)) note += "HDR ";
        std::printf("%-6d %-5s %-13s %-22s %8lldk %10s %5s %s\n",
                    f.itag, Downloader::ext_for(f).c_str(), res.c_str(), codec.c_str(),
                    (long long)(f.effective_bitrate() / 1000),
                    f.content_length > 0 ? human_bytes(f.content_length).c_str() : "-",
                    f.has_audio ? std::to_string(f.audio_channels).c_str() : "-",
                    note.c_str());
    }
}

static void print_ladder(const VideoInfo& info) {
    std::printf("video\n");
    for (const auto& r : Selector::ladder(info.formats))
        std::printf("  %-10s itag %-5d %-15.*s %-6.*s %10s %7lldk\n",
                    r.label().c_str(), r.itag,
                    (int)r.codec.size(), r.codec.data(),
                    (int)r.container.size(), r.container.data(),
                    r.bytes > 0 ? human_bytes(r.bytes).c_str() : "-",
                    (long long)(r.bitrate / 1000));
    std::printf("audio\n");
    for (auto* a : Selector::audio_ladder(info.formats)) {
        std::string label = std::to_string(a->effective_bitrate() / 1000) + "kbps";
        if (a->audio_channels > 2) label += " " + std::to_string(a->audio_channels) + "ch";
        std::printf("  %-10s itag %-5d %-15.*s %-6.*s %10s\n",
                    label.c_str(), a->itag,
                    (int)a->audio_codec.size(), a->audio_codec.data(),
                    (int)a->container.size(), a->container.data(),
                    a->content_length > 0 ? human_bytes(a->content_length).c_str() : "-");
    }
}

static void emit_format(yjw::Writer& w, const StreamFormat& f) {
    w.obj();
    w.kv("itag", f.itag);
    w.kv("format_id", std::to_string(f.itag));
    w.kv("ext", Downloader::ext_for(f));
    w.kv("url", f.url);
    w.kv("mime_type", f.mime_type);
    w.kv("container", f.container);
    w.kv("width", f.width);
    w.kv("height", f.height);
    w.kv("fps", f.fps);
    w.kv("vcodec", f.video_codec.empty() ? std::string_view("none") : f.video_codec);
    w.kv("acodec", f.audio_codec.empty() ? std::string_view("none") : f.audio_codec);
    w.kv("bitrate", f.bitrate);
    w.kv("average_bitrate", f.average_bitrate);
    w.kv("filesize", f.content_length);
    w.kv("audio_channels", f.audio_channels);
    w.kv("audio_sample_rate", f.audio_sample_rate);
    w.kv("has_video", f.has_video);
    w.kv("has_audio", f.has_audio);
    w.kv("is_drc", f.is_drc);
    w.kv("hdr", Selector::is_hdr(f));
    w.end();
}

static void emit_info(yjw::Writer& w, const VideoInfo& info, const Selection* sel) {
    w.obj();
    w.kv("id", info.id);
    w.kv("title", info.title);
    w.kv("channel", info.channel);
    w.kv("channel_id", info.channel_id);
    w.kv("description", info.description);
    w.kv("duration", info.duration_secs);
    w.kv("duration_string", info.duration_str);
    w.kv("view_count", info.view_count);
    w.kv("is_live", info.is_live);
    w.kv("thumbnail", info.thumbnail_url);
    w.kv("webpage_url", info.url);
    w.kv("sabr_only", info.sabr_only);
    w.kv("extractor", "youtube");

    w.key("formats").arr();
    for (const auto& f : info.formats) emit_format(w, f);
    w.end();

    w.key("ladder").arr();
    for (const auto& r : Selector::ladder(info.formats)) {
        w.obj();
        w.kv("label", r.label());
        w.kv("height", r.height);
        w.kv("fps", r.fps);
        w.kv("itag", r.itag);
        w.kv("hdr", r.hdr);
        w.kv("bitrate", r.bitrate);
        w.kv("filesize", r.bytes);
        w.end();
    }
    w.end();

    if (sel && sel->ok()) {
        w.key("selected").obj();
        w.kv("summary", sel->describe());
        w.kv("mode", mode_name(sel->mode));
        w.kv("muxed", sel->muxed);
        w.kv("total_bitrate", sel->total_bitrate());
        w.kv("total_filesize", sel->total_bytes());
        if (sel->video) { w.key("video"); emit_format(w, *sel->video); }
        if (sel->audio) { w.key("audio"); emit_format(w, *sel->audio); }
        w.end();
    }
    w.end();
}

// ---------------------------------------------------------------------------
// mpv
// ---------------------------------------------------------------------------
static std::vector<std::string> mpv_args(const Selection& sel, Mode mode,
                                         const std::string& title, int volume,
                                         bool force_window, const char* ua) {
    std::vector<std::string> a = {"mpv", "--ytdl=no",
                                  std::string("--user-agent=") + ua};
    if (volume >= 0)    a.push_back("--volume=" + std::to_string(volume));
    if (!title.empty()) a.push_back("--force-media-title=" + title);

    if (mode == Mode::AudioOnly) {
        a.push_back("--no-video");
        a.push_back(sel.audio ? sel.audio->url : sel.video->url);
        return a;
    }
    if (mode == Mode::VideoOnly) {
        a.push_back("--no-audio");
        a.push_back(sel.video->url);
        return a;
    }
    if (force_window) a.push_back("--force-window=yes");
    a.push_back(sel.video->url);
    // Separate adaptive tracks: mpv muxes them locally. Nothing to add when the
    // stream is muxed and already carries its own audio.
    if (sel.audio && !sel.muxed) a.push_back("--audio-file=" + sel.audio->url);
    return a;
}

// Set for the duration of spawn_mpv() so forward_to_mpv() can find the child.
// sig_atomic_t, not a mutex: only ever touched from the main thread and from
// inside a signal handler, so a lock would be both wrong (not async-signal-
// safe) and unnecessary.
static volatile sig_atomic_t g_mpv_pid = 0;

static void forward_to_mpv(int sig) {
    if (g_mpv_pid > 0) kill((pid_t)g_mpv_pid, sig);
}

static int spawn_mpv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { std::fprintf(stderr, "fork failed\n"); return 1; }
    if (pid == 0) { execvp("mpv", argv.data()); _exit(127); }

    // Without this, killing ytcui-dl (Ctrl-C, a signal from a wrapping TUI,
    // etc.) orphans mpv: fork() gives it its own pid, so a signal to us never
    // reaches it on its own, and it plays on indefinitely in the background.
    g_mpv_pid = pid;
    struct sigaction sa{}, old_int{}, old_term{};
    sa.sa_handler = forward_to_mpv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    int st = 0;
    waitpid(pid, &st, 0);

    sigaction(SIGINT,  &old_int,  nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
    g_mpv_pid = 0;

    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        std::fprintf(stderr, "mpv not found in PATH\n");
        return 127;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------
static void draw_progress(const Progress& p, const char* what) {
    if (g_quiet) return;
    if (p.percent() >= 0)
        std::fprintf(stderr, "\r  %-5s [%3d%%] %9s / %-9s %9s/s  ETA %-8s", what,
                     (int)p.percent(), human_bytes(p.downloaded).c_str(),
                     human_bytes(p.total).c_str(),
                     human_bytes((int64_t)p.speed_bps).c_str(),
                     human_duration(p.eta_seconds).c_str());
    else
        std::fprintf(stderr, "\r  %-5s %9s  %9s/s", what,
                     human_bytes(p.downloaded).c_str(),
                     human_bytes((int64_t)p.speed_bps).c_str());
    std::fflush(stderr);
}

static bool download_one(const StreamFormat& f, const std::string& path,
                         const DownloadOptions& base, const char* label) {
    DownloadOptions o = base;
    o.on_progress = [label](const Progress& p) { draw_progress(p, label); return true; };
    info_msg("  %-5s -> %s\n", label, path.c_str());
    auto r = Downloader::fetch(f.url, path, o);
    if (!g_quiet) std::fprintf(stderr, "\n");
    if (!r.ok) {
        std::fprintf(stderr, "  %s failed: %s\n", label,
                     r.error.empty() ? "unknown error" : r.error.c_str());
        if (r.error.find("403") != std::string::npos) {
            if (r.bytes > 0)
                std::fprintf(stderr,
                    "         Got %s before every further request on this stream started\n"
                    "         403ing, including small ones and ones on brand-new connections.\n"
                    "         That pattern -- not a bad signature, not this machine's IP, not\n"
                    "         connection reuse -- points at YouTube requiring a Proof-of-Origin\n"
                    "         token to serve more of this stream. Supply one with --po-token if\n"
                    "         you have a provider for it; see README known constraints.\n",
                    human_bytes(r.bytes).c_str());
            else
                std::fprintf(stderr,
                    "         Rejected from the first byte. Try -4/-6 to force one address\n"
                    "         family (see 'ytcui-dl --diag'), or the stream may need a\n"
                    "         Proof-of-Origin token: --po-token.\n");
        }
        return false;
    }
    info_msg("  %-5s done: %s in %.1fs (%s/s)\n", label, human_bytes(r.bytes).c_str(),
             r.seconds, human_bytes((int64_t)r.speed_bps()).c_str());
    return true;
}

// ---------------------------------------------------------------------------
// diagnostics
//
// Exists because "it doesn't work" is not actionable. Layers are checked in
// order, so the first failure identifies where the problem actually is.
// ---------------------------------------------------------------------------
static int run_diag(const std::string& test_id) {
    int problems = 0;
    auto ok  = [](const char* s, const std::string& d = "") {
        std::printf("  [ ok ] %-24s %s\n", s, d.c_str()); };
    auto bad = [&](const char* s, const std::string& d = "") {
        std::printf("  [FAIL] %-24s %s\n", s, d.c_str()); ++problems; };
    auto warn = [](const char* s, const std::string& d = "") {
        std::printf("  [warn] %-24s %s\n", s, d.c_str()); };

    std::printf("ytcui-dl %s diagnostics\n\nenvironment\n", YTFAST_VERSION);
    ok("openssl", OPENSSL_VERSION_TEXT);
    if (DiskCache::enabled()) ok("cache dir", DiskCache::dir());
    else                      warn("cache dir", "disabled (no HOME/XDG_CACHE_HOME)");
    { const Caps& c = caps();
      if (c.probed) ok("decoders", c.describe());
      else          warn("decoders", "mpv not found — assuming h264/vp9"); }
    if (have_ffmpeg()) ok("ffmpeg", "found — can mux video+audio");
    else               warn("ffmpeg", "missing — downloads stay as separate files");
    if (have_cmd("mpv")) ok("mpv", "found");
    else                 warn("mpv", "missing — --play unavailable");

    std::printf("\nnetwork\n");
    try {
        HttpClient h;
        h.set_timeouts(8000, 15000);
        auto r = h.get("https://www.youtube.com/robots.txt");
        if (r.status == 200) ok("https to youtube.com", human_bytes((int64_t)r.body.size()));
        else                 bad("https to youtube.com", "status " + std::to_string(r.status));
    } catch (const std::exception& e) { bad("https to youtube.com", e.what()); }

    auto& yt = InnertubeClient::get_instance();
    yt.bootstrap_visitor_data();
    const std::string vd = yt.visitor_data();
    if (!vd.empty()) ok("visitorData", std::to_string(vd.size()) + " chars");
    else             bad("visitorData", "empty — player requests will be rejected");

    std::printf("\ninnertube clients (video %s)\n", test_id.c_str());
    bool any_usable = false;
    for (size_t i = 0; i < PLAYER_CHAIN_N; ++i) {
        const ClientDef& c = *PLAYER_CHAIN[i];
        VideoInfo v = yt.probe_client(test_id, c);
        int adaptive = 0, with_url = 0, maxh = 0;
        int64_t maxabr = 0;
        for (const auto& f : v.formats) {
            if (f.is_muxed()) continue;
            ++adaptive;
            if (!f.url.empty()) ++with_url;
            if (f.height > maxh) maxh = f.height;
            if (f.is_audio_only() && f.effective_bitrate() > maxabr)
                maxabr = f.effective_bitrate();
        }
        if (v.sabr_only) {
            bad(c.name, "SABR — formats listed but none carry a URL");
        } else if (v.formats.empty()) {
            bad(c.name, "no formats returned");
        } else {
            char d[160];
            std::snprintf(d, sizeof d, "%d adaptive, max %dp, audio %lldk",
                          adaptive, maxh, (long long)(maxabr / 1000));
            ok(c.name, d);
            if (adaptive > 0 && with_url == adaptive) any_usable = true;
        }
    }
    if (!any_usable) bad("usable client", "none returned fetchable adaptive formats");

    std::printf("\nselection\n");
    auto res = yt.resolve(test_id, Mode::AudioVideo);
    if (res.ok()) {
        ok("audio+video", res.sel.describe());
        ok("quality ladder", std::to_string(res.ladder.size()) + " rungs");
    } else bad("audio+video", "nothing selectable");
    auto ra = yt.resolve(test_id, Mode::AudioOnly);
    ra.ok() ? ok("audio only", ra.sel.describe()) : bad("audio only");
    auto rv = yt.resolve(test_id, Mode::VideoOnly);
    rv.ok() ? ok("video only", rv.sel.describe()) : bad("video only");

    // The decisive check. Everything above can pass while playback still fails,
    // because the CDN binds each signed URL to the IP that asked for it.
    std::printf("\nmedia fetch (the check that matters)\n");
    if (res.ok() && res.sel.audio) {
        try {
            HttpClient h;
            h.set_timeouts(8000, 20000);
            std::vector<std::string> hdr = {std::string("User-Agent: ") + ua_for(res.info),
                                            "Range: bytes=0-65535"};
            size_t got = 0;
            long st = h.download(res.sel.audio->url,
                                 [&](const char*, size_t n) { got += n; return got < 100000; },
                                 {}, 0, hdr);
            if ((st == 200 || st == 206) && got > 1000)
                ok("fetch stream bytes", human_bytes((int64_t)got) + " — playback should work");
            else if (st == 403) {
                bad("fetch stream bytes", "HTTP 403 — CDN rejected the signed URL");
                std::printf(
                    "\n         A 403 here with everything above green means the URL was\n"
                    "         signed for a different IP than the one fetching it. Causes:\n"
                    "         a VPN or proxy that egresses on another address, or a split\n"
                    "         IPv4/IPv6 setup where the two requests take different paths.\n"
                    "         Try disabling the proxy, or forcing one address family.\n");
            } else {
                bad("fetch stream bytes", "status " + std::to_string(st));
            }
        } catch (const std::exception& e) { bad("fetch stream bytes", e.what()); }
    } else {
        warn("fetch stream bytes", "skipped — nothing selectable");
    }

    // A near-start probe alone passes even on a client whose URLs are capped
    // to roughly a minute of the stream -- that cap only bites once you reach
    // it. Probing near the end of the file is what actually distinguishes
    // "this URL streams the whole thing" from "this client needs a PO Token".
    if (res.ok() && res.sel.audio && res.sel.audio->content_length > 2 * 1024 * 1024) {
        try {
            HttpClient h;
            h.set_timeouts(8000, 20000);
            const int64_t off = res.sel.audio->content_length - 65536;
            std::vector<std::string> hdr = {std::string("User-Agent: ") + ua_for(res.info),
                                            "Range: bytes=" + std::to_string(off) + "-"};
            size_t got = 0;
            long st = h.download(res.sel.audio->url,
                                 [&](const char*, size_t n) { got += n; return true; },
                                 {}, 0, hdr);
            if ((st == 200 || st == 206) && got > 1000)
                ok("full-length fetch", human_bytes((int64_t)got) + " from near end of stream");
            else if (st == 403) {
                bad("full-length fetch", "HTTP 403 near the end of the stream");
                std::printf(
                    "\n         The start of the stream fetched fine but this offset didn't --\n"
                    "         that pattern means the resolving client (%s) needs a\n"
                    "         Proof-of-Origin token for full downloads. Pass one with\n"
                    "         --po-token if you have a provider, or wait for the client\n"
                    "         chain in yt_innertube.h to be updated to one that doesn't\n"
                    "         need one yet.\n", res.info.client_name ? res.info.client_name : "?");
            } else {
                bad("full-length fetch", "status " + std::to_string(st));
            }
        } catch (const std::exception& e) { bad("full-length fetch", e.what()); }
    }

    std::printf("\n%s\n", problems ? (std::to_string(problems) + " problem(s) found").c_str()
                                   : "all checks passed");
    yt.shutdown();
    return problems ? 1 : 0;
}

// ---------------------------------------------------------------------------
static void usage() {
    std::printf(
"ytcui-dl %s — minimal YouTube resolver, player front end and downloader\n"
"\n"
"USAGE\n"
"    ytcui-dl [options] <url|video-id>\n"
"    ytcui-dl --search <query>\n"
"\n"
"MODE (default: audio+video)\n"
"    -a, --audio               audio track only\n"
"    -V, --video               video track only, no audio\n"
"    -b, --both                audio+video\n"
"\n"
"ACTION (default: print stream URLs)\n"
"    -g, --get-url             print the resolved stream URL(s)\n"
"    -d, --download            download to a file\n"
"    -p, --play                launch mpv\n"
"        --mpv-args            print the mpv command instead of running it\n"
"    -j, --json                full metadata as JSON\n"
"    -F, --list-formats        every available format\n"
"    -L, --ladder              the quality ladder for this video\n"
"    -s, --search <query>      search (combine with --json)\n"
"        --diag                run diagnostics and exit\n"
"        --caps                show which codecs this machine can decode\n"
"\n"
"QUALITY\n"
"    -q, --quality <n|best|worst>  target height, e.g. 1080\n"
"        --max-height <n>      never exceed this height\n"
"        --fps <n>             prefer at least this frame rate\n"
"        --vcodec <c>          av01 | vp9 | avc1 | h264\n"
"        --acodec <c>          opus | mp4a | aac\n"
"        --container <c>       mp4 | webm\n"
"        --hdr                 prefer HDR when available\n"
"        --max-bitrate <kbps>  skip streams above this bitrate\n"
"        --playable            rank by what this machine can decode\n"
"        --any-codec           disable playability ranking for --play\n"
"        --stereo              never pick a surround track\n"
"        --itag <n>            use exactly this itag\n"
"\n"
"DOWNLOAD\n"
"    -o, --output <path>       output file (default: derived from the title)\n"
"    -c, --connections <n>     parallel connections (default 3, 1 disables)\n"
"        --no-resume           always start from scratch\n"
"        --no-mux              keep video and audio as separate files\n"
"    -x, --extract-audio       skip this project's downloader: pipe straight\n"
"                              into ffmpeg, transcode to .mp3 (implies -a -d)\n"
"        --remux               skip this project's downloader: pipe straight\n"
"                              into ffmpeg instead, remux to .mp4 with no\n"
"                              re-encode unless the source codec isn't\n"
"                              mp4-legal (implies -d; mode still picks what\n"
"                              gets fetched, same as -a/-V/-b)\n"
"\n"
"OTHER\n"
"    -n, --max-results <n>     search result count (default 15)\n"
"        --volume <n>          mpv volume for --play\n"
"        --no-cache            skip the on-disk visitorData cache\n"
"        --cookies <file>      Netscape-format cookies.txt; authenticates as\n"
"                              that logged-in session (age-gated/members-\n"
"                              only/private videos). Not a PO token.\n"
"        --po-token <tok>      Proof-of-Origin token from an external\n"
"                              provider; needed if media fetches 403 after\n"
"                              an initial ~1 minute of a stream (see below)\n"
"        --pot-provider        fetch a PO token automatically from a local\n"
"                              provider at http://127.0.0.1:4416/get_pot\n"
"                              (the bgutil-ytdlp-pot-provider HTTP API --\n"
"                              run that separately; see README)\n"
"        --pot-provider-url <url>  same, at a non-default provider address\n"
"    -4, --ipv4                force IPv4 (fixes 403s on dual-stack networks)\n"
"    -6, --ipv6                force IPv6\n"
"        --quiet               suppress progress and status output\n"
"    -h, --help                this text\n"
"        --version             version\n"
"\n"
"EXAMPLES\n"
"    ytcui-dl -a -g dQw4w9WgXcQ                audio stream URL\n"
"    ytcui-dl -p -q 720 <url>                  play at 720p\n"
"    ytcui-dl -a -p <url>                      play as audio only\n"
"    ytcui-dl -d -q 1080 --vcodec avc1 <url>   download 1080p H.264\n"
"    ytcui-dl -a -d -o track.m4a <url>         download just the audio\n"
"    ytcui-dl -x <url>                         audio, straight to .mp3\n"
"    ytcui-dl -d -q 1080 --remux <url>         video, straight to .mp4\n"
"    ytcui-dl -L <url>                         what qualities exist\n"
"    ytcui-dl --diag                           why is it not working\n",
    YTFAST_VERSION);
}


// ---------------------------------------------------------------------------
// yt-dlp compatible entry point
// ---------------------------------------------------------------------------
static int ytdlp_main(int argc, char** argv) {
    using namespace ytdlp;
    Opts o;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", w); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--yt-dlp")            continue;   // the mode switch itself
        else if (a == "-h" || a == "--help") { ytdlp::usage(); return 0; }
        else if (a == "--version")           { std::printf("%s (ytcui-dl)\n", YTFAST_VERSION); return 0; }
        else if (a == "-q" || a == "--quiet")   o.quiet = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "-s" || a == "--simulate" || a == "--skip-download") o.simulate = true;
        else if (a == "-f" || a == "--format")  o.format = next("--format");
        else if (a == "-o" || a == "--output")  o.output = next("--output");
        else if (a == "-P" || a == "--paths")   o.paths  = next("--paths");
        else if (a == "-F" || a == "--list-formats") o.list_formats = true;
        else if (a == "-j" || a == "--dump-json" || a == "-J" || a == "--dump-single-json")
            o.dump_json = true;
        else if (a == "-g" || a == "--get-url")   o.get_url = true;
        else if (a == "-e" || a == "--get-title") o.get_title = true;
        else if (a == "--get-id")                 o.get_id = true;
        else if (a == "--get-duration")           o.get_duration = true;
        else if (a == "--get-thumbnail")          o.get_thumbnail = true;
        else if (a == "--get-filename")           o.get_filename = true;
        else if (a == "--print")                  o.print_tmpl = next("--print");
        else if (a == "-x" || a == "--extract-audio") o.extract_audio = true;
        else if (a == "--audio-format")   o.audio_format = next("--audio-format");
        else if (a == "--merge-output-format") o.merge_format = next("--merge-output-format");
        else if (a == "-N" || a == "--concurrent-fragments")
            o.concurrent = std::atoi(next("--concurrent-fragments").c_str());
        else if (a == "-r" || a == "--limit-rate") o.limit_rate = parse_rate(next("--limit-rate"));
        else if (a == "-c" || a == "--continue")   o.resume = true;
        else if (a == "--no-continue")             o.resume = false;
        else if (a == "-w" || a == "--no-overwrites") o.no_overwrites = true;
        else if (a == "--user-agent")   o.user_agent = next("--user-agent");
        else if (a == "-4" || a == "--force-ipv4") set_ip_family(IpFamily::V4);
        else if (a == "-6" || a == "--force-ipv6") set_ip_family(IpFamily::V6);
        // Options that take a value and that we accept without acting on, so
        // an existing command line still runs instead of dying on an unknown
        // flag. Consuming the value matters: otherwise it lands in urls.
        else if (a == "-S" || a == "--format-sort" || a == "--audio-quality" ||
                 a == "--cookies" || a == "--cookies-from-browser" ||
                 a == "--download-archive" || a == "--proxy" ||
                 a == "--retries" || a == "--fragment-retries" ||
                 a == "--sub-langs" || a == "--referer" || a == "--playlist-items" ||
                 a == "--ffmpeg-location" || a == "--cache-dir" ||
                 a == "--sleep-requests" || a == "--min-sleep-interval" ||
                 a == "--max-sleep-interval" || a == "--extractor-args" ||
                 a == "--postprocessor-args" || a == "--remux-video" ||
                 a == "--batch-file" || a == "-a") { (void)next(a.c_str()); }
        else if (!a.empty() && a[0] == '-') {
            // Valueless unknown flags are ignored outright.
            if (o.verbose) std::fprintf(stderr, "[ytcui-dl] ignoring %s\n", a.c_str());
        }
        else o.urls.push_back(a);
    }

    if (o.urls.empty()) { ytdlp::usage(); return 2; }
    if (o.quiet) g_quiet = true;

    auto& yt = InnertubeClient::get_instance();
    struct Cleanup { InnertubeClient& c; ~Cleanup() { c.shutdown(); } } cleanup{yt};

    int failures = 0;
    for (const auto& raw : o.urls) {
        const std::string id = extract_id(raw);
        VideoInfo info = yt.get_stream_formats(id);
        if (info.formats.empty()) {
            std::fprintf(stderr, "ERROR: [youtube] %s: no playable formats%s\n",
                         id.c_str(), info.sabr_only ? " (SABR response)" : "");
            ++failures;
            continue;
        }

        // ---- -F ----
        if (o.list_formats) {
            std::printf("[youtube] %s: Downloading webpage\n", id.c_str());
            std::printf("[info] Available formats for %s:\n", id.c_str());
            std::printf("ID  EXT   RESOLUTION FPS │   FILESIZE   TBR PROTO │ VCODEC       ACODEC\n");
            std::printf("─────────────────────────────────────────────────────────────────────────\n");
            for (const auto& f : info.formats) {
                std::string res = f.has_video
                    ? std::to_string(f.width) + "x" + std::to_string(f.height)
                    : std::string("audio only");
                std::printf("%-3d %-5s %-10s %3d │ %10s %5lldk https │ %-12.12s %-12.12s\n",
                            f.itag, Downloader::ext_for(f).c_str(), res.c_str(), f.fps,
                            f.content_length > 0 ? human_bytes(f.content_length).c_str() : "~",
                            (long long)(f.effective_bitrate() / 1000),
                            f.video_codec.empty() ? "audio only" : std::string(f.video_codec).c_str(),
                            f.audio_codec.empty() ? "video only" : std::string(f.audio_codec).c_str());
            }
            continue;
        }

        // ---- format selection ----
        std::string fmt = o.format;
        if (o.extract_audio && fmt == "bestvideo*+bestaudio/best") fmt = "bestaudio/best";
        Picked pick = pick_format(info.formats, fmt);
        if (!pick.ok()) {
            std::fprintf(stderr, "ERROR: [youtube] %s: requested format is not available\n",
                         id.c_str());
            ++failures;
            continue;
        }
        if (o.extract_audio && pick.audio) pick.video = nullptr;

        const StreamFormat* primary = pick.video ? pick.video : pick.audio;
        std::string ext = o.extract_audio && pick.audio
                        ? (o.audio_format == "best" ? Downloader::ext_for(*pick.audio)
                                                    : o.audio_format)
                        : (pick.video && pick.audio && !pick.muxed && !o.merge_format.empty()
                           ? o.merge_format
                           : Downloader::ext_for(*primary));

        std::string out_path = expand_template(o.output, info, primary, ext);
        if (!o.paths.empty()) out_path = o.paths + "/" + out_path;

        // ---- simulation-style outputs ----
        bool printed = false;
        if (!o.print_tmpl.empty()) {
            std::printf("%s\n", expand_template(o.print_tmpl, info, primary, ext).c_str());
            printed = true;
        }
        if (o.get_title)     { std::printf("%s\n", info.title.c_str()); printed = true; }
        if (o.get_id)        { std::printf("%s\n", info.id.c_str()); printed = true; }
        if (o.get_duration)  { std::printf("%s\n", info.duration_str.c_str()); printed = true; }
        if (o.get_thumbnail) { std::printf("%s\n", info.thumbnail_url.c_str()); printed = true; }
        if (o.get_filename)  { std::printf("%s\n", out_path.c_str()); printed = true; }
        if (o.get_url) {
            if (pick.video) std::printf("%s\n", pick.video->url.c_str());
            if (pick.audio && !pick.muxed) std::printf("%s\n", pick.audio->url.c_str());
            printed = true;
        }
        if (o.dump_json) {
            yjw::Writer w;
            Selection sel;
            sel.video = pick.video;
            sel.audio = pick.audio;
            sel.muxed = pick.muxed;
            sel.mode  = o.extract_audio ? Mode::AudioOnly : Mode::AudioVideo;
            emit_info(w, info, &sel);
            std::printf("%s\n", w.str().c_str());
            printed = true;
        }
        if (printed || o.simulate) continue;

        // ---- download ----
        if (o.no_overwrites) {
            struct stat st{};
            if (::stat(out_path.c_str(), &st) == 0) {
                if (!o.quiet)
                    std::printf("[download] %s has already been downloaded\n", out_path.c_str());
                continue;
            }
        }

        DownloadOptions d;
        d.connections = o.concurrent < 1 ? 1 : o.concurrent;
        d.resume = o.resume;
        d.user_agent = o.user_agent.empty() ? ua_for(info) : o.user_agent;

        if (!o.quiet) std::printf("[youtube] %s: %s\n", id.c_str(), info.title.c_str());

        const bool pair = pick.video && pick.audio && !pick.muxed;
        if (!pair) {
            if (!download_one(*primary, out_path, d, pick.video ? "video" : "audio")) {
                ++failures;
                continue;
            }
        } else {
            const size_t dot = out_path.rfind('.');
            const std::string stem = dot == std::string::npos ? out_path : out_path.substr(0, dot);
            const std::string vp = stem + ".f" + std::to_string(pick.video->itag) + "." +
                                   Downloader::ext_for(*pick.video);
            const std::string ap = stem + ".f" + std::to_string(pick.audio->itag) + "." +
                                   Downloader::ext_for(*pick.audio);
            if (!download_one(*pick.video, vp, d, "video")) { ++failures; continue; }
            if (!download_one(*pick.audio, ap, d, "audio")) { ++failures; continue; }
            if (!have_ffmpeg()) {
                std::fprintf(stderr,
                    "WARNING: ffmpeg not found, keeping separate files:\n  %s\n  %s\n",
                    vp.c_str(), ap.c_str());
                continue;
            }
            if (!o.quiet) std::printf("[Merger] Merging formats into \"%s\"\n", out_path.c_str());
            std::string err;
            if (!mux_av(vp, ap, out_path, &err)) {
                std::fprintf(stderr, "ERROR: merge failed (%s)\n", err.c_str());
                ++failures;
                continue;
            }
            ::remove(vp.c_str());
            ::remove(ap.c_str());
        }

        // -x with an explicit target container needs a transcode.
        if (o.extract_audio && o.audio_format != "best" && have_ffmpeg()) {
            const std::string src = out_path;
            const size_t dot = out_path.rfind('.');
            const std::string dst = (dot == std::string::npos ? out_path : out_path.substr(0, dot))
                                  + "." + o.audio_format;
            if (src != dst) {
                std::string cmd = "ffmpeg -y -loglevel error -i '" + src + "' '" + dst +
                                  "' >/dev/null 2>&1";
                if (std::system(cmd.c_str()) == 0) ::remove(src.c_str());
                else std::fprintf(stderr, "WARNING: could not convert to %s\n",
                                  o.audio_format.c_str());
            }
        }
    }
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    // Multi-call: behave like yt-dlp when invoked under that name (a symlink
    // is enough) or when asked explicitly. The two flag sets genuinely
    // conflict -- -q, -a, -c and -s all mean different things -- so they stay
    // separate rather than being merged into something ambiguous.
    {
        const char* slash = std::strrchr(argv[0], '/');
        const std::string self = slash ? slash + 1 : argv[0];
        const bool named = self.find("yt-dlp") != std::string::npos ||
                           self.find("youtube-dl") != std::string::npos;
        bool asked = false;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--yt-dlp") { asked = true; break; }
        if (named || asked) {
            CurlGlobalInit init;
            return ytdlp_main(argc, argv);
        }
    }

    if (argc < 2) { usage(); return 1; }

    Mode mode = Mode::AudioVideo;
    Quality q;
    std::string action, target, search_query, out_path;
    static std::string vcodec_buf, acodec_buf, container_buf;
    int  max_results = 15, connections = 3, volume = -1, want_itag = 0;
    bool json = false, no_resume = false, no_mux = false, no_cache = false;
    bool print_mpv = false, force_any_codec = false, via_ffmpeg = false;
    std::string po_token, cookies_file, pot_provider_url;
    bool use_pot_provider = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "-h" || a == "--help")     { usage(); return 0; }
        else if (a == "--version")               { std::printf("%s\n", YTFAST_VERSION); return 0; }
        else if (a == "-a" || a == "--audio")    mode = Mode::AudioOnly;
        else if (a == "-V" || a == "--video")    mode = Mode::VideoOnly;
        else if (a == "-b" || a == "--both")     mode = Mode::AudioVideo;
        else if (a == "-g" || a == "--get-url")  action = "url";
        else if (a == "-d" || a == "--download") action = "download";
        else if (a == "-x" || a == "--extract-audio") {
            mode = Mode::AudioOnly; via_ffmpeg = true;
            if (action.empty()) action = "download";
        }
        else if (a == "--remux") {
            via_ffmpeg = true;
            if (action.empty()) action = "download";
        }
        else if (a == "-p" || a == "--play")     action = "play";
        else if (a == "--mpv-args")            { action = "play"; print_mpv = true; }
        else if (a == "-j" || a == "--json")     json = true;
        else if (a == "-F" || a == "--list-formats") action = "formats";
        else if (a == "-L" || a == "--ladder")   action = "ladder";
        else if (a == "--diag")                  action = "diag";
        else if (a == "-s" || a == "--search")  { action = "search"; search_query = next("--search"); }
        else if (a == "-q" || a == "--quality") {
            const std::string v = next("--quality");
            if      (v == "best")  q.height = 0;
            else if (v == "worst" || v == "lowest") q = Quality::lowest();
            else                   q.height = std::atoi(v.c_str());
        }
        else if (a == "--max-height") q.max_height = std::atoi(next("--max-height").c_str());
        else if (a == "--fps")        q.fps = std::atoi(next("--fps").c_str());
        else if (a == "--vcodec")    { vcodec_buf    = next("--vcodec");    q.vcodec = vcodec_buf; }
        else if (a == "--acodec")    { acodec_buf    = next("--acodec");    q.acodec = acodec_buf; }
        else if (a == "--container") { container_buf = next("--container"); q.container = container_buf; }
        else if (a == "--hdr")        q.prefer_hdr = true;
        else if (a == "--max-bitrate") q.max_bitrate = (int64_t)std::atoi(next("--max-bitrate").c_str()) * 1000;
        else if (a == "--playable")   q.for_playback = true;
        else if (a == "--any-codec")  q.for_playback = false, force_any_codec = true;
        else if (a == "--caps")       action = "caps";
        else if (a == "--stereo")     q.allow_surround = false;
        else if (a == "--itag")       want_itag = std::atoi(next("--itag").c_str());
        else if (a == "-o" || a == "--output")      out_path = next("--output");
        else if (a == "-c" || a == "--connections") connections = std::atoi(next("--connections").c_str());
        else if (a == "--no-resume")  no_resume = true;
        else if (a == "--no-mux")     no_mux = true;
        else if (a == "--no-cache")   no_cache = true;
        else if (a == "--po-token")   po_token = next("--po-token");
        else if (a == "--cookies")    cookies_file = next("--cookies");
        else if (a == "--pot-provider") use_pot_provider = true;
        else if (a == "--pot-provider-url") {
            use_pot_provider = true;
            pot_provider_url = next("--pot-provider-url");
        }
        else if (a == "-4" || a == "--ipv4") set_ip_family(IpFamily::V4);
        else if (a == "-6" || a == "--ipv6") set_ip_family(IpFamily::V6);
        else if (a == "-n" || a == "--max-results") max_results = std::atoi(next("--max-results").c_str());
        else if (a == "--volume")     volume = std::atoi(next("--volume").c_str());
        else if (a == "--quiet")      g_quiet = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
        else target = a;
    }

    CurlGlobalInit init;
    auto& yt = InnertubeClient::get_instance();
    if (no_cache) yt.set_disk_cache(false);
    if (!cookies_file.empty() && !yt.set_cookies_file(cookies_file))
        std::fprintf(stderr,
            "warning: --cookies %s: couldn't read it, or no youtube/google "
            "cookies in it\n", cookies_file.c_str());
    if (!po_token.empty()) {
        yt.set_po_token(po_token);
    } else if (use_pot_provider) {
        const std::string url = pot_provider_url.empty()
            ? "http://127.0.0.1:4416" : pot_provider_url;
        if (!yt.fetch_po_token_from_provider(url))
            std::fprintf(stderr,
                "warning: --pot-provider: couldn't reach %s -- is a PO-Token "
                "provider (e.g. bgutil-ytdlp-pot-provider) running there? "
                "continuing without one.\n", url.c_str());
    }

    // The prefetch worker must be joined before static destructors run.
    struct Cleanup { InnertubeClient& c; ~Cleanup() { c.shutdown(); } } cleanup{yt};

    if (action == "caps") {
        const Caps& c = refresh_caps();
        std::printf("decoders: %s\n\n", c.describe().c_str());
        std::printf("  %-6s %-10s %s\n", "codec", "decoder", "hardware");
        struct { const char* n; bool d, hw; } rows[] = {
            {"h264", c.dec_h264, c.hw_h264}, {"hevc", c.dec_hevc, c.hw_hevc},
            {"vp9",  c.dec_vp9,  c.hw_vp9},  {"av1",  c.dec_av1,  c.hw_av1},
        };
        for (auto& r : rows)
            std::printf("  %-6s %-10s %s\n", r.n, r.d ? "yes" : "NO",
                        r.hw ? "yes" : "no");
        if (!c.probed)
            std::printf("\nmpv not found — these are assumed defaults.\n");
        std::printf("\nAbove 1080p a codec without hardware decode is ranked down for\n"
                    "playback; a codec with no decoder at all is never selected.\n");
        return 0;
    }

    if (action == "diag")
        return run_diag(target.empty() ? "aqz-KE-bpKQ" : extract_id(target));

    // ------------------------------------------------------------- search
    if (action == "search") {
        auto results = yt.search(search_query, max_results);
        if (json) {
            yjw::Writer w;
            w.arr();
            for (const auto& r : results) {
                w.obj();
                w.kv("id", r.id);
                w.kv("title", r.title);
                w.kv("channel", r.channel);
                w.kv("duration", r.duration_str);
                w.kv("duration_secs", r.duration_secs);
                w.kv("view_count", r.view_count);
                w.kv("upload_date", r.upload_date);
                w.kv("is_live", r.is_live);
                w.kv("url", r.url);
                w.kv("thumbnail", r.thumbnail_url);
                w.end();
            }
            w.end();
            std::printf("%s\n", w.str().c_str());
        } else {
            for (const auto& r : results) {
                // printf's %.22s precision cuts at a byte count, which can
                // land inside a multi-byte UTF-8 character (common: channel
                // names in CJK/Cyrillic/etc run 2-3 bytes per glyph) and
                // leave a dangling, invalid sequence right before the next
                // column. Truncate on a codepoint boundary first instead.
                std::string channel = r.channel;
                Downloader::utf8_safe_truncate(channel, 22);
                std::printf("%-11s  %-9s  %-22s  %s\n", r.id.c_str(),
                            r.is_live ? "LIVE" : r.duration_str.c_str(),
                            channel.c_str(), r.title.c_str());
            }
        }
        if (results.empty()) { std::fprintf(stderr, "no results\n"); return 1; }
        return 0;
    }

    if (target.empty()) { std::fprintf(stderr, "no video given\n"); return 2; }
    const std::string id = extract_id(target);

    VideoInfo info = yt.get_stream_formats(id);
    if (info.formats.empty()) {
        if (info.sabr_only)
            std::fprintf(stderr,
                "error: every client returned SABR (formats listed, no URLs).\n"
                "       Run 'ytcui-dl --diag' for details.\n");
        else
            std::fprintf(stderr, "error: no playable formats for %s\n", id.c_str());
        return 1;
    }

    if (action == "formats") { print_formats(info); return 0; }
    if (action == "ladder")  { print_ladder(info);  return 0; }

    // ---------------------------------------------------------- selection
    Selection sel;
    if (want_itag) {
        sel.mode = mode;
        for (const auto& f : info.formats) {
            if (f.itag != want_itag) continue;
            if (f.is_audio_only()) sel.audio = &f;
            else { sel.video = &f; sel.muxed = f.is_muxed(); }
            break;
        }
        if (!sel.video && !sel.audio) {
            std::fprintf(stderr, "itag %d not available (try -F)\n", want_itag);
            return 1;
        }
        // In audio+video mode an explicit itag only pins one half of the pair;
        // the other half still has to be chosen, in whichever direction is
        // missing. Without this, --itag <an audio itag> left the video slot
        // empty and the whole selection was reported unusable.
        if (mode == Mode::AudioVideo && !sel.muxed) {
            if (sel.video && !sel.audio)
                sel.audio = Selector::select(info.formats, Mode::AudioOnly, q).audio;
            else if (sel.audio && !sel.video)
                sel.video = Selector::select(info.formats, Mode::VideoOnly, q).video;
        }
        // An audio itag asked for in video-only mode (or the reverse) is a
        // contradiction; say so rather than silently substituting something.
        if (mode == Mode::VideoOnly && !sel.video) {
            std::fprintf(stderr, "itag %d is an audio track, but --video was requested\n", want_itag);
            return 1;
        }
        if (mode == Mode::AudioOnly && !sel.audio && !sel.muxed) {
            std::fprintf(stderr, "itag %d is a video track, but --audio was requested\n", want_itag);
            return 1;
        }
    } else {
        // Playing is the case where a stream this machine cannot decode is a
        // hard failure rather than a slow download, so rank by playability
        // unless the user explicitly asked not to.
        if (action == "play" && !force_any_codec) q.for_playback = true;
        sel = Selector::select(info.formats, mode, q);
    }

    if (!sel.ok()) {
        std::fprintf(stderr, "error: nothing matches that request (try -L)\n");
        return 1;
    }

    if (json) {
        yjw::Writer w;
        emit_info(w, info, &sel);
        std::printf("%s\n", w.str().c_str());
        return 0;
    }

    // ---------------------------------------------------------------- play
    if (action == "play") {
        auto args = mpv_args(sel, mode, info.title, volume, mode != Mode::AudioOnly, ua_for(info));
        if (print_mpv) {
            for (size_t i = 0; i < args.size(); ++i)
                std::printf("%s%s", args[i].c_str(), i + 1 < args.size() ? " " : "\n");
            return 0;
        }
        info_msg("%s\n%s — %s\n", info.title.c_str(), mode_name(mode), sel.describe().c_str());
        return spawn_mpv(args);
    }

    // ------------------------------------------------------------ download
    if (action == "download") {
        // -x / --remux: skip this project's own chunked engine and hand the
        // URL(s) straight to ffmpeg, which fetches and remuxes/transcodes in
        // one pass. See yt_stream_dl.h for why this is a separate path from
        // Downloader::fetch rather than a replacement for it.
        if (via_ffmpeg) {
            const bool audio_only = mode == Mode::AudioOnly;
            const char* ext = audio_only ? "mp3" : "mp4";
            std::string path = out_path;
            if (path.empty()) {
                const StreamFormat* f = (audio_only && sel.audio) ? sel.audio : sel.video;
                path = Downloader::suggest_filename(info, *f);
                const size_t dot = path.rfind('.');
                path = (dot == std::string::npos ? path : path.substr(0, dot)) + "." + ext;
            }
            const std::string video_url = audio_only ? "" : (sel.video ? sel.video->url : "");
            // A muxed fallback (no separate audio track) carries the audio
            // on the video URL too -- ffmpeg's -vn/-map picks it out either way.
            const std::string audio_url = sel.audio ? sel.audio->url
                                        : (sel.muxed && sel.video ? sel.video->url : "");

            info_msg("%s\n%s — %s (via ffmpeg -> %s)\n", info.title.c_str(),
                     mode_name(mode), sel.describe().c_str(), ext);
            auto r = StreamDownloader::fetch(video_url, audio_url, path, ua_for(info),
                                             audio_only, g_quiet);
            if (!r.ok) {
                std::fprintf(stderr, "ffmpeg failed: %s\n", r.error.c_str());
                return 1;
            }
            info_msg("done: %s%s\n", path.c_str(),
                     r.transcoded ? " (transcoded)" : " (remuxed, no re-encode)");
            return 0;
        }

        DownloadOptions o;
        o.connections = connections < 1 ? 1 : connections;
        o.resume = !no_resume;
        o.user_agent = ua_for(info);

        info_msg("%s\n%s — %s", info.title.c_str(), mode_name(mode), sel.describe().c_str());
        if (sel.total_bytes() > 0) info_msg("  (%s)", human_bytes(sel.total_bytes()).c_str());
        info_msg("\n");

        const bool need_both = mode == Mode::AudioVideo && sel.video && sel.audio && !sel.muxed;

        if (!need_both) {
            const StreamFormat* f = (mode == Mode::AudioOnly && sel.audio) ? sel.audio : sel.video;
            const std::string path = out_path.empty()
                                   ? Downloader::suggest_filename(info, *f) : out_path;
            return download_one(*f, path, o, mode == Mode::AudioOnly ? "audio" : "video") ? 0 : 1;
        }

        // Two streams: fetch both, then mux if ffmpeg is available.
        const std::string base = out_path.empty()
                               ? Downloader::suggest_filename(info, *sel.video) : out_path;
        const size_t dot = base.rfind('.');
        const std::string stem  = dot == std::string::npos ? base : base.substr(0, dot);
        const std::string vpath = stem + ".video." + Downloader::ext_for(*sel.video);
        const std::string apath = stem + ".audio." + Downloader::ext_for(*sel.audio);

        if (!download_one(*sel.video, vpath, o, "video")) return 1;
        if (!download_one(*sel.audio, apath, o, "audio")) return 1;

        if (no_mux) {
            info_msg("kept separate:\n  %s\n  %s\n", vpath.c_str(), apath.c_str());
            return 0;
        }
        if (!have_ffmpeg()) {
            std::fprintf(stderr, "ffmpeg not found — leaving separate files:\n  %s\n  %s\n",
                         vpath.c_str(), apath.c_str());
            return 0;
        }
        info_msg("muxing...\n");
        std::string err;
        if (!mux_av(vpath, apath, base, &err)) {
            std::fprintf(stderr, "mux failed (%s) — separate files kept:\n  %s\n  %s\n",
                         err.c_str(), vpath.c_str(), apath.c_str());
            return 1;
        }
        ::remove(vpath.c_str());
        ::remove(apath.c_str());
        info_msg("done: %s\n", base.c_str());
        return 0;
    }

    // ----------------------------------------------------------- print URLs
    if (mode == Mode::AudioOnly)
        std::printf("%s\n", (sel.audio ? sel.audio : sel.video)->url.c_str());
    else if (mode == Mode::VideoOnly)
        std::printf("%s\n", sel.video->url.c_str());
    else {
        std::printf("%s\n", sel.video->url.c_str());
        if (sel.audio && !sel.muxed) std::printf("%s\n", sel.audio->url.c_str());
    }
    return 0;
}
