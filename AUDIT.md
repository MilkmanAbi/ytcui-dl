# ytcui-dl 0.6.1 code audit

Scope: integrated browser-session commands, cookie storage/decryption, JSON and
SQLite parsing, HTTP transport, parallel downloads/resume, and resolution selection.
This is a source review with targeted regressions, not a formal security certification.

The live HLS/session branch was already an ancestor of main. This release unifies
the browser helper into `ytcui-dl session`; only `ytcui-dl` is built and installed.

## Fixes and regression coverage

- HLS selection now reads master-playlist variants and honors requested height and
  the hard maximum height, retaining the selected variant's external audio.
- Muxed-format selection no longer exceeds the requested target just because a
  higher-resolution format happens to appear first in the response.
- Cookie domains use hostname boundaries; cookie fields reject row/header injection.
  Cookie jars use private same-directory temporary files and atomic replacement,
  preserving 0600 permissions without following an existing destination symlink.
- Chromium CBC cookies require valid PKCS#7 padding. Schema-24 domain hashes are
  checked against the actual cookie domain rather than blindly discarded.
- Truncated JSON keys cannot underflow a string_view length; integer parsing checks
  overflow. The old shared random generator is now thread-local.
- Download completion requires validated range offsets and exact byte counts.
  Interrupted parallel files use a durable per-chunk ledger instead of trusting a
  preallocated file's apparent length. Final names appear only after success;
  existing final files are not overwritten.
- Streaming HTTP downloads decode chunk framing, reject truncated bodies, remove
  credentials across origins and reject HTTPS downgrade redirects. TLS handshakes
  now have socket timeouts. Metadata/decompression sizes are bounded, and TLS
  verification distinguishes IP addresses from DNS hostnames.

- ffmpeg muxing/transcoding now uses argv execution and private staging directories.
  Existing final outputs are preserved, and cleanup only removes files owned by the
  current invocation. Compatibility audio conversion never interprets filenames as shell code.

## Limits retained

The custom HTTP/1.1 transport and browser integration use POSIX APIs. Native release
targets are Linux, macOS and BSD; Windows uses WSL. Browser sessions are explicit:
ordinary media commands do not inspect a browser profile. Safari and Windows
Chromium app-bound encryption are not implemented. The small SQLite reader does
not replay a browser's WAL; close the browser before exporting recent changes.

The custom client does not run YouTube player JavaScript or mint BotGuard tokens.
Some videos/clients require an external PO-token provider. Authentication cannot
guarantee access to every video or prevent service-side restrictions. HLS video
download/recording remains a separate ffmpeg operation; direct download operates on
progressive/adaptive byte resources. CI uses synthetic browser credentials and
local HTTP fixtures, never the user's browser data or YouTube network endpoints.
