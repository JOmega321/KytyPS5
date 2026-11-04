#ifndef EMULATOR_SRC_LOCAL_FSR4_FRAMEGEN_H_
#define EMULATOR_SRC_LOCAL_FSR4_FRAMEGEN_H_
// AMD FSR Frame Generation 4.0.2 on Linux (KYTY_FSR4FG_DIR, with KYTY_FRAMEGEN_BACKEND=fsr): AMD's own runtime
// (amd_fidelityfx_framegeneration_vk.dll of FidelityFX SDK 2.4.0, MIT, the user's copy) run without Wine by
// tools/fsr4fg-vk/fg_runtime.c, on a thread of its own, with the programs prepare_fg.py prepared for RADV. Same frames in
// the same place as FSR 3 built in (fsr-native.h), which stays the fallback whatever goes wrong:
// - At process start fsr4fg-gpu --self-test runs AMD's runtime in a child: a crash in the driver, which nothing in this
//   process could catch, costs that child only. Its verdict is kept (KYTY_FSR4FG_DIR/self-test.json) until the driver,
//   GPU, DLL, programs or test change.
// - Every reason FSR 3 is used instead is logged once, as "Frame generation: FSR 4 not used, FSR 3 instead: ...".
// KYTY_FSR4FG_DIR holds amd_fidelityfx_framegeneration_vk.dll and prepared/ (tools/fsr4fg-vk/build_assets.sh).
// KYTY_FSR4FG_TOOL: the self-test program, by default beside the emulator. KYTY_FSR4FG_VERBOSE=1: AMD's messages.
#include "local/fsr-native.h"

#include <array>

namespace Libs::Graphics {
struct GraphicContext;
namespace Fsr4Fg {

struct Camera {
	std::array<float, 3> position {}, up {0.0f, 1.0f, 0.0f}, right {1.0f, 0.0f, 0.0f}, forward {0.0f, 0.0f, 1.0f};
};
enum class Result {
	Generated,   // recorded: the output will hold the frame between the last one and this
	NoFrame,     // recorded, nothing to show (a reset)
	Starting,    // nothing recorded: AMD's pipelines are compiling on their own thread
	Unavailable, // from now on FSR 3 (the reason is logged)
};

#if defined(__linux__) && defined(__x86_64__)
// Process start, before the Vulkan instance: runs the self-test when KYTY_FSR4FG_DIR asks for FSR 4.
void Initialize();
// After Initialize: FSR 4 will be tried on this device (which then needs what AMD's programs declare).
[[nodiscard]] bool Requested();
// Before the device is made: it cannot run AMD's programs.
void Refuse(const char* reason);
// The device exists: AMD's thread starts, if it is the device the self-test ran on.
void OnDevice(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device);
// Present thread: AMD's work for this frame into `command`.
[[nodiscard]] Result Generate(GraphicContext& graphics, vk::CommandBuffer command, const FsrNative::InterpolationInputs& inputs,
                              const FrameGen::UpscaleImage& output, const Camera& camera);
// FSR 4 has generated, and has not stopped (the KYTY_FPS_HUD panel's name).
[[nodiscard]] bool Generating();
#else
inline void Initialize() {}
[[nodiscard]] inline bool Requested() { return false; }
inline void Refuse(const char* /*reason*/) {}
inline void OnDevice(vk::Instance /*instance*/, vk::PhysicalDevice /*physical_device*/, vk::Device /*device*/) {}
[[nodiscard]] inline Result Generate(GraphicContext& /*graphics*/, vk::CommandBuffer /*command*/,
                                     const FsrNative::InterpolationInputs& /*inputs*/, const FrameGen::UpscaleImage& /*output*/,
                                     const Camera& /*camera*/) {
	return Result::Unavailable;
}
[[nodiscard]] inline bool Generating() { return false; }
#endif

} // namespace Fsr4Fg
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_LOCAL_FSR4_FRAMEGEN_H_
