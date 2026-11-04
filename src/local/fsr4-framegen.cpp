#include "local/fsr4-framegen.h"

#if defined(__linux__) && defined(__x86_64__)

#include "graphics/host_gpu/graphicContext.h"

#include "fg_runtime.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // NOLINT

namespace Libs::Graphics::Fsr4Fg {
namespace {

namespace fs = std::filesystem;

// Above the guest's address range (src/kernel/memoryAddressSpace.inc: HOST_USER_MAX + 1), which AMD's preferred base
// 0x180000000 is inside of.
constexpr uint64_t AboveGuest = 0xFC00000000ull;
// A compile takes seconds. Past this something is wrong: FSR 3 takes over and AMD's thread is left to itself.
constexpr auto StartingLimit = std::chrono::seconds(120);

struct State {
	std::string dll, prepared, key;
	bool        requested = false, started = false, generated = false, reported = false;
	std::chrono::steady_clock::time_point starting_since {};
	GraphicContext* graphics = nullptr;
} g;

void Say(const char* format, auto... args) {
	std::printf(format, args...);
	std::fflush(stdout);
}

// (The JSON is fsr4fg-gpu's own: a string field on a line of its own.)
std::string JsonString(const std::string& text, const char* name) {
	const std::string opening = std::string("\"") + name + "\": \"";
	const auto        at      = text.find(opening);
	if (at == std::string::npos) return {};
	const auto start = at + opening.size();
	const auto end   = text.find("\"\n", start) == std::string::npos ? text.find("\",\n", start) : std::min(text.find("\"\n", start), text.find("\",\n", start));
	return end == std::string::npos ? std::string {} : text.substr(start, end - start);
}

std::string ReadFile(const fs::path& path) {
	std::ifstream     file(path);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

fs::path ToolPath() {
	if (const char* tool = std::getenv("KYTY_FSR4FG_TOOL"); tool != nullptr && *tool != '\0') return tool;
	char      self[PATH_MAX];
	const auto size = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (size <= 0) return "fsr4fg-gpu";
	self[size] = '\0';
	return fs::path(self).parent_path() / "fsr4fg-gpu";
}

void WaitIdle() {
	if (g.graphics == nullptr) return;
	Common::LockGuard lock(g.graphics->queue_mutex);
	(void)g.graphics->queue.waitIdle();
}

FgRtImage ToImage(const FrameGen::UpscaleImage& image) {
	return {static_cast<VkImage>(image.image),
	        static_cast<VkFormat>(image.image_format != vk::Format::eUndefined ? image.image_format : image.format),
	        image.extent.width,
	        image.extent.height,
	        static_cast<VkImageUsageFlags>(image.usage),
	        static_cast<VkImageLayout>(image.layout)};
}

} // namespace

void Refuse(const char* reason) {
	if (!g.reported) Say("Frame generation: FSR 4 not used, FSR 3 instead: %s\n", reason);
	g.reported  = true;
	g.requested = false;
	g.generated = false;
}

void Initialize() {
	const char* value = std::getenv("KYTY_FSR4FG_DIR");
	if (value == nullptr || *value == '\0') return;
	const fs::path dir(value);
	g.dll      = (dir / "amd_fidelityfx_framegeneration_vk.dll").string();
	g.prepared = (dir / "prepared").string();
	std::error_code error;
	if (!fs::is_regular_file(g.dll, error) || !fs::is_regular_file(fs::path(g.prepared) / "manifest.json", error)) {
		Refuse(("no prepared FSR 4 frame generation files in " + dir.string()).c_str());
		return;
	}
	const auto tool = ToolPath();
	if (!fs::is_regular_file(tool, error)) {
		Refuse(("its self-test program is missing: " + tool.string()).c_str());
		return;
	}
	// RADV exposes VK_NV_cooperative_matrix2 only with this, read as the instance is made (a value the user set stays).
	setenv("radv_cooperative_matrix2_nv", "true", 0);
	const auto  tool_text = tool.string();
	const auto  dir_text  = dir.string();
	const char* arguments[] = {tool_text.c_str(), g.dll.c_str(), g.prepared.c_str(), dir_text.c_str(), "--self-test", nullptr};
	std::fflush(stdout);
	pid_t pid = 0;
	if (const int failed = posix_spawn(&pid, tool_text.c_str(), nullptr, nullptr, const_cast<char* const*>(arguments), environ); failed != 0) {
		Refuse((std::string("cannot start its self-test: ") + std::strerror(failed)).c_str());
		return;
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	const auto verdict = ReadFile(dir / "self-test.json");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		// (0: pass. 1: a verdict with a reason. Anything else: it could not test.)
		const auto reason = WIFEXITED(status) && WEXITSTATUS(status) == 1 ? JsonString(verdict, "reason") : std::string {};
		Refuse(reason.empty() ? "its self-test could not run" : ("self-test: " + reason).c_str());
		return;
	}
	g.key       = JsonString(verdict, "key");
	g.requested = true;
	Say("Frame generation: FSR 4 self-test passed (%s)\n", g.key.c_str());
}

bool Requested() {
	return g.requested;
}

void OnDevice(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device) {
	if (!g.requested || g.started) return;
	vk::PhysicalDeviceDriverProperties driver {};
	vk::PhysicalDeviceProperties2      properties {};
	properties.pNext = &driver;
	physical_device.getProperties2(&properties);
	char identity[600];
	std::snprintf(identity, sizeof(identity), "%04x:%04x:%u:%s:%s", properties.properties.vendorID, properties.properties.deviceID,
	              properties.properties.driverVersion, driver.driverName.data(), driver.driverInfo.data());
	if (g.key.find(identity) == std::string::npos) {
		Refuse((std::string("the self-test ran on another device than this one, ") + identity).c_str());
		return;
	}
	// (From the loader: the dispatcher's entries are the recording layer's, and AMD's thread is none of its business.)
	void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	auto  get_instance_proc_addr = loader != nullptr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
	auto  get_device_proc_addr = get_instance_proc_addr != nullptr
	                                 ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr(static_cast<VkInstance>(instance), "vkGetDeviceProcAddr"))
	                                 : nullptr;
	if (get_device_proc_addr == nullptr) {
		Refuse("the Vulkan loader's entry points were not found");
		return;
	}
	const char* verbose = std::getenv("KYTY_FSR4FG_VERBOSE");
	FgRtConfig  config {};
	config.dll                    = g.dll.c_str();
	config.prepared               = g.prepared.c_str();
	config.provider               = "4.0.2";
	config.instance               = static_cast<VkInstance>(instance);
	config.physical_device        = static_cast<VkPhysicalDevice>(physical_device);
	config.device                 = static_cast<VkDevice>(device);
	config.get_instance_proc_addr = get_instance_proc_addr;
	config.get_device_proc_addr   = get_device_proc_addr;
	config.minimum_address        = AboveGuest;
	config.full_subgroups         = true;
	config.hudless_desc           = true;
	config.verbose                = verbose != nullptr && verbose[0] == '1';
	config.wait_idle              = WaitIdle;
	FgRtStart(&config);
	g.started = true;
}

Result Generate(GraphicContext& graphics, vk::CommandBuffer command, const FsrNative::InterpolationInputs& inputs,
                const FrameGen::UpscaleImage& output, const Camera& camera) {
	if (!g.requested || !g.started) return Result::Unavailable;
	g.graphics = &graphics;
	// KYTY_FRAMEGEN_MV_SIGN=1 (a test): the game's vectors as they are. They point along the motion in UV units; FSR wants
	// render pixels towards the previous position. (The same as FSR 3 built in and the Windows runtime.)
	static const float mv_sign = [] {
		const char* value = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
		return value != nullptr && std::atof(value) > 0 ? 1.0f : -1.0f;
	}();
	FgRtFrame frame {};
	frame.command = static_cast<VkCommandBuffer>(command);
	frame.color   = ToImage(inputs.backbuffer);
	if (inputs.hudless.image != nullptr) frame.hudless = ToImage(inputs.hudless);
	frame.depth   = ToImage(inputs.depth);
	frame.motion  = ToImage(inputs.motion);
	frame.output  = ToImage(output);
	frame.rect[0] = inputs.region.offset.x;
	frame.rect[1] = inputs.region.offset.y;
	frame.rect[2] = static_cast<int32_t>(inputs.region.extent.width);
	frame.rect[3] = static_cast<int32_t>(inputs.region.extent.height);
	// (AMD's sample negates the projection's offset, as the Windows path does; jitter_px is FSR 3's sign.)
	frame.jitter[0]       = -inputs.jitter_px[0];
	frame.jitter[1]       = -inputs.jitter_px[1];
	frame.motion_scale[0] = mv_sign * static_cast<float>(inputs.motion.extent.width);
	frame.motion_scale[1] = mv_sign * static_cast<float>(inputs.motion.extent.height);
	frame.frame_ms        = inputs.frame_ms;
	frame.near_plane      = inputs.near_plane;
	frame.fov_y           = inputs.fov_y;
	frame.reset           = inputs.reset;
	frame.pq              = inputs.pq;
	for (size_t i = 0; i < 3; ++i) {
		frame.position[i] = camera.position[i];
		frame.up[i]       = camera.up[i];
		frame.right[i]    = camera.right[i];
		frame.forward[i]  = camera.forward[i];
	}
	switch (FgRtGenerate(&frame)) {
		case FG_RT_GENERATED:
			if (!g.generated) {
				const auto stats = FgRtGetStats();
				Say("Frame generation: FSR %s %ux%u, depth and motion %ux%u, 2x (AMD's runtime on its own thread: %u pipelines in %.1f s, %.1f MB)\n",
				    FgRtActiveProvider(), frame.color.width, frame.color.height, frame.depth.width, frame.depth.height, stats.pipelines,
				    stats.create_seconds, static_cast<double>(stats.memory_bytes) / 1e6);
			}
			g.generated      = true;
			g.starting_since = {};
			return Result::Generated;
		case FG_RT_NOT_MADE:
			g.starting_since = {};
			return Result::NoFrame;
		case FG_RT_NOT_YET: {
			const auto now = std::chrono::steady_clock::now();
			if (g.starting_since == std::chrono::steady_clock::time_point {}) g.starting_since = now;
			if (now - g.starting_since < StartingLimit) return Result::Starting;
			Refuse("AMD's context was not ready after two minutes");
			return Result::Unavailable;
		}
		case FG_RT_UNAVAILABLE: break;
	}
	Refuse(FgRtReason());
	return Result::Unavailable;
}

bool Generating() {
	return g.requested && g.generated;
}

} // namespace Libs::Graphics::Fsr4Fg

#endif
