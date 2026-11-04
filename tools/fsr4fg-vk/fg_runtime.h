// SPDX-License-Identifier: GPL-2.0-or-later
// fg_runtime: AMD's FSR Frame Generation 4.0.2 runtime (amd_fidelityfx_framegeneration_vk.dll, SDK 2.4.0) on a Linux
// Vulkan device, for whoever records the frames: the standalone test (gpu_fg.c) and the emulator run the same code.
//
// All of AMD's code runs on one thread of its own (a Windows TEB and a large stack, pehost): the caller's thread needs
// nothing special and never stalls on the first compile of AMD's pipelines. Whatever stops AMD's runtime (a call the host
// does not serve, a failed create) leaves the state FG_RT_FAILED with a reason, and the caller goes on without it. A
// crash in the driver cannot be caught here: fsr4fg-gpu --self-test answers that beforehand, from a child process.
#ifndef KYTY_TOOLS_FSR4FG_FG_RUNTIME_H_
#define KYTY_TOOLS_FSR4FG_FG_RUNTIME_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FgRtState { FG_RT_NOT_STARTED, FG_RT_LOADING, FG_RT_READY, FG_RT_FAILED } FgRtState;

typedef struct FgRtConfig {
	const char*               dll;      // SDK 2.4.0's amd_fidelityfx_framegeneration_vk.dll (the caller checks its hash)
	const char*               prepared; // prepare_fg.py's output: replaces AMD's programs where RADV needs it
	const char*               provider; // the provider to run, e.g. "4.0.2"
	VkInstance                instance;
	VkPhysicalDevice          physical_device;
	VkDevice                  device;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	PFN_vkGetDeviceProcAddr   get_device_proc_addr;
	uint64_t                  minimum_address; // never map the image below this (the emulator's guest range)
	bool                      full_subgroups;  // matrix pipelines get full subgroups (RADV needs them)
	bool                      hudless_desc;    // the context is told a HUD-less image will come
	bool                      verbose;         // a line per pipeline, AMD's messages
	bool                      probe;           // load and list the providers only
	void (*on_thread_start)(void);             // on AMD's thread before anything else (crash reports)
	void (*wait_idle)(void);                   // no GPU work in flight: before a context is destroyed (a new size)
} FgRtConfig;

typedef struct FgRtImage {
	VkImage           image;
	VkFormat          format; // the image's own (a depth image's: D32S8)
	uint32_t          width, height;
	VkImageUsageFlags usage;
	VkImageLayout     layout; // as the command buffer leaves it for AMD, who puts it back
} FgRtImage;

typedef struct FgRtFrame {
	VkCommandBuffer command;
	FgRtImage       color;   // the finished frame, at the display size
	FgRtImage       hudless; // the same without the HUD (image null: none)
	FgRtImage       depth, motion; // at the render size
	FgRtImage       output;  // receives the generated frame: general layout, storage
	int32_t         rect[4]; // x, y, width, height of the game's picture in `color`
	float           jitter[2];       // pixels, as FSR's sample negates them
	float           motion_scale[2]; // takes the vectors to render pixels towards the previous position
	float           frame_ms, near_plane, fov_y;
	float           position[3], up[3], right[3], forward[3];
	bool            reset; // nothing before this frame to generate from
	bool            pq;    // `color` is HDR10 (ST 2084 PQ), not sRGB
} FgRtFrame;

typedef enum FgRtResult {
	FG_RT_GENERATED,   // recorded: `output` will hold the frame between the last one and this
	FG_RT_NOT_MADE,    // recorded, nothing to show (a reset)
	FG_RT_NOT_YET,     // nothing recorded: AMD's thread is loading or compiling; call again next frame
	FG_RT_UNAVAILABLE, // nothing to rely on from now on: FgRtReason says why (some commands may have been recorded)
} FgRtResult;

typedef struct FgRtStats {
	unsigned replaced, as_shipped, matrix_pipelines, pipelines, pipeline_failures, thunks_bound, thunks_total;
	unsigned queue_calls; // vkGetDeviceQueue, vkQueueSubmit*, vkDeviceWaitIdle made by AMD's code
	double   create_seconds, compile_seconds;
	uint64_t memory_bytes;
	uintptr_t image_base;
	size_t    image_size;
} FgRtStats;

// Any thread, once: starts AMD's thread with `config`, whose strings and handles must stay valid.
void FgRtStart(const FgRtConfig* config);
// The thread that records the frames: records AMD's work for `frame` into frame->command. A context of the frame's size
// is made in the background, so the first frames answer FG_RT_NOT_YET; a new size makes it again.
FgRtResult FgRtGenerate(const FgRtFrame* frame);

FgRtState   FgRtGetState(void);
void        FgRtWaitLoaded(void); // until the DLL is loaded or failed
const char* FgRtReason(void);     // why it failed; "" when it did not
bool        FgRtStopped(void);    // it failed by something stopping AMD's runtime itself (PE_EXIT_FATAL in the test)
bool        FgRtNotOffered(void); // it failed because AMD's runtime does not offer that provider on this device
void        FgRtProviders(const char** names, unsigned* count); // what AMD's runtime offers (FgRtWaitLoaded first)
const char* FgRtActiveProvider(void); // the context's provider, "" until there is one
FgRtStats   FgRtGetStats(void);
// A test: makes AMD's thread stop as a call the host does not serve would.
void        FgRtInjectStop(void);

// What AMD's thread is doing, for a crash report (async-signal-safe to read). Null when idle.
extern const char* volatile fg_rt_stage;
extern char                 fg_rt_stage_detail[256];

void FgRtSha256Hex(const void* data, size_t size, char hex[65]);

#ifdef __cplusplus
}
#endif
#endif
