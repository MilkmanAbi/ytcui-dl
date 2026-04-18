# ytcui-dl

A fast, native C++17 YouTube stream resolver and downloader. No Python, no yt-dlp subprocess, no JavaScript interpreter. Talks directly to YouTube's InnerTube API via libcurl.

---

## Why

`yt-dlp` is great but slow — Python startup alone costs 500ms–1s, and nsig cipher solving adds another 1–3s. On macOS and BSD the lag is even worse. ytcui-dl resolves stream URLs in **~65ms** by speaking InnerTube directly, with results cached so repeat access is **<1ms**.

| Operation | yt-dlp | ytcui-dl |
|-----------|--------|----------|
| Search (10 results) | 3–6s cold | ~450ms cold, ~80ms warm |
| Stream URL | 3–8s | ~65ms |
| Second click (cached) | full re-fetch | <1ms |
| Startup overhead | ~500ms Python | none |

---

## Install

### Linux (Ubuntu/Debian)
```bash
sudo apt install libcurl4-openssl-dev libssl-dev g++
make
sudo make install   # installs to /usr/local/bin/ytcui-dl
```

### macOS
```bash
brew install curl openssl
make
sudo make install
```

### FreeBSD
```bash
pkg install curl
make
sudo make install
```

### OpenBSD / NetBSD
```bash
pkg_add curl   # or pkgin install curl
make
sudo make install
```

### Custom install prefix
```bash
make PREFIX=/usr install        # installs to /usr/bin/ytcui-dl
make DESTDIR=/staging install   # for packaging
```

---

## CLI usage

### Print stream URL
```bash
# Best video (muxed, ≤1080p)
ytcui-dl -g 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Best audio only
ytcui-dl -g -f bestaudio 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Best video-only stream (no audio)
ytcui-dl -g -f bestvideo 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Up to 720p
ytcui-dl -g -f 'best[height<=720]' 'URL'

# Specific itag
ytcui-dl -g -f 140 'URL'
```

> **Shell quoting:** Always quote the URL. If using `best[height<=720]`, quote the format string too — bash expands `[...]` as a glob.

### Download
```bash
# Download best audio (webm/opus or m4a/aac, auto-named)
ytcui-dl -x 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Download best video ≤1080p (auto-named)
ytcui-dl --download 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Download with custom output path
ytcui-dl --download -o rickroll.mp4 'URL'

# Download audio with custom path
ytcui-dl -x -o rickroll.m4a 'URL'

# Download up to 720p
ytcui-dl --download -f 'best[height<=720]' -o clip720.mp4 'URL'

# Download specific format by itag
ytcui-dl --download -f 140 -o audio.m4a 'URL'
```

Downloads show a progress bar with percentage and MB when running in a terminal:
```
Title:    Never Gonna Give You Up (4K Remaster)
Format:   audio/webm; codecs="opus"  audio-only
Size:     ~3.3 MB
Output:   Never_Gonna_Give_You_Up.webm
Downloading...
  [100%] 3.3 / 3.3 MB
Done: Never_Gonna_Give_You_Up.webm (3.27 MB)
```

If a download is interrupted, re-running the same command resumes from where it stopped.

### Search
```bash
# Search, print JSON (10 results by default)
ytcui-dl --search 'lofi hip hop'

# Limit results
ytcui-dl --search 'lofi hip hop' 5

# Parse with jq
ytcui-dl --search 'lofi hip hop' 3 | jq '.[].title'

# Use in a shell pipeline
ytcui-dl --search 'never gonna give you up' 1 | \
    jq -r '.[0].id' | \
    xargs -I{} ytcui-dl -g -f bestaudio 'https://youtube.com/watch?v={}'
```

Search JSON output fields:
```json
{
  "id":             "dQw4w9WgXcQ",
  "title":          "Rick Astley - Never Gonna Give You Up",
  "channel":        "Rick Astley",
  "channel_id":     "UCuAXFkgsw1L7xaCfnd5JJOw",
  "duration":       "3:33",
  "duration_secs":  213,
  "view_count":     1763587372,
  "view_count_str": "1.76B",
  "upload_date":    "16 years ago",
  "thumbnail_url":  "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg",
  "url":            "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
  "is_live":        false
}
```

> **Note on `upload_date`:** The search API only returns a relative string like `"3 years ago"`. An ISO date (`"YYYY-MM-DD"`) is only available when microformat is present in the player response, which depends on the client and region.

### Video info
```bash
ytcui-dl --info 'https://youtube.com/watch?v=dQw4w9WgXcQ'
```
```json
{
  "id":            "dQw4w9WgXcQ",
  "title":         "Rick Astley - Never Gonna Give You Up",
  "channel":       "Rick Astley",
  "channel_id":    "UCuAXFkgsw1L7xaCfnd5JJOw",
  "description":   "The official video...",
  "duration":      "3:33",
  "duration_secs": 213,
  "view_count":    1763587372,
  "view_count_str":"1.76B",
  "thumbnail_url": "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg",
  "upload_date":   "",
  "url":           "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
  "is_live":       false,
  "format_count":  30
}
```

### List formats
```bash
ytcui-dl --formats 'https://youtube.com/watch?v=dQw4w9WgXcQ'
```
Returns a JSON array. Each entry:
```json
{
  "itag":          140,
  "mime_type":     "audio/mp4; codecs=\"mp4a.40.2\"",
  "quality":       "tiny",
  "width":         0,
  "height":        0,
  "bitrate":       130671,
  "has_video":     false,
  "has_audio":     true,
  "audio_codec":   "mp4a.40.2",
  "video_codec":   "",
  "content_length":2784154,
  "url":           "https://rr1---sn-..."
}
```

### Warm the visitor_data cache
```bash
# First-run bootstrap — fetches YouTube homepage to get real visitor_data token.
# Run once after install. Subsequent calls use the cached token.
ytcui-dl --bootstrap
```

---

## Format strings

| String | Description |
|--------|-------------|
| `bestaudio` / `ba` | Best audio-only stream (prefers opus/webm) |
| `bestvideo` / `bv` | Best video-only stream (no audio) |
| `best` | Best muxed video+audio stream |
| `best[height<=N]` | Best video up to N pixels tall (e.g. `best[height<=720]`) |
| `<itag>` | Specific format by itag number (e.g. `140`, `251`, `137`) |

---

## Library usage

Add `include/` to your include path, link with `-lcurl -lssl -lcrypto -lpthread`, and include `ytfast.h`.

### Quick start
```cpp
#include "ytfast.h"

// Global init — call once at program startup
ytfast::CurlGlobalInit curl_init;

// All calls use the process-wide singleton automatically
// (shared visitor_data and URL cache across your whole program)

// Search
auto results = ytfast::yt_search("lofi hip hop", 10);
for (auto& r : results) {
    printf("[%s] %s — %s (%s views)\n",
        r.id.c_str(), r.title.c_str(),
        r.channel.c_str(), r.view_count_str.c_str());
}

// Get all stream formats for a video
auto info = ytfast::yt_get_formats("dQw4w9WgXcQ");
printf("title: %s\n", info.title.c_str());
printf("formats: %zu\n", info.formats.size());

// Get best audio URL for downloading (opus/webm, has &range= for single HTTP response)
std::string dl_audio = ytfast::yt_best_audio("dQw4w9WgXcQ");

// Get best audio URL for streaming/mpv (aac/mp4, no range param)
std::string stream_audio = ytfast::InnertubeClient::select_best_streaming_audio(info.formats);

// Get best video URL for streaming/mpv (no range param)
std::string stream_video = ytfast::InnertubeClient::select_best_streaming_video(info.formats, 1080);

// Prefetch in background — call when user highlights a result
// so stream URLs are ready before they click play
ytfast::yt_prefetch("dQw4w9WgXcQ");
```

### Using with mpv

```cpp
// IMPORTANT: always pass --ytdl=no when giving mpv a direct CDN URL.
// Without it, mpv tries to run yt-dlp on the URL and exits if yt-dlp isn't installed.

std::string audio_url = ytfast::InnertubeClient::select_best_streaming_audio(info.formats);

// Build mpv command
std::vector<std::string> args = {
    "mpv",
    "--no-video",
    "--ytdl=no",          // REQUIRED — disable yt-dlp hook
    "--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
    "--cache=yes",
    "--demuxer-max-bytes=50M",
    audio_url
};
```

### URL types — download vs streaming

ytcui-dl exposes two URL variants per format:

| Field | Usage | Has `&range=`? |
|-------|-------|----------------|
| `StreamFormat::url` | Downloading (curl, wget) | Yes — forces single HTTP response for DASH streams |
| `StreamFormat::stream_url` | Streaming (mpv, ffplay) | No — raw CDN URL, mpv handles buffering |

Selector functions that return the right URL for each use case:

```cpp
// For downloading to disk:
std::string dl  = ytfast::InnertubeClient::select_best_audio(formats);   // opus/webm + range
std::string dl2 = ytfast::InnertubeClient::select_best_video(formats);   // best muxed + range

// For streaming with mpv/ffplay:
std::string str = ytfast::InnertubeClient::select_best_streaming_audio(formats); // aac/mp4, no range
std::string sv  = ytfast::InnertubeClient::select_best_streaming_video(formats); // no range
```

### Singleton vs instance

```cpp
// Singleton (recommended for most use cases)
// Shares visitor_data and URL cache across your whole program.
// If your search code and playback code are in different modules,
// they both benefit from the same warm session.
auto& yt = ytfast::InnertubeClient::get_instance();
auto results = yt.search("query", 15);
auto info    = yt.get_stream_formats("videoId");

// Direct instance (if you need isolation, e.g. multiple accounts)
ytfast::InnertubeClient yt2;
auto info2 = yt2.get_stream_formats("videoId");
```

### VideoInfo fields

```cpp
struct VideoInfo {
    std::string id;               // 11-char YouTube video ID
    std::string title;            // Full title, UTF-8, HTML entities decoded
    std::string channel;          // Channel display name
    std::string channel_id;       // Channel ID (UCxxxxxxxxxxxxxxxxxxxxxxxx)
    std::string description;      // Full description from player, snippet from search
    std::string thumbnail_url;    // maxresdefault.jpg (1280x720+)
    std::string upload_date;      // "YYYY-MM-DD" from microformat, or "N years ago" from search
    std::string duration_str;     // "H:MM:SS" or "M:SS"
    int         duration_secs;    // Total seconds
    int64_t     view_count;       // Raw view count
    std::string view_count_str;   // "1.76B", "119.06M", etc.
    bool        is_live;          // True for ongoing live streams
    std::string url;              // Full watch URL
    std::vector<StreamFormat> formats; // Empty unless from get_stream_formats()
};
```

### StreamFormat fields

```cpp
struct StreamFormat {
    int         itag;             // YouTube itag (140=aac128, 251=opus, 137=1080p, etc.)
    std::string url;              // Download URL — has &range=0-N for DASH streams
    std::string stream_url;       // Streaming URL — raw, no range param
    std::string mime_type;        // e.g. "audio/mp4; codecs=\"mp4a.40.2\""
    std::string quality;          // "hd1080", "hd720", "medium", "small", "tiny"
    int         width;            // 0 for audio-only
    int         height;           // 0 for audio-only
    int64_t     bitrate;          // bits per second
    int64_t     content_length;   // bytes (0 if unknown / live)
    bool        has_video;
    bool        has_audio;
    std::string audio_codec;      // e.g. "mp4a.40.2", "opus"
    std::string video_codec;      // e.g. "avc1.4d401f", "vp9"
    int         audio_sample_rate;
    int         audio_channels;
};
```

---

## How it works

ytcui-dl talks directly to YouTube's internal InnerTube API — the same JSON API the official YouTube apps use. It does not scrape HTML or run JavaScript.

### Client chain

YouTube requires a "client" context in every API request. Different clients return different formats and have different rate-limit profiles. ytcui-dl uses:

1. **`ANDROID` (id=3)** — Primary player client. `REQUIRE_JS_PLAYER=False` means stream URLs are returned unsigned and ready to use — no cipher/nsig solving at all. Returns up to 1080p on residential IPs.
2. **`IOS` (id=5)** — Fallback. Also `REQUIRE_JS_PLAYER=False`. Returns up to 1080p plus an HLS manifest.
3. **`ANDROID_VR` (id=28)** — Last resort. Very low bot suspicion. May be limited to lower resolutions depending on IP.

For search, the **`WEB` (id=1)** client is used — it returns the richest metadata including description snippets and channel IDs.

### Session bootstrap

On first use, ytcui-dl fetches `www.youtube.com` to extract a real `VISITOR_DATA` token from the `ytcfg.set({...})` blobs embedded in the page. This token is sent as `X-Goog-Visitor-Id` on all subsequent requests, which significantly reduces bot detection. The token is cached and refreshed every 12 hours.

If the homepage fetch fails (cloud IP, consent redirect), ytcui-dl falls back to extracting `visitorData` from the first API `responseContext` — which still works fine.

### URL cache

Resolved stream URLs are cached in memory with a 5-hour TTL (CDN URLs are valid for ~6 hours). Repeated calls for the same video ID hit the cache instantly.

### Async prefetch

`search()` spawns a background thread to pre-resolve stream URLs for the top result. By the time the user clicks, the formats are already cached.

### Translated from yt-dlp

The InnerTube logic is translated 1:1 from yt-dlp's Python source:

| ytcui-dl | yt-dlp source |
|---------|--------------|
| `INNERTUBE_CLIENTS` constants | `_base.py` `INNERTUBE_CLIENTS` dict |
| `player_request()` | `_video.py` `_extract_player_response()` |
| `search()` content path | `_tab.py` `_search_results()` content_keys |
| `_generate_player_context` | `_video.py` `_generate_player_context()` |
| `gen_cpn()` | `_video.py` line 2274 ("works even with dummy cpn") |
| `bootstrap_visitor_data()` | `_base.py` `_download_ytcfg()` + `extract_ytcfg()` |
| `extract_visitor_data()` | `_base.py` `_extract_visitor_data()` |

---

## Files

```
ytcui-dl/
├── include/
│   ├── ytfast.h              — public API (include only this)
│   ├── ytfast_types.h        — VideoInfo, StreamFormat, SearchResult structs
│   ├── ytfast_http.h         — libcurl HTTP client (thread-local, keep-alive, gzip)
│   ├── ytfast_innertube.h    — InnerTube client (all the logic)
│   └── nlohmann/json.hpp     — bundled JSON parser (single-header, MIT)
├── cli/
│   └── ytcui-dl.cpp          — CLI tool
├── test/
│   └── test_ytfast.cpp       — test suite
├── Makefile
└── README.md
```

---

## Build requirements

| Dependency | Version | Notes |
|------------|---------|-------|
| C++ compiler | C++17 | g++ ≥7 or clang++ ≥5 |
| libcurl | ≥7.28 | With SSL support |
| OpenSSL / LibreSSL | any | For TLS |
| pthreads | POSIX | For background prefetch thread |
| nlohmann/json | 3.x | Bundled in `include/nlohmann/` |

---

## Limitations

- **No age-restricted content** without cookies (cookie injection not yet implemented)
- **No private videos** (requires authentication)
- **No live stream downloading** (live streams have no `content_length`, range URLs are skipped)
- **`upload_date`** is a relative string (`"3 years ago"`) from search; ISO date only available when microformat is present in player response
- **Resolution cap** varies by IP type — residential IPs typically get 1080p from the android client; cloud/datacenter IPs may be capped lower

---

## License

MIT
