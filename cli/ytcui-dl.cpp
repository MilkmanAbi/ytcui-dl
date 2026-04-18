/*
 * ytcui-dl — command-line tool
 *
 * Fast YouTube stream resolver and downloader.
 *
 * Usage:
 *   ytcui-dl -g [-f FORMAT] URL           print stream URL(s)
 *   ytcui-dl -x URL                       download best audio (m4a/webm)
 *   ytcui-dl -x -f bestaudio URL          download best audio, explicit
 *   ytcui-dl --download [-f FORMAT] URL   download video (default: best ≤1080p)
 *   ytcui-dl --download -f best[height<=720] URL
 *   ytcui-dl -o filename.mp4 URL          set output filename
 *   ytcui-dl --search QUERY [N=10]        search, JSON output
 *   ytcui-dl --info URL                   video metadata as JSON
 *   ytcui-dl --formats URL                list all formats as JSON
 *   ytcui-dl --bootstrap                  warm visitor_data cache and exit
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
    auto sl = url.rfind('/');
    if (sl != std::string::npos) { auto seg=url.substr(sl+1,11); if(seg.size()==11) return seg; }
    return url;
}

static std::string resolve_format(const std::string& sel,
                                   const std::vector<ytfast::StreamFormat>& fmts) {
    if (sel == "bestaudio" || sel == "ba" || sel == "audio")
        return ytfast::InnertubeClient::select_best_audio(fmts);
    if (sel == "bestvideo" || sel == "bv") {
        // video-only (no audio) — best adaptive video stream
        const ytfast::StreamFormat* best = nullptr;
        for (auto& f : fmts)
            if (f.has_video && !f.has_audio && (!best || f.height > best->height)) best = &f;
        return best ? best->url : ytfast::InnertubeClient::select_best_video(fmts, 1080);
    }
    if (sel.find("best") != std::string::npos || sel.find("video") != std::string::npos) {
        int max_h = 1080;
        // Handle best[height<=720] — even if bash ate the brackets, look for digits after <=
        // Handles: "best[height<=720]", "bestheight<=720" (bash glob), "720", etc.
        auto p = sel.find("<=");
        if (p != std::string::npos) {
            try {
                std::string num = sel.substr(p+2);
                // strip trailing ] or other chars
                while (!num.empty() && !isdigit((unsigned char)num[0])) num = num.substr(1);
                size_t end = 0;
                max_h = std::stoi(num, &end);
            } catch (...) {}
        }
        return ytfast::InnertubeClient::select_best_video(fmts, max_h);
    }
    // itag number
    try {
        int itag = std::stoi(sel);
        for (auto& f : fmts) if (f.itag == itag) return f.url;
    } catch (...) {}
    // default: best video
    return ytfast::InnertubeClient::select_best_video(fmts, 1080);
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
    // Trim to 200 chars to stay sane on every FS
    if (out.size() > 200) out.resize(200);
    // Remove trailing dots/spaces
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    if (out.empty()) out = "download";
    return out + "." + ext;
}

// Guess extension from mime_type
static std::string mime_to_ext(const std::string& mime, bool audio_only) {
    if (mime.find("webm") != std::string::npos) return audio_only ? "webm" : "webm";
    if (mime.find("mp4")  != std::string::npos) return audio_only ? "m4a"  : "mp4";
    if (mime.find("ogg")  != std::string::npos) return "ogg";
    return audio_only ? "m4a" : "mp4";
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
    if (fwrite(ptr, 1, n, ctx->fp) != n) return 0; // signals curl to abort
    ctx->received += (int64_t)n;
    return n;
}

static int dl_progress(void* ud, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DlCtx*>(ud);
    if (!ctx->tty) return 0;
    if (dltotal <= 0) {
        fprintf(stderr, "\r  %.1f MB downloaded...",
                dlnow / 1048576.0);
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
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "identity"); // no gzip on binary
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         0L);         // no timeout for large files
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  10L);
    // Resume support: if file exists and is partial, resume
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
"ytcui-dl v0.1.0 — fast YouTube resolver & downloader\n\n"
"Stream URL resolution:\n"
"  %s -g [-f FORMAT] URL         print resolved stream URL\n\n"
"Download:\n"
"  %s -x URL                     download best audio (webm/m4a)\n"
"  %s --download URL             download best video+audio ≤1080p\n"
"  %s --download -f FORMAT URL   download with specific format\n"
"  %s -o OUTPUT.mp4 ...          set output path (default: <title>.<ext>)\n\n"
"Metadata:\n"
"  %s --search QUERY [N=10]      search YouTube (JSON output)\n"
"  %s --info URL                 video metadata (JSON)\n"
"  %s --formats URL              all available formats (JSON)\n"
"  %s --bootstrap                fetch visitor_data and exit\n\n"
"Format strings:\n"
"  bestaudio / ba                best audio-only stream\n"
"  bestvideo / bv                best video-only stream\n"
"  best                          best muxed video+audio\n"
"  best[height<=720]             best video up to 720p\n"
"  <itag>                        specific format by itag number\n\n"
"Examples:\n"
"  ytcui-dl -g -f bestaudio 'https://youtube.com/watch?v=dQw4w9WgXcQ'\n"
"  ytcui-dl -x 'https://youtube.com/watch?v=dQw4w9WgXcQ'\n"
"  ytcui-dl --download -f best[height<=720] -o clip.mp4 'URL'\n"
"  ytcui-dl --search 'lofi hip hop' 5\n\n"
"Build deps: -lcurl -lssl -lcrypto -lpthread\n",
    prog,prog,prog,prog,prog,prog,prog,prog,prog);
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
    std::string out_path;   // -o
    int         search_n  = 10;


    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-g")           { mode = "get_url"; }
        else if (a == "-x")           { mode = "download"; fmt_sel = "bestaudio"; }
        else if (a == "--download")   { mode = "download"; }
        else if (a == "--info")       { mode = "info";    }
        else if (a == "--formats")    { mode = "formats"; }
        else if (a == "--bootstrap")  { mode = "bootstrap"; }
        else if (a == "--search") {
            mode = "search";
            if (i+1 < argc) url_arg = argv[++i];
            if (i+1 < argc) { try { search_n = std::stoi(argv[i+1]); ++i; } catch(...) {} }
        }
        else if ((a=="-f"||a=="--format") && i+1<argc) { fmt_sel  = argv[++i]; }
        else if ((a=="-o"||a=="--output") && i+1<argc) { out_path = argv[++i]; }
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

        if (mode == "get_url") {
            auto info = yt.get_stream_formats(video_id);
            std::string resolved = resolve_format(fmt_sel, info.formats);
            if (resolved.empty()) {
                fprintf(stderr, "Error: no format matching '%s'\n", fmt_sel.c_str());
                return 1;
            }
            printf("%s\n", resolved.c_str());
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
                {"url",info.url},{"is_live",info.is_live},{"format_count",(int)info.formats.size()},
            };
            printf("%s\n", j.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "formats") {
            auto info = yt.get_stream_formats(video_id);
            json arr = json::array();
            for (auto& f : info.formats)
                arr.push_back({
                    {"itag",f.itag},{"mime_type",f.mime_type},{"quality",f.quality},
                    {"width",f.width},{"height",f.height},{"bitrate",f.bitrate},
                    {"has_video",f.has_video},{"has_audio",f.has_audio},
                    {"audio_codec",f.audio_codec},{"video_codec",f.video_codec},
                    {"content_length",f.content_length},{"url",f.url},
                });
            printf("%s\n", arr.dump(2).c_str());
            return 0;
        }

        // ------------------------------------------------------------------
        if (mode == "download") {
            auto info = yt.get_stream_formats(video_id);

            // Resolve which stream to download
            std::string stream_url = resolve_format(fmt_sel, info.formats);
            if (stream_url.empty()) {
                fprintf(stderr, "Error: no format matching '%s'\n", fmt_sel.c_str());
                return 1;
            }

            // Find the format entry for metadata (mime, content_length, UA to use)
            const ytfast::StreamFormat* fmt = nullptr;
            for (auto& f : info.formats)
                if (f.url == stream_url) { fmt = &f; break; }

            bool is_audio_only = fmt && fmt->has_audio && !fmt->has_video;

            // Determine output path
            if (out_path.empty()) {
                std::string ext = fmt ? mime_to_ext(fmt->mime_type, is_audio_only) :
                                  (is_audio_only ? "m4a" : "mp4");
                out_path = safe_filename(info.title, ext);
            }

            // Pick the right User-Agent for the CDN request
            // Use Android UA to match what the player request used
            std::string ua = ytfast::ANDROID_UA;

            int64_t content_length = fmt ? fmt->content_length : 0;

            fprintf(stderr, "Title:    %s\n", info.title.c_str());
            fprintf(stderr, "Format:   %s  %s%s\n",
                    fmt ? fmt->mime_type.c_str() : "unknown",
                    is_audio_only ? "audio-only" : "video",
                    fmt && fmt->height > 0 ? (" " + std::to_string(fmt->height) + "p").c_str() : "");
            if (content_length > 0)
                fprintf(stderr, "Size:     ~%.1f MB\n", content_length / 1048576.0);
            fprintf(stderr, "Output:   %s\n", out_path.c_str());
            fprintf(stderr, "Downloading...\n");

            bool ok = download_url(stream_url, out_path, ua, content_length);
            if (ok) {
                // Report actual file size
                struct stat st{};
                stat(out_path.c_str(), &st);
                fprintf(stderr, "Done: %s (%.2f MB)\n",
                        out_path.c_str(), st.st_size / 1048576.0);
                return 0;
            }
            return 1;
        }

        // ------------------------------------------------------------------
        print_usage(argv[0]);
        return 1;

    } catch (const std::exception& e) {
        fprintf(stderr, "ytcui-dl error: %s\n", e.what());
        return 1;
    }
}
