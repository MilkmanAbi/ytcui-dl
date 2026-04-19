# ytcui-dl

**Blazing-fast YouTube stream resolver.** Drop-in yt-dlp alternative targeting YouTube specifically.

ytcui-dl resolves YouTube video/audio stream URLs in **~100ms** (cached: **<0.01ms**) vs yt-dlp's typical 2-5 seconds. Built as a C++17 header-only library + CLI, designed for YouTube TUI apps, music players, and anything that needs instant stream resolution.

## Why not yt-dlp?

yt-dlp is incredible software. But it's a 200+ site generic extractor written in Python. If your app only targets YouTube:

| | yt-dlp | ytcui-dl |
|---|---|---|
| First resolve | 2-5s | ~100ms |
| Cached resolve | N/A | <0.01ms |
| Binary size | ~40MB (Python + deps) | ~450KB |
| Dependencies | Python 3.8+, ffmpeg | libcurl, openssl |
| Startup time | 300-800ms (Python import) | 0ms (native binary) |
| YouTube-specific optimizations | No | Yes (client chain, visitor_data, prefetch) |

## Quick Start

```bash
# Build
make

# Get best audio stream URL (for piping to mpv)
ytcui-dl -g -f bestaudio 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Download best audio
ytcui-dl -x 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Download best video
ytcui-dl --download 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# List all formats (like yt-dlp -F)
ytcui-dl -F 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Full metadata JSON (like yt-dlp -j)
ytcui-dl -j 'https://youtube.com/watch?v=dQw4w9WgXcQ'

# Search YouTube
ytcui-dl --search 'lofi hip hop' 10
```

## Build

Requires: C++17 compiler, libcurl, openssl. Works on Linux, macOS, FreeBSD, OpenBSD, NetBSD.

```bash
# Linux (Debian/Ubuntu)
sudo apt install build-essential libcurl4-openssl-dev libssl-dev

# macOS
brew install curl openssl

# Build
make

# Install system-wide
sudo make install

# Run tests
make test
```

## CLI Reference

### Stream URL Resolution

```bash
ytcui-dl -g [-f FORMAT] URL          # Print resolved stream URL
ytcui-dl --get-url [-f FORMAT] URL   # yt-dlp compat alias
```

### Download

```bash
ytcui-dl -x URL                      # Download best audio (opus/m4a)
ytcui-dl -x -f 'bestaudio[ext=m4a]' URL  # Download best AAC audio
ytcui-dl --download URL              # Download best video+audio ≤1080p
ytcui-dl --download -f 'best[height<=720]' -o clip.mp4 URL
```

### Metadata

```bash
ytcui-dl -j URL                      # Full JSON metadata (yt-dlp -j compat)
ytcui-dl -F URL                      # Human-readable format table
ytcui-dl --formats URL               # All formats as JSON
ytcui-dl --get-thumbnail URL         # Print best thumbnail URL
ytcui-dl --print FIELD URL           # Print specific field
ytcui-dl --search QUERY [N]          # Search YouTube, JSON output
ytcui-dl --info URL                  # Video info as JSON
```

### Format Strings (yt-dlp compatible)

```
bestaudio / ba              Best audio-only stream (opus preferred for download)
bestaudio[ext=m4a]          Best AAC audio in mp4 container
bestaudio[ext=webm]         Best opus audio in webm container
bestaudio[abr>=128]         Best audio ≥128kbps
worstaudio / wa             Worst audio (smallest file)
bestvideo / bv              Best video-only adaptive stream
bestvideo[height<=720]      Best video up to 720p
best / b                    Best muxed video+audio (default)
best[height<=720]           Best muxed up to 720p
worst / w                   Worst quality muxed
<itag>                      Specific format by itag number
```

### Printable Fields

`id`, `title`, `channel`, `channel_id`, `description`, `thumbnail`, `url`, `duration`, `duration_secs`, `view_count`, `upload_date`, `category`, `is_live`

## Library Usage (Header-Only)

```cpp
#include "ytfast.h"

int main() {
    ytfast::CurlGlobalInit curl_init;  // once at startup

    // Search
    auto results = ytfast::yt_search("lofi hip hop", 10);

    // Get stream URLs (for mpv/playback — no &range= param)
    std::string audio = ytfast::yt_best_audio(results[0].id);
    std::string video = ytfast::yt_best_video_stream(results[0].id);

    // Get download URLs (with &range= for full file download)
    std::string audio_dl = ytfast::yt_best_audio_download(results[0].id);
    std::string video_dl = ytfast::yt_best_video(results[0].id, 1080);

    // Full format access
    auto info = ytfast::yt_get_formats(results[0].id);
    for (auto& f : info.formats) {
        printf("itag=%d %s %dp %lldkbps\n",
            f.itag, f.container.c_str(), f.height, f.bitrate/1000);
    }

    // Format selection with filters
    std::string url = ytfast::InnertubeClient::resolve_format_string(
        "bestaudio[ext=m4a]", info.formats, true /*for_streaming*/);

    // Prefetch (background thread, warms cache before user clicks)
    ytfast::yt_prefetch(results[1].id);
}
```

Link with: `-lcurl -lssl -lcrypto -lpthread`

## Architecture

- **Client chain**: android → ios → android_vr. No JS player/cipher solving needed.
- **Visitor data bootstrap**: Extracts real VISITOR_DATA from youtube.com on first use, eliminating bot detection.
- **5-hour URL cache**: CDN URLs valid ~6h, cached to make repeated access instant.
- **Async prefetch**: Search auto-prefetches top result's streams in a background thread.
- **Singleton pattern**: All callers share visitor_data + URL cache (search warms cache, play is instant).
- **Thread-safe**: Thread-local HTTP handles, mutex-protected shared state.

## URL Types

ytcui-dl distinguishes between two URL types for every stream:

- **`stream_url`** — Raw CDN URL, no `&range=` param. Use for mpv/ffmpeg streaming playback.
- **`url`** — Download URL with `&range=0-N` appended. Use for curl/wget full-file download.

The `-g` flag returns stream URLs (for piping to players). The `--download`/`-x` flags use download URLs internally.

## License

MIT
