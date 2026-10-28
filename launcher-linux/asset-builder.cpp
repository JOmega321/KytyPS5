#include "asset-builder.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

AssetBuilder::~AssetBuilder() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}
void AssetBuilder::Cancel() { cancel_.store(true); }
AssetBuilder::Status AssetBuilder::Snapshot() const {
    std::lock_guard lock(mutex_);
    return status_;
}
bool AssetBuilder::Installed(const std::filesystem::path& directory) {
    std::ifstream marker(directory / "ASSET_VERSION");
    std::string version;
    std::getline(marker, version);
    if (version != "FSR412_VULKAN_INT8_4.1.2.4555") return false;
    // A complete INT8 set for both output tiers and quality model families.
    static const char* passes[] = {"spd", "prepass", "pass0_post", "pass1", "pass1_post", "pass2", "pass2_post",
        "pass3", "pass3_post", "pass4", "pass4_post", "pass5", "pass5_post", "pass6", "pass6_post", "pass7",
        "pass7_post", "pass8", "pass8_post", "pass9", "pass9_post", "pass10", "pass10_post", "pass11",
        "pass11_post", "pass12", "pass12_post", "postpass", "rcas"};
    std::error_code error;
    for (const char* set : {"t1080_m0", "t1080_m1", "t2160_m0", "t2160_m1"}) {
        auto path = directory / set;
        if (std::filesystem::file_size(path / "initializer.bin", error) != 131072 || error) return false;
        for (const char* pass : passes) {
            std::ifstream file(path / (std::string(pass) + ".spv"), std::ios::binary);
            unsigned char magic[4] {};
            if (!file.read(reinterpret_cast<char*>(magic), 4) || magic[0] != 3 || magic[1] != 2 || magic[2] != 35 || magic[3] != 7)
                return false;
        }
    }
    return true;
}
bool AssetBuilder::Start(std::vector<std::string> argv, std::filesystem::path log, std::function<void()> wake) {
    if (Snapshot().running || argv.empty()) return false;
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    {
        std::lock_guard lock(mutex_);
        status_ = {true, -1, "Starting asset builder", "", log.string()};
    }
    worker_ = std::thread([this, argv = std::move(argv), log = std::move(log), wake = std::move(wake)]() mutable {
        auto notify = [&] { if (wake) wake(); };
        auto finish = [&](int code, const std::string& message) {
            { std::lock_guard lock(mutex_); status_.running = false; status_.exit_code = code; status_.stage = message; }
            notify();
        };
        std::error_code error;
        std::filesystem::create_directories(log.parent_path(), error);
        std::ofstream file(log, std::ios::binary | std::ios::trunc);
        if (!file) { finish(-1, "Could not create build log: " + log.string()); return; }
        int pipe_fds[2];
        if (pipe2(pipe_fds, O_CLOEXEC) != 0) { finish(-1, "Could not create output pipe"); return; }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        std::vector<char*> args;
        for (auto& arg : argv) args.push_back(arg.data());
        args.push_back(nullptr);
        pid_t pid = -1;
        const int spawn_error = posix_spawnp(&pid, args[0], &actions, &attributes, args.data(), environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_fds[1]);
        if (spawn_error) {
            close(pipe_fds[0]); finish(-1, "Could not start asset builder: " + std::string(std::strerror(spawn_error))); return;
        }
        fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
        std::string pending;
        auto append = [&](const char* data, size_t size) {
            file.write(data, size); file.flush();
            pending.append(data, size);
            {
                std::lock_guard lock(mutex_);
                status_.output.append(data, size);
                if (status_.output.size() > 128 * 1024) status_.output.erase(0, status_.output.size() - 128 * 1024);
                size_t end;
                while ((end = pending.find_first_of("\r\n")) != std::string::npos) {
                    auto line = pending.substr(0, end); pending.erase(0, end + 1);
                    for (const std::string prefix : {"[FSR411] ", "[FSR412] "})
                        if (line.rfind(prefix, 0) == 0) status_.stage = line.substr(prefix.size());
                }
                if (pending.size() > 8192) pending.erase(0, pending.size() - 8192);
            }
            notify();
        };
        int status = 0;
        bool stopped = false;
        auto stop_time = std::chrono::steady_clock::time_point{};
        for (;;) {
            if (cancel_.load()) {
                if (!stopped) {
                    kill(-pid, SIGTERM); stopped = true; stop_time = std::chrono::steady_clock::now();
                    { std::lock_guard lock(mutex_); status_.stage = "Cancelling..."; }
                    notify();
                } else if (std::chrono::steady_clock::now() - stop_time > std::chrono::seconds(2)) kill(-pid, SIGKILL);
            }
            pollfd entry {pipe_fds[0], POLLIN, 0};
            poll(&entry, 1, 100);
            char buffer[4096]; ssize_t n;
            while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) append(buffer, static_cast<size_t>(n));
            auto done = waitpid(pid, &status, WNOHANG);
            if (done == pid || (done < 0 && errno != EINTR)) break;
        }
        // Reap descendants too on cancellation (a capture/tool must not survive closing the launcher).
        if (stopped) kill(-pid, SIGKILL);
        char buffer[4096]; ssize_t n;
        while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) append(buffer, static_cast<size_t>(n));
        close(pipe_fds[0]);
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        finish(stopped ? -1 : code, stopped ? "Cancelled" : code == 0 ? "Asset generation finished" :
               "Build failed (exit " + std::to_string(code) + ") - see the output below");
    });
    return true;
}
