// SPDX-License-Identifier: GPL-2.0-or-later
// fg_runtime: see fg_runtime.h.
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "fg_runtime.h"

#include <float.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ffx/api/include/vk/ffx_api_vk.h"
#include "ffx/framegeneration/include/ffx_framegeneration.h"
#include "pehost.h"
#include "vk_thunks.h"

#define W __attribute__((ms_abi))

const char* volatile fg_rt_stage;
char                 fg_rt_stage_detail[256];

// ------------------------------------------------------------------------------------------------ SHA-256
typedef struct Sha { uint32_t h[8]; uint64_t n; uint8_t b[64]; } Sha;
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha* s, const uint8_t* p) {
	uint32_t w[64], a[8];
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++)
		w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
		       (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
	memcpy(a, s->h, sizeof(a));
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
		uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
		memmove(a + 1, a, 7 * sizeof(uint32_t));
		a[4] += t1;
		a[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
void FgRtSha256Hex(const void* data, size_t n, char out[65]) {
	Sha s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}};
	const uint8_t* p = data;
	size_t i = 0;
	for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
	uint8_t tail[128] = {0};
	size_t r = n - i;
	memcpy(tail, p + i, r);
	tail[r] = 0x80;
	size_t len = r < 56 ? 64 : 128;
	uint64_t bits = (uint64_t)n * 8;
	for (int k = 0; k < 8; k++) tail[len - 1 - k] = (uint8_t)(bits >> (8 * k));
	sha_block(&s, tail);
	if (len == 128) sha_block(&s, tail + 64);
	for (int k = 0; k < 8; k++) sprintf(out + 8 * k, "%08x", s.h[k]);
}

// ------------------------------------------------------------------------------------------------ state
typedef W ffxReturnCode_t (*FnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnDestroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnConfigure)(ffxContext*, const ffxConfigureDescHeader*);
typedef W ffxReturnCode_t (*FnQuery)(ffxContext*, ffxQueryDescHeader*);
typedef W ffxReturnCode_t (*FnDispatch)(ffxContext*, const ffxDispatchDescHeader*);

typedef enum Job { JOB_NONE, JOB_CREATE, JOB_DESTROY, JOB_GENERATE, JOB_STOP } Job;

static FgRtConfig      cfg;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  changed = PTHREAD_COND_INITIALIZER;
static FgRtState       state;
static char            reason[1024];
static bool            stopped, not_offered, creating, have_context;
static Job             job;
static const FgRtFrame* job_frame;
static FgRtResult      job_result;
static uint32_t        context_width, context_height;
static VkFormat        context_format;
static char            active_provider[32];
static const char*     provider_names[8];
static unsigned        provider_count;
static uint64_t        chosen_provider;
static FgRtStats       stats;

static FnCreate    ffx_create;
static FnDestroy   ffx_destroy;
static FnConfigure ffx_configure;
static FnQuery     ffx_query;
static FnDispatch  ffx_dispatch;
static ffxContext  context;
static VkDeviceContext device_context;
static uint64_t    frame_id;

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// (lock held)
static void fail_locked(const char* text) {
	if (state == FG_RT_FAILED) return;
	snprintf(reason, sizeof(reason), "%s", text);
	state = FG_RT_FAILED;
	pthread_cond_broadcast(&changed);
}
static void fail(const char* text) {
	pthread_mutex_lock(&lock);
	fail_locked(text);
	pthread_mutex_unlock(&lock);
}

// pehost stops here on whichever of AMD's threads: the caller goes on without AMD's runtime, and this thread, holding
// whatever AMD's code held, never runs again.
static void stop_handler(const char* text) {
	pthread_mutex_lock(&lock);
	stopped = true;
	fail_locked(text);
	pthread_mutex_unlock(&lock);
	for (;;) pause();
}

// ------------------------------------------------------------------------------------------------ AMD's Vulkan calls
typedef struct ModuleInfo { VkShaderModule module; bool matrix, replaced; char hash[17]; } ModuleInfo;
static ModuleInfo* modules;
static size_t      module_count, module_cap;

static bool has_matrix_capability(const uint32_t* w, size_t words) {
	for (size_t at = 5; at < words;) {
		uint32_t n = w[at] >> 16, op = w[at] & 0xffff;
		if (!n) break;
		if (op == 17 && at + 1 < words && w[at + 1] == 6022) return true;
		if (op == 54) break;
		at += n;
	}
	return false;
}

W static VkResult o_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* a,
                                         VkShaderModule* out) {
	(void)a;
	char hex[65], path[4096 + 80];
	FgRtSha256Hex(info->pCode, info->codeSize, hex);
	fg_rt_stage = "creating shader module";
	snprintf(fg_rt_stage_detail, sizeof(fg_rt_stage_detail), "(AMD module %.16s..)", hex);
	snprintf(path, sizeof(path), "%s/%s.spv", cfg.prepared, hex);
	VkShaderModuleCreateInfo replaced = *info;
	uint32_t* code = NULL;
	FILE* f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		code = n > 0 && n % 4 == 0 ? malloc((size_t)n) : NULL;
		bool whole = code && fread(code, 1, (size_t)n, f) == (size_t)n;
		fclose(f);
		if (!whole) {
			free(code);
			fprintf(stderr, "fg_runtime: bad prepared module %s\n", path);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		replaced.pCode = code;
		replaced.codeSize = (size_t)n;
		stats.replaced++;
	} else {
		stats.as_shipped++;
	}
	VkResult r = ((PFN_vkCreateShaderModule)VkThunkReal("vkCreateShaderModule"))(d, &replaced, NULL, out);
	if (r == VK_SUCCESS) {
		if (module_count == module_cap) {
			module_cap = module_cap ? module_cap * 2 : 512;
			modules = realloc(modules, module_cap * sizeof(*modules));
		}
		ModuleInfo* added = &modules[module_count++];
		*added = (ModuleInfo){*out, has_matrix_capability(replaced.pCode, replaced.codeSize / 4), code != NULL, {0}};
		memcpy(added->hash, hex, 16);
	} else {
		fprintf(stderr, "fg_runtime: vkCreateShaderModule failed (%d) for AMD module %s\n", r, hex);
	}
	free(code);
	return r;
}

// Newest first: AMD destroys modules after building their pipelines, so the driver hands the same handle out again.
static const ModuleInfo* module_info(VkShaderModule m) {
	for (size_t i = module_count; i-- > 0;) if (modules[i].module == m) return &modules[i];
	return NULL;
}

// Matrix programs run with full subgroups (as the native FSR 4 upscaler requires on RADV); AMD's runtime asks only
// for subgroup size 32.
W static VkResult o_vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo* infos,
                                             const VkAllocationCallbacks* a, VkPipeline* out) {
	(void)a;
	VkComputePipelineCreateInfo* copy = malloc(n * sizeof(*copy));
	memcpy(copy, infos, n * sizeof(*copy));
	bool matrix = false;
	for (uint32_t i = 0; i < n; i++) {
		const ModuleInfo* shader = module_info(copy[i].stage.module);
		if (shader && shader->matrix) {
			matrix = true;
			if (cfg.full_subgroups) {
				copy[i].stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
				stats.matrix_pipelines++;
			}
		}
	}
	const ModuleInfo* shader = module_info(copy[0].stage.module);
	fg_rt_stage = "compiling pipeline";
	snprintf(fg_rt_stage_detail, sizeof(fg_rt_stage_detail), "%u: %s%s, module %s (%s)", stats.pipelines + 1, copy[0].stage.pName, matrix ? " (FP8 matrix)" : "",
	         shader ? shader->hash : "?", shader && shader->replaced ? "prepared" : "as shipped");
	if (cfg.verbose) {
		printf("compiling pipeline %s\n", fg_rt_stage_detail);
		fflush(stdout);
	}
	double start = now_ms();
	VkResult r = ((PFN_vkCreateComputePipelines)VkThunkReal("vkCreateComputePipelines"))(d, cache, n, copy, NULL, out);
	double ms = now_ms() - start;
	stats.compile_seconds += ms / 1000;
	stats.pipelines += n;
	if (ms > 2000 && cfg.verbose) printf("  (that pipeline took %.1f s)\n", ms / 1000);
	fg_rt_stage = "AMD context creation";
	fg_rt_stage_detail[0] = 0;
	if (r != VK_SUCCESS) {
		stats.pipeline_failures++;
		for (uint32_t i = 0; i < n; i++) fprintf(stderr, "fg_runtime: vkCreateComputePipelines failed (%d): entry %s\n", r, copy[i].stage.pName);
	}
	free(copy);
	return r;
}

// AMD's Windows provider expects a device LUID; Linux drivers have none. A synthetic one is reported to it.
W static void o_vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2* props) {
	((PFN_vkGetPhysicalDeviceProperties2)VkThunkReal("vkGetPhysicalDeviceProperties2"))(p, props);
	for (VkBaseOutStructure* s = props->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
			VkPhysicalDeviceIDProperties* id = (void*)s;
			if (!id->deviceLUIDValid) {
				memset(id->deviceLUID, 3, VK_LUID_SIZE);
				id->deviceLUIDValid = VK_TRUE;
				id->deviceNodeMask = 1;
			}
		}
}

// What AMD makes of the depth image: a color view of a depth image would read nothing, and nothing would say so.
static volatile VkImage watched_depth;
W static VkResult o_vkCreateImageView(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* a, VkImageView* out) {
	(void)a;
	if (cfg.verbose && info->image == watched_depth) {
		printf("AMD's view of the depth image: format %d, aspect %#x\n", info->format, info->subresourceRange.aspectMask);
		fflush(stdout);
	}
	return ((PFN_vkCreateImageView)VkThunkReal("vkCreateImageView"))(d, info, NULL, out);
}

// AMD's runtime must not touch a queue on its own here: the emulator's queues are guarded by its own locks. Counted, so
// the test can say so.
W static void o_vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue* queue) {
	stats.queue_calls++;
	((PFN_vkGetDeviceQueue)VkThunkReal("vkGetDeviceQueue"))(d, family, index, queue);
}
W static VkResult o_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
	stats.queue_calls++;
	return ((PFN_vkQueueSubmit)VkThunkReal("vkQueueSubmit"))(queue, count, submits, fence);
}
W static VkResult o_vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
	stats.queue_calls++;
	return ((PFN_vkQueueSubmit2)VkThunkReal("vkQueueSubmit2"))(queue, count, submits, fence);
}
W static VkResult o_vkDeviceWaitIdle(VkDevice d) {
	stats.queue_calls++;
	return ((PFN_vkDeviceWaitIdle)VkThunkReal("vkDeviceWaitIdle"))(d);
}

W static void* dll_get_device_proc_addr(VkDevice d, const char* name) {
	(void)d;
	void* f = VkThunkLookup(name);
	if (!f && cfg.verbose) fprintf(stderr, "AMD runtime asked for %s: not provided by the driver\n", name);
	return f;
}

static void* import_resolver(const char* dll, const char* name) {
	if (strcmp(dll, "vulkan-1.dll")) return NULL;
	if (!strcmp(name, "vkGetDeviceProcAddr")) return (void*)dll_get_device_proc_addr;
	void* f = VkThunkLookup(name);
	if (!f && cfg.verbose) fprintf(stderr, "vulkan-1.dll!%s: not provided by the driver\n", name);
	return f;
}

W static void message(uint32_t type, const uint16_t* text) {
	char b[1024];
	size_t i = 0;
	for (; text && text[i] && i + 1 < sizeof(b); i++) b[i] = text[i] < 128 ? (char)text[i] : '?';
	b[i] = 0;
	if (cfg.verbose) fprintf(stderr, "AMD runtime message %u: %s\n", type, b);
}

// ------------------------------------------------------------------------------------------------ AMD's thread
// (AMD's ffxApiGetSurfaceFormatVK and ffxApiGetImageResourceDescriptionVK are C++ only: the same for the formats used.)
static uint32_t surface_format(VkFormat format) {
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
	default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
	}
}
static struct FfxApiResource resource(const FgRtImage* im, bool written) {
	struct FfxApiResource r;
	memset(&r, 0, sizeof(r));
	uint32_t usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
	if (im->format == VK_FORMAT_D32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT_S8_UINT) usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
	if (im->format == VK_FORMAT_D32_SFLOAT_S8_UINT) usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
	if (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) usage |= FFX_API_RESOURCE_USAGE_UAV;
	r.resource = (void*)im->image;
	r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	r.description.format = surface_format(im->format);
	r.description.width = im->width;
	r.description.height = im->height;
	r.description.depth = 1;
	r.description.mipCount = 1;
	r.description.usage = usage;
	r.state = written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
	          : im->layout == VK_IMAGE_LAYOUT_GENERAL ? FFX_API_RESOURCE_STATE_COMMON : FFX_API_RESOURCE_STATE_COMPUTE_READ;
	return r;
}

static bool load(void) {
	fg_rt_stage = "loading AMD's DLL";
	stats.thunks_bound = VkThunksBind(cfg.get_instance_proc_addr, cfg.instance, cfg.get_device_proc_addr, cfg.device);
	stats.thunks_total = VkThunkCount();
	VkThunkOverride("vkCreateShaderModule", (void*)o_vkCreateShaderModule);
	VkThunkOverride("vkCreateComputePipelines", (void*)o_vkCreateComputePipelines);
	VkThunkOverride("vkCreateImageView", (void*)o_vkCreateImageView);
	VkThunkOverride("vkGetDeviceQueue", (void*)o_vkGetDeviceQueue);
	VkThunkOverride("vkQueueSubmit", (void*)o_vkQueueSubmit);
	VkThunkOverride("vkQueueSubmit2", (void*)o_vkQueueSubmit2);
	VkThunkOverride("vkQueueSubmit2KHR", (void*)o_vkQueueSubmit2);
	VkThunkOverride("vkDeviceWaitIdle", (void*)o_vkDeviceWaitIdle);
	VkThunkOverride("vkGetPhysicalDeviceProperties2", (void*)o_vkGetPhysicalDeviceProperties2);
	VkThunkOverride("vkGetPhysicalDeviceProperties2KHR", (void*)o_vkGetPhysicalDeviceProperties2);
	PeSetMinimumAddress(cfg.minimum_address);
	PeSetImportResolver(import_resolver);
	PeModule* m = PeLoad(cfg.dll);
	PeImageRange(m, &stats.image_base, &stats.image_size);
	if (!PeAttach(m)) { fail("DllMain of AMD's DLL failed"); return false; }
	ffx_create = (FnCreate)PeExport(m, "ffxCreateContext");
	ffx_destroy = (FnDestroy)PeExport(m, "ffxDestroyContext");
	ffx_configure = (FnConfigure)PeExport(m, "ffxConfigure");
	ffx_query = (FnQuery)PeExport(m, "ffxQuery");
	ffx_dispatch = (FnDispatch)PeExport(m, "ffxDispatch");
	if (!ffx_create || !ffx_destroy || !ffx_configure || !ffx_query || !ffx_dispatch) { fail("AMD's DLL lacks an ffx entry point"); return false; }
	device_context = (VkDeviceContext){cfg.device, cfg.physical_device, (PFN_vkGetDeviceProcAddr)(void*)dll_get_device_proc_addr};
	uint64_t count = 8, ids[8] = {0};
	const char* names[8] = {0};
	struct ffxQueryDescGetVersions versions = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL}, .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
	                                           .device = &device_context, .outputCount = &count, .versionIds = ids, .versionNames = names};
	if (ffx_query(NULL, &versions.header) || count > 8) { fail("asking AMD's runtime for its providers failed"); return false; }
	pthread_mutex_lock(&lock);
	provider_count = (unsigned)count;
	for (uint64_t i = 0; i < count; i++) {
		provider_names[i] = names[i];
		if (names[i] && cfg.provider && !strcmp(names[i], cfg.provider)) chosen_provider = ids[i];
	}
	pthread_mutex_unlock(&lock);
	if (!cfg.probe && !chosen_provider) {
		char text[128];
		snprintf(text, sizeof(text), "AMD's runtime does not offer frame generation %s on this device", cfg.provider);
		pthread_mutex_lock(&lock);
		not_offered = true;
		fail_locked(text);
		pthread_mutex_unlock(&lock);
		return false;
	}
	return true;
}

static void create_context(void) {
	struct ffxCreateBackendVKDesc backend = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, NULL}, .deviceContext = device_context};
	struct ffxOverrideVersion override = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header}, .versionId = chosen_provider};
	struct ffxCreateContextDescFrameGenerationVersion version = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &override.header},
	                                                             .version = FFX_FRAMEGENERATION_VERSION};
	uint32_t format = surface_format(context_format);
	struct ffxCreateContextDescFrameGenerationHudless hudless = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS, &version.header},
	                                                             .hudlessBackBufferFormat = format};
	// (The render size can change every frame, up to the display's, as on Windows.)
	struct ffxCreateContextDescFrameGeneration desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, cfg.hudless_desc ? &hudless.header : &version.header},
	                                                   .flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE,
	                                                   .displaySize = {context_width, context_height}, .maxRenderSize = {context_width, context_height},
	                                                   .backBufferFormat = format};
	fg_rt_stage = "AMD context creation";
	double start = now_ms();
	ffxContext made = NULL;
	ffxReturnCode_t rc = ffx_create(&made, &desc.header, NULL);
	stats.create_seconds = (now_ms() - start) / 1000;
	if (rc || !made || stats.pipeline_failures) {
		char text[128];
		snprintf(text, sizeof(text), "AMD's context creation failed (result %u, %u pipeline failures)", rc, stats.pipeline_failures);
		fail(text);
		return;
	}
	context = made;
	struct ffxConfigureDescGlobalDebug debug = {.header = {FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG, NULL}, .fpMessage = (ffxApiMessage)(void*)message,
	                                            .debugLevel = 0xffffffffu};
	ffx_configure(&context, &debug.header);
	frame_id = 0;
	struct ffxQueryGetProviderVersion provider = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
	if (!ffx_query(&context, &provider.header) && provider.versionName) {
		pthread_mutex_lock(&lock);
		snprintf(active_provider, sizeof(active_provider), "%s", provider.versionName);
		pthread_mutex_unlock(&lock);
	}
	struct FfxApiEffectMemoryUsage usage = {0};
	struct ffxQueryDescFrameGenerationGetGPUMemoryUsage memory = {.header = {FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE, NULL},
	                                                              .gpuMemoryUsageFrameGeneration = &usage};
	if (!ffx_query(&context, &memory.header)) stats.memory_bytes = usage.totalUsageInBytes;
	pthread_mutex_lock(&lock);
	have_context = true;
	pthread_mutex_unlock(&lock);
}

static FgRtResult generate(const FgRtFrame* f) {
	uint64_t id = ++frame_id;
	watched_depth = f->depth.image;
	struct FfxApiRect2D rect = {f->rect[0], f->rect[1], f->rect[2], f->rect[3]};
	struct ffxConfigureDescFrameGeneration config;
	memset(&config, 0, sizeof(config));
	config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.frameGenerationEnabled = true;
	config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
	if (f->hudless.image) config.HUDLessColor = resource(&f->hudless, false);
	config.generationRect = rect;
	config.frameID = id;
	char text[128];
	ffxReturnCode_t rc = ffx_configure(&context, &config.header);
	if (rc) {
		snprintf(text, sizeof(text), "AMD's configure failed (result %u)", rc);
		fail(text);
		return FG_RT_UNAVAILABLE;
	}
	struct ffxDispatchDescFrameGenerationPrepareV2 prepare;
	memset(&prepare, 0, sizeof(prepare));
	prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
	prepare.frameID = id;
	prepare.commandList = f->command;
	prepare.renderSize = (struct FfxApiDimensions2D){f->depth.width, f->depth.height};
	prepare.jitterOffset = (struct FfxApiFloatCoords2D){f->jitter[0], f->jitter[1]};
	prepare.motionVectorScale = (struct FfxApiFloatCoords2D){f->motion_scale[0], f->motion_scale[1]};
	prepare.frameTimeDelta = f->frame_ms;
	prepare.reset = f->reset;
	prepare.cameraNear = f->near_plane;
	prepare.cameraFar = FLT_MAX; // (an infinite far plane, as on Windows)
	prepare.cameraFovAngleVertical = f->fov_y;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth = resource(&f->depth, false);
	prepare.motionVectors = resource(&f->motion, false);
	memcpy(prepare.cameraPosition, f->position, sizeof(f->position));
	memcpy(prepare.cameraUp, f->up, sizeof(f->up));
	memcpy(prepare.cameraRight, f->right, sizeof(f->right));
	memcpy(prepare.cameraForward, f->forward, sizeof(f->forward));
	rc = ffx_dispatch(&context, &prepare.header);
	if (rc) {
		snprintf(text, sizeof(text), "AMD's prepare failed (result %u)", rc);
		fail(text);
		return FG_RT_UNAVAILABLE;
	}
	struct ffxDispatchDescFrameGeneration gen;
	memset(&gen, 0, sizeof(gen));
	gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
	gen.commandList = f->command;
	gen.presentColor = resource(&f->color, false);
	gen.outputs[0] = resource(&f->output, true);
	gen.numGeneratedFrames = 1;
	gen.reset = f->reset;
	gen.backbufferTransferFunction = f->pq ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	gen.minMaxLuminance[1] = f->pq ? 1000.0f : 1.0f;
	gen.generationRect = rect;
	gen.frameID = id;
	rc = ffx_dispatch(&context, &gen.header);
	if (rc) {
		snprintf(text, sizeof(text), "AMD's generate failed (result %u)", rc);
		fail(text);
		return FG_RT_UNAVAILABLE;
	}
	return f->reset ? FG_RT_NOT_MADE : FG_RT_GENERATED;
}

static int worker(void* unused) {
	(void)unused;
	if (cfg.on_thread_start) cfg.on_thread_start();
	bool loaded = load();
	pthread_mutex_lock(&lock);
	if (loaded && state != FG_RT_FAILED) state = FG_RT_READY;
	pthread_cond_broadcast(&changed);
	fg_rt_stage = NULL;
	for (;;) {
		while (job == JOB_NONE) pthread_cond_wait(&changed, &lock);
		Job current = job;
		const FgRtFrame* frame = job_frame;
		pthread_mutex_unlock(&lock);
		FgRtResult result = FG_RT_UNAVAILABLE;
		switch (current) {
		case JOB_CREATE: create_context(); break;
		case JOB_DESTROY:
			if (ffx_destroy && context) ffx_destroy(&context, NULL);
			context = NULL;
			break;
		case JOB_GENERATE:
			fg_rt_stage = "recording AMD's frame";
			snprintf(fg_rt_stage_detail, sizeof(fg_rt_stage_detail), "%llu", (unsigned long long)frame_id + 1);
			result = generate(frame);
			break;
		case JOB_STOP: PeFatal("injected"); break;
		default: break;
		}
		fg_rt_stage = NULL;
		pthread_mutex_lock(&lock);
		if (current == JOB_CREATE) creating = false;
		job_result = result;
		job = JOB_NONE;
		pthread_cond_broadcast(&changed);
	}
	return 0;
}

static void* worker_host(void* unused) { return (void*)(intptr_t)PeRunOnWindowsThread(worker, unused); }

void FgRtStart(const FgRtConfig* config) {
	pthread_mutex_lock(&lock);
	if (state != FG_RT_NOT_STARTED) { pthread_mutex_unlock(&lock); return; }
	cfg = *config;
	state = FG_RT_LOADING;
	pthread_mutex_unlock(&lock);
	PeSetFatalHandler(stop_handler);
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_host, NULL)) { fail("cannot start AMD's thread"); return; }
	pthread_detach(thread);
}

// (lock held) Posts `kind` once AMD's thread is free, and waits for it unless `background`.
static bool post_locked(Job kind, const FgRtFrame* frame, bool background) {
	while (job != JOB_NONE && state != FG_RT_FAILED) pthread_cond_wait(&changed, &lock);
	if (state == FG_RT_FAILED) return false;
	job = kind;
	job_frame = frame;
	pthread_cond_broadcast(&changed);
	if (background) return true;
	while (job != JOB_NONE && state != FG_RT_FAILED) pthread_cond_wait(&changed, &lock);
	return state != FG_RT_FAILED;
}

FgRtResult FgRtGenerate(const FgRtFrame* frame) {
	FgRtResult result = FG_RT_NOT_YET;
	pthread_mutex_lock(&lock);
	if (state == FG_RT_FAILED || state == FG_RT_NOT_STARTED) {
		result = FG_RT_UNAVAILABLE;
	} else if (state == FG_RT_READY && !creating) {
		uint32_t width = frame->color.width, height = frame->color.height;
		VkFormat format = frame->color.format;
		if (have_context && (context_width != width || context_height != height || context_format != format)) {
			// (A new size is a rare event: the wait is the same one FSR 3's context has.)
			have_context = false;
			active_provider[0] = 0;
			pthread_mutex_unlock(&lock);
			if (cfg.wait_idle) cfg.wait_idle();
			pthread_mutex_lock(&lock);
			if (post_locked(JOB_DESTROY, NULL, false)) stats.matrix_pipelines = stats.pipelines = 0;
		}
		if (state == FG_RT_FAILED) {
			result = FG_RT_UNAVAILABLE;
		} else if (!have_context) {
			if (surface_format(format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
				char text[96];
				snprintf(text, sizeof(text), "FSR has no surface format for VkFormat %d", format);
				fail_locked(text);
				result = FG_RT_UNAVAILABLE;
			} else {
				context_width = width;
				context_height = height;
				context_format = format;
				creating = true;
				post_locked(JOB_CREATE, NULL, true);
			}
		} else if (post_locked(JOB_GENERATE, frame, false)) {
			result = job_result;
		} else {
			result = FG_RT_UNAVAILABLE;
		}
	}
	pthread_mutex_unlock(&lock);
	return result;
}

FgRtState FgRtGetState(void) {
	pthread_mutex_lock(&lock);
	FgRtState s = state;
	pthread_mutex_unlock(&lock);
	return s;
}
void FgRtWaitLoaded(void) {
	pthread_mutex_lock(&lock);
	while (state == FG_RT_LOADING) pthread_cond_wait(&changed, &lock);
	pthread_mutex_unlock(&lock);
}
const char* FgRtReason(void) { return reason; }
bool        FgRtStopped(void) { return stopped; }
bool        FgRtNotOffered(void) { return not_offered; }
void FgRtProviders(const char** names, unsigned* count) {
	pthread_mutex_lock(&lock);
	unsigned n = provider_count < *count ? provider_count : *count;
	for (unsigned i = 0; i < n; i++) names[i] = provider_names[i];
	*count = n;
	pthread_mutex_unlock(&lock);
}
const char* FgRtActiveProvider(void) { return active_provider; }
FgRtStats FgRtGetStats(void) {
	pthread_mutex_lock(&lock);
	FgRtStats s = stats;
	pthread_mutex_unlock(&lock);
	return s;
}
void FgRtInjectStop(void) {
	pthread_mutex_lock(&lock);
	post_locked(JOB_STOP, NULL, true);
	pthread_mutex_unlock(&lock);
}
