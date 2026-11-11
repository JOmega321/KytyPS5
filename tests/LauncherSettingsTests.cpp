#include "settings.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LauncherSettingsTests: %s\n", message);
		std::exit(1);
	}
}

std::string Read(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
	char pattern[] = "/tmp/kyty-settings-XXXXXX";
	const char* directory = mkdtemp(pattern);
	Check(directory != nullptr, "create isolated settings directory");
	const std::filesystem::path root(directory);
	const auto path = root / "launcher-settings.json";
	{
		std::ofstream file(path);
		file << R"({"game":"original","unknown_setting":42})";
	}
	const auto original = Read(path);
	const pid_t child = fork();
	Check(child >= 0, "fork settings failure fixture");
	if (child == 0) {
		Settings settings(path);
		settings.Set("game", "changed");
		rlimit limit {};
		Check(getrlimit(RLIMIT_FSIZE, &limit) == 0, "read file size limit");
		limit.rlim_cur = 0;
		Check(signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore file size limit signal");
		Check(setrlimit(RLIMIT_FSIZE, &limit) == 0, "force buffered write failure");
		_exit(settings.Save() ? 1 : 0);
	}
	int status = 0;
	pid_t waited;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	const bool reported_failure = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	const bool preserved = Read(path) == original;
	Settings settings(path);
	settings.Set("game", "updated");
	const bool saved = settings.Save();
	Settings reloaded(path);
	const bool roundtrip = reloaded.Str("game") == "updated" && reloaded.Int("unknown_setting") == 42;
	settings.read_only = true;
	settings.Set("game", "read-only");
	const auto before = Read(path);
	const bool read_only = settings.Save() && Read(path) == before;
	std::filesystem::remove_all(root);
	Check(reported_failure, "report failure when the buffered write fails on close");
	Check(preserved, "keep previous settings intact after a failed write");
	Check(saved && roundtrip, "save changes and preserve unknown keys");
	Check(read_only, "read-only mode leaves settings untouched");
	std::puts("LauncherSettingsTests: passed");
}
