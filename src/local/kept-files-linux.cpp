// Linux APR file retention and bounded read-ahead. Guest memory is still written by libAmpr,
// after PrepareHostOverwrite; the worker reads only into its own staging buffer.
#if defined(__linux__)
#include "local-platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <linux/ioprio.h>
#include <list>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LocalPlatform {
namespace {
using Clock                      = std::chrono::steady_clock;
constexpr uint64_t AheadBytes    = 256 * 1024;
constexpr uint64_t StreamBytes   = 16 * 1024 * 1024;
constexpr uint64_t PendingBytes  = 32 * 1024 * 1024;
constexpr size_t   PendingFiles  = 32;
constexpr size_t   RecordedFiles = 262144;

uint64_t Setting(const char* name, uint64_t fallback, uint64_t maximum) {
	const char* text = std::getenv(name);
	if (text == nullptr || *text == '\0' || *text == '-') return fallback;
	char* end        = nullptr;
	errno            = 0;
	const auto value = std::strtoull(text, &end, 10);
	return errno == 0 && end != text && *end == '\0' ? std::min<uint64_t>(value, maximum)
	                                                 : fallback;
}

size_t FileLimit() {
	rlimit   limit {};
	uint64_t available = 512;
	if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
		available =
		    limit.rlim_cur > 64 ? std::min<uint64_t>(limit.rlim_cur / 4, limit.rlim_cur - 64) : 0;
	return std::min<uint64_t>(Setting("KYTY_FILE_OPEN_MAX", 512, 4096), available);
}

uint32_t ReadAt(uint64_t handle, uint64_t offset, void* buffer, uint32_t size) {
	if (handle == 0 || handle - 1 > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
	    offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
		return 0;
	ssize_t got;
	do {
		got = pread(static_cast<int>(handle - 1), buffer, size, static_cast<off_t>(offset));
	} while (got < 0 && errno == EINTR);
	return got > 0 ? static_cast<uint32_t>(got) : 0;
}

class KeptFiles {
	struct File {
		int                              fd      = -1;
		uint64_t                         size    = 0;
		uint32_t                         readers = 0;
		bool                             idle = false, prefetched = false, observed = false;
		Clock::time_point                last {};
		std::list<std::string>::iterator position;
		~File() {
			if (fd >= 0) close(fd);
		}
	};
	struct Job {
		std::string path;
		uint64_t    bytes;
	};

public:
	KeptFiles()
	    : enabled(Setting("KYTY_KEPT_FILES", 1, 1) != 0), maximum(FileLimit()),
	      idle_time(Setting("KYTY_FILE_IDLE_CLOSE", 10, 86400)),
	      prefetch(Setting("KYTY_FILE_PREFETCH", 1, 1) != 0),
	      window(Setting("KYTY_FILE_FOLLOW_WINDOW", 8, PendingFiles)) {
		if (enabled && maximum != 0) {
			try {
				worker = std::thread([this] { Work(); });
			} catch (const std::system_error&) {
				enabled = false;
			}
		}
	}
	~KeptFiles() {
		{
			std::lock_guard lock(mutex);
			stop = true;
		}
		ready.notify_one();
		if (worker.joinable()) worker.join();
	}

	uint64_t Open(const std::string& path, uint64_t* size, bool demand = true) {
		if (!enabled || maximum == 0) return 0;
		{
			std::lock_guard lock(mutex);
			Expire();
			if (const auto found = files.find(path); found != files.end()) {
				Activate(*found->second);
				if (size != nullptr) *size = found->second->size;
				if (demand && !std::exchange(found->second->observed, true)) FollowOpen(path);
				return static_cast<uint64_t>(found->second->fd) + 1;
			}
			if (files.size() >= maximum && idle.empty()) return 0;
		}
		// Open and stat misses outside the cache mutex. Nonblocking rejects special files
		// without hanging on a FIFO; only regular immutable game assets enter the cache.
		auto opened = std::make_unique<File>();
		do {
			opened->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		} while (opened->fd < 0 && errno == EINTR);
		struct stat stat {};
		if (opened->fd < 0 || fstat(opened->fd, &stat) != 0 || !S_ISREG(stat.st_mode) ||
		    stat.st_size < 0)
			return 0;
		opened->size = static_cast<uint64_t>(stat.st_size);
		std::lock_guard lock(mutex);
		auto            found = files.find(path);
		if (found == files.end()) {
			while (files.size() >= maximum && !idle.empty())
				Evict();
			// If every descriptor is in use, the caller uses APR's ordinary read path.
			if (files.size() >= maximum) return 0;
			found = files.emplace(path, std::move(opened)).first;
		}
		Activate(*found->second);
		if (size != nullptr) *size = found->second->size;
		if (demand && !std::exchange(found->second->observed, true)) FollowOpen(path);
		return static_cast<uint64_t>(found->second->fd) + 1;
	}

	void Done(const std::string& path) {
		std::lock_guard lock(mutex);
		const auto      found = files.find(path);
		if (found == files.end() || found->second->readers == 0) return;
		auto& file = *found->second;
		if (--file.readers == 0) {
			file.last = Clock::now();
			idle.push_back(path);
			file.position = std::prev(idle.end());
			file.idle     = true;
		}
	}

	void Prefetch(const std::string& path) {
		std::lock_guard lock(mutex);
		const auto      found = files.find(path);
		if (found == files.end() || found->second->prefetched) return;
		// Explicit audio-stream requests have priority over recorded look-ahead.
		if (Queue(path, std::min(found->second->size, StreamBytes), true))
			found->second->prefetched = true;
	}

	void Follow(const std::string& recording, const std::string& root) {
		if (!enabled || !prefetch || Setting("KYTY_FILE_FOLLOW", 1, 1) == 0) return;
		std::lock_guard lock(mutex);
		if (following) return;
		following = true;
		base      = std::filesystem::path(root).lexically_normal();
		std::ifstream input(recording);
		std::string   relative;
		while (paths.size() < RecordedFiles && std::getline(input, relative)) {
			if (!relative.empty() && relative.back() == '\r') relative.pop_back();
			if (relative.empty()) continue;
			const auto path  = (base / relative).lexically_normal();
			const auto under = path.lexically_relative(base);
			if (under.empty() || *under.begin() == "..") continue;
			AddPath(path.string());
		}
		std::error_code error;
		std::filesystem::create_directories(std::filesystem::path(recording).parent_path(), error);
		record.open(recording, std::ios::app);
	}

private:
	void Activate(File& file) {
		if (file.idle) {
			idle.erase(file.position);
			file.idle = false;
		}
		++file.readers;
	}
	void Evict() {
		files.erase(idle.front());
		idle.pop_front();
	}
	void Expire() {
		if (idle_time.count() == 0) return;
		const auto now = Clock::now();
		while (!idle.empty() && now - files.at(idle.front())->last >= idle_time)
			Evict();
	}
	void AddPath(const std::string& path) {
		if (indices.emplace(path, paths.size()).second) {
			paths.push_back(path);
			followed.push_back(false);
		}
	}
	bool Queue(const std::string& path, uint64_t bytes, bool priority) {
		if (!prefetch || bytes == 0 || stop) return false;
		if (pending.contains(path)) {
			// Upgrade a queued prefix to a whole small stream; an executing prefix can be retried
			// later.
			for (auto& job: jobs)
				if (job.path == path) {
					if (bytes > job.bytes && bytes - job.bytes <= PendingBytes - pending_bytes) {
						pending_bytes += bytes - job.bytes;
						job.bytes = bytes;
					}
					return job.bytes >= bytes;
				}
			return false;
		}
		if (pending.size() >= PendingFiles || bytes > PendingBytes - pending_bytes) return false;
		pending.insert(path);
		pending_bytes += bytes;
		if (priority)
			jobs.push_front({path, bytes});
		else
			jobs.push_back({path, bytes});
		ready.notify_one();
		return true;
	}
	void FollowOpen(const std::string& path) {
		if (!following) return;
		auto found = indices.find(path);
		if (found == indices.end()) {
			if (paths.size() >= RecordedFiles) return;
			const auto relative = std::filesystem::path(path).lexically_relative(base);
			if (relative.empty() || *relative.begin() == "..") return;
			AddPath(path);
			found = indices.find(path);
			if (record) {
				record << relative.generic_string() << '\n';
				if (++unflushed == 64) {
					record.flush();
					unflushed = 0;
				}
			}
		}
		const size_t at = found->second;
		followed[at]    = true;
		for (size_t next = at + 1; next < std::min(paths.size(), at + 1 + window); ++next) {
			if (!followed[next] && Queue(paths[next], AheadBytes, false)) followed[next] = true;
		}
	}
	void Work() {
		(void)pthread_setname_np(pthread_self(), "Kyty.FileAhead");
		(void)setpriority(PRIO_PROCESS, 0, 10);
		(void)syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
		              IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0));
		std::vector<uint8_t> buffer(prefetch ? 1u << 20u : 0u);
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				ready.wait_for(lock, std::chrono::seconds(1),
				               [this] { return stop || !jobs.empty(); });
				Expire();
				if (stop) return;
				if (jobs.empty()) continue;
				job = std::move(jobs.front());
				jobs.pop_front();
			}
			uint64_t   size   = 0;
			const auto handle = Open(job.path, &size, false);
			if (handle != 0) {
				for (uint64_t at = 0, end = std::min(size, job.bytes); at < end;) {
					{
						std::lock_guard lock(mutex);
						if (stop) break;
					}
					const auto got =
					    ReadAt(handle, at, buffer.data(),
					           static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), end - at)));
					if (got == 0) break;
					at += got;
				}
				Done(job.path);
			}
			std::lock_guard lock(mutex);
			pending.erase(job.path);
			pending_bytes -= job.bytes;
		}
	}

	bool                                                   enabled;
	const size_t                                           maximum;
	const std::chrono::seconds                             idle_time;
	const bool                                             prefetch;
	const size_t                                           window;
	std::mutex                                             mutex;
	std::condition_variable                                ready;
	std::unordered_map<std::string, std::unique_ptr<File>> files;
	std::list<std::string>                                 idle;
	std::deque<Job>                                        jobs;
	std::unordered_set<std::string>                        pending;
	uint64_t                                               pending_bytes = 0;
	bool                                                   stop = false, following = false;
	std::filesystem::path                                  base;
	std::vector<std::string>                               paths;
	std::unordered_map<std::string, size_t>                indices;
	std::vector<bool>                                      followed;
	std::ofstream                                          record;
	uint32_t                                               unflushed = 0;
	std::thread                                            worker;
};

KeptFiles& Files() {
	static KeptFiles files;
	return files;
}
} // namespace

uint64_t OpenKeptReadFile(const std::string& path, uint64_t* size) {
	return Files().Open(path, size);
}
uint32_t ReadOpenFileAt(uint64_t file, uint64_t offset, void* buffer, uint32_t size) {
	return ReadAt(file, offset, buffer, size);
}
void DoneKeptReadFile(const std::string& path) {
	Files().Done(path);
}
void PrefetchKeptFile(const std::string& path) {
	Files().Prefetch(path);
}
void FollowKeptFiles(const std::string& recording, const std::string& root) {
	Files().Follow(recording, root);
}
} // namespace LocalPlatform
#endif
