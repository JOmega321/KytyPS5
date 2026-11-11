#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LauncherPlatformTests: %s\n", message);
		std::exit(1);
	}
}

void CheckTimeout(const std::string& executable, const char* mode, bool capture) {
	std::string output;
	const auto start = std::chrono::steady_clock::now();
	const int result = Platform::Run({executable, mode}, capture ? &output : nullptr, 100);
	Check(result == -1, "timed-out helper reports failure");
	Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
	      "timeout kills the helper before waiting for its exit");
	int status = 0;
	Check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
	      "timed-out helper is reaped");
}
} // namespace

int main(int argc, char** argv) {
	if (argc > 1) {
		const std::string mode = argv[1];
		if (mode == "--output") {
			std::puts("captured output");
			return 7;
		}
		if (mode == "--close-output") close(STDOUT_FILENO);
		sleep(2);
		return 0;
	}
	const auto executable = fs::read_symlink("/proc/self/exe").string();
	std::string output;
	Check(Platform::Run({executable, "--output"}, &output, 3000) == 7, "preserve exit code");
	Check(output == "captured output\n", "capture standard output");
	Check(Platform::Run({}, nullptr, 100) == -1, "reject an empty command");
	CheckTimeout(executable, "--sleep", true);
	CheckTimeout(executable, "--sleep", false);
	CheckTimeout(executable, "--close-output", true);
	std::puts("LauncherPlatformTests: passed");
}
