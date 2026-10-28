#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs the existing asset script without blocking the GUI. Owns and reaps its process group.
class AssetBuilder {
public:
    struct Status {
        bool running = false;
        int exit_code = -1;
        std::string stage = "Not started", output, log_path;
    };
    ~AssetBuilder();
    bool Start(std::vector<std::string> argv, std::filesystem::path log,
               std::function<void()> wake = {});
    void Cancel();
    Status Snapshot() const;
    static bool Installed(const std::filesystem::path& directory);
private:
    mutable std::mutex mutex_;
    Status status_;
    std::atomic<bool> cancel_ {false};
    std::thread worker_;
};
