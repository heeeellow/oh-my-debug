#include "test_paths.h"
#include "application/device/file_transfer_service.h"
#include <chrono>
#include <fstream>
#include <iostream>
int main(int argc, char** argv)
{
    // 明确选择的设备只做目录/属性读取，不进入下面的本地文件创建样本。
    if (argc == 3 && std::string(argv[1]) == "--live-read-only") {
        auto service = testtools::createFileTransferService();
        std::atomic<bool> cancel{false};
        for (const auto& directory : {"/", "/proc", "/sdcard/oem_log"}) {
            const auto begin = std::chrono::steady_clock::now();
            const auto listing = service->listRemote({argv[2], "device", ""}, directory, cancel);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            if (!listing.error.empty()) { std::cerr << directory << ": " << listing.error << '\n'; return 1; }
            std::cout << "{\"path\":\"" << directory << "\",\"milliseconds\":" << ms
                      << ",\"entries\":" << listing.entries.size() << "}\n";
            if (std::string(directory) == "/") {
                for (const auto& item : listing.entries) if (item.name == "sdcard" && !item.browsable()) {
                    std::cerr << "sdcard directory link must stay browsable\n"; return 1;
                }
            }
        }
        return 0;
    }
    if (argc != 1) return 2;
    namespace fs = std::filesystem;
    const auto root = testOutputDirectory() / "perf-fixture" /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    for (int n : {1000, 10000}) {
        for (int i = 0; i < n; ++i) {
            std::ofstream f(root / ("item-" + std::to_string(i) + ".log"));
        }
        std::atomic<bool> cancel{false};
        const auto begin = std::chrono::steady_clock::now();
        auto listing = testtools::FileTransferService::listLocal(root, cancel);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
        if (!listing.error.empty() || listing.entries.size() != n)
            return 1;
        std::cout << n << " real local metadata listing=" << ms << "ms\n";
    }
    return 0;
}
