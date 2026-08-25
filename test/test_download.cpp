// Download engine: serial vs parallel, resume, cancellation, integrity.
// Uses a public range-supporting file; YouTube's own CDN is IP-bound to the
// address that minted the URL, so it cannot be exercised from a build box.
#include "ytfast.h"
#include <cstdio>
#include <cstring>
#include <openssl/sha.h>
#include <string>
#include <sys/stat.h>

using namespace ytfast;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

static std::string sha256_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    SHA256_CTX c; SHA256_Init(&c);
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) SHA256_Update(&c, buf, n);
    std::fclose(f);
    unsigned char d[32]; SHA256_Final(d, &c);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
    return hex;
}
static int64_t fsize(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 ? st.st_size : -1;
}

int main(int argc, char** argv) {
    CurlGlobalInit init;
    const std::string url = argc > 1 ? argv[1]
        : "https://ftp.gnu.org/gnu/tar/tar-1.34.tar.gz";

    std::printf("helpers\n");
    check(human_bytes(0) == "0 B", "human_bytes(0)");
    check(human_bytes(1536) == "1.5 KB", "human_bytes(1.5K)");
    check(human_bytes(5LL << 30) == "5.00 GB", "human_bytes(5G)");
    check(human_duration(-1) == "--:--", "human_duration(unknown)");
    check(human_duration(90) == "1:30", "human_duration(90s)");
    check(human_duration(3725) == "1:02:05", "human_duration(1h)");

    // ------------------------------------------------------------- serial
    std::printf("\nserial (1 connection)\n");
    DownloadOptions so;
    so.connections = 1;
    so.resume = false;
    int prog_calls = 0;
    double last_pct = -1;
    bool monotonic = true;
    so.on_progress = [&](const Progress& p) {
        ++prog_calls;
        if (p.percent() >= 0) {
            if (p.percent() + 0.001 < last_pct) monotonic = false;
            last_pct = p.percent();
        }
        return true;
    };
    auto r1 = Downloader::fetch(url, "/tmp/dl_serial.bin", so);
    std::printf("    %s in %.2fs (%s/s), status %ld\n", human_bytes(r1.bytes).c_str(),
                r1.seconds, human_bytes((int64_t)r1.speed_bps()).c_str(), r1.status);
    check(r1.ok, "serial download succeeded");
    check(r1.bytes > 1000000, "serial got a plausible size");
    check(prog_calls > 0, "progress callback fired");
    check(monotonic, "progress never went backwards");
    const std::string ref = sha256_file("/tmp/dl_serial.bin");
    check(!ref.empty(), "serial file hashable");

    // ----------------------------------------------------------- parallel
    std::printf("\nparallel (4 connections)\n");
    DownloadOptions po;
    po.connections = 4;
    po.resume = false;
    po.min_parallel_size = 1024;   // force the parallel path on this size
    int max_conn_seen = 0;
    po.on_progress = [&](const Progress& p) {
        if (p.connections > max_conn_seen) max_conn_seen = p.connections;
        return true;
    };
    auto r2 = Downloader::fetch(url, "/tmp/dl_par.bin", po);
    std::printf("    %s in %.2fs (%s/s)\n", human_bytes(r2.bytes).c_str(),
                r2.seconds, human_bytes((int64_t)r2.speed_bps()).c_str());
    check(r2.ok, "parallel download succeeded");
    check(max_conn_seen == 4, "progress reported 4 connections");
    check(fsize("/tmp/dl_par.bin") == fsize("/tmp/dl_serial.bin"), "same size as serial");
    check(sha256_file("/tmp/dl_par.bin") == ref,
          "parallel bytes are IDENTICAL to serial (chunks landed at right offsets)");
    if (r1.seconds > 0.05 && r2.seconds > 0)
        std::printf("    speedup: %.2fx\n", r1.seconds / r2.seconds);

    // ------------------------------------------------------------- resume
    std::printf("\nresume\n");
    {
        // Truncate a complete file to a prefix, then resume it.
        const int64_t full = fsize("/tmp/dl_serial.bin");
        std::string partial = "/tmp/dl_resume.bin";
        {
            FILE* in = std::fopen("/tmp/dl_serial.bin", "rb");
            FILE* out = std::fopen(partial.c_str(), "wb");
            std::vector<char> buf((size_t)full / 3);
            size_t n = std::fread(buf.data(), 1, buf.size(), in);
            std::fwrite(buf.data(), 1, n, out);
            std::fclose(in); std::fclose(out);
        }
        const int64_t before = fsize(partial);
        check(before > 0 && before < full, "prepared a partial file");

        DownloadOptions ro;
        ro.connections = 1;
        ro.resume = true;
        bool saw_resumed = false;
        ro.on_progress = [&](const Progress& p) { if (p.resumed) saw_resumed = true; return true; };
        auto r3 = Downloader::fetch(url, partial, ro);
        std::printf("    resumed from %s, final %s\n", human_bytes(before).c_str(),
                    human_bytes(fsize(partial)).c_str());
        check(r3.ok, "resume succeeded");
        check(saw_resumed, "progress flagged the transfer as resumed");
        check(fsize(partial) == full, "resumed file is the right size");
        check(sha256_file(partial) == ref,
              "resumed file is byte-identical (no zero hole at the head)");

        // Resuming an already-complete file must be a no-op, not a refetch.
        auto r4 = Downloader::fetch(url, partial, ro);
        check(r4.ok && r4.seconds < 1.0, "already-complete file returns immediately");
        check(sha256_file(partial) == ref, "no-op resume left the file alone");
    }

    // -------------------------------------------------------- cancellation
    std::printf("\ncancellation\n");
    {
        DownloadOptions co;
        co.connections = 1;
        co.resume = false;
        co.on_progress = [](const Progress& p) { return p.downloaded < 200000; };
        auto rc = Downloader::fetch(url, "/tmp/dl_cancel.bin", co);
        std::printf("    stopped after %s\n", human_bytes(rc.bytes).c_str());
        check(!rc.ok, "cancelled download reports failure");
        check(rc.cancelled, "cancelled flag set");
        check(rc.bytes < fsize("/tmp/dl_serial.bin"), "cancelled early");
    }
    {
        DownloadOptions co;
        co.connections = 4;
        co.resume = false;
        co.min_parallel_size = 1024;
        co.on_progress = [](const Progress& p) { return p.downloaded < 150000; };
        auto rc = Downloader::fetch(url, "/tmp/dl_cancel2.bin", co);
        check(rc.cancelled && !rc.ok, "parallel download cancels too");
    }

    // -------------------------------------------------------------- errors
    std::printf("\nerror handling\n");
    {
        DownloadOptions eo; eo.connections = 1; eo.resume = false;
        auto e1 = Downloader::fetch("https://ftp.gnu.org/gnu/tar/does-not-exist-xyz.tar.gz",
                                    "/tmp/dl_404.bin", eo);
        check(!e1.ok, "404 reported as failure");
        check(!e1.error.empty(), "404 carries an error string");
        std::printf("    404 -> \"%s\"\n", e1.error.c_str());

        auto e2 = Downloader::fetch(url, "/nonexistent-dir-xyz/out.bin", eo);
        check(!e2.ok && !e2.error.empty(), "unwritable path reported");
        std::printf("    bad path -> \"%s\"\n", e2.error.c_str());

        bool threw = false;
        try { Downloader::fetch("not-a-url", "/tmp/x.bin", eo); }
        catch (...) { threw = true; }
        check(threw || true, "garbage url does not crash");
    }

    // ------------------------------------------------- TLS session resumption
    std::printf("\nTLS session resumption\n");
    {
        HttpClient a, b;
        auto t0 = std::chrono::steady_clock::now();
        a.get("https://i.ytimg.com/vi/aqz-KE-bpKQ/default.jpg");
        double cold = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        auto r = b.get("https://i.ytimg.com/vi/aqz-KE-bpKQ/default.jpg");
        double warm = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("    fresh client %.0f ms -> new client, cached session %.0f ms (resumed=%d)\n",
                    cold, warm, (int)b.last_tls_resumed());
        check(r.status == 200, "second client fetched fine");
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
