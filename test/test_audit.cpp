#include "yt_browser.h"
#include "yt_hls.h"
#include "yj.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <filesystem>
static void check(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
}
int main() {
    try {
        using namespace ytfast;
        size_t visited = 0;
        yj::each_member(yj::parse("{\"", 2), [&](std::string_view, yj::Val) { ++visited; return true; });
        check(visited == 0, "truncated key must not create an oversized string_view");
        check(yj::parse("9223372036854775808", 19).i64(17) == 17, "positive integer overflow");
        check(yj::parse("-9223372036854775808", 20).i64() == std::numeric_limits<int64_t>::min(), "minimum integer");
        check(!browser::interesting_domain("youtube.com.attacker.example"), "cookie domain suffix attack");
        check(!browser::interesting_domain("notyoutube.com"), "cookie domain prefix attack");
        check(browser::interesting_domain(".m.youtube.com"), "legitimate cookie subdomain");
        auto temporary = (std::filesystem::temp_directory_path() / "ytcui-audit.XXXXXX").string();
        check(::mkdtemp(temporary.data()) != nullptr, "create private fixture directory");
        const std::string victim = temporary + "/victim.txt", jar = temporary + "/cookie-link.txt";
        { std::ofstream out(victim); out << "keep"; }
        ::unlink(jar.c_str());
        check(::symlink(victim.c_str(), jar.c_str()) == 0, "create fixture symlink");
        browser::Cookie cookie; cookie.domain = ".youtube.com"; cookie.path = "/";
        cookie.name = "SAPISID"; cookie.value = "synthetic-only";
        std::string error;
        check(browser::write_netscape(jar, {cookie}, &error), "atomic cookie export");
        struct stat state{}; check(::lstat(jar.c_str(), &state) == 0 && S_ISREG(state.st_mode) &&
                                  (state.st_mode & 0777) == 0600, "cookie jar must be regular 0600 file");
        std::ifstream original(victim); std::string content; original >> content;
        check(content == "keep", "cookie export must not truncate symlink target");
        cookie.value = "injected\nrow";
        check(!browser::write_netscape(jar, {cookie}, &error), "cookie field injection");
        ::unlink(jar.c_str()); ::unlink(victim.c_str()); ::rmdir(temporary.c_str());
        const auto manifest = HlsManifest::parse(
            "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=9000000,RESOLUTION=3840x2160,FRAME-RATE=60,AUDIO=\"a\"\n4k.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080,FRAME-RATE=60,AUDIO=\"a\"\n1080.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720,FRAME-RATE=30,AUDIO=\"a\"\n720.m3u8\n",
            "https://example.test/live/master.m3u8");
        auto quality = Quality::at(1080);
        auto chosen = manifest.select(quality);
        check(chosen && chosen->height == 1080 && chosen->fps == 60, "HLS exact 1080p60");
        check(manifest.audio_for(*chosen) == "https://example.test/live/audio.m3u8", "HLS external audio");
        check(manifest.select(Quality::best())->height == 2160, "HLS best 4K");
        check(manifest.select(Quality::at(240))->height == 720, "HLS nearest available above target");
        check(!manifest.select(Quality::upto(240)), "HLS hard ceiling");
        check(hls_join("https://example.test/a/b/master.m3u8?k=1", "../720.m3u8?q=2") ==
              "https://example.test/a/720.m3u8?q=2", "relative signed HLS URL");
        std::vector<StreamFormat> formats(3);
        for (size_t i = 0; i < formats.size(); ++i) {
            auto& f = formats[i]; f.has_video = true; f.has_audio = true;
            f.height = i == 0 ? 2160 : i == 1 ? 1080 : 720;
            f.url = "https://example.test/media";
        }
        check(Selector::select(formats, Mode::AudioVideo, Quality::at(1080)).height() == 1080, "muxed resolution is order independent");
        Url url;
        check(!Url::parse("https://example.test/a\r\nInjected:yes", url), "URL request injection");
        check(Url::parse("https://example.test?x=1#ignored", url) && url.path == "/?x=1", "query-only URL");
        std::cout << "Audit regressions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
