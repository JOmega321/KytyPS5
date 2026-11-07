#include "local-platform.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "KeptFilesLinuxTests: %s\n", message);
    std::abort();
  }
}
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("kyty-kept-files-" + std::to_string(getpid()));
  Fixture() { std::filesystem::create_directories(root); }
  ~Fixture() { std::filesystem::remove_all(root); }
  std::string Write(const std::string &name, size_t bytes = 65536) {
    const auto path = root / name;
    std::ofstream file(path, std::ios::binary);
    for (size_t i = 0; i < bytes; ++i)
      file.put(static_cast<char>(i % 251));
    file.close();
    Check(!file.fail(), "write fixture");
    return path.string();
  }
};
bool IsOpen(const std::string &path) {
  for (const auto &fd : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    if (std::filesystem::read_symlink(fd.path(), error) == path && !error)
      return true;
  }
  return false;
}
template <class Predicate>
void Await(Predicate predicate, const char *message) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Check(predicate(), message);
}
} // namespace

int main(int argc, char **argv) {
  using namespace LocalPlatform;
  setenv("KYTY_KEPT_FILES", "1", 1);
  setenv("KYTY_FILE_PREFETCH", "1", 1);
  setenv("KYTY_FILE_FOLLOW", "1", 1);
  setenv("KYTY_FILE_OPEN_MAX", "4", 1);
  setenv("KYTY_FILE_IDLE_CLOSE", "1", 1);
  setenv("KYTY_FILE_FOLLOW_WINDOW", "2", 1);
  Fixture fixture;
  const auto path = fixture.Write("asset with spaces.bin");
  uint64_t size = 0;
  if (argc > 1 && std::string(argv[1]) == "--disabled") {
    setenv("KYTY_KEPT_FILES", "0", 1);
    Check(OpenKeptReadFile(path, &size) == 0,
          "disabled cache requests APR fallback");
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--fd-zero") {
    close(STDIN_FILENO);
    const auto handle = OpenKeptReadFile(path, &size);
    Check(handle == 1, "descriptor zero has a nonzero API handle");
    unsigned char byte = 0;
    Check(ReadOpenFileAt(handle, 1, &byte, 1) == 1 && byte == 1,
          "read descriptor zero");
    DoneKeptReadFile(path);
    return 0;
  }
  Check(OpenKeptReadFile((fixture.root / "missing").string(), &size) == 0,
        "missing file falls back");
  Check(OpenKeptReadFile(fixture.root.string(), &size) == 0,
        "directory is not cached");
  const auto held = OpenKeptReadFile(path, &size);
  Check(held != 0 && size == 65536, "open and size");
  Check(OpenKeptReadFile(path, &size) == held,
        "concurrent opens share descriptor");
  DoneKeptReadFile(path);
  std::vector<std::thread> readers;
  for (unsigned t = 0; t < 8; ++t)
    readers.emplace_back([&, t] {
      uint64_t length = 0;
      const auto handle = OpenKeptReadFile(path, &length);
      Check(handle == held, "reader pins shared descriptor");
      std::array<unsigned char, 127> bytes{};
      for (unsigned i = 0; i < 1000; ++i) {
        const uint64_t at = (i * 131u + t * 997u) % (length - bytes.size());
        Check(ReadOpenFileAt(handle, at, bytes.data(), bytes.size()) ==
                  bytes.size(),
              "concurrent positional read size");
        for (size_t j = 0; j < bytes.size(); ++j)
          Check(bytes[j] == (at + j) % 251, "independent offsets");
      }
      DoneKeptReadFile(path);
    });
  for (auto &reader : readers)
    reader.join();
  std::array<unsigned char, 8> tail{};
  Check(ReadOpenFileAt(held, size - 3, tail.data(), tail.size()) == 3,
        "short read at EOF");
  Check(ReadOpenFileAt(held, size, tail.data(), tail.size()) == 0, "EOF");
  Check(ReadOpenFileAt(held, std::numeric_limits<uint64_t>::max(), tail.data(),
                       1) == 0,
        "unrepresentable offset");
  Check(ReadOpenFileAt(0, 0, tail.data(), 1) == 0, "invalid handle");

  std::vector<std::string> pinned;
  for (unsigned i = 0; i < 3; ++i) {
    pinned.push_back(fixture.Write("pin" + std::to_string(i)));
    Check(OpenKeptReadFile(pinned.back(), &size) != 0, "pin cache slot");
  }
  const auto overflow = fixture.Write("overflow");
  Check(OpenKeptReadFile(overflow, &size) == 0, "full pinned cache falls back");
  DoneKeptReadFile(pinned.front());
  const auto replacement = OpenKeptReadFile(overflow, &size);
  Check(replacement != 0, "idle slot can be evicted");
  Check(ReadOpenFileAt(held, 5, tail.data(), 1) == 1 && tail[0] == 5,
        "eviction preserves active reads");
  DoneKeptReadFile(overflow);
  for (size_t i = 1; i < pinned.size(); ++i)
    DoneKeptReadFile(pinned[i]);

  PrefetchKeptFile(path);
  // Pressure the cache while the prefetch worker may be acquiring or using the
  // shared file.
  for (unsigned i = 0; i < 32; ++i) {
    const auto churn = fixture.Write("churn" + std::to_string(i), 17);
    if (OpenKeptReadFile(churn, &size) != 0)
      DoneKeptReadFile(churn);
    Check(ReadOpenFileAt(held, i, tail.data(), 1) == 1 && tail[0] == i,
          "prefetch and eviction preserve pins");
  }
  DoneKeptReadFile(path);
  Await([&] { return !IsOpen(path); }, "idle descriptor is closed");

  const auto first = fixture.Write("first");
  const auto next = fixture.Write("next");
  const auto recording = fixture.root / "reads.files";
  {
    std::ofstream out(recording);
    out << "first\nnext\n";
  }
  FollowKeptFiles(recording.string(), fixture.root.string());
  Check(OpenKeptReadFile(first, &size) != 0, "open recorded first file");
  DoneKeptReadFile(first);
  Await([&] { return IsOpen(next); },
        "recorded successor opened in background");
  std::filesystem::rename(next, fixture.root / "moved");
  const auto ahead = OpenKeptReadFile(next, &size);
  Check(ahead != 0, "prefetched descriptor retained after path rename");
  Check(ReadOpenFileAt(ahead, 42, tail.data(), 1) == 1 && tail[0] == 42,
        "prefetched data correct");
  DoneKeptReadFile(next);
  std::puts("KeptFilesLinuxTests: passed");
}
