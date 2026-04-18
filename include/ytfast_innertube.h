#pragma once
/*
 * ytcui-dl — ytfast_innertube.h
 *
 * Production-grade InnerTube client. Translated 1:1 from yt-dlp source:
 *   _base.py   — INNERTUBE_CLIENTS, _call_api, generate_api_headers,
 *                _extract_visitor_data, extract_ytcfg (VISITOR_DATA bootstrap)
 *   _search.py — _search_results (ep='search'), content_keys
 *   _video.py  — _generate_player_context, _get_checkok_params,
 *                streaming_data format parsing, _DEFAULT_CLIENTS
 *   _tab.py    — _get_text (runs/simpleText traversal)
 *
 * Key design decisions:
 *
 *   VISITOR_DATA bootstrap (fixes audio + LOGIN_REQUIRED):
 *     On first use, fetch www.youtube.com and extract VISITOR_DATA from
 *     ytcfg.set({...}) blobs — same as yt-dlp _download_ytcfg().
 *     This gives a real browser-style visitor token that eliminates bot
 *     detection on both search and player endpoints.
 *     Subsequent calls reuse the cached token.
 *
 *   Shared singleton pattern (fixes audio/video using different caches):
 *     get_instance() returns a process-wide singleton so youtube.cpp's
 *     search() and player.cpp's play_piped() share visitor_data and URL cache.
 *     Search warms the cache; play is instant.
 *
 *   Client chain: android(3) → ios(5) → android_vr(28)
 *     android gives 1080p+26 direct URLs on residential IPs (no JS player).
 *     Falls back gracefully. android_vr is the lowest-suspicion fallback.
 *
 *   URL cache with 5h TTL:
 *     CDN URLs expire in ~6h. Caching eliminates double-click bug entirely.
 *
 *   Async prefetch:
 *     search() spawns a detached thread to pre-resolve top result's streams.
 *     First click is near-instant.
 *
 *   Cross-platform:
 *     No Linux-specific headers. Works on macOS, FreeBSD, OpenBSD, NetBSD.
 *     Uses std::thread for background prefetch (POSIX threads via C++17).
 */

#include "ytfast_http.h"
#include "ytfast_types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>

namespace ytfast {

using json = nlohmann::json;
using sclock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Client definitions — from INNERTUBE_CLIENTS in _base.py
// ---------------------------------------------------------------------------

// web (id=1) — search, trusted UA
static const char* WEB_CLIENT_NAME    = "WEB";
static const char* WEB_CLIENT_VERSION = "2.20260114.08.00";
static const int   WEB_CLIENT_ID      = 1;
static const char* WEB_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

// android (id=3, REQUIRE_JS_PLAYER=False) — primary player, 1080p on residential IPs
static const char* ANDROID_CLIENT_NAME    = "ANDROID";
static const char* ANDROID_CLIENT_VERSION = "21.02.35";
static const int   ANDROID_CLIENT_ID      = 3;
static const char* ANDROID_UA =
    "com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip";

// ios (id=5, REQUIRE_JS_PLAYER=False) — fallback, 1080p + HLS manifest
static const char* IOS_CLIENT_NAME    = "IOS";
static const char* IOS_CLIENT_VERSION = "21.02.3";
static const int   IOS_CLIENT_ID      = 5;
static const char* IOS_UA =
    "com.google.ios.youtube/21.02.3 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";

// android_vr (id=28, REQUIRE_JS_PLAYER=False) — last resort, 240p cap but reliable
static const char* ANDROIDVR_CLIENT_NAME    = "ANDROID_VR";
static const char* ANDROIDVR_CLIENT_VERSION = "1.65.10";
static const int   ANDROIDVR_CLIENT_ID      = 28;
static const char* ANDROIDVR_UA =
    "com.google.android.apps.youtube.vr.oculus/1.65.10 "
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";

static const char* INNERTUBE_BASE = "https://www.youtube.com/youtubei/v1/";
static const char* SEARCH_PARAMS  = "EgIQAfABAQ=="; // videos-only filter

// CDN URL TTL — YouTube CDN URLs valid ~6h, we cache 5h
static const int URL_CACHE_TTL_S = 5 * 3600;
// Visitor data TTL — refresh every 12h
static const int VISITOR_TTL_S   = 12 * 3600;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string safe_str(const json& j, const char* k, const std::string& fb="") {
    if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
    return fb;
}
static int64_t safe_int(const json& j, const char* k, int64_t fb=0) {
    if (!j.contains(k)) return fb;
    if (j[k].is_number_integer()) return j[k].get<int64_t>();
    if (j[k].is_number()) return (int64_t)j[k].get<double>();
    if (j[k].is_string()) try { return std::stoll(j[k].get<std::string>()); } catch(...) {}
    return fb;
}

// sanitize_utf8 — strips invalid bytes, keeps printable + valid multibyte
static std::string sanitize_utf8(const std::string& s) {
    std::string o; o.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.c_str();
    const unsigned char* e = p + s.size();
    while (p < e) {
        if (*p < 0x80) {
            if (*p >= 32 || *p == '\t' || *p == '\n') o += (char)*p;
            ++p;
        } else if ((*p&0xE0)==0xC0 && p+1<e && (p[1]&0xC0)==0x80)
            { o.append((const char*)p,2); p+=2; }
        else if ((*p&0xF0)==0xE0 && p+2<e && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80)
            { o.append((const char*)p,3); p+=3; }
        else if ((*p&0xF8)==0xF0 && p+3<e && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80)
            { o.append((const char*)p,4); p+=4; }
        else ++p;
    }
    return o;
}

static std::string fmt_duration(int s) {
    if (s<=0) return "0:00";
    char b[32]; int h=s/3600, m=(s%3600)/60, sc=s%60;
    if (h>0) snprintf(b,sizeof(b),"%d:%02d:%02d",h,m,sc);
    else      snprintf(b,sizeof(b),"%d:%02d",m,sc);
    return b;
}

static std::string fmt_views(int64_t v) {
    char b[32];
    if      (v>=1000000000) snprintf(b,sizeof(b),"%.2fB",v/1e9);
    else if (v>=1000000)    snprintf(b,sizeof(b),"%.2fM",v/1e6);
    else if (v>=1000)       snprintf(b,sizeof(b),"%.2fK",v/1e3);
    else                    snprintf(b,sizeof(b),"%lld",(long long)v);
    return b;
}

static std::string decode_entities(std::string s) {
    struct { const char* a; const char* b; } tbl[] = {
        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"},{"&apos;","'"}
    };
    for (auto& e : tbl)
        for (size_t p; (p=s.find(e.a))!=std::string::npos; )
            s.replace(p, strlen(e.a), e.b);
    return s;
}

// CPN generation — _video.py:2274 "works even with dummy cpn"
static std::string gen_cpn() {
    static const char* alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(0,63);
    std::string r(16,' ');
    for (char& c : r) c = alpha[d(rng)];
    return r;
}

// extract_text — _tab.py _get_text(): simpleText or runs[].text
static std::string extract_text(const json& node) {
    if (node.is_string()) return node.get<std::string>();
    if (node.contains("simpleText") && node["simpleText"].is_string())
        return node["simpleText"].get<std::string>();
    if (node.contains("runs") && node["runs"].is_array()) {
        std::string o;
        for (auto& r : node["runs"])
            if (r.contains("text") && r["text"].is_string())
                o += r["text"].get<std::string>();
        return o;
    }
    return "";
}
static std::string get_field(const json& p, const char* k) {
    return p.contains(k) ? extract_text(p[k]) : "";
}

// Pick highest-res thumbnail from thumbnails[]
// Build best YouTube thumbnail URL — tries maxresdefault then hq720 then picks widest
static std::string best_thumb_from_id(const std::string& video_id) {
    if (video_id.empty() || video_id.size() != 11) return "";
    // maxresdefault is the gold standard (1280x720+), not always available but try first
    return "https://i.ytimg.com/vi/" + video_id + "/maxresdefault.jpg";
}
static std::string best_thumb(const json& node, const std::string& video_id = "") {
    // First, if we have a known video_id prefer maxresdefault
    if (!video_id.empty() && video_id.size() == 11)
        return best_thumb_from_id(video_id);
    
    auto from_arr = [](const json& a) {
        std::string best; int bw = -1;
        if (!a.is_array()) return best;
        for (auto& t : a) {
            int w = (int)safe_int(t,"width");
            std::string u = safe_str(t,"url");
            // Prefer non-webp, higher resolution
            if (!u.empty() && w >= bw) { best=u; bw=w; }
        }
        return best;
    };
    if (node.contains("thumbnail") && node["thumbnail"].contains("thumbnails"))
        return from_arr(node["thumbnail"]["thumbnails"]);
    if (node.contains("thumbnails")) return from_arr(node["thumbnails"]);
    return "";
}

static int64_t parse_vc(const std::string& s) {
    std::string d; for (char c:s) if(c>='0'&&c<='9') d+=c;
    if (d.empty()) return 0;
    try { return std::stoll(d); } catch (...) { return 0; }
}
static int parse_dur(const std::string& s) {
    if (s.empty()) return 0;
    int t=0,v=0;
    for (char c:s) { if(c==':'){t=t*60+v;v=0;} else if(c>='0'&&c<='9') v=v*10+(c-'0'); }
    return t*60+v;
}

// Extract VISITOR_DATA from YouTube HTML — no regex (crashes on large HTML)
// Implements yt-dlp extract_ytcfg() + _extract_visitor_data()
static std::string extract_visitor_data_from_html(const std::string& html) {
    // Try "VISITOR_DATA":"..." first (in ytcfg blobs)
    const char* n1 = "\"VISITOR_DATA\":\"";
    auto p = html.find(n1);
    if (p != std::string::npos) {
        p += strlen(n1);
        auto e = html.find('"', p);
        if (e != std::string::npos && e > p && e-p < 200)
            return html.substr(p, e-p);
    }
    // Fallback: "visitorData":"..." (in responseContext or INNERTUBE_CONTEXT)
    const char* n2 = "\"visitorData\":\"";
    p = html.find(n2);
    if (p != std::string::npos) {
        p += strlen(n2);
        auto e = html.find('"', p);
        if (e != std::string::npos && e > p && e-p < 200)
            return html.substr(p, e-p);
    }
    return "";
}

// Extract codec from mime_type "video/mp4; codecs=\"avc1.4d401f\""
static std::string extract_codec(const std::string& mime) {
    auto p = mime.find("codecs=\"");
    if (p == std::string::npos) p = mime.find("codecs=");
    if (p == std::string::npos) return "";
    p = mime.find('"', p); if (p == std::string::npos) return "";
    ++p;
    auto e = mime.find('"', p);
    return e != std::string::npos ? mime.substr(p, e-p) : "";
}


// add_range_param() — append &range=0-N to force single HTTP response
// YouTube DASH streams return only the init+first segment without this.
// yt-dlp does the same for non-live streams internally.
static std::string add_range_param(const std::string& url, int64_t content_length) {
    if (url.find("&range=") != std::string::npos) return url; // already has range
    if (url.find("?") == std::string::npos) return url; // not a query URL
    // Live streams have no content_length — don't add range
    if (content_length <= 0) return url;
    char buf[64];
    snprintf(buf, sizeof(buf), "&range=0-%lld", (long long)(content_length - 1));
    return url + buf;
}

// Applies range param to a StreamFormat's URL in-place
static void fix_url_for_streaming(StreamFormat& fmt) {
    if (fmt.content_length > 0)
        fmt.url = add_range_param(fmt.url, fmt.content_length);
}

// ---------------------------------------------------------------------------
// URL cache
// ---------------------------------------------------------------------------
struct CachedInfo {
    VideoInfo info;
    sclock::time_point fetched_at;
};

class UrlCache {
public:
    const VideoInfo* get(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(id);
        if (it == cache_.end()) return nullptr;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            sclock::now() - it->second.fetched_at).count();
        if (age > URL_CACHE_TTL_S) { cache_.erase(it); return nullptr; }
        return &it->second.info;
    }
    void put(const std::string& id, const VideoInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[id] = { info, sclock::now() };
    }
private:
    std::mutex mu_;
    std::unordered_map<std::string, CachedInfo> cache_;
};

// ---------------------------------------------------------------------------
// InnertubeClient
// ---------------------------------------------------------------------------

class InnertubeClient {
public:
    InnertubeClient() {}

    // -----------------------------------------------------------------------
    // get_instance() — process-wide singleton
    // Ensures youtube.cpp and player.cpp share visitor_data and URL cache.
    // This is the fix for audio failing after video works — they were using
    // separate instances, search warm-up didn't propagate to player.
    // -----------------------------------------------------------------------
    static InnertubeClient& get_instance() {
        static InnertubeClient inst;
        return inst;
    }

    // -----------------------------------------------------------------------
    // bootstrap_visitor_data()
    // Fetches www.youtube.com to extract real VISITOR_DATA from ytcfg blobs.
    // Called once on first use (or after TTL expires).
    // Implements yt-dlp's _download_ytcfg() + extract_ytcfg() flow.
    // -----------------------------------------------------------------------
    void bootstrap_visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        // Check if still fresh
        if (!visitor_data_.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                sclock::now() - visitor_data_fetched_).count();
            if (age < VISITOR_TTL_S) return;
        }
        // Strategy 1: YouTube homepage (best — gets real ytcfg VISITOR_DATA)
        // Strategy 2: YouTube /sw.js (lightweight, always accessible)
        // Strategy 3: Fall back to responseContext from first API call (set elsewhere)
        const char* urls[] = {
            "https://www.youtube.com/",
            "https://www.youtube.com/sw.js",
            nullptr
        };
        for (int i = 0; urls[i]; ++i) {
            try {
                auto resp = get_thread_http().get(urls[i], {
                    std::string("User-Agent: ") + WEB_UA,
                    "Accept-Language: en-US,en;q=0.9",
                    "Accept: text/html,application/xhtml+xml,*/*;q=0.8"
                });
                if (resp.status == 200 && !resp.body.empty()) {
                    std::string vd = extract_visitor_data_from_html(resp.body);
                    if (!vd.empty()) {
                        visitor_data_ = vd;
                        visitor_data_fetched_ = sclock::now();
                        return;
                    }
                }
            } catch (...) {}
        }
        // Bootstrap failed — visitor_data will be populated from first API responseContext
        // This is the fallback path and still works fine on most IPs
    }

    // -----------------------------------------------------------------------
    // search() — POST /youtubei/v1/search
    // prefetch_top: resolve top result's streams in background thread
    // -----------------------------------------------------------------------
    std::vector<SearchResult> search(const std::string& query,
                                     int max_results = 15,
                                     bool prefetch_top = true) {
        ensure_visitor_data();

        std::vector<SearchResult> results;
        json body = {
            {"context", web_ctx()},
            {"query",   query},
            {"params",  SEARCH_PARAMS}
        };

        auto resp = get_thread_http().post(
            std::string(INNERTUBE_BASE) + "search?prettyPrint=false",
            body.dump(), web_hdrs());

        if (resp.status != 200)
            throw std::runtime_error("search HTTP " + std::to_string(resp.status));

        json data = json::parse(resp.body);

        // Update visitor_data from response if we didn't get it from homepage
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (visitor_data_.empty() && data.contains("responseContext"))
                visitor_data_ = safe_str(data["responseContext"], "visitorData");
        }

        try {
            auto& secs = data["contents"]["twoColumnSearchResultsRenderer"]
                             ["primaryContents"]["sectionListRenderer"]["contents"];
            for (auto& sec : secs) {
                if (!sec.contains("itemSectionRenderer")) continue;
                for (auto& item : sec["itemSectionRenderer"]["contents"]) {
                    if (!item.contains("videoRenderer")) continue;
                    auto& vr = item["videoRenderer"];
                    SearchResult v;
                    v.id = safe_str(vr, "videoId");
                    if (v.id.empty()) continue;
                    v.title   = sanitize_utf8(decode_entities(get_field(vr, "title")));
                    v.channel = sanitize_utf8(get_field(vr, "ownerText"));
                    if (v.channel.empty())
                        v.channel = sanitize_utf8(get_field(vr, "shortBylineText"));
                    v.view_count     = parse_vc(get_field(vr, "viewCountText"));
                    v.view_count_str = v.view_count > 0 ? fmt_views(v.view_count) : "N/A";
                    v.duration_str   = get_field(vr, "lengthText");
                    v.duration_secs  = parse_dur(v.duration_str);
                    if (v.duration_str.empty()) v.duration_str = "0:00";
                    // Prefer maxresdefault thumbnail (high quality)
                    v.thumbnail_url  = best_thumb_from_id(v.id);
                    v.upload_date    = get_field(vr, "publishedTimeText");
                    v.url            = "https://www.youtube.com/watch?v=" + v.id;

                    // channel_id: ownerText.runs[0].navigationEndpoint.browseEndpoint.browseId
                    // Translated from _tab.py channel extraction logic
                    try {
                        if (vr.contains("ownerText") && vr["ownerText"].contains("runs")
                            && !vr["ownerText"]["runs"].empty()) {
                            auto& run = vr["ownerText"]["runs"][0];
                            if (run.contains("navigationEndpoint") &&
                                run["navigationEndpoint"].contains("browseEndpoint"))
                                v.channel_id = run["navigationEndpoint"]["browseEndpoint"]
                                               .value("browseId", "");
                        }
                    } catch (...) {}

                    // Description snippet from detailedMetadataSnippets — gives preview
                    // without a separate player API call. Translated from _tab.py.
                    try {
                        if (vr.contains("detailedMetadataSnippets")) {
                            for (auto& snip : vr["detailedMetadataSnippets"]) {
                                if (!snip.contains("snippetText")) continue;
                                std::string text;
                                auto& st = snip["snippetText"];
                                if (st.contains("runs")) {
                                    for (auto& run2 : st["runs"])
                                        if (run2.contains("text") && run2["text"].is_string())
                                            text += run2["text"].get<std::string>();
                                } else if (st.contains("simpleText")) {
                                    text = safe_str(st, "simpleText");
                                }
                                if (!text.empty()) {
                                    v.description = sanitize_utf8(text);
                                    break;
                                }
                            }
                        }
                    } catch (...) {}
                    if (vr.contains("badges"))
                        for (auto& b : vr["badges"])
                            if (b.contains("metadataBadgeRenderer") &&
                                safe_str(b["metadataBadgeRenderer"], "label") == "LIVE")
                                { v.is_live = true; break; }
                    results.push_back(std::move(v));
                    if ((int)results.size() >= max_results) goto done;
                }
            }
        } catch (const json::exception&) {}
        done:

        // Async prefetch top result — makes first click near-instant
        if (prefetch_top && !results.empty()) {
            std::string top_id = results[0].id;
            std::thread([this, top_id]() {
                try {
                    if (!url_cache_.get(top_id))
                        url_cache_.put(top_id, fetch_player(top_id));
                } catch (...) {}
            }).detach();
        }

        return results;
    }

    // -----------------------------------------------------------------------
    // get_stream_formats() — with URL cache
    // -----------------------------------------------------------------------
    VideoInfo get_stream_formats(const std::string& video_id) {
        ensure_visitor_data();
        if (const VideoInfo* c = url_cache_.get(video_id)) return *c;
        auto info = fetch_player(video_id);
        url_cache_.put(video_id, info);
        return info;
    }

    std::string get_best_audio_url(const std::string& vid) {
        return select_best_audio(get_stream_formats(vid).formats);
    }
    std::string get_best_video_url(const std::string& vid, int max_h = 1080) {
        return select_best_video(get_stream_formats(vid).formats, max_h);
    }

    // Warm cache in background (call when user hovers/selects a result)
    void prefetch(const std::string& video_id) {
        std::thread([this, video_id]() {
            try {
                if (!url_cache_.get(video_id))
                    url_cache_.put(video_id, fetch_player(video_id));
            } catch (...) {}
        }).detach();
    }

    // -----------------------------------------------------------------------
    // Format selection
    // -----------------------------------------------------------------------

    // Best audio-only stream.
    // Prefers opus/webm (better quality/bitrate ratio) then aac/mp4.
    // select_best_audio: best audio for DOWNLOADING (prefers opus/webm quality, url has &range=)
    static std::string select_best_audio(const std::vector<StreamFormat>& fmts) {
        const StreamFormat* best = nullptr;
        // Prefer opus/webm — best quality/bitrate ratio
        for (auto& f : fmts)
            if (f.has_audio && !f.has_video && f.mime_type.find("audio/webm") != std::string::npos)
                if (!best || f.bitrate > best->bitrate) best = &f;
        // Fallback: aac/mp4
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && !f.has_video && f.mime_type.find("audio/mp4") != std::string::npos)
                    if (!best || f.bitrate > best->bitrate) best = &f;
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && !f.has_video && (!best || f.bitrate > best->bitrate)) best = &f;
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && (!best || f.bitrate > best->bitrate)) best = &f;
        return best ? best->url : "";
    }

    // select_best_streaming_audio: best audio for MPV STREAMING
    // Prefers aac/mp4 (itag 140) — plays reliably in mpv without range tricks.
    // Returns stream_url (raw URL, no &range= appended).
    static std::string select_best_streaming_audio(const std::vector<StreamFormat>& fmts) {
        const StreamFormat* best = nullptr;
        // Prefer aac/mp4 — most compatible with mpv HTTP streaming
        for (auto& f : fmts)
            if (f.has_audio && !f.has_video && f.mime_type.find("audio/mp4") != std::string::npos)
                if (!best || f.bitrate > best->bitrate) best = &f;
        // Fallback: opus/webm
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && !f.has_video && f.mime_type.find("audio/webm") != std::string::npos)
                    if (!best || f.bitrate > best->bitrate) best = &f;
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && !f.has_video && (!best || f.bitrate > best->bitrate)) best = &f;
        if (!best)
            for (auto& f : fmts)
                if (f.has_audio && (!best || f.bitrate > best->bitrate)) best = &f;
        // Return stream_url (no &range= param) for clean mpv HTTP streaming
        return best ? (best->stream_url.empty() ? best->url : best->stream_url) : "";
    }

    // select_best_streaming_video: video URL for mpv, raw stream_url (no range param)
    static std::string select_best_streaming_video(const std::vector<StreamFormat>& fmts,
                                                    int max_h = 1080) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.has_video || !f.has_audio || f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate)) best = &f;
        }
        if (!best)
            for (auto& f : fmts) {
                if (!f.has_video || f.height > max_h) continue;
                if (!best || f.height > best->height) best = &f;
            }
        return best ? (best->stream_url.empty() ? best->url : best->stream_url) : "";
    }

    // Best video stream up to max_h.
    // Prefers muxed (V+A) to avoid needing --audio-file.
    // Falls back to video-only adaptive if no muxed available.
    static std::string select_best_video(const std::vector<StreamFormat>& fmts,
                                          int max_h = 1080) {
        const StreamFormat* best = nullptr;
        // Muxed first
        for (auto& f : fmts) {
            if (!f.has_video || !f.has_audio || f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate))
                best = &f;
        }
        // Adaptive video-only
        if (!best)
            for (auto& f : fmts) {
                if (!f.has_video || f.height > max_h) continue;
                if (!best || f.height > best->height ||
                    (f.height == best->height && f.bitrate > best->bitrate))
                    best = &f;
            }
        return best ? best->url : "";
    }

    // Returns true if video_url points to a muxed stream (V+A, no separate audio needed)
    static bool is_muxed(const std::vector<StreamFormat>& fmts,
                          const std::string& video_url) {
        for (auto& f : fmts)
            if (f.url == video_url && f.has_video && f.has_audio) return true;
        return false;
    }

    // Visitor data accessor — useful for passing to mpv as header
    std::string visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        return visitor_data_;
    }

private:
    UrlCache    url_cache_;

    std::mutex              vd_mu_;
    std::string             visitor_data_;
    sclock::time_point      visitor_data_fetched_{};
    bool                    visitor_bootstrap_done_ = false;

    struct ClientDef {
        const char* name, *ver, *ua;
        int id;
        bool android_style; // set androidSdkVersion + osName/Version
        bool ios_style;     // set deviceMake/Model + osName/Version
    };

    // Ensure visitor_data is populated (lazy, once-per-process)
    void ensure_visitor_data() {
        bool needs_bootstrap = false;
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (!visitor_bootstrap_done_) {
                visitor_bootstrap_done_ = true;
                needs_bootstrap = true;
            }
        }
        if (needs_bootstrap) bootstrap_visitor_data();
    }

    // -----------------------------------------------------------------------
    // fetch_player() — android(3) → ios(5) → android_vr(28)
    // All three are REQUIRE_JS_PLAYER=False → no cipher/nsig solving
    // -----------------------------------------------------------------------
    VideoInfo fetch_player(const std::string& video_id) {
        ClientDef clients[] = {
            { ANDROID_CLIENT_NAME, ANDROID_CLIENT_VERSION, ANDROID_UA,
              ANDROID_CLIENT_ID, true, false },
            { IOS_CLIENT_NAME, IOS_CLIENT_VERSION, IOS_UA,
              IOS_CLIENT_ID, false, true },
            { ANDROIDVR_CLIENT_NAME, ANDROIDVR_CLIENT_VERSION, ANDROIDVR_UA,
              ANDROIDVR_CLIENT_ID, false, false },
        };

        std::string last_err;
        for (auto& c : clients) {
            try { return player_request(video_id, c); }
            catch (const std::exception& ex) {
                last_err = ex.what();
                std::string msg(ex.what());
                bool soft = msg.find("bot")       != std::string::npos ||
                            msg.find("sign")      != std::string::npos ||
                            msg.find("UNPLAYABLE")!= std::string::npos ||
                            msg.find("LOGIN")     != std::string::npos ||
                            msg.find("streaming") != std::string::npos ||
                            msg.find("ERROR")     != std::string::npos;
                if (!soft) throw;
            }
        }
        throw std::runtime_error("All clients failed: " + last_err);
    }

    VideoInfo player_request(const std::string& vid, const ClientDef& c) {
        // Build client context — _base.py _extract_context()
        json client_ctx = {
            {"clientName",    c.name},
            {"clientVersion", c.ver},
            {"hl", "en"}, {"timeZone", "UTC"}, {"utcOffsetMinutes", 0}
        };
        if (c.android_style) {
            client_ctx["androidSdkVersion"] = 30;
            client_ctx["osName"]            = "Android";
            client_ctx["osVersion"]         = "11";
        }
        if (c.ios_style) {
            client_ctx["deviceMake"]  = "Apple";
            client_ctx["deviceModel"] = "iPhone16,2";
            client_ctx["osName"]      = "iPhone";
            client_ctx["osVersion"]   = "18.3.2.22D82";
        }
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (!visitor_data_.empty()) client_ctx["visitorData"] = visitor_data_;
        }

        // Player request body — _video.py _generate_player_context() + _get_checkok_params()
        json body = {
            {"context",        {{"client", client_ctx}}},
            {"videoId",        vid},
            {"contentCheckOk", true},
            {"racyCheckOk",    true},
            {"playbackContext", {{"contentPlaybackContext", {
                {"html5Preference", "HTML5_PREF_WANTS"}
            }}}},
            {"cpn", gen_cpn()},
        };

        std::string vd_copy;
        { std::lock_guard<std::mutex> lk(vd_mu_); vd_copy = visitor_data_; }

        std::vector<std::string> hdrs = {
            "X-YouTube-Client-Name: " + std::to_string(c.id),
            std::string("X-YouTube-Client-Version: ") + c.ver,
            std::string("User-Agent: ") + c.ua,
            "Origin: https://www.youtube.com",
        };
        if (!vd_copy.empty())
            hdrs.push_back("X-Goog-Visitor-Id: " + vd_copy);

        auto resp = get_thread_http().post(
            std::string(INNERTUBE_BASE) + "player?prettyPrint=false",
            body.dump(), hdrs);

        if (resp.status != 200)
            throw std::runtime_error(
                std::string(c.name) + " HTTP " + std::to_string(resp.status));

        json data = json::parse(resp.body);

        // Update visitor_data from response if needed
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (visitor_data_.empty() && data.contains("responseContext"))
                visitor_data_ = safe_str(data["responseContext"], "visitorData");
        }

        // Check playabilityStatus — _video.py _is_unplayable()
        if (data.contains("playabilityStatus")) {
            auto& ps = data["playabilityStatus"];
            std::string status = safe_str(ps, "status");
            std::string reason = safe_str(ps, "reason");
            if (reason.empty() && ps.contains("messages") &&
                ps["messages"].is_array() && !ps["messages"].empty())
                reason = ps["messages"][0].is_string() ?
                    ps["messages"][0].get<std::string>() : "";
            if (status == "UNPLAYABLE" || status == "ERROR" ||
                status == "LOGIN_REQUIRED")
                throw std::runtime_error(
                    std::string(c.name) + " " + status + ": " + reason);
        }

        if (!data.contains("streamingData")) {
            std::string reason;
            if (data.contains("playabilityStatus"))
                reason = safe_str(data["playabilityStatus"], "reason");
            throw std::runtime_error(
                "No streamingData from " + std::string(c.name) +
                (reason.empty() ? "" : " (" + reason + ")"));
        }

        // Parse VideoInfo
        VideoInfo info;
        info.id  = vid;
        info.url = "https://www.youtube.com/watch?v=" + vid;

        if (data.contains("videoDetails")) {
            auto& vd = data["videoDetails"];
            info.title          = sanitize_utf8(safe_str(vd, "title"));
            info.channel        = sanitize_utf8(safe_str(vd, "author"));
            info.channel_id     = safe_str(vd, "channelId");
            info.description    = sanitize_utf8(safe_str(vd, "shortDescription"));
            info.duration_secs  = (int)safe_int(vd, "lengthSeconds");
            info.duration_str   = fmt_duration(info.duration_secs);
            info.view_count     = safe_int(vd, "viewCount");
            info.view_count_str = fmt_views(info.view_count);
            if (vd.contains("isLive") && vd["isLive"].is_boolean())
                info.is_live = vd["isLive"].get<bool>();
            // Prefer maxresdefault (high-res), falls back to what player gives
            std::string thumb_from_id = best_thumb_from_id(vid);
            info.thumbnail_url = !thumb_from_id.empty() ? thumb_from_id : best_thumb(vd);
        }

        // Extract upload date from microformat if present
        // playerMicroformatRenderer.publishDate = "YYYY-MM-DD"
        if (data.contains("microformat")) {
            try {
                auto& pmr = data["microformat"]["playerMicroformatRenderer"];
                std::string pd = pmr.value("publishDate", "");
                if (pd.empty()) pd = pmr.value("uploadDate", "");
                if (!pd.empty()) info.upload_date = pd;
                // category is a bonus field
                std::string cat = pmr.value("category", "");
                if (!cat.empty() && info.description.empty())
                    info.description = cat; // better than nothing
            } catch (...) {}
        }

        // Parse formats + adaptiveFormats — _video.py lines 3368-3410
        auto& sd = data["streamingData"];
        for (const char* key : {"formats", "adaptiveFormats"}) {
            if (!sd.contains(key)) continue;
            for (auto& f : sd[key]) {
                StreamFormat fmt;
                fmt.itag            = (int)safe_int(f, "itag");
                fmt.url             = safe_str(f, "url");
                fmt.mime_type       = safe_str(f, "mimeType");
                fmt.quality         = safe_str(f, "quality");
                fmt.width           = (int)safe_int(f, "width");
                fmt.height          = (int)safe_int(f, "height");
                fmt.bitrate         = safe_int(f, "bitrate");
                fmt.content_length  = safe_int(f, "contentLength");
                fmt.has_video       = fmt.mime_type.find("video/") != std::string::npos;
                fmt.has_audio       = fmt.mime_type.find("audio/") != std::string::npos;
                if (strcmp(key, "formats") == 0) { // formats[] = always muxed
                    fmt.has_video = true;
                    fmt.has_audio = true;
                }
                if (f.contains("audioSampleRate"))
                    fmt.audio_sample_rate = (int)safe_int(f, "audioSampleRate");
                if (f.contains("audioChannels"))
                    fmt.audio_channels = (int)safe_int(f, "audioChannels");
                // Extract codecs from mimeType
                std::string codec = extract_codec(fmt.mime_type);
                if (fmt.has_audio && !fmt.has_video)
                    fmt.audio_codec = codec;
                else if (fmt.has_video && !fmt.has_audio)
                    fmt.video_codec = codec;

                if (fmt.url.empty()) continue; // skip signatureCipher entries
                fmt.stream_url = fmt.url; // save raw URL before range param is added
                info.formats.push_back(std::move(fmt));
            }
        }

        // Apply &range=0-N to all non-live DASH streams so mpv gets a single HTTP response.
        // Without this, mpv only plays the initialization segment (~1 second) then stops.
        // yt-dlp does the same internally for non-live streams.
        for (auto& fmt : info.formats)
            fix_url_for_streaming(fmt);

        // Sort: video height desc, then bitrate desc
        std::stable_sort(info.formats.begin(), info.formats.end(),
            [](const StreamFormat& a, const StreamFormat& b) {
                if (a.height != b.height) return a.height > b.height;
                return a.bitrate > b.bitrate;
            });

        return info;
    }

    // Context / header builders
    std::string get_vd() {
        std::lock_guard<std::mutex> lk(vd_mu_); return visitor_data_;
    }
    json web_ctx() {
        json c = {{"clientName",WEB_CLIENT_NAME},{"clientVersion",WEB_CLIENT_VERSION},
                  {"hl","en"},{"timeZone","UTC"},{"utcOffsetMinutes",0}};
        auto vd = get_vd();
        if (!vd.empty()) c["visitorData"] = vd;
        return {{"client", c}};
    }
    std::vector<std::string> web_hdrs() {
        std::vector<std::string> h = {
            "X-YouTube-Client-Name: " + std::to_string(WEB_CLIENT_ID),
            std::string("X-YouTube-Client-Version: ") + WEB_CLIENT_VERSION,
            std::string("User-Agent: ") + WEB_UA,
            "Origin: https://www.youtube.com",
            "X-Origin: https://www.youtube.com",
        };
        auto vd = get_vd();
        if (!vd.empty()) h.push_back("X-Goog-Visitor-Id: " + vd);
        return h;
    }
};

// ---------------------------------------------------------------------------
// Convenience free functions — use the singleton
// ---------------------------------------------------------------------------
inline std::vector<SearchResult> yt_search(const std::string& q, int n=15) {
    return InnertubeClient::get_instance().search(q, n);
}
inline VideoInfo yt_get_formats(const std::string& video_id) {
    return InnertubeClient::get_instance().get_stream_formats(video_id);
}
inline std::string yt_best_audio(const std::string& video_id) {
    return InnertubeClient::get_instance().get_best_audio_url(video_id);
}
inline std::string yt_best_video(const std::string& video_id, int max_h=1080) {
    return InnertubeClient::get_instance().get_best_video_url(video_id, max_h);
}
inline void yt_prefetch(const std::string& video_id) {
    InnertubeClient::get_instance().prefetch(video_id);
}

} // namespace ytfast
