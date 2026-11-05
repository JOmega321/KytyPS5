#ifndef EMULATOR_SRC_LOCAL_FRAME_CSV_H_
#define EMULATOR_SRC_LOCAL_FRAME_CSV_H_
// KYTY_FRAME_CSV=<file> (opt-in, for A/Bs such as FSR 4 against FSR 3 frame generation): one line per game frame the
// presenter gets, with its flip-to-flip time, the render thread's busy part of it, and frame generation's cost: the CPU
// time recording it (with the renderer's lock held) and its GPU time. Each call costs one relaxed-style check when off.
// tools/local/frame-csv-summary.py prints mean, p50, p95, p99 and max per column.
#include "graphics/host_gpu/graphicContext.h"

#include <cstdint>

namespace Libs::Graphics::FrameCsv {

// Present thread, for each new game frame (not a refresh of the last one).
void GameFrame();
// Present thread, around frame generation's recording into `command`.
void BeginGenerate(GraphicContext& graphics, vk::CommandBuffer command);
void EndGenerate(vk::CommandBuffer command, uint32_t generated_frames, const char* name);
// Present thread: how long the frame's GPU work took to end after its generated frame was presented (KYTY_FG_PACE_GPU=1).
void NotePaceWait(double ms);

} // namespace Libs::Graphics::FrameCsv

#endif // EMULATOR_SRC_LOCAL_FRAME_CSV_H_
