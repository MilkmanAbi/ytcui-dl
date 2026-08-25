# Encoding, language, and resolution — test notes

Hands-on testing against the live InnerTube API (not a synthetic test), run
2026-08-25 against `ytcui-dl` built from this tree. Two real bugs were found
and fixed along the way; both are called out below and are already in the
tree, not just described.

## TL;DR

| Question | Answer |
|---|---|
| Non-ASCII / UTF-8 search queries | Work correctly — Japanese, Korean, Cyrillic, Arabic (RTL), Thai, full-width Latin, emoji, ZWJ sequences, combining diacritics all tested live |
| "Wide chars" (UTF-16 surrogate-pair territory) | Not applicable — the codebase has no `wchar_t` anywhere; UTF-8 is just bytes on POSIX, so there's nothing to convert |
| JSON body escaping of query text | Correct — non-ASCII passed through raw (valid, since JSON strings are UTF-8), control chars escaped, quotes/backslashes escaped |
| Non-ASCII video titles → output filenames | Correct on the filesystem (macOS APFS is UTF-8-native) — **but the truncation for very long titles was byte-based, not codepoint-based, and could produce an invalid, corrupted filename. Fixed.** |
| Non-ASCII channel names in the `-s` search table | **Same bug, second location** (a `printf` byte-precision cut). Fixed. |
| Typical max resolution | Whatever YouTube actually offers for that upload: capped at 1080p for a plain live stream in this test, true 4320p60 HDR (8K, ~3.9 GB) resolved and fetched cleanly for an 8K upload, 2160p60 the practical ceiling for most regular VOD content |

## Search queries: languages and scripts

Tested via `ytcui-dl -s "<query>"` against the live search endpoint. Every
query below returned real, correctly-decoded results with no corruption in
either the query going out or the titles/channels coming back:

- **ASCII** — `lofi hip hop` — baseline, works as expected.
- **Japanese** — `音楽 ローファイ` — results include native romaji/kanji titles,
  e.g. `2時間 🌠 夜のLo-fiプレイリスト - リラックス / 勉強 / ...`.
- **Korean** — `한국 음악` — titles came back with Hangul plus, in one result,
  Unicode *Mathematical Alphanumeric Symbols* (`𝗣𝗟𝗔𝗬𝗟𝗜𝗦𝗧`) — a 4-byte-per-
  codepoint astral-plane block, rendered correctly.
- **Russian (Cyrillic)** — `русская музыка` — channel names in all-caps
  Cyrillic (`МЕГАХИТ`) came back intact.
- **Arabic (right-to-left)** — `موسيقى عربية` — titles mixing Arabic and Latin
  script in one string (`ح  حسين الجسمي - بشرة خير ... | Hussain Al Jassmi`)
  round-tripped correctly. No bidi-related corruption; this project does no
  text-direction handling of its own, which is the correct amount — that's a
  terminal/renderer concern, not a data-correctness one.
- **Thai** — `เพลงไทย 🎸 rock` — mixed Thai script (no visible spaces between
  words, a script quirk unrelated to this tool) plus an emoji plus ASCII in
  one query, worked.
- **Full-width (zenkaku) Latin** — `Ｈｅｌｌｏ　Ｍｕｓｉｃ` (using U+FF21-style
  full-width letters and a full-width space) — matched normal-width `Hello`
  results, meaning YouTube's search does the normalization; this tool just
  has to get the bytes there undamaged, which it does.
- **Emoji-only** — `🎵🎶🎧` — returned relevant music-note results.
- **ZWJ emoji sequence + combining diacritics** — `café 👨‍👩‍👧‍👦 naïve résumé`
  — the family emoji here is 7 codepoints / 25 bytes (four person emoji
  joined by U+200D ZERO WIDTH JOINER). Genuinely returned zero results
  (nobody's titled a video that), but importantly: valid, well-formed empty
  JSON (`[]`), exit code 1 ("no results"), no crash, no garbled output.
  Splitting the same string to drop just the ZWJ sequence
  (`café naïve résumé`) returned real, correctly-accented results
  (`Émission " Pause café 🌻 " Résumé de l'épisode 2 "La Naïveté "`),
  confirming the combining-diacritics half was never the issue.
- **Embedded quotes and backslashes** — `test "quoted" \backslash\ query` —
  correctly escaped into the outgoing JSON request body; the response JSON
  came back well-formed with an em dash (`—`, a 3-byte UTF-8 character) intact
  in a returned title.

### Why this works: the design already avoids the usual traps

- **No `wchar_t`, no `std::wstring`, no `setlocale`, no `MultiByteToWideChar`-
  style conversion anywhere in the codebase** (checked with a full-tree grep).
  On POSIX, `argv`, file paths, and `std::string` are just byte sequences —
  UTF-8 needs no special handling to pass through untouched, and this project
  doesn't add any locale-dependent processing that could break it.
- **JSON escaping (`json_escape_to` in `yt_innertube.h`) operates byte-by-
  byte** and only escapes what JSON actually requires (control chars, `"`,
  `\`); every other byte, including every byte of a multi-byte UTF-8
  sequence, passes through unchanged. That's exactly correct — JSON string
  values are defined as UTF-8 text, and multi-byte UTF-8 needs no escaping.
- **`yj.h`'s zero-allocation JSON scanner** returns `string_view` slices
  directly into the response buffer for title/channel/etc. — no
  re-encoding, no transcoding step where corruption could sneak in.
- **`HttpClient::url_encode`'s `isalnum(c)` call is locale-safe**: the loop
  variable is `unsigned char`, so the value passed to `isalnum` is always in
  `[0, 255]`, which is well-defined (the classic bug here is passing a
  possibly-negative plain `char` on platforms where `char` is signed, which
  is undefined behavior for `isalnum`). This function turned out to be dead
  code — nothing calls it, since the search endpoint takes the query as a
  JSON POST field rather than a URL parameter — but it's correct as written
  and worth keeping that way if something starts calling it.

## Bugs found and fixed: byte-based truncation of UTF-8

Both of the same shape: code that truncates a string to a fixed **byte**
count, which can land in the middle of a multi-byte UTF-8 sequence and leave
a dangling, invalid tail. Both are realistic, not contrived — YouTube allows
100-character video titles, and CJK/emoji characters run 3-4 bytes each in
UTF-8, so a title anywhere near that limit in a non-Latin script routinely
exceeds byte thresholds well past 100.

### 1. Output filenames (`Downloader::suggest_filename`, `yt_download.h`)

```cpp
if (safe.size() > 180) safe.resize(180);   // before
```

Confirmed with a synthetic title (178 ASCII bytes + two 3-byte UTF-8
characters straddling the cutoff): the old code produced a filename ending in
two dangling bytes of a truncated character, rendering as `�` (U+FFFD
replacement character) — an invalid filename on disk, not just an ugly one.

**Fix**: added `Downloader::utf8_safe_truncate()`, which truncates to the
byte budget and then backs off over any trailing UTF-8 continuation bytes,
dropping the lead byte too if its sequence didn't fully fit. The result is
always a whole number of codepoints. `suggest_filename` now calls this
instead of a raw `resize`.

### 2. Search results table (`-s` without `--json`, `ytcui-dl.cpp`)

```cpp
std::printf("%-22.22s", r.channel.c_str());   // before
```

`printf`'s `%.Ns` precision is also byte-based. Confirmed the same failure
mode directly against libc: a 20-ASCII-byte string plus a straddling 3-byte
character, cut with `%-22.22s`, produced two dangling bytes right before the
next column.

**Fix**: pre-truncate the channel string with the same
`Downloader::utf8_safe_truncate()` (now public, reused here) and drop the
`printf` precision, keeping only the width for padding.

Neither bug affected `--json` output (which prints titles/channels
untruncated) or the actual search/resolve logic — both were purely in
user-facing text formatting, but both were real, reachable, and now fixed.

## Resolution: what's actually available in practice

Selection logic itself was already covered thoroughly by the existing test
suite (`test/test_select.cpp`) and by extensive live testing earlier in this
project's work (see `README.md`). What's new here is characterizing what
"max resolution" looks like across different *kinds* of real content, since
the ceiling is set by what YouTube encoded for that specific upload, not by
anything in this tool:

- **Regular 4K60 VOD** (the project's standard test video): 2160p60 up to
  ~9 Mbps AV1, full ladder down to 144p, both AV1 and VP9 codec tracks
  available at most rungs. This is the "normal" ceiling for the large
  majority of content on YouTube.
- **True 8K HDR upload**: `-L` on an actual 8K-HDR-tagged video returned a
  **4320p60 HDR** rung (itag 702, AV1, ~3.9 GB, `hdr: true` in `-j` output)
  as the literal top of the ladder, with a parallel HDR/SDR track at every
  resolution down to 144p (HDR itags 694-702 alongside the plain SDR AV1/VP9
  ones). `-q best` correctly picked the 4320p60 HDR rung over everything
  else, and a real byte-range fetch against the resolved URL succeeded
  (`206`, full bytes returned) — confirmed working end to end, not just
  resolved. (The full 3.9 GB file wasn't downloaded start to finish as part
  of this test — a partial fetch confirms the URL and pipeline are correct
  the same way it does for any other stream.)
- **Live stream**: capped at 1080p in this test (a typical 24/7 radio-style
  live stream) — no 4K rung was offered by YouTube for that particular
  broadcast. Formats correctly report no filesize (`-`, since live content
  has no fixed `Content-Length`), and `is_live: true` is set correctly in
  JSON output.
- **Shorts URL form** (`/shorts/<id>`) — parses correctly via `extract_id`
  and resolves normally. Width/height are passed straight through from
  whatever YouTube's API reports (`yt_innertube.h` copies `width`/`height`
  fields with no orientation logic of its own), so a genuinely vertical
  upload would report portrait dimensions correctly; the specific `#shorts`-
  tagged videos found via search in this session turned out to be regular
  landscape 16:9 uploads using the hashtag rather than true vertical Shorts,
  so that specific case (portrait dimensions in practice) wasn't directly
  observed this session — worth a follow-up spot-check with a confirmed
  vertical upload if it matters for a specific use case.

## What this doesn't cover

- Filesystem-level testing on Linux (ext4) or Windows — this was tested on
  macOS/APFS, which is UTF-8-native for filenames. ext4 is filesystem-encoding-
  agnostic (stores raw bytes), so should behave the same; Windows filesystems
  use UTF-16 internally and would need actual testing on that platform before
  assuming the same guarantees hold, though nothing in this codebase does
  Windows-specific path handling one way or the other today.
- A genuinely vertical (portrait) Shorts upload, for the reason noted above.
- Extremely long queries (URL/POST body length limits) — not attempted.
