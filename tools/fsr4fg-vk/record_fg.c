// SPDX-License-Identifier: GPL-2.0-or-later
// fsr4fg-record: records the GPU work AMD's frame generation provider (amd_fidelityfx_framegeneration_vk.dll, FSR SDK
// 2.4) asks for, on synthetic frames, without Wine and without a GPU (pehost + fakevk). Writes trace.jsonl, shaders and
// buffer contents to the current directory. Nothing is rendered: the trace is the provider's host-side decisions.
#define VK_NO_PROTOTYPES
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "fakevk.h"
#include "ffx/api/include/vk/ffx_api_vk.h"
#include "ffx/framegeneration/include/ffx_framegeneration.h"
#include "pehost.h"

#define W __attribute__((ms_abi))
typedef W ffxReturnCode_t (*FnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnDestroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnConfigure)(ffxContext*, const ffxConfigureDescHeader*);
typedef W ffxReturnCode_t (*FnQuery)(ffxContext*, ffxQueryDescHeader*);
typedef W ffxReturnCode_t (*FnDispatch)(ffxContext*, const ffxDispatchDescHeader*);

typedef struct Options {
	const char* dll;
	uint32_t    dw, dh, rw, rh, frames, flags;
	const char* provider;
	int         fp8;
	VkFormat    backbuffer;
	bool        hudless;
} Options;

static FnCreate    create_fn;
static FnDestroy   destroy_fn;
static FnConfigure configure_fn;
static FnQuery     query_fn;
static FnDispatch  dispatch_fn;

static void* vulkan_import(const char* dll, const char* name) {
	if (strcmp(dll, "vulkan-1.dll") != 0) return NULL;
	void* fn = FakeVkLookup(name);
	if (!fn) fprintf(stderr, "recorder: vulkan-1.dll!%s is not implemented by the synthetic device\n", name);
	return fn;
}

W static void message(uint32_t type, const uint16_t* text) {
	char b[2048];
	size_t i = 0;
	for (; text && text[i] && i + 1 < sizeof(b); i++) b[i] = text[i] < 128 ? (char)text[i] : '?';
	b[i] = 0;
	fprintf(stderr, "FFX message %u: %s\n", type, b);
	char json[2300];
	size_t o = 0;
	for (size_t k = 0; b[k] && o + 3 < 2048; k++) {
		if (b[k] == '"' || b[k] == '\\') json[o++] = '\\';
		json[o++] = b[k] < 32 ? ' ' : b[k];
	}
	json[o] = 0;
	char note[2400];
	snprintf(note, sizeof(note), "\"stage\":\"message\",\"type\":%u,\"text\":\"%s\"", type, json);
	FakeVkNote(note);
}

static uint64_t g_next_image = 0;
static uint32_t surface_format(VkFormat f) {
	switch (f) {
	case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
	case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
	default: fprintf(stderr, "recorder: unsupported format %d\n", f); exit(2);
	}
}
// An input or output image made on the synthetic device itself, so its handle is known to the trace.
static struct FfxApiResource image(VkFormat format, uint32_t w, uint32_t h, bool written, const char* name) {
	typedef W VkResult (*CreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*);
	VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
	                        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL,
	                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
	VkImage img = NULL;
	((CreateImage)FakeVkLookup("vkCreateImage"))((VkDevice)(uintptr_t)0x100000, &ci, NULL, &img);
	char note[256];
	snprintf(note, sizeof(note), "\"stage\":\"input\",\"image\":%llu,\"name\":\"%s\"", (unsigned long long)(uintptr_t)img, name);
	FakeVkNote(note);
	g_next_image++;
	struct FfxApiResource r;
	memset(&r, 0, sizeof(r));
	r.resource = img;
	r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	r.description.format = surface_format(format);
	r.description.width = w;
	r.description.height = h;
	r.description.depth = 1;
	r.description.mipCount = 1;
	r.description.usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
	r.state = written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
	return r;
}

static void note_result(const char* stage, ffxReturnCode_t rc) {
	char n[128];
	snprintf(n, sizeof(n), "\"stage\":\"%s\",\"result\":%u", stage, rc);
	FakeVkNote(n);
	printf("%s=%u\n", stage, rc);
	fflush(stdout);
}

static int run(void* p) {
	Options* o = p;
	PeModule* m = PeLoad(o->dll);
	if (!PeAttach(m)) { fprintf(stderr, "recorder: DllMain failed\n"); return 1; }
	create_fn = (FnCreate)PeExport(m, "ffxCreateContext");
	destroy_fn = (FnDestroy)PeExport(m, "ffxDestroyContext");
	configure_fn = (FnConfigure)PeExport(m, "ffxConfigure");
	query_fn = (FnQuery)PeExport(m, "ffxQuery");
	dispatch_fn = (FnDispatch)PeExport(m, "ffxDispatch");
	if (!create_fn || !destroy_fn || !configure_fn || !query_fn || !dispatch_fn) { fprintf(stderr, "recorder: missing exports\n"); return 1; }

	VkDeviceContext device = {(VkDevice)(uintptr_t)0x100000, (VkPhysicalDevice)(uintptr_t)0x200000,
	                          (PFN_vkGetDeviceProcAddr)(void*)FakeVkGetDeviceProcAddr};
	uint64_t count = 32, ids[32] = {0};
	const char* names[32] = {0};
	struct ffxQueryDescGetVersions versions = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
	                                           .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
	                                           .device = &device, .outputCount = &count, .versionIds = ids, .versionNames = names};
	ffxReturnCode_t rc = query_fn(NULL, &versions.header);
	printf("versions query=%u count=%llu\n", rc, (unsigned long long)count);
	if (rc || count > 32) return 1;
	uint64_t chosen = 0;
	for (uint64_t i = 0; i < count; i++) {
		printf("provider %016llx %s\n", (unsigned long long)ids[i], names[i] ? names[i] : "?");
		char n[256];
		snprintf(n, sizeof(n), "\"stage\":\"provider\",\"id\":\"%016llx\",\"name\":\"%s\"", (unsigned long long)ids[i], names[i] ? names[i] : "?");
		FakeVkNote(n);
		if (!chosen && names[i] && strstr(names[i], o->provider)) chosen = ids[i];
	}
	if (!chosen) { fprintf(stderr, "recorder: no provider named like \"%s\"; refusing a fallback\n", o->provider); return 5; }

	struct ffxCreateBackendVKDesc backend = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, NULL}, .deviceContext = device};
	struct ffxOverrideVersion override = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header}, .versionId = chosen};
	struct ffxCreateContextDescFrameGenerationVersion version = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &override.header},
	                                                             .version = FFX_FRAMEGENERATION_VERSION};
	struct ffxCreateContextDescFrameGenerationHudless hudless = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS, &version.header},
	                                                             .hudlessBackBufferFormat = surface_format(o->backbuffer)};
	struct ffxCreateContextDescFrameGeneration desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, o->hudless ? &hudless.header : &version.header},
	                                                   .flags = o->flags, .displaySize = {o->dw, o->dh}, .maxRenderSize = {o->rw, o->rh},
	                                                   .backBufferFormat = surface_format(o->backbuffer)};
	ffxContext context = NULL;
	FakeVkFrame(0xffffffffu); // setup work, before the first frame
	rc = create_fn(&context, &desc.header, NULL);
	note_result("create", rc);
	if (rc || !context) return 1;
	struct ffxQueryGetProviderVersion selected = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
	rc = query_fn(&context, &selected.header);
	printf("selected %016llx %s\n", (unsigned long long)selected.versionId, selected.versionName ? selected.versionName : "?");
	{
		char n[256];
		snprintf(n, sizeof(n), "\"stage\":\"selected\",\"id\":\"%016llx\",\"name\":\"%s\"", (unsigned long long)selected.versionId,
		         selected.versionName ? selected.versionName : "?");
		FakeVkNote(n);
	}
	if (rc || selected.versionId != chosen) { fprintf(stderr, "recorder: the provider in use is not the one asked for\n"); return 1; }
	struct ffxConfigureDescGlobalDebug debug = {.header = {FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG, NULL}, .fpMessage = (ffxApiMessage)(void*)message,
	                                            .debugLevel = 0xffffffffu};
	note_result("debug", configure_fn(&context, &debug.header));

	struct FfxApiResource color[2] = {image(o->backbuffer, o->dw, o->dh, false, "present_color_a"),
	                                  image(o->backbuffer, o->dw, o->dh, false, "present_color_b")};
	struct FfxApiResource hud = image(o->backbuffer, o->dw, o->dh, false, "hudless_color");
	struct FfxApiResource output = image(o->backbuffer, o->dw, o->dh, true, "generated_output");
	struct FfxApiResource depth = image(VK_FORMAT_R32_SFLOAT, o->rw, o->rh, false, "depth");
	struct FfxApiResource motion = image(VK_FORMAT_R16G16_SFLOAT, o->rw, o->rh, false, "motion_vectors");

	for (uint32_t f = 0; f < o->frames; f++) {
		FakeVkFrame(f);
		uint64_t frame_id = f + 1;
		struct ffxConfigureDescFrameGeneration config;
		memset(&config, 0, sizeof(config));
		config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.frameGenerationEnabled = true;
		config.allowAsyncWorkloads = false;
		config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		if (o->hudless) config.HUDLessColor = hud;
		config.generationRect = (struct FfxApiRect2D){0, 0, (int32_t)o->dw, (int32_t)o->dh};
		config.frameID = frame_id;
		rc = configure_fn(&context, &config.header);
		note_result("configure", rc);
		if (rc) return 1;

		struct ffxDispatchDescFrameGenerationPrepareV2 prepare;
		memset(&prepare, 0, sizeof(prepare));
		prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prepare.frameID = frame_id;
		prepare.commandList = (void*)(uintptr_t)0x300000;
		prepare.renderSize = (struct FfxApiDimensions2D){o->rw, o->rh};
		prepare.jitterOffset = (struct FfxApiFloatCoords2D){0.25f * (float)(f % 4) - 0.375f, 0.125f};
		prepare.motionVectorScale = (struct FfxApiFloatCoords2D){(float)o->rw, (float)o->rh};
		prepare.frameTimeDelta = 16.6f;
		prepare.reset = f == 0;
		prepare.cameraNear = 0.1f;
		prepare.cameraFar = 1000.0f;
		prepare.cameraFovAngleVertical = 1.0f;
		prepare.viewSpaceToMetersFactor = 1.0f;
		prepare.depth = depth;
		prepare.motionVectors = motion;
		float pos[3] = {1.0f + 0.01f * (float)f, 2.0f, 3.0f}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
		memcpy(prepare.cameraPosition, pos, sizeof(pos));
		memcpy(prepare.cameraUp, up, sizeof(up));
		memcpy(prepare.cameraRight, right, sizeof(right));
		memcpy(prepare.cameraForward, fwd, sizeof(fwd));
		rc = dispatch_fn(&context, &prepare.header);
		note_result("prepare", rc);
		if (rc) return 1;

		struct ffxDispatchDescFrameGeneration gen;
		memset(&gen, 0, sizeof(gen));
		gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
		gen.commandList = (void*)(uintptr_t)0x300000;
		gen.presentColor = color[f & 1];
		gen.outputs[0] = output;
		gen.numGeneratedFrames = 1;
		gen.reset = f == 0;
		gen.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		gen.minMaxLuminance[0] = 0.0f;
		gen.minMaxLuminance[1] = 1000.0f;
		gen.generationRect = (struct FfxApiRect2D){0, 0, (int32_t)o->dw, (int32_t)o->dh};
		gen.frameID = frame_id;
		rc = dispatch_fn(&context, &gen.header);
		note_result("generate", rc);
		printf("frame %u recorded (NOT executed)\n", f);
		if (rc) return 1;
	}
	struct FfxApiEffectMemoryUsage usage = {0};
	struct ffxQueryDescFrameGenerationGetGPUMemoryUsage mem = {.header = {FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE, NULL},
	                                                           .gpuMemoryUsageFrameGeneration = &usage};
	if (!query_fn(&context, &mem.header)) {
		char n[160];
		snprintf(n, sizeof(n), "\"stage\":\"memory\",\"total\":%llu,\"aliasable\":%llu", (unsigned long long)usage.totalUsageInBytes,
		         (unsigned long long)usage.aliasableUsageInBytes);
		FakeVkNote(n);
		printf("provider memory: %llu bytes (%llu aliasable)\n", (unsigned long long)usage.totalUsageInBytes, (unsigned long long)usage.aliasableUsageInBytes);
	}
	rc = destroy_fn(&context, NULL);
	note_result("destroy", rc);
	FakeVkFlush();
	return rc ? 1 : 0;
}

static bool size(const char* s, uint32_t* w, uint32_t* h) { return sscanf(s, "%ux%u", w, h) == 2 && *w && *h && *w <= 8192 && *h <= 8192; }

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	Options o = {.provider = "4.", .fp8 = 1, .backbuffer = VK_FORMAT_B8G8R8A8_UNORM, .hudless = true,
	             .flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE};
	if (argc < 5 || !size(argv[2], &o.dw, &o.dh) || !size(argv[3], &o.rw, &o.rh) || (o.frames = (uint32_t)atoi(argv[4])) < 1 || o.frames > 16) {
		fprintf(stderr,
		        "usage: fsr4fg-record DLL DISPLAY_WxH RENDER_WxH FRAMES [--provider TEXT] [--no-fp8] [--no-hudless] [--hdr] [--flags N]\n"
		        "Run in a NEW empty directory. Synthetic device, NO GPU execution: records the provider's requests only.\n");
		return 2;
	}
	o.dll = argv[1];
	for (int i = 5; i < argc; i++) {
		if (!strcmp(argv[i], "--provider") && i + 1 < argc) o.provider = argv[++i];
		else if (!strcmp(argv[i], "--no-fp8")) o.fp8 = 0;
		else if (!strcmp(argv[i], "--no-hudless")) o.hudless = false;
		else if (!strcmp(argv[i], "--hdr")) o.backbuffer = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		else if (!strcmp(argv[i], "--flags") && i + 1 < argc) o.flags = (uint32_t)strtoul(argv[++i], NULL, 0);
		else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
	}
	// Radeon RX 9060 XT identity (RDNA 4), RADV driver id: SYNTHETIC, no GPU is opened.
	FakeVkProfile profile = {.vendor_id = 0x1002, .device_id = 0x7590, .driver_id = VK_DRIVER_ID_MESA_RADV, .fp8 = o.fp8};
	FakeVkSetProfile(&profile);
	PeGdiAdapter adapter = {.asic_family = o.fp8 ? 0x98 : 0x91, .asic_revision = 1};
	PeSetGdiAdapter(&adapter);
	PeSetImportResolver(vulkan_import);
	printf("OFFLINE synthetic device; no GPU execution.\n");
	return PeRunOnWindowsThread(run, &o);
}
