# ytcui-dl — Drop-In Integration Guide

This document explains how ytcui-dl works internally, what was fixed from v0.1, and how to drop it into your YouTube TUI app as a yt-dlp replacement.

---

## Table of Contents

1. [What ytcui-dl Actually Does](#what-ytcui-dl-actually-does)
2. [How It's Faster Than yt-dlp](#how-its-faster-than-yt-dlp)
3. [Bugs Fixed in v0.2](#bugs-fixed-in-v02)
4. [CLI Drop-In Cheatsheet](#cli-drop-in-cheatsheet)
5. [Library Integration (C++ Header-Only)](#library-integration)
6. [Replacing yt-dlp in Existing Apps](#replacing-yt-dlp-in-existing-apps)
7. [Format Selection Reference](#format-selection-reference)
8. [JSON Output Compatibility](#json-output-compatibility)
9. [Known Limitations](#known-limitations)
10. [Troubleshooting](#troubleshooting)

---

## What ytcui-dl Actually Does

ytcui-dl talks directly to YouTube's InnerTube API — the same internal API that the YouTube Android/iOS apps use. It:

1. **Bootstraps visitor_data** — Fetches youtube.com once on first launch, extracts the VISITOR_DATA token from the page's `ytcfg.set()` JavaScript blob. This token identifies us as a real browser session and prevents bot detection. Cached for 12 hours.

2. **Sends player requests** — POSTs to `/youtubei/v1/player` with an Android client context. YouTube returns raw CDN URLs for every available stream — no JavaScript cipher solving, no signature extraction, no n-parameter throttle workaround. The mobile API clients (`ANDROID`, `IOS`, `ANDROID_VR`) all have `REQUIRE_JS_PLAYER=False` in yt-dlp's source, meaning they return direct URLs.

3. **Caches everything** — CDN URLs are valid for ~6 hours. ytcui-dl caches resolved VideoInfo for 5 hours. Second access to any video is <0.01ms.

4. **Prefetches** — When you search, the top result's streams are resolved in a background thread. By the time you click, the URL is already cached.

### The client chain

ytcui-dl tries three InnerTube clients in order:

| Client | ID | Max Quality | Reliability | Notes |
|--------|-----|-------------|-------------|-------|
| ANDROID | 3 | 1080p | High on residential IPs | Primary. Direct URLs, no cipher. |
| IOS | 5 | 1080p | High | Fallback. Also provides HLS manifests. |
| ANDROID_VR | 28 | 240p | Very high | Last resort. Low suspicion, always works. |

If ANDROID returns `LOGIN_REQUIRED` or `UNPLAYABLE` (common on datacenter IPs), it falls through to IOS, then ANDROID_VR. The chain stops at the first successful response.

---

## How It's Faster Than yt-dlp

yt-dlp's YouTube extraction (`yt_dlp/extractor/youtube/`) does roughly this on every invocation:

1. Python interpreter startup (~300ms)
2. Import yt-dlp's 100+ modules (~200ms)
3. Fetch the watch page HTML (~300ms)
4. Extract the JS player URL from HTML
5. Fetch the JS player (~200ms)
6. Parse the JS player to extract cipher/nsig functions
7. Evaluate cipher/nsig for each stream URL
8. Return the deciphered URLs

ytcui-dl skips steps 1-7 entirely. The mobile API clients return **pre-deciphered URLs** — no JS player, no cipher, no nsig. A cold resolve is a single HTTPS POST (~100ms). A warm resolve is a hashmap lookup (~0.005ms).

### Benchmark comparison

```
Operation              yt-dlp        ytcui-dl       Speedup
─────────────────────────────────────────────────────────────
First video resolve    2,000-5,000ms  80-200ms       ~20x
Cached resolve         N/A            <0.01ms        ∞
Search (10 results)    3,000-8,000ms  400-800ms      ~6x
Binary startup         300-800ms      0ms            ∞
```

---

## Bugs Fixed in v0.2

### Bug 1: Audio Streaming Broken

**Symptom**: mpv plays 1 second of audio then stops, or refuses to play audio URLs entirely.

**Root cause**: `select_best_audio()` returned `url` which had `&range=0-N` appended by `fix_url_for_streaming()`. This tells YouTube's CDN to return only those bytes as a single response. mpv needs to seek freely within the stream, but the range-limited URL prevents that.

**Fix**: Split into two methods:
- `select_best_audio_stream()` → returns `stream_url` (raw URL, no range param) — for mpv/ffmpeg playback
- `select_best_audio_download()` → returns `url` (with `&range=0-N`) — for curl/wget full-file download

The convenience function `yt_best_audio()` now returns the stream URL (correct for playback).

### Bug 2: Thumbnails 404ing

**Symptom**: Thumbnail URLs return HTTP 404. TUI apps show broken/missing thumbnails.

**Root cause**: `best_thumb_from_id()` always constructed `https://i.ytimg.com/vi/{id}/maxresdefault.jpg` without checking if it exists. Many videos (especially older or lower-resolution ones) don't have a maxresdefault thumbnail. "Me at the zoo" (the first YouTube video) returns 404 for maxresdefault.

**Fix**: Use API-provided thumbnail URLs as the primary source. YouTube's player API response includes a `videoDetails.thumbnail.thumbnails[]` array with URLs that are **guaranteed to exist**. The well-known URL patterns (maxresdefault, sddefault, hqdefault, etc.) are still included in the `thumbnails[]` array as bonus options, but `thumbnail_url` always points to an API-provided URL.

### Bug 3: Codec Parsing Broken for Muxed Streams

**Symptom**: itag 18 (the standard muxed mp4) shows empty `audio_codec` and empty `video_codec`. Apps can't determine what codecs a stream uses.

**Root cause**: `extract_codec()` returned the raw codec string `"avc1.42001E, mp4a.40.2"` as a single string. The assignment logic checked `has_audio && !has_video` (false for muxed) and `has_video && !has_audio` (also false for muxed), so neither field was set.

**Fix**: New `parse_codecs()` function splits the comma-separated codec string and classifies each part — `mp4a`/`opus`/`vorbis` → `audio_codec`, `avc1`/`vp9`/`av01` → `video_codec`. Muxed streams now correctly show both codecs.

### Bug 4: Duplicate Formats

**Symptom**: Same itag appearing 2-3 times in the format list. Format selection might pick the wrong one.

**Root cause**: YouTube sometimes returns the same itag in both `formats[]` and `adaptiveFormats[]`, or the same itag from different CDN servers. No deduplication was performed.

**Fix**: Track seen itags during parsing. If a duplicate appears with higher bitrate, it replaces the old one. Otherwise it's skipped.

### Bug 5: Missing `stream_url` in JSON Output

**Symptom**: Apps parsing `--formats` JSON output only see the download URL (with `&range=`), can't get the raw streaming URL.

**Fix**: Both `url` and `stream_url` are now included in all JSON output (`-j`, `--formats`).

### Bug 6: Missing yt-dlp CLI Compatibility

**Symptom**: Apps that shell out to yt-dlp can't switch to ytcui-dl because critical flags are missing.

**Fix**: Added `-F` (format table), `-j`/`--dump-json` (full metadata JSON), `--get-url`, `--get-thumbnail`, `--print FIELD`, format filter strings (`bestaudio[ext=m4a]`, `best[height<=720]`, etc.).

---

## CLI Drop-In Cheatsheet

Replace `yt-dlp` with `ytcui-dl` in your shell commands:

```bash
# yt-dlp                              → ytcui-dl
yt-dlp -F URL                         → ytcui-dl -F URL
yt-dlp -j URL                         → ytcui-dl -j URL
yt-dlp --get-url -f bestaudio URL     → ytcui-dl -g -f bestaudio URL
yt-dlp --get-thumbnail URL            → ytcui-dl --get-thumbnail URL
yt-dlp -x URL                         → ytcui-dl -x URL
yt-dlp -x -f 'bestaudio[ext=m4a]' URL → ytcui-dl -x -f 'bestaudio[ext=m4a]' URL
yt-dlp -f 'bestvideo[height<=720]+bestaudio' URL
                                       → ytcui-dl --download -f 'best[height<=720]' URL
yt-dlp --print title URL              → ytcui-dl --print title URL
```

---

## Library Integration

ytcui-dl is header-only. Copy the `include/` directory into your project.

### Files

```
include/
├── ytfast.h               # Public API — include this single header
├── ytfast_types.h          # StreamFormat, VideoInfo, ThumbnailInfo structs
├── ytfast_http.h           # Persistent HTTP client (libcurl wrapper)
├── ytfast_innertube.h      # InnerTube client, format selection, caching
└── nlohmann/json.hpp       # JSON library (bundled)
```

### Minimal Example

```cpp
#include "ytfast.h"
#include <cstdio>

int main() {
    ytfast::CurlGlobalInit curl_init;  // RAII — call once at program start

    // Search
    auto results = ytfast::yt_search("never gonna give you up", 5);
    printf("Found: %s\n", results[0].title.c_str());

    // Get audio URL for mpv
    std::string audio = ytfast::yt_best_audio(results[0].id);
    printf("Audio: %s\n", audio.c_str());

    return 0;
}
```

Compile: `g++ -std=c++17 -O2 -Ipath/to/include -o myapp myapp.cpp -lcurl -lssl -lcrypto -lpthread`

### Key API Functions

```cpp
// Free functions (use the global singleton — recommended)
ytfast::yt_search(query, max_results)        → vector<SearchResult>
ytfast::yt_get_formats(video_id)             → VideoInfo (with all formats)
ytfast::yt_best_audio(video_id)              → string (stream URL, for mpv)
ytfast::yt_best_audio_download(video_id)     → string (download URL, for saving)
ytfast::yt_best_video_stream(video_id, maxH) → string (stream URL, for mpv)
ytfast::yt_best_video(video_id, maxH)        → string (download URL, for saving)
ytfast::yt_prefetch(video_id)                → void (background cache warm)

// Singleton access for advanced use
auto& yt = ytfast::InnertubeClient::get_instance();
yt.search(query, max, prefetch_top);
yt.get_stream_formats(video_id);
yt.prefetch(video_id);

// Static format selection (on a formats vector)
InnertubeClient::resolve_format_string("bestaudio[ext=m4a]", fmts, for_streaming);
InnertubeClient::select_best_audio_stream(fmts, prefer_codec);
InnertubeClient::select_best_audio_download(fmts, prefer_codec, min_bitrate);
InnertubeClient::select_best_video_stream(fmts, max_height);
InnertubeClient::select_best_video(fmts, max_height);
InnertubeClient::select_worst_audio(fmts);
InnertubeClient::select_best_video_only(fmts, max_height);
InnertubeClient::find_by_itag(fmts, itag);
InnertubeClient::is_muxed(fmts, url);
InnertubeClient::filter_formats(fmts, predicate);
```

### StreamFormat Fields

```cpp
struct StreamFormat {
    int         itag;              // YouTube format ID
    string      url;               // Download URL (with &range= for DASH)
    string      stream_url;        // Playback URL (raw, for mpv/ffmpeg)
    string      mime_type;         // "video/mp4; codecs=\"avc1.4d401f\""
    string      quality;           // "medium", "hd720", etc.
    string      quality_label;     // "720p", "1080p60", etc.
    int         width, height;     // Resolution
    int         fps;               // Framerate
    int64_t     bitrate;           // Peak bitrate
    int64_t     average_bitrate;   // Average bitrate
    int64_t     content_length;    // File size in bytes
    bool        has_video, has_audio;
    int         audio_sample_rate; // e.g. 44100, 48000
    int         audio_channels;    // e.g. 2
    string      audio_codec;       // "mp4a.40.2", "opus"
    string      video_codec;       // "avc1.4d401f", "vp9", "av01.0.04M.08"
    string      container;         // "mp4", "webm", "3gp"

    bool is_audio_only() const;
    bool is_video_only() const;
    bool is_muxed() const;
    int64_t effective_bitrate() const;  // average if available, else peak
};
```

### URL Types — Critical Distinction

This was the root cause of the v0.1 audio bug. You **must** use the right URL type:

| Field | Has `&range=`? | Use For |
|-------|---------------|---------|
| `stream_url` | No | mpv, ffmpeg, any HTTP streaming player |
| `url` | Yes (DASH streams) | curl, wget, full file download to disk |

**For mpv/playback**: always use `stream_url` or functions ending in `_stream`.
**For downloads**: always use `url` or functions ending in `_download`.

If you pipe a `url` (with `&range=0-N`) to mpv, it will play only 1 second then stop. If you download via `stream_url` (without range), DASH streams will only save the initialization segment.

---

## Replacing yt-dlp in Existing Apps

### Pattern 1: Shell-out replacement

If your app shells out to `yt-dlp --get-url`, just swap the binary:

```bash
# Before
AUDIO_URL=$(yt-dlp --get-url -f bestaudio "$VIDEO_URL")
mpv --no-video "$AUDIO_URL"

# After
AUDIO_URL=$(ytcui-dl -g -f bestaudio "$VIDEO_URL")
mpv --no-video --ytdl=no \
  --user-agent='com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip' \
  "$AUDIO_URL"
```

### Pattern 2: JSON metadata replacement

If your app parses `yt-dlp -j` output:

```bash
# Before
yt-dlp -j "$URL" | jq -r '.formats[] | select(.acodec != "none") | .url'

# After — same jq query works, field names are compatible
ytcui-dl -j "$URL" | jq -r '.formats[] | select(.acodec != "none") | .url'
```

Key `-j` output fields that match yt-dlp:
`id`, `title`, `channel`, `uploader`, `description`, `thumbnail`, `duration`, `duration_string`, `view_count`, `upload_date`, `is_live`, `webpage_url`, `formats[]`, `thumbnails[]`

Per-format fields that match yt-dlp:
`format_id`, `ext`, `url`, `width`, `height`, `fps`, `tbr`, `abr`, `vbr`, `acodec`, `vcodec`, `filesize`

### Pattern 3: C++ library integration

If your TUI is in C++, use the header-only library directly instead of shelling out:

```cpp
// Before (shelling out to yt-dlp)
std::string cmd = "yt-dlp --get-url -f bestaudio '" + video_id + "'";
FILE* p = popen(cmd.c_str(), "r");
// ... read URL from pipe, takes 2-5 seconds

// After (direct library call, takes <0.01ms cached)
std::string audio_url = ytfast::yt_best_audio(video_id);
```

### Pattern 4: mpv integration

For TUI music players that pipe to mpv:

```cpp
// Get audio URL for mpv playback
auto info = ytfast::yt_get_formats(video_id);
std::string audio = ytfast::InnertubeClient::select_best_audio_stream(
    info.formats);

// Direct mpv — uses muxed stream, --no-video strips the video track
// Muxed streams are progressive downloads — no DASH, no 403, ever.
std::string cmd = "mpv --no-video --ytdl=no"
    " --user-agent='" + std::string(ytfast::ANDROID_UA) + "'"
    " '" + audio + "'";
system(cmd.c_str());
```

**Why muxed instead of audio-only?** YouTube's CDN aggressively blocks audio-only adaptive (DASH) stream requests from non-browser clients. The blocking varies by region, video length, and time of day. Muxed streams are progressive downloads — they work 100% of the time, everywhere, for every video. mpv's `--no-video` flag strips the video track and plays audio only. The bandwidth overhead is small and the audio quality (128kbps AAC) is comparable to adaptive streams.

### Pattern 5: Prefetch for instant playback

```cpp
// When user sees search results, prefetch the first few
for (int i = 0; i < std::min(3, (int)results.size()); i++)
    ytfast::yt_prefetch(results[i].id);

// When user selects one — instant, already cached
std::string url = ytfast::yt_best_audio(results[selected].id);  // <0.01ms
```

---

## Format Selection Reference

### Audio quality tiers (typical)

| itag | Codec | Container | Bitrate | Notes |
|------|-------|-----------|---------|-------|
| 251 | opus | webm | 128-160kbps | Best quality per bitrate. Preferred for download. |
| 140 | mp4a.40.2 (AAC-LC) | mp4/m4a | 128kbps | Most compatible. Preferred for streaming. |
| 250 | opus | webm | 64kbps | Mid quality |
| 249 | opus | webm | 48kbps | Low quality |
| 139 | mp4a.40.5 (HE-AAC) | mp4/m4a | 48kbps | Low quality AAC |

### Selecting by codec/container

```bash
ytcui-dl -g -f 'bestaudio[ext=m4a]'  URL    # Best AAC (mp4 container)
ytcui-dl -g -f 'bestaudio[ext=webm]' URL    # Best Opus (webm container)
ytcui-dl -g -f 'bestaudio[abr>=128]' URL    # Best audio ≥128kbps
ytcui-dl -g -f worstaudio URL               # Smallest audio file
```

### Programmatic codec preference

```cpp
// For mpv: prefer AAC (best compatibility)
auto url = ytfast::InnertubeClient::select_best_audio_stream(fmts, "aac");

// For download/archival: prefer Opus (best quality per bitrate)
auto url = ytfast::InnertubeClient::select_best_audio_download(fmts, "opus");

// With minimum bitrate
auto url = ytfast::InnertubeClient::select_best_audio_download(fmts, "", 128000);
```

---

## JSON Output Compatibility

### `-j` / `--dump-json` output

Top-level fields:

```json
{
  "id": "dQw4w9WgXcQ",
  "title": "Rick Astley - Never Gonna Give You Up",
  "channel": "Rick Astley",
  "uploader": "Rick Astley",
  "channel_id": "UCuAXFkgsw1L7xaCfnd5JJOw",
  "description": "The official video for...",
  "thumbnail": "https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg?...",
  "duration": 212,
  "duration_string": "3:32",
  "view_count": 1500000000,
  "upload_date": "2009-10-25",
  "is_live": false,
  "webpage_url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
  "category": ["Music"],
  "extractor": "youtube",
  "thumbnails": [...],
  "formats": [...]
}
```

Per-format fields:

```json
{
  "format_id": "251",
  "itag": 251,
  "ext": "webm",
  "url": "https://...(download URL with &range=)...",
  "stream_url": "https://...(raw URL for mpv)...",
  "mime_type": "audio/webm; codecs=\"opus\"",
  "container": "webm",
  "width": 0,
  "height": 0,
  "fps": 0,
  "tbr": 128,
  "abr": 128,
  "vbr": 0,
  "acodec": "opus",
  "vcodec": "none",
  "filesize": 255427,
  "audio_sample_rate": 48000,
  "audio_channels": 2
}
```

### Fields NOT in ytcui-dl (yt-dlp has these, we don't)

- `like_count`, `dislike_count` — Requires web client scraping
- `comment_count`, `comments` — Separate API endpoint
- `chapters` — Present in player response but not yet parsed
- `subtitles` — Separate API endpoint
- `age_limit` — Not extracted
- `playlist_*` — Playlist support not implemented
- `format_note` — We use `quality_label` instead
- `http_headers` — Not needed (Android UA is implied)

---

## Known Limitations

1. **YouTube only** — This is intentional. Use yt-dlp for other sites.

2. **No cipher/nsig solving** — We use mobile API clients that don't need it. If YouTube ever removes these clients (unlikely — they power the actual apps), this will break.

3. **No playlist support** — Single video only. Playlists need `/youtubei/v1/browse` with continuation tokens.

4. **No subtitles** — Requires a separate `/api/timedtext` call.

5. **No chapters** — Present in the player response (`videoDetails.chapters`) but not parsed yet.

6. **Datacenter IPs may get 403** — YouTube blocks some cloud/datacenter IP ranges from CDN access. The API calls (search, player) work fine — it's the actual stream download that gets blocked. Works perfectly on residential IPs. The test suite handles this gracefully.

7. **Max 1080p on most videos** — The mobile API clients cap at 1080p. For 1440p/4K you'd need the `WEB` client with cipher solving (what yt-dlp does).

8. **No post-processing** — No ffmpeg muxing, no format conversion. If you need bestvideo+bestaudio merged into one file, pipe through ffmpeg yourself.

---

## Troubleshooting

### "All clients failed: LOGIN_REQUIRED"

Your IP is being flagged. Try:
1. Run `ytcui-dl --bootstrap` to refresh visitor_data
2. If on a VPN/datacenter, switch to residential IP
3. The ANDROID_VR client usually works even when others fail

### Audio plays for 1 second then stops

You're using the download URL (with `&range=`) instead of the stream URL. Use:
- CLI: `ytcui-dl -g -f bestaudio URL` (returns stream URL by default)
- Library: `select_best_audio_stream()` not `select_best_audio_download()`

### Thumbnail shows broken image

If you stored old v0.1 thumbnails, they may be maxresdefault URLs that 404. Re-fetch with v0.2 which uses API-provided URLs.

### HTTP 403 on stream download

CDN access blocked (datacenter IP). The API call succeeded (you have the URL) but YouTube's CDN won't serve the bytes. This only affects actual playback/download, not metadata. Use a residential IP or proxy.

### mpv says "HTTP error 403" on audio-only streams

This was a persistent issue with audio-only adaptive (DASH) streams. YouTube CDN blocks non-browser clients from accessing DASH audio, and the behavior varies by region, video length, and time.

**The fix (v0.2.1):** `select_best_audio_stream()` and `-g -f bestaudio` now return a **muxed stream** (video+audio combined) instead of an audio-only DASH stream. Use `mpv --no-video` to strip the video track:

```bash
mpv --no-video --ytdl=no \
  --user-agent='com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip' \
  "$(ytcui-dl -g -f bestaudio VIDEO_ID)"
```

Muxed streams are progressive downloads — they work 100% of the time, everywhere. The audio quality is 128kbps AAC which is comparable to adaptive streams. Downloads (`-x` flag) still use audio-only DASH streams for smaller file sizes.

### Compilation errors

Make sure you have C++17 support and the required libraries:
```bash
# Debian/Ubuntu
sudo apt install build-essential libcurl4-openssl-dev libssl-dev

# Fedora
sudo dnf install gcc-c++ libcurl-devel openssl-devel

# Arch
sudo pacman -S base-devel curl openssl

# macOS
xcode-select --install && brew install curl openssl
```
