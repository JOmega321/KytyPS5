#include "local/frame-csv.h"

#include "local/perf-overlay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace Libs::Graphics::FrameCsv {
namespace {

constexpr uint32_t Slots = 32; // frames in flight at most; a result not there by then is left blank

struct Row {
	double      time_ms = 0, game_frame_ms = 0, busy_ms = 0, generate_cpu_ms = -1;
	uint32_t    generated = 0;
	int         slot = -1;
	const char* name = "";
};

using Clock = std::chrono::steady_clock;

FILE* Output() {
	static FILE* file = [] {
		const char* path = std::getenv("KYTY_FRAME_CSV");
		if (path == nullptr || *path == '\0') return static_cast<FILE*>(nullptr);
		FILE* opened = std::fopen(path, "w");
		if (opened == nullptr) {
			std::printf("KYTY_FRAME_CSV: cannot write %s\n", path);
			return static_cast<FILE*>(nullptr);
		}
		std::fprintf(opened, "time_ms,game_frame_ms,render_busy_ms,generated,generate_cpu_ms,generate_gpu_ms,generator\n");
		std::printf("KYTY_FRAME_CSV: writing %s\n", path);
		return opened;
	}();
	return file;
}

const Clock::time_point start = Clock::now();
Clock::time_point       last_frame {}, generate_start {};
std::deque<Row>         rows;
GraphicContext*         graphics_used = nullptr;
vk::QueryPool           pool = nullptr;
uint32_t                next_slot = 0;
double                  period_ns = 1.0;

void Write(const Row& row, double gpu_ms) {
	std::fprintf(Output(), "%.3f,%.3f,%.3f,%u,", row.time_ms, row.game_frame_ms, row.busy_ms, row.generated);
	if (row.generate_cpu_ms >= 0) std::fprintf(Output(), "%.3f", row.generate_cpu_ms);
	std::fputc(',', Output());
	if (gpu_ms >= 0) std::fprintf(Output(), "%.3f", gpu_ms);
	std::fprintf(Output(), ",%s\n", row.name);
}

// Lines in order, as their GPU times arrive; one that never does (a dropped submit) is written without it.
void Flush() {
	while (!rows.empty()) {
		const Row& row = rows.front();
		double     gpu_ms = -1;
		if (row.slot >= 0) {
			uint64_t values[4] {};
			const auto result = graphics_used->device.getQueryPoolResults(pool, 2 * static_cast<uint32_t>(row.slot), 2, sizeof(values), values, 2 * sizeof(uint64_t),
			                                                              vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
			const bool ready = result == vk::Result::eSuccess && values[1] != 0 && values[3] != 0;
			if (!ready && rows.size() < Slots) return;
			if (ready) gpu_ms = static_cast<double>(values[2] - values[0]) * period_ns / 1e6;
		}
		Write(row, gpu_ms);
		rows.pop_front();
	}
	std::fflush(Output());
}

} // namespace

void GameFrame() {
	if (Output() == nullptr) return;
	const auto now = Clock::now();
	Row        row;
	row.time_ms = std::chrono::duration<double, std::milli>(now - start).count();
	if (last_frame != Clock::time_point {}) row.game_frame_ms = std::chrono::duration<double, std::milli>(now - last_frame).count();
	last_frame = now;
	std::vector<float> frame_ms, busy_ms;
	PerfOverlay::RecentFrames(1, &frame_ms, &busy_ms);
	if (!busy_ms.empty()) row.busy_ms = busy_ms.back();
	Flush();
	rows.push_back(row);
}

void BeginGenerate(GraphicContext& graphics, vk::CommandBuffer command) {
	if (Output() == nullptr || rows.empty()) return;
	if (pool == nullptr) {
		graphics_used = &graphics;
		period_ns     = static_cast<double>(graphics.GetPhysicalDeviceProperties().limits.timestampPeriod);
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = 2 * Slots;
		if (graphics.device.createQueryPool(&info, nullptr, &pool) != vk::Result::eSuccess) pool = nullptr;
		if (pool != nullptr) command.resetQueryPool(pool, 0, 2 * Slots);
	}
	if (pool != nullptr) {
		rows.back().slot = static_cast<int>(next_slot);
		// (This slot's last use is long since written out: Flush never leaves more than Slots lines waiting.)
		command.resetQueryPool(pool, 2 * next_slot, 2);
		command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, 2 * next_slot);
	}
	generate_start = Clock::now();
}

void EndGenerate(vk::CommandBuffer command, uint32_t generated_frames, const char* name) {
	if (Output() == nullptr || rows.empty() || generate_start == Clock::time_point {}) return;
	Row& row             = rows.back();
	row.generate_cpu_ms  = std::chrono::duration<double, std::milli>(Clock::now() - generate_start).count();
	row.generated        = generated_frames;
	row.name             = name;
	generate_start       = {};
	if (row.slot >= 0) {
		command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, 2 * static_cast<uint32_t>(row.slot) + 1);
		next_slot = (next_slot + 1) % Slots;
	}
}

} // namespace Libs::Graphics::FrameCsv
