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
    static bool InstalledFp8(const std::filesystem::path& directory);
    // FSR 4 frame generation's files (tools/fsr4fg-vk/build_assets.sh): AMD's DLL and the prepared programs.
    static bool InstalledFrameGen(const std::filesystem::path& directory);
    static std::filesystem::path FrameGenFolder(const std::filesystem::path& root);
private:
    mutable std::mutex mutex_;
    Status status_;
    std::atomic<bool> cancel_ {false};
    std::thread worker_;
};
