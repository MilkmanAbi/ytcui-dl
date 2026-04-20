/*
 * ytcui-dl v0.2.0 — command-line tool
 *
 * Fast YouTube stream resolver and downloader.
 * Drop-in replacement for yt-dlp in YouTube TUI applications.
 *
 * Usage:
 *   ytcui-dl -g [-f FORMAT] URL           print stream URL(s)
 *   ytcui-dl -x URL                       download best audio (m4a/webm)
 *   ytcui-dl -x -f bestaudio[ext=m4a] URL download best AAC audio
 *   ytcui-dl --download [-f FORMAT] URL   download video (default: best ≤1080p)
 *   ytcui-dl -o filename.mp4 URL          set output filename
 *   ytcui-dl --search QUERY [N=10]        search, JSON output
 *   ytcui-dl -j URL  /  --dump-json URL   video metadata as JSON (yt-dlp compat)
 *   ytcui-dl --info URL                   video metadata as JSON (alias)
 *   ytcui-dl -F URL  /  --list-formats    human-readable format table
 *   ytcui-dl --formats URL                all formats as JSON
 *   ytcui-dl --get-url [-f FMT] URL       print resolved URL (yt-dlp compat)
 *   ytcui-dl --get-thumbnail URL          print best thumbnail URL
 *   ytcui-dl --print FIELD URL            print a specific field
 *   ytcui-dl --bootstrap                  warm visitor_data cache and exit
 *
 * Format strings (yt-dlp compatible):
 *   bestaudio / ba                  best audio-only stream (opus/webm)
 *   bestaudio[ext=m4a]              best AAC audio in mp4/m4a container
 *   bestaudio[ext=webm]             best opus audio in webm container
 *   bestaudio[abr>=128]             best audio ≥128kbps
 *   worstaudio / wa                 worst audio (smallest file)
 *   bestvideo / bv                  best video-only adaptive stream
 *   bestvideo[height<=720]          best video up to 720p
 *   best / b                        best muxed video+audio
 *   best[height<=720]               best muxed up to 720p
 *   worst / w                       worst quality muxed
 *   <itag>                          specific format by itag number
 */

#include "ytfast.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#else
  #include <unistd.h>
  #include <sys/stat.h>
#endif

using json = nlohmann::json;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

static std::string extract_id(const std::string& url) {
    if (url.size() == 11 && url.find('/') == std::string::npos) return url;
    auto vp = url.find("v=");
    if (vp != std::string::npos) { auto id=url.substr(vp+2,11); if(id.size()==11) return id; }
    auto bp = url.find("youtu.be/");
    if (bp != std::string::npos) return url.substr(bp+9, 11);
    // Handle /shorts/ID and /embed/ID
    for (const char* prefix : {"/shorts/", "/embed/", "/v/"}) {
        auto sp = url.find(prefix);
        if (sp != std::string::npos) {
            auto seg = url.substr(sp + strlen(prefix), 11);
            if (seg.size() == 11) return seg;
        }
    }
    auto sl = url.rfind('/');
    if (sl != std::string::npos) { auto seg=url.substr(sl+1,11); if(seg.size()==11) return seg; }
    return url;
}

// Sanitise a title string into a safe filename
static std::string safe_filename(const std::string& title, const std::string& ext) {
    std::string out;
    for (unsigned char c : title) {
        if (c >= 32 && c < 127 &&
            std::string("<>:\"/\\|?*").find(c) == std::string::npos)
            out += (char)c;
        else if (c == ' ') out += '_';
    }
    if (out.size() > 200) out.resize(200);
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    if (out.empty()) out = "download";
    return out + "." + ext;
}

// Guess extension from StreamFormat
static std::string fmt_to_ext(const ytfast::StreamFormat& fmt) {
    if (fmt.is_audio_only()) {
        if (fmt.container == "webm") return "webm";
        if (fmt.container == "mp4")  return "m4a";
        return "m4a";
    }
    if (fmt.container == "webm") return "webm";
    return "mp4";
}

// -------------------------------------------------------------------------
// -F: Human-readable format table (like yt-dlp)
// -------------------------------------------------------------------------

static void print_format_table(const ytfast::VideoInfo& info) {
    fprintf(stderr, "[%s] %s\n", info.id.c_str(), info.title.c_str());
    fprintf(stderr, "%-6s %-4s %-12s %-4s  %-10s %-10s  %-6s  %s\n",
            "ID", "EXT", "RESOLUTION", "FPS", "FILESIZE", "BITRATE", "TYPE", "CODEC");
    fprintf(stderr, "-----------------------------------------------------------------------\n");

    for (auto& f : info.formats) {
        std::string ext = fmt_to_ext(f);

        char resolution[32];
        if (f.height > 0)
            snprintf(resolution, sizeof(resolution), "%dx%d", f.width, f.height);
        else
            snprintf(resolution, sizeof(resolution), "audio");

        char fps_str[16];
        if (f.fps > 0) snprintf(fps_str, sizeof(fps_str), "%d", f.fps);
        else           snprintf(fps_str, sizeof(fps_str), "-");

        char size_str[32];
        if (f.content_length > 0)
            snprintf(size_str, sizeof(size_str), "%.1fMB", f.content_length / 1048576.0);
        else
            snprintf(size_str, sizeof(size_str), "~");

        char bitrate_str[32];
        int64_t br = f.effective_bitrate();
        if (br > 0)
            snprintf(bitrate_str, sizeof(bitrate_str), "%lldkbps", (long long)(br / 1000));
        else
            snprintf(bitrate_str, sizeof(bitrate_str), "~");

        const char* type = f.is_muxed() ? "muxed" : f.is_audio_only() ? "audio" : "video";

        std::string codec;
        if (!f.video_codec.empty()) codec += f.video_codec;
        if (!f.video_codec.empty() && !f.audio_codec.empty()) codec += ", ";
        if (!f.audio_codec.empty()) codec += f.audio_codec;

        fprintf(stderr, "%-6d %-4s %-12s %-4s  %-10s %-10s  %-6s  %s\n",
                f.itag, ext.c_str(), resolution, fps_str,
                size_str, bitrate_str, type, codec.c_str());
    }
}

// -------------------------------------------------------------------------
// -j / --dump-json: yt-dlp compatible JSON output
// -------------------------------------------------------------------------

static json format_to_json(const ytfast::StreamFormat& f) {
    json j = {
        {"format_id",        std::to_string(f.itag)},
        {"itag",             f.itag},
        {"ext",              fmt_to_ext(f)},
        {"url",              f.url},
        {"stream_url",       f.stream_url},
        {"mime_type",        f.mime_type},
        {"quality",          f.quality},
        {"quality_label",    f.quality_label},
        {"container",        f.container},
        {"width",            f.width},
        {"height",           f.height},
        {"fps",              f.fps},
        {"tbr",              f.bitrate / 1000},       // total bitrate in kbps
        {"abr",              f.is_audio_only() ? f.effective_bitrate() / 1000 : 0},
        {"vbr",              f.is_video_only() ? f.effective_bitrate() / 1000 : 0},
        {"bitrate",          f.bitrate},
        {"average_bitrate",  f.average_bitrate},
        {"content_length",   f.content_length},
        {"filesize",         f.content_length},
        {"has_video",        f.has_video},
        {"has_audio",        f.has_audio},
        {"audio_codec",      f.audio_codec},
        {"video_codec",      f.video_codec},
        {"acodec",           f.audio_codec.empty() ? "none" : f.audio_codec},
        {"vcodec",           f.video_codec.empty() ? "none" : f.video_codec},
        {"audio_sample_rate",f.audio_sample_rate},
        {"audio_channels",   f.audio_channels},
    };
    return j;
}

static json info_to_json(const ytfast::VideoInfo& info) {
    json j = {
        {"id",               info.id},
        {"title",            info.title},
        {"fulltitle",        info.title},
        {"channel",          info.channel},
        {"uploader",         info.channel},
        {"channel_id",       info.channel_id},
        {"uploader_id",      info.channel_id},
        {"description",      info.description},
        {"thumbnail",        info.thumbnail_url},
        {"upload_date",      info.upload_date},
        {"duration",         info.duration_secs},
        {"duration_string",  info.duration_str},
        {"view_count",       info.view_count},
        {"is_live",          info.is_live},
        {"webpage_url",      info.url},
        {"original_url",     info.url},
        {"category",         {info.category}},
        {"format_count",     (int)info.formats.size()},
        {"extractor",        "youtube"},
        {"extractor_key",    "Youtube"},
    };

    // Thumbnails array
    json thumbs = json::array();
    for (auto& t : info.thumbnails)
        thumbs.push_back({{"url", t.url}, {"width", t.width}, {"height", t.height}});
    j["thumbnails"] = thumbs;

    // Formats array
    json fmts = json::array();
    for (auto& f : info.formats)
        fmts.push_back(format_to_json(f));
    j["formats"] = fmts;

    // Requested format info (best muxed)
    std::string best_url = ytfast::InnertubeClient::select_best_video(info.formats, 1080);
    for (auto& f : info.formats) {
        if (f.url == best_url) {
            j["format"] = std::to_string(f.itag) + " - " +
                          std::to_string(f.height) + "p";
            j["format_id"] = std::to_string(f.itag);
            j["ext"] = fmt_to_ext(f);
            j["width"] = f.width;
            j["height"] = f.height;
            j["fps"] = f.fps;
            j["vcodec"] = f.video_codec.empty() ? "none" : f.video_codec;
            j["acodec"] = f.audio_codec.empty() ? "none" : f.audio_codec;
            j["url"] = f.url;
            break;
        }
    }

    return j;
}

// -------------------------------------------------------------------------
// Downloader — streams CDN URL straight to disk via libcurl
// -------------------------------------------------------------------------

struct DlCtx {
    FILE*    fp       = nullptr;
    int64_t  received = 0;
    int64_t  total    = 0;
    bool     tty      = false;
};

static size_t dl_write(char* ptr, size_t sz, size_t nmemb, void* ud) {
    auto* ctx = static_cast<DlCtx*>(ud);
    size_t n = sz * nmemb;
    if (fwrite(ptr, 1, n, ctx->fp) != n) return 0;
    ctx->received += (int64_t)n;
    return n;
}

static int dl_progress(void* ud, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DlCtx*>(ud);
    if (!ctx->tty) return 0;
    if (dltotal <= 0) {
        fprintf(stderr, "\r  %.1f MB downloaded...", dlnow / 1048576.0);
    } else {
        int pct = (int)(100.0 * dlnow / dltotal);
        double mb_done = dlnow  / 1048576.0;
        double mb_tot  = dltotal / 1048576.0;
        fprintf(stderr, "\r  [%3d%%] %.1f / %.1f MB", pct, mb_done, mb_tot);
    }
    fflush(stderr);
    return 0;
}

static bool download_url(const std::string& url,
                          const std::string& out_path,
                          const std::string& ua,
                          int64_t content_length) {
    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s' for writing: %s\n",
                out_path.c_str(), strerror(errno));
        return false;
    }

    DlCtx ctx;
    ctx.fp    = fp;
    ctx.total = content_length;
    ctx.tty   = isatty(fileno(stderr));

    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   dl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION,dl_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA,    &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,      0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,       ua.c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  10L);
    // Resume support
    if (content_length > 0) {
        long existing = 0;
        struct stat st;
        if (stat(out_path.c_str(), &st) == 0) existing = st.st_size;
        if (existing > 0 && existing < content_length) {
            curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)existing);
            ctx.received = existing;
            fseek(fp, existing, SEEK_SET);
        }
    }

    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    fclose(fp);

    if (ctx.tty) fprintf(stderr, "\n");

    if (rc != CURLE_OK) {
        fprintf(stderr, "Download error: %s\n", curl_easy_strerror(rc));
        return false;
    }
    if (http_code >= 400) {
        fprintf(stderr, "Download error: HTTP %ld\n", http_code);
        remove(out_path.c_str());
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// Usage
// -------------------------------------------------------------------------

static void print_usage(const char* prog) {
    fprintf(stderr,
"ytcui-dl v0.2.0 — fast YouTube resolver & downloader\n\n"
"Stream URL resolution:\n"
"  %s -g [-f FORMAT] URL         print resolved stream URL\n"
"  %s --get-url [-f FMT] URL     (yt-dlp compat alias)\n\n"
"Download:\n"
"  %s -x URL                     download best audio (webm/m4a)\n"
"  %s --download URL             download best video+audio ≤1080p\n"
"  %s --download -f FORMAT URL   download with specific format\n"
"  %s -o OUTPUT.mp4 ...          set output path\n\n"
"Metadata:\n"
"  %s --search QUERY [N=10]      search YouTube (JSON output)\n"
"  %s -j URL / --dump-json URL   full metadata + formats (yt-dlp compat JSON)\n"
"  %s --info URL                 video metadata (JSON)\n"
"  %s -F URL / --list-formats    human-readable format table\n"
"  %s --formats URL              all formats as JSON\n"
"  %s --get-thumbnail URL        print best thumbnail URL\n"
"  %s --print FIELD URL          print a field (id,title,url,thumbnail,...)\n"
"  %s --bootstrap                fetch visitor_data and exit\n\n"
"Format strings (yt-dlp compatible):\n"
"  bestaudio / ba                best audio-only (opus preferred)\n"
"  bestaudio[ext=m4a]            best AAC audio\n"
"  bestaudio[ext=webm]           best opus audio\n"
"  bestaudio[abr>=128]           best audio ≥128kbps\n"
"  worstaudio / wa               worst audio (smallest file)\n"
"  bestvideo / bv                best video-only adaptive\n"
"  bestvideo[height<=720]        best video up to 720p\n"
"  best / b                      best muxed video+audio (default)\n"
"  best[height<=720]             best muxed up to 720p\n"
"  worst / w                     worst quality muxed\n"
"  <itag>                        specific format by itag number\n\n"
"Examples:\n"
"  ytcui-dl -g -f bestaudio 'https://youtube.com/watch?v=dQw4w9WgXcQ'\n"
"  ytcui-dl -x -f 'bestaudio[ext=m4a]' URL\n"
"  ytcui-dl -F URL\n"
"  ytcui-dl -j URL | jq .formats\n"
"  ytcui-dl --get-thumbnail URL\n"
"  ytcui-dl --download -f 'best[height<=720]' -o clip.mp4 'URL'\n"
"  ytcui-dl --search 'lofi hip hop' 5\n\n"
"Build deps: -lcurl -lssl -lcrypto -lpthread\n",
    prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog);
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    ytfast::CurlGlobalInit curl_init;
    auto& yt = ytfast::InnertubeClient::get_instance();

    std::string mode;
    std::string fmt_sel   = "best";
    std::string url_arg;
    std::string out_path;
    std::string print_field;
    int         search_n  = 10;
    bool        for_streaming = false;  // -g for streaming URLs

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-g" || a == "--get-url") { mode = "get_url"; for_streaming = true; }
        else if (a == "-x")           { mode = "download"; fmt_sel = "bestaudio"; }
        else if (a == "--download")   { mode = "download"; }
        else if (a == "-j" || a == "--dump-json") { mode = "dump_json"; }
        else if (a == "--info")       { mode = "info"; }
        else if (a == "-F" || a == "--list-formats") { mode = "list_formats"; }
        else if (a == "--formats")    { mode = "formats"; }
        else if (a == "--get-thumbnail") { mode = "get_thumbnail"; }
        else if (a == "--bootstrap")  { mode = "bootstrap"; }
        else if (a == "--print" && i+1 < argc) {
            mode = "print";
            print_field = argv[++i];
        }
        else if (a == "--search") {
            mode = "search";
            if (i+1 < argc) url_arg = argv[++i];
            if (i+1 < argc) { try { search_n = std::stoi(argv[i+1]); ++i; } catch(...) {} }
        }
        else if ((a=="-f"||a=="--format") && i+1<argc) { fmt_sel  = argv[++i]; }
        else if ((a=="-o"||a=="--output") && i+1<argc) { out_path = argv[++i]; }
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a[0] != '-' && url_arg.empty())        { url_arg  = a; }
    }

    try {
        // ------------------------------------------------------------------
        if (mode == "bootstrap") {
            fprintf(stderr, "Bootstrapping visitor_data...\n");
            yt.bootstrap_visitor_data();
            fprintf(stderr, "visitor_data: %s\n", yt.visitor_data().c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "search") {
            if (url_arg.empty()) { fprintf(stderr,"Error: no query\n"); return 1; }
            auto results = yt.search(url_arg, search_n, false);
            json arr = json::array();
            for (auto& r : results)
                arr.push_back({
                    {"id",r.id},{"title",r.title},{"channel",r.channel},
                    {"channel_id",r.channel_id},{"duration",r.duration_str},
                    {"duration_secs",r.duration_secs},{"view_count",r.view_count},
                    {"view_count_str",r.view_count_str},{"upload_date",r.upload_date},
                    {"thumbnail_url",r.thumbnail_url},{"url",r.url},{"is_live",r.is_live},
                });
            printf("%s\n", arr.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (url_arg.empty()) { print_usage(argv[0]); return 1; }
        std::string video_id = extract_id(url_arg);

        // ------------------------------------------------------------------
        if (mode == "get_url") {
            auto info = yt.get_stream_formats(video_id);
            std::string resolved = ytfast::InnertubeClient::resolve_format_string(
                fmt_sel, info.formats, for_streaming);
            if (resolved.empty()) {
                fprintf(stderr, "Error: no format matching '%s'\n", fmt_sel.c_str());
                return 1;
            }
            printf("%s\n", resolved.c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "dump_json") {
            auto info = yt.get_stream_formats(video_id);
            json j = info_to_json(info);
            printf("%s\n", j.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "info") {
            auto info = yt.get_stream_formats(video_id);
            json j = {
                {"id",info.id},{"title",info.title},{"channel",info.channel},
                {"channel_id",info.channel_id},{"description",info.description},
                {"duration",info.duration_str},{"duration_secs",info.duration_secs},
                {"view_count",info.view_count},{"view_count_str",info.view_count_str},
                {"thumbnail_url",info.thumbnail_url},{"upload_date",info.upload_date},
                {"url",info.url},{"is_live",info.is_live},
                {"category",info.category},
                {"format_count",(int)info.formats.size()},
            };
            printf("%s\n", j.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "list_formats") {
            auto info = yt.get_stream_formats(video_id);
            print_format_table(info);
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "formats") {
            auto info = yt.get_stream_formats(video_id);
            json arr = json::array();
            for (auto& f : info.formats)
                arr.push_back(format_to_json(f));
            printf("%s\n", arr.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "get_thumbnail") {
            auto info = yt.get_stream_formats(video_id);
            if (info.thumbnail_url.empty()) {
                fprintf(stderr, "Error: no thumbnail available\n");
                return 1;
            }
            printf("%s\n", info.thumbnail_url.c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "print") {
            auto info = yt.get_stream_formats(video_id);
            std::string val;
            if      (print_field == "id")          val = info.id;
            else if (print_field == "title")       val = info.title;
            else if (print_field == "channel" || print_field == "uploader") val = info.channel;
            else if (print_field == "channel_id")  val = info.channel_id;
            else if (print_field == "description") val = info.description;
            else if (print_field == "thumbnail" || print_field == "thumbnail_url")
                                                   val = info.thumbnail_url;
            else if (print_field == "url" || print_field == "webpage_url") val = info.url;
            else if (print_field == "duration")    val = info.duration_str;
            else if (print_field == "duration_secs")val = std::to_string(info.duration_secs);
            else if (print_field == "view_count")  val = std::to_string(info.view_count);
            else if (print_field == "upload_date") val = info.upload_date;
            else if (print_field == "category")    val = info.category;
            else if (print_field == "is_live")     val = info.is_live ? "true" : "false";
            else {
                fprintf(stderr, "Unknown field: %s\n", print_field.c_str());
                fprintf(stderr, "Available: id, title, channel, channel_id, description, "
                        "thumbnail, url, duration, duration_secs, view_count, upload_date, "
                        "category, is_live\n");
                return 1;
            }
            printf("%s\n", val.c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "download") {
            auto info = yt.get_stream_formats(video_id);

            // Resolve which stream to download (download mode = not streaming)
            std::string stream_url = ytfast::InnertubeClient::resolve_format_string(
                fmt_sel, info.formats, false);
            if (stream_url.empty()) {
                fprintf(stderr, "Error: no format matching '%s'\n", fmt_sel.c_str());
                return 1;
            }

            // Find the format entry for metadata
            const ytfast::StreamFormat* fmt = nullptr;
            for (auto& f : info.formats)
                if (f.url == stream_url) { fmt = &f; break; }

            bool is_audio_only = fmt && fmt->is_audio_only();

            // Determine output path
            if (out_path.empty()) {
                std::string ext = fmt ? fmt_to_ext(*fmt) : (is_audio_only ? "m4a" : "mp4");
                out_path = safe_filename(info.title, ext);
            }

            std::string ua = ytfast::ANDROID_UA;
            int64_t content_length = fmt ? fmt->content_length : 0;

            fprintf(stderr, "Title:    %s\n", info.title.c_str());
            fprintf(stderr, "Format:   itag=%d  %s  %s%s",
                    fmt ? fmt->itag : 0,
                    fmt ? fmt->mime_type.c_str() : "unknown",
                    is_audio_only ? "audio-only" : "video",
                    fmt && fmt->height > 0 ? (" " + std::to_string(fmt->height) + "p").c_str() : "");
            if (fmt && !fmt->audio_codec.empty())
                fprintf(stderr, "  [%s]", fmt->audio_codec.c_str());
            if (fmt && !fmt->video_codec.empty())
                fprintf(stderr, "  [%s]", fmt->video_codec.c_str());
            fprintf(stderr, "\n");
            if (content_length > 0)
                fprintf(stderr, "Size:     ~%.1f MB\n", content_length / 1048576.0);
            fprintf(stderr, "Output:   %s\n", out_path.c_str());
            fprintf(stderr, "Downloading...\n");

            bool ok = download_url(stream_url, out_path, ua, content_length);
            if (ok) {
                struct stat st{};
                stat(out_path.c_str(), &st);
                fprintf(stderr, "Done: %s (%.2f MB)\n",
                        out_path.c_str(), st.st_size / 1048576.0);
                return 0;
            }
            return 1;
        }

        // ------------------------------------------------------------------
        // No mode specified — default to get_url for URL args, help otherwise
        if (!url_arg.empty()) {
            auto info = yt.get_stream_formats(video_id);
            std::string resolved = ytfast::InnertubeClient::resolve_format_string(
                fmt_sel, info.formats, true);
            if (!resolved.empty()) {
                printf("%s\n", resolved.c_str());
                return 0;
            }
        }

        print_usage(argv[0]);
        return 1;

    } catch (const std::exception& e) {
        fprintf(stderr, "ytcui-dl error: %s\n", e.what());
        return 1;
    }
}
