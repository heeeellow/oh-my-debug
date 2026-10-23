#include "application/device/file_transfer_service.h"
#include <chrono>
#include <fstream>
#include <iostream>
int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::absolute("build/iteration-0.8.0/perf-fixture") /
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
