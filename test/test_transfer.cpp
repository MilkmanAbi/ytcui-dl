#include "yt_download.h"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc < 3) return 2;
        if (argc > 3 && std::string(argv[3]) == "get") {
            ytfast::HttpClient http;
            auto result = http.get(argv[1], {"Cookie: SID=synthetic", "Authorization: Bearer synthetic"});
            std::cout << result.body;
            return result.status == 200 ? 0 : 1;
        }
        ytfast::DownloadOptions options;
        options.connections = 3;
        options.min_parallel_size = 1;
        options.timeout_seconds = 15;
        if (argc > 3 && std::string(argv[3]) == "cancel")
            options.on_progress = [](const ytfast::Progress& p) { return p.downloaded < 1048576; };
        auto result = ytfast::Downloader::fetch(argv[1], argv[2], options);
        if (!result.ok) std::cerr << result.error << '\n';
        return result.ok ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
