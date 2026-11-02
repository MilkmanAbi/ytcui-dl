## What's new in this branch

Testing against live YouTube turned up a handful of real breaks. Worth saying
plainly first: **the main VOD path was not one of them.** Search, `-L`, `-g`,
progressive downloads, muxing, `-x` and the yt-dlp compat shim all worked on a
fresh clone, and `--diag` passed. These did not.

### Live streams never worked

`streamingData.hlsManifestUrl` was not read anywhere. A live response *does*
carry adaptive formats with URLs, which is why this looked fine — but none of
them has a `contentLength`, so the downloader computed a zero-length span,
wrote **exactly one byte, and reported success.**

Live now resolves to the HLS manifest. `-p` and `-g` hand it straight to mpv,
which reads HLS natively, and `-d` refuses with the `ffmpeg` command to record
it yourself. Verified by decoding 60 s straight through a live manifest.

### `ANDROID_VR` was second in the chain and serves nothing

Its player response looks perfect — 26 adaptive formats, every one carrying a
URL — and the CDN 403s the end of all of them. Upstream dates the break to
2026-08-17. Sitting second in `PLAYER_CHAIN`, it was answering for precisely
the videos `VISIONOS` declines, and serving none of them. It is now last.

### `--diag` called byte-capped clients `[ ok ]`

It probed only the *start* of a stream per client, which passes whether or not
a cap exists, because the cap only bites once you reach it — the exact silent
failure that mode exists to surface. Each client now gets a real fetch near the
end of its own audio URL and reads `CAPPED: 403 near end of stream`.

### HLS-only responses died at selection

A client can answer SABR for every progressive format and still return a usable
manifest (current `IOS` versions do). That response reached
`Selector::select()` with an empty format list and failed with *"nothing
matches that request"*. It is now routed like live.

### New flags and fields

| | |
|---|---|
| `--hls` | force the HLS manifest for `-p` and `-g` |
| `--no-hls` | never fall back to HLS (live always uses it — there is no alternative) |
| `is_live_now`, `hls_manifest_url` | added to `--json` |

`--play` also switches to HLS on its own when the resolving client is
byte-capped and a manifest exists, and warns up front — before spending time
on a download — when a capped client resolved something longer than the cap.

The yt-dlp compat shim still serves metadata for live streams (`-j`, `-e`,
`--print`, `--get-*`) and refuses only the download.

### `ytcui-we`, and a SQLite reader to go with it

A second binary that gets a logged-in session into `ytcui-dl` without embedding
a browser — see [ytcui-we](#ytcui-we-signing-in-without-shipping-a-browser).
Reading a browser's cookie store means reading SQLite, so `include/yt_sqlite.h`
is a ~340-line read-only B-tree reader. `libsqlite3` would have broken this
project's one real property, and the `sqlite3` CLI is not installed by default
(confirmed absent on a stock Ubuntu 24.04), so leaning on it would have meant
the feature silently did not exist for most people.

### Documentation corrected against measurement

Several things this README used to claim or imply were wrong:

- `--pot-provider` **cannot** lift the ANDROID/IOS byte cap. It mints *web*
  BotGuard tokens, and tokens do not cross platforms.
- **"Made for kids" videos cannot be fixed tokenlessly**, at all.
- The web-family HLS route — the obvious-looking escape — is a **measured dead
  end**, written up so it does not get re-attempted.

### Two things to know

- **`make test-live` has a pre-existing flaky check**, unrelated to these
  changes: `already-complete file returns immediately` asserts
  `r4.seconds < 1.0`, but `Downloader::fetch` makes a network `probe_size()`
  call *before* the already-complete early exit, so the bound is
  latency-dependent. `yt_download.h` is untouched here.
- **`ytcui-we`'s extraction has not yet been run against a real installed
  browser profile.** It is covered by tests against a real SQLite file with
  real AES-`v10` ciphertext, and the launch path by command construction, but
  the first run on an actual Firefox or Chrome profile is still unproven.

---

## Quick start

```sh
make               # builds ytcui-dl and ytcui-we
make diag          # what works, what doesn't, and why
make test          # offline suite
make install       # PREFIX=/usr/local by default
```

```sh
ytcui-dl -a -g <url>                   # audio stream URL
ytcui-dl -p -q 720 <url>               # play at 720p
ytcui-dl -a -p <url>                   # play as audio only
ytcui-dl -d -q 1080 --vcodec avc1 <url>
ytcui-dl -a -d -o track.m4a <url>
ytcui-dl -x <url>                      # audio, straight to .mp3
ytcui-dl -d -q 1080 --remux <url>      # video, straight to .mp4
ytcui-dl -L <url>                      # what qualities exist
ytcui-dl -p <live-url>                 # live, via HLS
ytcui-dl -s "query" --json
```

Build dependencies: OpenSSL, zlib, pthreads. Optional at runtime: `mpv` for
`--play`, `ffmpeg` for muxing and for `-x`/`--remux`.

Platforms: Linux, macOS, FreeBSD, OpenBSD, NetBSD. POSIX throughout — there is
no Windows build.

---

## Downloading

Two paths, picked by which flag you reach for.

**`-d` / `--download`** — this project's own engine (`Downloader::fetch`,
`yt_download.h`): parallel ranged connections, automatic resume, and it hands
back whatever format YouTube actually served (raw `.m4a`/`.opus`/`.webm` for
audio-only; muxed via `ffmpeg` for video+audio if it is on `$PATH`). Reach for
this when you want a specific resolution, codec or itag, or the file is big
enough that resume matters.

**`-x` / `--extract-audio`** and **`--remux`** — a second path
(`StreamDownloader::fetch`, `yt_stream_dl.h`) for when you just want one
finished ordinary file: `-x` always produces a `.mp3` (transcoded, since
YouTube never serves MP3 source), `--remux` always produces a `.mp4` (stream
copy where the codec allows, transcoded automatically the rare times it
doesn't).

```sh
# -d: keeps the source format(s)
ytcui-dl -d <url>                          # best video+audio, muxed via ffmpeg
ytcui-dl -a -d <url>                       # audio only, raw
ytcui-dl -V -d -q 1080 <url>               # video only, capped at 1080p
ytcui-dl -d -q 1080 --vcodec avc1 <url>    # 1080p, H.264 specifically
ytcui-dl -d --itag 251 <url>               # one exact format, no selection logic
ytcui-dl -d -c 1 <url>                     # single connection (default 3)
ytcui-dl -d <url>                          # re-run on a partial file: resumes

# -x / --remux: ffmpeg finishes the job
ytcui-dl -x <url>                          # audio -> .mp3
ytcui-dl -d --remux <url>                  # video+audio -> .mp4
ytcui-dl -V -d --remux -q 720 <url>        # video only -> .mp4, capped at 720p
```

Quality flags work identically for both paths — they decide *what* gets
fetched; `-d` versus `-x`/`--remux` only decides what happens to it afterwards.
Full list: `ytcui-dl --help`.

**Why `-x` doesn't just hand the URL to ffmpeg.** ffmpeg's http protocol holds
one continuous connection for the whole input, and past an initial burst this
CDN throttles a sustained single-connection read to roughly realtime. A
634-second video came down at ~1.5x through ffmpeg directly versus ~180x
through this project's chunked engine on the same file. So `StreamDownloader`
pulls the source to a temp file first and only ever hands ffmpeg local paths.

**Why parallelism is not a micro-optimisation.** YouTube rate-limits an
individual connection well below what a link can carry, so a single-stream
download is throttle-bound rather than bandwidth-bound. Measured 2.84x on a
4.4 MB file, with byte-identical output. Workers `pwrite()` into their own
regions of one preallocated file, so there is no locking and no shared offset.
A server that ignores `Range`, or a response with no `Content-Length`, falls
back to a single sequential stream.

---

## Live streams and HLS

A live stream has no file behind it, so it is only ever played, never
downloaded:

```sh
ytcui-dl -p <live-url>      # plays, via streamingData.hlsManifestUrl
ytcui-dl -g <live-url>      # prints the manifest URL
ytcui-dl -d <live-url>      # refuses, and tells you the ffmpeg command
```

The same manifest is also the way past a byte cap *where one is offered*: it is
not subject to the ~60 s GVS cap, verified at the start, the middle, and 18 s
from the end of a 3:33 VOD, where that client's progressive URLs 403. `--play`
uses it automatically when the resolving client is capped; `--hls` forces it,
`--no-hls` forbids it.

`-q` does not apply on a manifest. mpv picks the variant itself and its only
lever is `--hls-bitrate` (`no`/`min`/`max`/a rate), which selects by bitrate
rather than height; mapping a height onto a `BANDWIDTH` would mean fetching and
parsing the master playlist, which this does not do. Ignored by design rather
than half-honoured.

---

## ytcui-we: signing in, without shipping a browser

```sh
ytcui-we login          # opens your default browser at YouTube's sign-in page
ytcui-we export         # writes ~/.config/ytcui-dl/cookies.txt, mode 0600
ytcui-we check          # confirms the session actually resolves a video
ytcui-we list           # which browser profiles it can see
ytcui-we play <url>     # hand off to ytcui-dl -p with the session attached
```

**What a session buys is narrower than it looks.**

| | |
|---|---|
| It **unlocks** | age-gated, members-only and private videos, plus a logged-in session for search and the bootstrap |
| It does **not** fix | "made for kids" videos, or the ~60 s byte cap on `ANDROID`/`IOS` media URLs |

Both of those need a GVS PO Token, and a browser session is not one. See
[the dead-end note](#known-constraints) for how thoroughly that was checked.

**No browser is embedded, on purpose.** The first design here was a minimal
WebKitGTK window, and it was wrong twice over: it needs ~50 development
packages to build, so it cannot be set up unattended, and Google routinely
refuses sign-in from an unrecognised embedded engine ("this browser or app may
not be secure"). The browser you already use has neither problem — it is
installed, it is trusted by Google, and it is where you are already signed in.
So `ytcui-we` only opens it, and afterwards reads the result.

- **Launch** honours `$BROWSER` first, including the `%s` placeholder
  convention, and skips an entry naming something that isn't installed. Then
  `xdg-open`, `gio`, `x-www-browser`, `sensible-browser` on Linux/BSD, and
  `open` on macOS. `ytcui-we login --print-command` shows what it would run
  without running it.
- **Cookie stores read:** Firefox, where values are plaintext, and the Chromium
  family — Chrome, Chromium, Brave, Edge, Vivaldi, Opera — where they are
  AES-128-CBC encrypted. Flatpak and snap profile locations are searched too.
  The key comes from the platform: the Keychain on macOS (via `security`),
  `secret-tool` or Chromium's documented fallback password on Linux. Chrome
  130+ prepends a 32-byte SHA-256 of the cookie's domain to the plaintext with
  no version bump to detect it by, so that is detected by content and stripped.
- **Safari is not supported**, deliberately: its store is a different binary
  format (`Cookies.binarycookies`), not SQLite, and it is sandboxed under TCC.
  Sign in with Firefox or a Chromium-family browser instead.

**The jar is a credential.** It is written `0600` inside a `0700` directory,
and anyone holding it is signed in as you. A throwaway account is the sensible
choice, since YouTube can flag automated access. Close the browser before
exporting — a running browser may still be holding its newest cookies in a
write-ahead log, which `ytcui-we` warns about rather than quietly exporting a
stale session.

---

## Three modes

```cpp
Mode::AudioOnly    // audio track only        — music, radio, background
Mode::VideoOnly    // video track, no audio   — silent preview, scrubbing
Mode::AudioVideo   // both                    — normal playback
```

Video and audio are selected **independently**. That is what lifts quality
above the muxed ceiling, and what makes resolution switching cheap.

## Playback versus downloading

Ranking by compression is right for downloading and wrong for playing. AV1
compresses best, so a compression-ranked selector reaches for it every time —
but AV1 hardware decode only exists on recent GPUs, and software AV1 above
1080p stalls or kills a player on most machines. A stream that will not decode
is worse than a larger one that will.

`Quality::playback()` ranks by what the machine can actually do:

```
$ ytcui-dl --caps
decoders: h264(hw) hevc(hw) vp9(hw) av1(hw)

-q 2160            2160p60 av01   (smallest file)
-q 2160 --playable 2160p60 vp9    (cheapest to decode here)
```

Probed once from `mpv --vd=help` and `mpv --hwdec=help`, then cached for a
week. Codecs with no decoder are never selected; software-only codecs rank down
above 1080p. With mpv absent it assumes H.264 and VP9 rather than refusing to
play anything.

### Graceful degradation

`Selector::degrade()` returns the next thing to try after playback failed:
same picture with a cheaper codec first, then a rung down, then the muxed
stream. It returns an unusable selection when exhausted, so a retry loop
terminates.

```
2160p60 vp9  ->  1440p60 vp9  ->  1080p60 avc1  ->  720p60 avc1
             ->  480p avc1    ->  ...           ->  360p avc1 (muxed)
```

A player that dies seconds in has told you the pick is unusable, and the format
list cannot say why — missing decoder, a driver claiming hardware support it
lacks, a link that cannot sustain the bitrate. Stepping down is the only honest
response.

### Playing

The CLI can print the exact mpv invocation instead of running it:

```sh
$ ytcui-dl -b --mpv-args -q 720 <url>
mpv --ytdl=no --user-agent=<...> --force-window=yes <video-url> --audio-file=<audio-url>

$ ytcui-dl -a --mpv-args <url>
mpv --ytdl=no --user-agent=<...> --no-video <audio-url>
```

The User-Agent must match the client that signed the URL or the CDN rejects it.

---

## Diagnostics

`--diag` checks each layer in order, so the first failure identifies where the
problem actually is:

```
environment      openssl, cache dir, ffmpeg, mpv
network          HTTPS reachability, visitorData
innertube        every client in the chain: SABR, HLS, and a tail fetch
selection        all three modes plus the ladder
media fetch      actually pulls bytes — twice, including near the end
```

The media fetch is the one that matters, and it is deliberately done **twice**.
Everything above it can pass while playback still fails, and a near-start probe
passes even on a client whose URLs stop serving a minute in. A 403 at the start
with everything else green means the URL was signed for a different address
than the one fetching it — a VPN or proxy egressing elsewhere, or a dual-stack
host where the two requests took different paths; `-4`/`-6` force one family
and are the first thing to try. A 403 only near the *end* means the resolving
client is byte-capped.

## yt-dlp compatible mode

Symlink it and existing scripts work:

```sh
ln -s ytcui-dl yt-dlp
yt-dlp -f 'bestvideo[height<=1080]+bestaudio' -o '%(title)s.%(ext)s' <url>
yt-dlp -x --audio-format mp3 <url>
yt-dlp -F <url>
yt-dlp -g -f bestaudio <url>
yt-dlp --print '%(id)s %(resolution)s' <url>
```

Also reachable as `ytcui-dl --yt-dlp ...`.

A separate mode rather than one merged flag set, because the interfaces
genuinely collide: `-q` is `--quality` here and `--quiet` there, `-a` is
`--audio` versus `--batch-file`, `-c` is `--connections` versus `--continue`,
`-s` is `--search` versus `--simulate`. Merging them would silently do the
wrong thing for one set of users.

Supported: `-f` (including `+` pairs, `/` fallback chains, and
`[height<=N][ext=][fps>=][vcodec=][tbr<=]` filters), `-o` templates, `-P`,
`-F`, `-j`, `-g`, `-e`, `--get-*`, `--print`, `-x`, `--audio-format`,
`--merge-output-format`, `-N`, `-c`/`--no-continue`, `-w`, `-s`, `-4`/`-6`,
`--user-agent`. Unknown options are accepted and ignored so existing command
lines still run. On a live stream, metadata and `-g` work and the download
refuses.

Not implemented: playlists and channels, subtitles, SponsorBlock, archives.

---

## Library use

Header-only. `#include "ytfast.h"`, link `-lssl -lcrypto -lz -lpthread`.

```cpp
#include "ytfast.h"
using namespace ytfast;

CurlGlobalInit init;                       // once, at startup
auto& yt = InnertubeClient::get_instance();

auto r = yt.resolve("aqz-KE-bpKQ", Mode::AudioVideo, Quality::at(1080));
if (r.ok()) {
    r.sel.video->url;        // video-only stream
    r.sel.audio->url;        // audio-only stream
    r.sel.describe();        // "1080p60 av01 + 387kbps mp4a 6ch"
    r.ladder;                // every rung, for a quality menu
}

yt.shutdown();                             // joins the prefetch worker
```

`VideoInfo` also carries `is_live_now` and `hls_url`; when `formats` is empty
but `hls_url` is not, the manifest is the only transport and belongs straight
in a player.

### Quality

Every field is optional. A default-constructed `Quality` asks for the best
available and always resolves to something.

```cpp
Quality q;
q.height         = 1080;     // target; 0 = best
q.max_height     = 1440;     // hard ceiling
q.fps            = 60;       // prefer at least this
q.prefer_hdr     = true;
q.allow_surround = false;    // stereo only
q.allow_muxed    = false;    // forbid the muxed fallback
q.smallest       = true;     // invert everything: smallest picture, lowest bitrate
q.vcodec         = "av01";   // av01 | vp9 | avc1 | h264
q.acodec         = "opus";   // opus | mp4a | aac
q.container      = "webm";   // mp4 | webm
q.for_playback   = true;     // rank by what this machine can decode

Quality::best();  Quality::at(720);  Quality::upto(1080);  Quality::lowest();
Quality::playback(1080);     // the for_playback form, as a factory
```

Constraints relax in tiers, so an over-specified request degrades to a working
pick rather than returning nothing. Asking for a height that does not exist
lands on the **nearest rung above** it, not on the largest stream available —
requesting 240p on a 1080p-and-2160p upload gives you 1080p.

### Graceful resolution switching

In `AudioVideo` mode the audio track is chosen independently of the video, so
changing resolution only swaps the video stream. A player can reload the video
input without a gap in sound and without re-buffering audio.

```cpp
auto sel  = Selector::select(info.formats, Mode::AudioVideo, Quality::at(1080));
auto next = Selector::switch_video(info.formats, sel, 720);
// next.audio == sel.audio   — guaranteed, and covered by a test

int h = Selector::step_height(info.formats, current_height, +1);   // one rung up
```

`switch_video` returns the current selection unchanged when the video track
cannot change, so a caller can compare pointers to decide whether a reload is
even needed. Stepping settles at either end rather than wrapping.

### Quality menu

```cpp
for (const auto& r : Selector::ladder(info.formats))
    printf("%-10s itag %d  %s\n", r.label().c_str(), r.itag,
           human_bytes(r.bytes).c_str());
```

One rung per height, with 60 fps kept separate from 30 fps because users do
choose between "720p" and "720p60". Sub-30 fps variants collapse into their
height's rung — a menu should not show "144p" twice.
`Selector::audio_ladder()` is the audio equivalent.

### Downloading

```cpp
DownloadOptions o;
o.connections = 3;                       // parallel ranged chunks; 1 disables
o.resume      = true;
o.on_progress = [](const Progress& p) {
    printf("\r%3.0f%%  %s/s  ETA %s", p.percent(),
           human_bytes((int64_t)p.speed_bps).c_str(),
           human_duration(p.eta_seconds).c_str());
    return true;                         // return false to cancel
};

auto r = Downloader::fetch(url, path, o);
```

Resume opens `r+b`. Opening `wb` truncates, which leaves a hole of zeros at the
head of the file after seeking to the resume offset — that bug was in the
previous version and is now covered by a checksum test.

```cpp
auto r = StreamDownloader::fetch(video_url, audio_url, out_path,
                                 info.client_ua, /*audio_only=*/false);
r.transcoded;  // false = remuxed (stream copy, no re-encode)
```

### Session cookies

```cpp
yt.set_cookies_file("/path/to/cookies.txt");   // Netscape format
```

A `SAPISID` (or `__Secure-3PAPISID`) cookie is turned into a real
`SAPISIDHASH` `Authorization` header, the same origin-bound signature a
logged-in browser sends. `ytcui-we` writes a file this reads directly; so does
any browser "export cookies" extension, and `curl`/`yt-dlp` jars.

---

## Known constraints

This section is the most useful thing in the repository. YouTube's behaviour
here is a moving target, and almost everything below was established by
measurement rather than inference — including several things that look obvious
and are false.

### The client chain, and the byte cap

Current order, in `PLAYER_CHAIN` (`yt_innertube.h`):
`VISIONOS -> ANDROID -> IOS -> ANDROID_VR`.

`ANDROID`, `IOS` and `ANDROID_VR` all require a GVS PO Token for media fetch
now. They still hand back a fetchable `url` per adaptive format — the player
response looks fine — but as of 2026-08 the CDN caps how many bytes it will
actually serve from that URL to roughly a minute of the stream (scaled by
bitrate: ~3 MB for a 390 kbps audio track, ~10–12 MB for 1080p60 video) before
403ing everything past it, regardless of offset, request size, connection
count, or how long you wait. Confirmed against fresh URLs, fresh connections
and unrelated videos, so it is not this project's signing, IP or retry logic.

`VISIONOS` (Apple Vision Pro) has no such cap — confirmed by pulling a 679 MB
2160p60 stream through one end to end — which is why it leads. The tradeoff is
that it does not offer the 5.1 surround tracks (`mp4a`/`ec-3`/`ac-3` at
~387 kbps) the others do, topping out around opus ~155 kbps stereo. The other
three stay in the chain for what `VISIONOS` declines and for clips short enough
that the cap never bites.

Whichever client actually resolved a video is tracked on `VideoInfo::client_ua`
and travels with every later request for it (media fetch, `--play`, download),
so a download never signs a request with the wrong client's User-Agent.

**Version pins are load-bearing.** Re-checked against yt-dlp's
`INNERTUBE_CLIENTS` table on 2026-11-01:

| client | pinned here | upstream now | why the pin |
|---|---|---|---|
| `ANDROID` | 20.10.38 | 21.26.364 | upstream's version answers SABR — 0 of 29 formats carried a URL when asked directly |
| `IOS` | 20.10.4 | 21.26.4 | upstream's is likewise URL-less; the pin returns 27 URL-carrying formats plus an HLS manifest |
| `VISIONOS` | 1.02 | 1.02 | still the only entry with `REQUIRE_JS_PLAYER: False` *and* no GVS PO policy |

The `ANDROID` pin costs something: it returns `LOGIN_REQUIRED` without
`visitorData`, so the bootstrap is not optional.

**If YouTube closes the `VISIONOS` gap**, the fix is the same shape as last
time: find whichever client in that table has no `GVS_PO_TOKEN_POLICY` override
and no `REQUIRE_JS_PLAYER`, and move it to the front of `PLAYER_CHAIN`.

### PO Tokens

**They are not minted here, on purpose — nothing does this without running
Google's JS.** A PO Token is a BotGuard attestation; producing one means
executing Google's obfuscated challenge JS somewhere. That is true of yt-dlp
too: its core ships zero token-minting logic, only a plugin interface. Three
ways to get one in:

- **`--po-token <tok>`** — paste one you already have. Used as-is.
- **`--pot-provider`** (or `--pot-provider-url <url>`) — fetched from a
  locally-running
  [bgutil-ytdlp-pot-provider](https://github.com/Brainicism/bgutil-ytdlp-pot-provider),
  the same service yt-dlp's own `bgutil` plugin talks to
  (`POST /get_pot {content_binding}` -> `{poToken}`). A response that doesn't
  decode as plausible base64url is rejected rather than forwarded — confirmed
  live that YouTube's player endpoint 400s the *entire* request, every client
  in the chain, on a malformed token, so a broken provider degrades to "no
  token" instead of taking down playback.
- **`--cookies <file>`** — not a PO Token at all, and does not become one. What
  it does do is real: a `SAPISIDHASH` header unlocking age-gated, members-only
  and private videos. `ytcui-we` exists to produce this file.

**The correction that matters:** a token from `--pot-provider` is a *web*
BotGuard attestation, and per yt-dlp's PO-Token guide tokens do not cross
platforms — *"a Web PO Token cannot be used on Android or iOS"*. So it **does
not** lift the ANDROID/IOS byte cap, and it cannot rescue a "made for kids"
video, which is exactly the case that falls through to those clients. It is
useful for a web-family client, and nothing else here.

### "Made for kids" videos cannot be fixed tokenlessly, full stop

`VISIONOS` answers `UNPLAYABLE` ("This video is not available") for them by
design — yt-dlp's own table carries the same note — so the chain falls to
`ANDROID`, whose URLs stop serving about a minute in and which offers no HLS to
escape to. Every other tokenless candidate was tried and rejected on
measurement: `TVHTML5` and `TVHTML5_SIMPLY` answer "The page needs to be
reloaded" (and `LOGIN_REQUIRED`, *"Sign in to confirm you're not a bot"*, on a
kids video); `WEB_EMBEDDED_PLAYER` and `web_safari` answer "Video unavailable"
without a logged-in session; `ANDROID_VR` 403s the end of every stream it
serves. `ytcui-dl` now says so up front instead of failing two thirds of the
way into a download.

One genuinely useful discovery along the way: supplying a `signatureTimestamp`,
scraped as a plain number out of the player's `base.js` with no JS executed,
flips `WEB` and `TVHTML5_SIMPLY` from `UNPLAYABLE` to `OK`. It does not help
here — `WEB` is then SABR-only and `TVHTML5_SIMPLY` returns every format
signature-ciphered, which needs a JS engine this project does not have — but it
is the missing ingredient if anyone wants to pursue a web-family client.

### The web-family HLS route is a dead end

Measured 2026-11-01, written down so nobody retries it.

It looks like the obvious escape from the byte cap. A web client *does* return
an `hlsManifestUrl`, and under a Safari UA plus a scraped `signatureTimestamp`
it does so even for a "made for kids" video when a logged-in session is
attached — **the only thing found that offers a manifest for one at all.**

It does not work. The master manifest fetches 200, the variant playlists fetch
200, and **every media segment 403s** — first, middle and last alike. That was
tested with real web GVS PO Tokens from `bgutil-ytdlp-pot-provider` across the
whole matrix: content binding of the video ID *and* of the exact `visitorData`;
the token placed in the player request's `serviceIntegrityDimensions` *and*
left out; spliced into the manifest the way yt-dlp does it —
`manifest_path.rstrip('/') + f'/pot/{po_token}'`, note a **path segment** for
manifests and a `pot=` **query param** only for progressive/DASH format URLs.
The splice demonstrably takes effect: the returned segment URLs contain `pot`.
They still 403.

The explanation that fits is upstream's own note against `web` in the PO-Token
guide table — *"Only SABR formats available"* — i.e. the manifest a web client
returns is vestigial and is simply not served any more.

The practical upshot: **a browser-based playback fallback of this shape cannot
be built today.** The real web player's transport is SABR — confirmed directly,
its media requests are `POST videoplayback` with ~1.5 KB protobuf bodies — and
neither mpv nor ffmpeg speaks that. This is why `ytcui-we` is a session helper
and not a second engine.

### No SABR/UMP support

If every client ends up requiring a PO Token, this approach stops working
entirely, and the SABR gate says so rather than silently dropping to 360p.
Implementing SABR is a large job; upstream's own downloader for it is still an
unmerged, experimental PR.

---

## How it got here: v1 to v2

v2 is a full rewrite, not an incremental update. The old version worked in the
sense that it ran, but it had a ceiling baked into its architecture. Diffed
against the old codebase directly:

| | v1 (old) | v2 (this repo) |
|---|---|---|
| Dependencies | libcurl + bundled nlohmann/json | none beyond OpenSSL/zlib/pthreads — custom HTTP/1.1 client (`yt_http.h`) and a zero-allocation JSON scanner (`yj.h`) |
| Best download, ever | **Hard-capped at 360p** | video and audio resolved independently then muxed — the ceiling is whatever YouTube encoded (2160p60 ordinary, confirmed 4320p60 HDR on an 8K upload; see `ENCODING_AND_RESOLUTION.md`) |
| Download mechanism | one `curl_easy_perform()`, one connection, no resume, no retry | parallel chunked engine, automatic resume, byte-identical output verified by checksum test, adaptive per-chunk retry/split |
| SABR (URL-less format lists) | **not detected at all** | explicit `sabr_only` gate; advances to the next client instead of settling for garbage |
| Client chain | `ANDROID -> IOS`, hardcoded, no fallback logic | `VISIONOS -> ANDROID -> IOS -> ANDROID_VR`, re-tuned against yt-dlp's current policy table |
| User-Agent on media fetch | **hardcoded to the ANDROID UA**, silently wrong whenever a fallback client won | tracked per-video on `VideoInfo::client_ua` and used for every later request |
| Audio-only / "just give me a file" | `-x` meant "download bestaudio", raw | `-x`/`--remux` always produce one finished `.mp3`/`.mp4` |
| Live streams | not handled | HLS manifest; play or refuse, never a 1-byte file |
| Diagnostics | none | `--diag` across five layers, with a deliberate near-end fetch |
| Non-ASCII titles | untested | tested against Japanese, Korean, Cyrillic, Arabic, Thai, emoji and ZWJ sequences; two byte-vs-codepoint truncation bugs found and fixed |
| Tests | none | 7 test binaries, offline and network |

### Why the old version only gave you 360p

itag 18 is a muxed 360p stream with a ~96 kbps AAC track, and it was the only
thing the previous version could return. The format selector was not the
reason. The InnerTube client version it used, `ANDROID 21.02.35`, receives a
**SABR** player response: the complete adaptive format list comes back with no
`url` on any entry, only `initRange`/`indexRange` and a top-level
`serverAbrStreamingUrl`. Fetching those needs YouTube's UMP/SABR protocol. The
single muxed itag 18 was the one directly fetchable stream left.

It failed in the worst possible way — silently. `playabilityStatus` was `OK`
and the format list was non-empty, so nothing looked broken and no fallback
fired.

```
before:   1 format,   360p,   ~96 kbps AAC
after:   29 formats,  2160p,  389 kbps 5.1
```

That is why the SABR gate exists, and why `--diag` reports per-client SABR
state instead of just "it works".

---

## Layout

```
include/
  yj.h            zero-allocation JSON scanner      (replaces nlohmann)
  yjw.h           JSON writer
  yt_itag.h       static itag table
  yt_types.h      StreamFormat, VideoInfo, id parsing
  yt_http.h       HTTP/1.1 over OpenSSL             (replaces libcurl)
  yt_cache.h      on-disk visitorData cache
  yt_caps.h       decoder capability probe
  yt_select.h     modes, ladder, switching, degradation
  yt_download.h   parallel downloads, muxing
  yt_stream_dl.h  -x/--remux: fetch + ffmpeg, one finished .mp3/.mp4
  yt_sqlite.h     read-only SQLite reader           (replaces libsqlite3)
  yt_browser.h    default-browser launch, cookie stores, decryption
  yt_innertube.h  InnerTube client, the player chain
  ytfast.h        umbrella header
cli/
  ytcui-dl.cpp    the main front end
  ytdlp_compat.h  yt-dlp compatible command line
  ytcui-we.cpp    session helper
```

### Parser

`yj.h` is a single-pass scanner: no DOM, no allocation, no recursion, no
exceptions. Strings are returned as `string_view` slices into the response
buffer; only fields you keep are copied. Verified byte-identical to nlohmann.

```
player 180 KB   1711 us / 4215 allocs / 463 KB  ->    98 us / 89 allocs / 54 KB
search 317 KB   4469 us / 22363 allocs / 1157 KB -> 1333 us / 60 allocs / 8.4 KB
```

`X-Goog-FieldMask` on every request cuts player responses from ~183 KB to
~69 KB with an identical format list.

### SQLite reader

`yt_sqlite.h` does one job: find a table in `sqlite_schema` and walk its
B-tree, following overflow page chains, decoding records by serial type. No
SQL, no query planner, no writing, no index pages. It exists because reading a
browser's cookie store means reading SQLite, and the two alternatives were both
worse — `libsqlite3` would have broken the project's only real property, and
the `sqlite3` CLI is absent from a stock Ubuntu 24.04, so the feature would
have silently not existed for most people.

It does not replay a write-ahead log, so a `-wal` sidecar is detected and
reported rather than ignored; close the browser first, which is the same advice
yt-dlp gives for the same reason.

## Testing

```sh
make test        # offline: parser, extraction, itag table, selection, SQLite
make test-live   # network: TLS, gzip, redirects, InnerTube, downloads
```

Offline tests run against captured fixtures, including a real SABR response so
that regression cannot return quietly. Clean under ASan, UBSan, TSan and
`-Wall -Wextra -Wpedantic -Wshadow`.

The SQLite and cookie tests run against `test/fixtures/browser_cookies.sqlite`,
generated with the **real** sqlite3 library — a parser tested only against its
own output proves nothing. The fixture deliberately contains a 9000-byte value
that cannot fit in one 4 KiB page (so the overflow chain is exercised),
multi-byte UTF-8 and emoji, a column order that is *not* the obvious one so
position-based lookups would fail, real `v10` ciphertext, Chrome's
hash-prefixed variant, and cookies for an unrelated domain that must be
filtered out. One test asserts the full round trip: what `ytcui-we` writes is
exactly what `ytcui-dl`'s own cookie parser reads back.

## Embedded notes

- **Header-only**, no build system requirements beyond a C++17 compiler.
- **No global state is required.** `InnertubeClient` is constructible directly;
  the singleton is a convenience, not a dependency.
- **Selection does no I/O and no allocation.** `Selector` operates on an
  already-fetched format list, so it is safe to call from a UI thread on every
  keystroke.
- **One background thread, at most.** The prefetch worker starts on first use
  and is joined by `shutdown()`. Never call it and there is no thread at all.
- **The disk cache is optional**: `set_disk_cache(false)` for read-only
  filesystems. It only ever holds `visitorData`.
- **`StreamFormat` holds one `std::string`** (the URL). Everything else is a
  `string_view` into a static itag table or the owning `VideoInfo`'s arena, so
  a `StreamFormat` must not outlive its `VideoInfo`.
- **Exceptions** are used for transport errors only. Parsing never throws:
  malformed JSON yields empty values, never UB.

## Licence

MIT. See `LICENSE`.
