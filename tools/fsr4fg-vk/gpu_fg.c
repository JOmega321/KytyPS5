// SPDX-License-Identifier: GPL-2.0-or-later
// fsr4fg-gpu: runs AMD's FSR Frame Generation 4.0.2 runtime (amd_fidelityfx_framegeneration_vk.dll, SDK 2.4.0) on
// the real Vulkan device of this Linux machine, through pehost and Windows-ABI thunks, with AMD's shaders replaced by
// the prepared ones (prepare_fg.py) where RADV needs them. It renders a synthetic scene whose objects move by known
// amounts, generates the frame between each pair, reads it back and compares it with the exact midpoint picture and
// with simple baselines (repeating a frame, blending two). A test tool: it changes nothing in the emulator.
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include "ffx/api/include/vk/ffx_api_vk.h"
#include "ffx/framegeneration/include/ffx_framegeneration.h"
#include "pehost.h"
#include "vk_thunks.h"

#define W __attribute__((ms_abi))
#define CHECK(x)                                                                                                       \
	do {                                                                                                               \
		VkResult r_ = (x);                                                                                             \
		if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(4); }           \
	} while (0)

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
static void sha256_hex(const void* data, size_t n, char out[65]) {
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

// ------------------------------------------------------------------------------------------------ options/state
typedef struct Options {
	const char *dll, *prepared, *out, *provider;
	uint32_t    dw, dh, rw, rh, frames, timing_frames;
	bool        hud, full_subgroups, probe, organic;
} Options;
static Options opt;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr   gdpa;
static VkInstance                instance;
static VkPhysicalDevice          gpu;
static VkDevice                  device;
static VkQueue                   queue;
static uint32_t                  queue_family;
static uint64_t                  timestamp_mask;
static VkPhysicalDeviceMemoryProperties memory_props;
static unsigned substituted, passed_through, full_subgroup_pipelines, pipeline_failures, pipelines_created;
static double   pipeline_ms;

// ------------------------------------------------------------------------------------------------ crash reports
// What the test was doing, for a report if the process dies (a crash in the driver, in AMD's code or here).
static const char* volatile stage = "starting";
static char                 stage_detail[256];
static uintptr_t            dll_base;
static size_t               dll_size;
static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void say(const char* text) { ssize_t r = write(2, text, strlen(text)); (void)r; }
static void where(const char* what, uintptr_t address) {
	char line[512];
	Dl_info info;
	if (dll_base && address >= dll_base && address < dll_base + dll_size)
		snprintf(line, sizeof(line), "  %s %#lx: in AMD's DLL at RVA %#lx\n", what, (unsigned long)address, (unsigned long)(address - dll_base));
	else if (address && dladdr((void*)address, &info) && info.dli_fname)
		snprintf(line, sizeof(line), "  %s %#lx: in %s (%s+%#lx)\n", what, (unsigned long)address, info.dli_fname,
		         info.dli_sname ? info.dli_sname : "?", (unsigned long)(address - (uintptr_t)(info.dli_sname ? info.dli_saddr : info.dli_fbase)));
	else
		snprintf(line, sizeof(line), "  %s %#lx: unknown\n", what, (unsigned long)address);
	say(line);
}
static void crashed(int sig, siginfo_t* si, void* uc_) {
	ucontext_t* uc = uc_;
	char line[768];
	snprintf(line, sizeof(line), "\n[FG] STOPPED by signal %d (%s) while: %s %s\n  last Vulkan call from AMD's code: %s (%lu calls)\n",
	         sig, strsignal(sig), stage, stage_detail, vk_thunk_last ? vk_thunk_last : "none", vk_thunk_calls);
	say(line);
	if (sig != SIGINT && sig != SIGTERM) {
		uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
		snprintf(line, sizeof(line), "  fault address %#lx\n", (unsigned long)(uintptr_t)si->si_addr);
		say(line);
		where("instruction", rip);
		if (rip < 4096 || !dladdr((void*)rip, &(Dl_info){0})) where("return address", *(uintptr_t*)rsp);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}
static void install_crash_reports(void) {
	static uint8_t alt[1 << 16];
	stack_t ss = {.ss_sp = alt, .ss_size = sizeof(alt)};
	sigaltstack(&ss, NULL);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGINT, SIGTERM};
	for (size_t i = 0; i < sizeof(sigs) / sizeof(*sigs); i++) sigaction(sigs[i], &sa, NULL);
}

#define VKFN(name) static PFN_##name name
VKFN(vkCreateInstance); VKFN(vkEnumeratePhysicalDevices); VKFN(vkGetPhysicalDeviceProperties);
VKFN(vkGetPhysicalDeviceProperties2); VKFN(vkGetPhysicalDeviceFeatures2); VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
VKFN(vkGetPhysicalDeviceMemoryProperties); VKFN(vkEnumerateDeviceExtensionProperties); VKFN(vkCreateDevice);
VKFN(vkGetDeviceQueue); VKFN(vkCreateImage); VKFN(vkGetImageMemoryRequirements); VKFN(vkAllocateMemory);
VKFN(vkBindImageMemory); VKFN(vkCreateBuffer); VKFN(vkGetBufferMemoryRequirements); VKFN(vkBindBufferMemory);
VKFN(vkMapMemory); VKFN(vkCreateCommandPool); VKFN(vkAllocateCommandBuffers); VKFN(vkBeginCommandBuffer);
VKFN(vkEndCommandBuffer); VKFN(vkCmdPipelineBarrier); VKFN(vkCmdCopyBufferToImage); VKFN(vkCmdCopyImageToBuffer);
VKFN(vkQueueSubmit); VKFN(vkCreateFence); VKFN(vkWaitForFences); VKFN(vkResetFences); VKFN(vkCreateQueryPool);
VKFN(vkCmdResetQueryPool); VKFN(vkCmdWriteTimestamp); VKFN(vkGetQueryPoolResults); VKFN(vkDeviceWaitIdle);
VKFN(vkResetCommandBuffer);

// ------------------------------------------------------------------------------------------------ overrides (ms_abi)
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
	char hex[65], path[4096];
	sha256_hex(info->pCode, info->codeSize, hex);
	stage = "creating shader module";
	snprintf(stage_detail, sizeof(stage_detail), "(AMD module %.16s..)", hex);
	snprintf(path, sizeof(path), "%s/%s.spv", opt.prepared, hex);
	VkShaderModuleCreateInfo replaced = *info;
	uint32_t* code = NULL;
	FILE* f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		code = malloc((size_t)n);
		if (n <= 0 || n % 4 || fread(code, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "bad prepared module %s\n", path); exit(4); }
		fclose(f);
		replaced.pCode = code;
		replaced.codeSize = (size_t)n;
		substituted++;
	} else {
		passed_through++;
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
		fprintf(stderr, "vkCreateShaderModule failed (%d) for AMD module %s\n", r, hex);
	}
	free(code);
	return r;
}

// Newest first: AMD destroys modules after building their pipelines, so the driver hands the same handle out again.
static const ModuleInfo* module_info(VkShaderModule m) {
	for (size_t i = module_count; i-- > 0;) if (modules[i].module == m) return &modules[i];
	return NULL;
}
static bool module_is_matrix(VkShaderModule m) {
	const ModuleInfo* info = module_info(m);
	return info && info->matrix;
}

// Matrix programs run with full subgroups (as the native FSR 4 upscaler requires on RADV); AMD's runtime asks only
// for subgroup size 32.
W static VkResult o_vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo* infos,
                                             const VkAllocationCallbacks* a, VkPipeline* out) {
	(void)a;
	VkComputePipelineCreateInfo* copy = malloc(n * sizeof(*copy));
	memcpy(copy, infos, n * sizeof(*copy));
	bool matrix = false;
	for (uint32_t i = 0; i < n; i++)
		if (module_is_matrix(copy[i].stage.module)) {
			matrix = true;
			if (opt.full_subgroups) {
				copy[i].stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
				full_subgroup_pipelines++;
			}
		}
	stage = "compiling pipeline";
	const ModuleInfo* shader = module_info(copy[0].stage.module);
	snprintf(stage_detail, sizeof(stage_detail), "%u: %s%s, module %s (%s)", pipelines_created + 1, copy[0].stage.pName, matrix ? " (FP8 matrix)" : "",
	         shader ? shader->hash : "?", shader && shader->replaced ? "prepared" : "as shipped");
	printf("compiling pipeline %s\n", stage_detail);
	fflush(stdout);
	double start = now_ms();
	VkResult r = ((PFN_vkCreateComputePipelines)VkThunkReal("vkCreateComputePipelines"))(d, cache, n, copy, NULL, out);
	double ms = now_ms() - start;
	pipeline_ms += ms;
	pipelines_created += n;
	if (ms > 2000) printf("  (that pipeline took %.1f s)\n", ms / 1000);
	stage = "AMD context creation";
	stage_detail[0] = 0;
	if (r != VK_SUCCESS) {
		pipeline_failures++;
		for (uint32_t i = 0; i < n; i++) fprintf(stderr, "vkCreateComputePipelines failed (%d): entry %s\n", r, copy[i].stage.pName);
	}
	free(copy);
	return r;
}

// AMD's Windows provider expects a device LUID; Linux drivers have none. A synthetic one is reported to it.
W static void o_vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2* props) {
	vkGetPhysicalDeviceProperties2(p, props);
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

W static void* dll_get_device_proc_addr(VkDevice d, const char* name) {
	(void)d;
	void* f = VkThunkLookup(name);
	if (!f) fprintf(stderr, "AMD runtime asked for %s: not provided by the driver\n", name);
	return f;
}

static void* import_resolver(const char* dll, const char* name) {
	if (strcmp(dll, "vulkan-1.dll")) return NULL;
	if (!strcmp(name, "vkGetDeviceProcAddr")) return (void*)dll_get_device_proc_addr;
	void* f = VkThunkLookup(name);
	if (!f) fprintf(stderr, "vulkan-1.dll!%s: not provided by the driver\n", name);
	return f;
}

// ------------------------------------------------------------------------------------------------ device
typedef struct Feature { const char* name; VkBool32* value; } Feature;

static bool has_extension(const VkExtensionProperties* e, uint32_t n, const char* name) {
	for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
	return false;
}

static void create_device(void) {
	void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!loader) { fprintf(stderr, "libvulkan.so.1 not found\n"); exit(3); }
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
#define G(name) name = (PFN_##name)gipa(instance, #name)
	G(vkCreateInstance);
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "kyty-fsr4fg-gpu-test", .apiVersion = VK_API_VERSION_1_3};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkResult ir = vkCreateInstance(&ici, NULL, &instance);
	if (ir != VK_SUCCESS) { fprintf(stderr, "No usable Vulkan driver (vkCreateInstance = %d)\n", ir); exit(3); }
	G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceProperties2);
	G(vkGetPhysicalDeviceFeatures2); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties);
	G(vkEnumerateDeviceExtensionProperties); G(vkCreateDevice);
	uint32_t count = 0;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
	VkPhysicalDevice list[16];
	if (count > 16) count = 16;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, list));
	const char* want = getenv("FSR4FG_DEVICE");
	for (uint32_t i = 0; i < count && !gpu; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(list[i], &p);
		if (want ? (uint32_t)atoi(want) == i : (p.vendorID == 0x1002 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) gpu = list[i];
	}
	if (!gpu && !want && count) gpu = list[0];
	if (!gpu) { fprintf(stderr, "no Vulkan device\n"); exit(3); }
	VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &driver};
	VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
	vkGetPhysicalDeviceProperties2(gpu, &props);
	printf("device: %s (vendor %04x device %04x), driver %s %s, Vulkan %u.%u\n", props.properties.deviceName, props.properties.vendorID,
	       props.properties.deviceID, driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(props.properties.apiVersion),
	       VK_API_VERSION_MINOR(props.properties.apiVersion));

	VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
	VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT atomic64 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT,
	                                                              .pNext = &derivatives};
	VkPhysicalDeviceCooperativeMatrix2FeaturesNV coop2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV, .pNext = &atomic64};
	VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, .pNext = &coop2};
	VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, .pNext = &coop};
	VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &fp8};
	VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
	VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
	VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
	uint32_t next = 0;
	CHECK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &next, NULL));
	VkExtensionProperties* ext = calloc(next, sizeof(*ext));
	CHECK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &next, ext));
	const char* wanted_ext[] = {"VK_EXT_shader_float8", "VK_KHR_cooperative_matrix", "VK_NV_cooperative_matrix2",
	                            "VK_EXT_shader_image_atomic_int64", "VK_KHR_compute_shader_derivatives"};
	const char* enabled_ext[5];
	uint32_t    enabled = 0;
	bool ok = true;
	for (size_t i = 0; i < 5; i++) {
		bool have = has_extension(ext, next, wanted_ext[i]);
		printf("  %-40s %s\n", wanted_ext[i], have ? "yes" : i < 4 ? "MISSING" : "no (FSR 3 only)");
		if (have) enabled_ext[enabled++] = wanted_ext[i];
		else if (i < 4) ok = false;
	}
	// Feature structures of extensions that are absent must not be chained.
	if (!has_extension(ext, next, "VK_KHR_compute_shader_derivatives")) atomic64.pNext = NULL;
	if (!has_extension(ext, next, "VK_EXT_shader_image_atomic_int64")) coop2.pNext = atomic64.pNext;
	if (!has_extension(ext, next, "VK_NV_cooperative_matrix2")) coop.pNext = coop2.pNext;
	if (!has_extension(ext, next, "VK_KHR_cooperative_matrix")) fp8.pNext = coop.pNext;
	if (!has_extension(ext, next, "VK_EXT_shader_float8")) v13.pNext = fp8.pNext;
	vkGetPhysicalDeviceFeatures2(gpu, &features);
	Feature needed[] = {
	    {"shaderInt16", &features.features.shaderInt16}, {"shaderInt64", &features.features.shaderInt64},
	    {"shaderStorageImageExtendedFormats", &features.features.shaderStorageImageExtendedFormats},
	    {"shaderStorageImageReadWithoutFormat", &features.features.shaderStorageImageReadWithoutFormat},
	    {"shaderStorageImageWriteWithoutFormat", &features.features.shaderStorageImageWriteWithoutFormat},
	    {"storageBuffer16BitAccess", &v11.storageBuffer16BitAccess}, {"variablePointers", &v11.variablePointers},
	    {"variablePointersStorageBuffer", &v11.variablePointersStorageBuffer}, {"shaderFloat16", &v12.shaderFloat16},
	    {"shaderInt8", &v12.shaderInt8}, {"vulkanMemoryModel", &v12.vulkanMemoryModel},
	    {"vulkanMemoryModelDeviceScope", &v12.vulkanMemoryModelDeviceScope}, {"timelineSemaphore", &v12.timelineSemaphore},
	    {"subgroupSizeControl", &v13.subgroupSizeControl}, {"computeFullSubgroups", &v13.computeFullSubgroups},
	    {"synchronization2", &v13.synchronization2}, {"shaderFloat8", &fp8.shaderFloat8},
	    {"shaderFloat8CooperativeMatrix", &fp8.shaderFloat8CooperativeMatrix}, {"cooperativeMatrix", &coop.cooperativeMatrix},
	    {"cooperativeMatrixConversions (NV)", &coop2.cooperativeMatrixConversions},
	    {"shaderImageInt64Atomics", &atomic64.shaderImageInt64Atomics},
	};
	for (size_t i = 0; i < sizeof(needed) / sizeof(*needed); i++) {
		printf("  %-40s %s\n", needed[i].name, *needed[i].value ? "yes" : "MISSING");
		ok &= *needed[i].value != 0;
	}
	bool wave32 = (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) && p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32;
	printf("  %-40s %s\n", "required subgroup size 32 (compute)", wave32 ? "yes" : "MISSING");
	ok &= wave32;
	if (!ok && !opt.probe && !strcmp(opt.provider, "4.0.2")) {
		fprintf(stderr, "Required features are missing. On RADV, VK_NV_cooperative_matrix2 needs radv_cooperative_matrix2_nv=true "
		                "(run-fg-test.sh sets it).\n");
		exit(3);
	}
	if (!ok) printf("(features for FSR 4.0.2 are missing: continuing for %s)\n", opt.probe ? "the provider probe" : opt.provider);
	features.features.robustBufferAccess = VK_FALSE;
	v13.robustImageAccess = VK_FALSE;
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL);
	VkQueueFamilyProperties qf[32];
	if (nq > 32) nq = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qf);
	queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < nq && queue_family == UINT32_MAX; i++)
		if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) queue_family = i;
	if (queue_family == UINT32_MAX || !qf[queue_family].timestampValidBits) { fprintf(stderr, "no graphics+compute queue with timestamps\n"); exit(3); }
	timestamp_mask = qf[queue_family].timestampValidBits >= 64 ? ~0ull : (1ull << qf[queue_family].timestampValidBits) - 1;
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family, .queueCount = 1,
	                               .pQueuePriorities = &priority};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features, .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci, .enabledExtensionCount = enabled, .ppEnabledExtensionNames = enabled_ext};
	CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
	free(ext);
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define D(name) name = (PFN_##name)gdpa(device, #name)
	D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
	D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateCommandPool);
	D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdPipelineBarrier);
	D(vkCmdCopyBufferToImage); D(vkCmdCopyImageToBuffer); D(vkQueueSubmit); D(vkCreateFence); D(vkWaitForFences);
	D(vkResetFences); D(vkCreateQueryPool); D(vkCmdResetQueryPool); D(vkCmdWriteTimestamp); D(vkGetQueryPoolResults);
	D(vkDeviceWaitIdle); D(vkResetCommandBuffer);
	vkGetDeviceQueue(device, queue_family, 0, &queue);
	vkGetPhysicalDeviceMemoryProperties(gpu, &memory_props);
	printf("Vulkan thunks bound: %u of %u commands\n", VkThunksBind(gipa, instance, gdpa, device), VkThunkCount());
	VkThunkOverride("vkCreateShaderModule", (void*)o_vkCreateShaderModule);
	VkThunkOverride("vkCreateComputePipelines", (void*)o_vkCreateComputePipelines);
	VkThunkOverride("vkGetPhysicalDeviceProperties2", (void*)o_vkGetPhysicalDeviceProperties2);
	VkThunkOverride("vkGetPhysicalDeviceProperties2KHR", (void*)o_vkGetPhysicalDeviceProperties2);
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return i;
	fprintf(stderr, "no memory type\n");
	exit(4);
}

typedef struct Image { VkImage image; VkFormat format; uint32_t w, h, bpp; VkImageLayout layout; } Image;
static Image make_image(VkFormat format, uint32_t w, uint32_t h, uint32_t bpp) {
	Image im = {.format = format, .w = w, .h = h, .bpp = bpp, .layout = VK_IMAGE_LAYOUT_UNDEFINED};
	VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
	                        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL,
	                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
	CHECK(vkCreateImage(device, &ci, NULL, &im.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, im.image, &req);
	VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	                           .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(device, &ai, NULL, &mem));
	CHECK(vkBindImageMemory(device, im.image, mem, 0));
	return im;
}
static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void** mapped) {
	VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
	VkBuffer b;
	CHECK(vkCreateBuffer(device, &ci, NULL, &b));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, b, &req);
	VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	                           .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(device, &ai, NULL, &mem));
	CHECK(vkBindBufferMemory(device, b, mem, 0));
	CHECK(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, mapped));
	return b;
}
static void transition(VkCommandBuffer cb, Image* im, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
	                          .oldLayout = im->layout, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->image,
	                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	im->layout = to;
}
static uint32_t ffx_format(VkFormat f) {
	switch (f) {
	case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
	case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
	default: exit(4);
	}
}
static struct FfxApiResource resource(const Image* im, bool written) {
	struct FfxApiResource r;
	memset(&r, 0, sizeof(r));
	r.resource = (void*)im->image;
	r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	r.description.format = ffx_format(im->format);
	r.description.width = im->w;
	r.description.height = im->h;
	r.description.depth = 1;
	r.description.mipCount = 1;
	r.description.usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
	r.state = written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
	return r;
}

// ------------------------------------------------------------------------------------------------ synthetic scene
// Display pixels; objects move by an even number of pixels per frame, so the exact midpoint picture has integer
// positions. Motion vectors (render pixels) point from the current position to the previous one.
typedef struct Object { float x, y, w, h, vx, vy, z; uint32_t seed; } Object;
static const Object objects[] = {
    {0.08f, 0.30f, 0.12f, 0.14f, 24, 0, 6, 1},
    {0.62f, 0.20f, 0.08f, 0.14f, -12, 8, 10, 2},
    {0.30f, 0.70f, 0.16f, 0.08f, 6, -4, 14, 3},
};
#define OBJECTS (sizeof(objects) / sizeof(*objects))
static void object_rect(const Object* o, double t, uint32_t w, uint32_t h, double* x0, double* y0, double* x1, double* y1) {
	double sx = w / (double)opt.dw;
	*x0 = (o->x * opt.dw + o->vx * t) * sx;
	*y0 = (o->y * opt.dh + o->vy * t) * sx;
	*x1 = *x0 + o->w * opt.dw * sx;
	*y1 = *y0 + o->h * opt.dh * sx;
}
static void background(uint32_t x, uint32_t y, uint8_t* p) {
	p[0] = (uint8_t)(110 + 60 * sin(x * 0.021) * cos(y * 0.013));
	p[1] = (uint8_t)(120 + 50 * sin(x * 0.007 + y * 0.011));
	p[2] = (uint8_t)(130 + 40 * cos(x * 0.017 - y * 0.009));
	p[3] = 255;
}
static bool in_hud(uint32_t x, uint32_t y) { return opt.hud && x >= 40 && x < 440 && y >= 40 && y < 120; }
// Colour picture at time t (display size): background, objects front to back by depth, HUD if asked.
static void render_color(double t, bool with_hud, uint8_t* out) {
	for (uint32_t y = 0; y < opt.dh; y++)
		for (uint32_t x = 0; x < opt.dw; x++) {
			uint8_t* p = out + 4 * ((size_t)y * opt.dw + x);
			float best = 1e9f;
			background(x, y, p);
			for (size_t i = 0; i < OBJECTS; i++) {
				double x0, y0, x1, y1;
				object_rect(&objects[i], t, opt.dw, opt.dh, &x0, &y0, &x1, &y1);
				if (x + 0.5 < x0 || x + 0.5 >= x1 || y + 0.5 < y0 || y + 0.5 >= y1 || objects[i].z >= best) continue;
				best = objects[i].z;
				int u = (int)floor(x + 0.5 - x0), v = (int)floor(y + 0.5 - y0);
				if (opt.organic) { // no repeating pattern, unlike the checker, so a motion estimate has one right answer
					double a = sin(u * 0.043 + objects[i].seed) * cos(v * 0.071 - 2.0 * objects[i].seed) + 0.5 * sin(u * 0.13 + v * 0.09),
					       b = sin(u * 0.019 - v * 0.057 + objects[i].seed) + 0.5 * sin(u * 0.31 - v * 0.27);
					p[0] = (uint8_t)(128 + 80 * a);
					p[1] = (uint8_t)(128 + 80 * b);
					p[2] = (uint8_t)(128 + 80 * sin(u * 0.011 + v * 0.023 + a));
					continue;
				}
				bool check = ((u / 16) + (v / 16)) & 1;
				p[0] = (uint8_t)(check ? 230 : 40 + 60 * objects[i].seed);
				p[1] = (uint8_t)(check ? 200 - 50 * objects[i].seed : 30);
				p[2] = (uint8_t)(check ? 40 : 220 - 40 * objects[i].seed);
			}
			if (with_hud && in_hud(x, y)) {
				bool stripe = ((x / 6) & 1) && ((y - 40) / 10) % 2 == 0;
				p[0] = stripe ? 255 : 20; p[1] = stripe ? 255 : 20; p[2] = stripe ? 255 : 30;
			}
		}
}
static void render_depth_motion(double t, float* depth, uint16_t* motion) {
	const float near = 0.1f;
	for (uint32_t y = 0; y < opt.rh; y++)
		for (uint32_t x = 0; x < opt.rw; x++) {
			size_t i = (size_t)y * opt.rw + x;
			float z = 1000.0f, mx = 0, my = 0;
			for (size_t k = 0; k < OBJECTS; k++) {
				double x0, y0, x1, y1;
				object_rect(&objects[k], t, opt.rw, opt.rh, &x0, &y0, &x1, &y1);
				if (x + 0.5 < x0 || x + 0.5 >= x1 || y + 0.5 < y0 || y + 0.5 >= y1 || objects[k].z >= z) continue;
				z = objects[k].z;
				mx = -objects[k].vx * (float)opt.rw / (float)opt.dw;
				my = -objects[k].vy * (float)opt.rh / (float)opt.dh;
			}
			depth[i] = near / z; // reversed-Z, infinite far plane
			_Float16 hx = (_Float16)mx, hy = (_Float16)my;
			memcpy(motion + 2 * i, &hx, 2);
			memcpy(motion + 2 * i + 1, &hy, 2);
		}
}

// ------------------------------------------------------------------------------------------------ metrics
typedef struct Region { double x0, y0, x1, y1; } Region;
static bool in_moving(uint32_t x, uint32_t y, double t) {
	for (size_t i = 0; i < OBJECTS; i++)
		for (int s = -1; s <= 0; s++) {
			double x0, y0, x1, y1;
			object_rect(&objects[i], t + s, opt.dw, opt.dh, &x0, &y0, &x1, &y1);
			double ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
			object_rect(&objects[i], t + s + 1, opt.dw, opt.dh, &x0, &y0, &x1, &y1);
			ax0 = fmin(ax0, x0);
			ay0 = fmin(ay0, y0);
			ax1 = fmax(ax1, x1);
			ay1 = fmax(ay1, y1);
			if (x + 4 >= ax0 && x <= ax1 + 4 && y + 4 >= ay0 && y <= ay1 + 4) return true;
		}
	return false;
}
// PSNR (dB, RGB) of `a` against `truth`, over every pixel, the moving objects' area, and the HUD.
static void psnr(const uint8_t* a, const uint8_t* truth, double t, double out[3]) {
	double se[3] = {0}, n[3] = {0};
	for (uint32_t y = 0; y < opt.dh; y++)
		for (uint32_t x = 0; x < opt.dw; x++) {
			const uint8_t *p = a + 4 * ((size_t)y * opt.dw + x), *q = truth + 4 * ((size_t)y * opt.dw + x);
			double e = 0;
			for (int c = 0; c < 3; c++) e += (double)(p[c] - q[c]) * (p[c] - q[c]);
			int r[3] = {1, in_moving(x, y, t) && !in_hud(x, y), in_hud(x, y)};
			for (int k = 0; k < 3; k++) if (r[k]) { se[k] += e; n[k] += 3; }
		}
	for (int k = 0; k < 3; k++) out[k] = n[k] == 0 ? NAN : se[k] == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / (se[k] / n[k]));
}
static void write_ppm(const char* name, const uint8_t* rgba) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", opt.out, name);
	FILE* f = fopen(path, "wb");
	if (!f) { perror(path); exit(4); }
	fprintf(f, "P6\n%u %u\n255\n", opt.dw, opt.dh);
	for (size_t i = 0; i < (size_t)opt.dw * opt.dh; i++) fwrite(rgba + 4 * i, 1, 3, f);
	fclose(f);
}
static int compare_double(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }

// ------------------------------------------------------------------------------------------------ AMD runtime
typedef W ffxReturnCode_t (*FnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnDestroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef W ffxReturnCode_t (*FnConfigure)(ffxContext*, const ffxConfigureDescHeader*);
typedef W ffxReturnCode_t (*FnQuery)(ffxContext*, ffxQueryDescHeader*);
typedef W ffxReturnCode_t (*FnDispatch)(ffxContext*, const ffxDispatchDescHeader*);

W static void message(uint32_t type, const uint16_t* text) {
	char b[1024];
	size_t i = 0;
	for (; text && text[i] && i + 1 < sizeof(b); i++) b[i] = text[i] < 128 ? (char)text[i] : '?';
	b[i] = 0;
	fprintf(stderr, "AMD runtime message %u: %s\n", type, b);
}

static int run(void* unused) {
	(void)unused;
	install_crash_reports();
	stage = "creating the Vulkan device";
	create_device();
	PeSetImportResolver(import_resolver);
	PeModule* m = PeLoad(opt.dll);
	PeImageRange(m, &dll_base, &dll_size);
	if (!PeAttach(m)) { fprintf(stderr, "DllMain failed\n"); return 4; }
	FnCreate create = (FnCreate)PeExport(m, "ffxCreateContext");
	FnDestroy destroy = (FnDestroy)PeExport(m, "ffxDestroyContext");
	FnConfigure configure = (FnConfigure)PeExport(m, "ffxConfigure");
	FnQuery query = (FnQuery)PeExport(m, "ffxQuery");
	FnDispatch dispatch = (FnDispatch)PeExport(m, "ffxDispatch");
	VkDeviceContext dc = {device, gpu, (PFN_vkGetDeviceProcAddr)(void*)dll_get_device_proc_addr};
	uint64_t count = 8, ids[8] = {0};
	const char* names[8] = {0};
	struct ffxQueryDescGetVersions versions = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL}, .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
	                                           .device = &dc, .outputCount = &count, .versionIds = ids, .versionNames = names};
	if (query(NULL, &versions.header) || count > 8) { fprintf(stderr, "version query failed\n"); return 4; }
	uint64_t chosen = 0;
	for (uint64_t i = 0; i < count; i++) {
		printf("AMD provider offered: %s\n", names[i] ? names[i] : "?");
		if (names[i] && !strcmp(names[i], opt.provider)) chosen = ids[i];
	}
	if (opt.probe) return 0;
	if (!chosen) { fprintf(stderr, "AMD's runtime does not offer frame generation %s on this device\n", opt.provider); return 5; }

	struct ffxCreateBackendVKDesc backend = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, NULL}, .deviceContext = dc};
	struct ffxOverrideVersion override = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header}, .versionId = chosen};
	struct ffxCreateContextDescFrameGenerationVersion version = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &override.header},
	                                                             .version = FFX_FRAMEGENERATION_VERSION};
	struct ffxCreateContextDescFrameGenerationHudless hudless_desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS, &version.header},
	                                                                  .hudlessBackBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM};
	struct ffxCreateContextDescFrameGeneration desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &hudless_desc.header},
	                                                   .flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE,
	                                                   .displaySize = {opt.dw, opt.dh}, .maxRenderSize = {opt.rw, opt.rh},
	                                                   .backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM};
	ffxContext context = NULL;
	stage = "AMD context creation";
	double create_start = now_ms();
	ffxReturnCode_t rc = create(&context, &desc.header, NULL);
	printf("AMD context creation took %.1f s (%u pipelines, %.1f s compiling)\n", (now_ms() - create_start) / 1000, pipelines_created,
	       pipeline_ms / 1000);
	printf("create=%u; AMD shaders: %u replaced by prepared ones, %u used as shipped; %u matrix pipelines with full subgroups; %u pipeline failures\n",
	       rc, substituted, passed_through, full_subgroup_pipelines, pipeline_failures);
	if (rc || !context || pipeline_failures) return 4;
	struct ffxConfigureDescGlobalDebug debug = {.header = {FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG, NULL}, .fpMessage = (ffxApiMessage)(void*)message,
	                                            .debugLevel = 0xffffffffu};
	configure(&context, &debug.header);

	Image present[2] = {make_image(VK_FORMAT_R8G8B8A8_UNORM, opt.dw, opt.dh, 4), make_image(VK_FORMAT_R8G8B8A8_UNORM, opt.dw, opt.dh, 4)};
	Image hudless = make_image(VK_FORMAT_R8G8B8A8_UNORM, opt.dw, opt.dh, 4);
	Image output = make_image(VK_FORMAT_R8G8B8A8_UNORM, opt.dw, opt.dh, 4);
	Image depth = make_image(VK_FORMAT_R32_SFLOAT, opt.rw, opt.rh, 4);
	Image motion = make_image(VK_FORMAT_R16G16_SFLOAT, opt.rw, opt.rh, 4);
	size_t dbytes = (size_t)opt.dw * opt.dh * 4, rbytes = (size_t)opt.rw * opt.rh * 4;
	uint8_t *upload, *readback;
	VkBuffer upload_buffer = make_buffer(2 * dbytes + 2 * rbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, (void**)&upload);
	VkBuffer readback_buffer = make_buffer(dbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void**)&readback);
	VkCommandPool pool;
	VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                               .queueFamilyIndex = queue_family};
	CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	CHECK(vkAllocateCommandBuffers(device, &cai, &cb));
	VkFence fence;
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK(vkCreateFence(device, &fci, NULL, &fence));
	VkQueryPool queries;
	VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
	CHECK(vkCreateQueryPool(device, &qpi, NULL, &queries));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);

	uint8_t *truth = malloc(dbytes), *previous = malloc(dbytes), *current = malloc(dbytes), *blend = malloc(dbytes);
	double* gpu_ms = calloc(opt.frames + opt.timing_frames, sizeof(double));
	double sums[4][3] = {{0}};
	unsigned scored = 0;
	FILE* csv;
	{
		char path[4096];
		snprintf(path, sizeof(path), "%s/metrics.csv", opt.out);
		csv = fopen(path, "w");
		if (!csv) { perror(path); return 4; }
		fprintf(csv, "frame,gpu_ms,generated_all,generated_moving,generated_hud,repeat_all,repeat_moving,blend_all,blend_moving,next_all,next_moving\n");
	}
	for (uint32_t f = 0; f < opt.frames + opt.timing_frames; f++) {
		// Timing frames reuse the last pictures, so the GPU stays busy instead of idling while the CPU draws the scene.
		bool scoring = f < opt.frames;
		Image* color = &present[f & 1];
		stage = "recording a frame";
		snprintf(stage_detail, sizeof(stage_detail), "%u", f);
		if (scoring) {
			render_color(f, true, upload);
			render_color(f, false, upload + dbytes);
			render_depth_motion(f, (float*)(upload + 2 * dbytes), (uint16_t*)(upload + 2 * dbytes + rbytes));
			memcpy(previous, current, dbytes);
			memcpy(current, upload, dbytes);
		}
		VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		CHECK(vkResetCommandBuffer(cb, 0));
		CHECK(vkBeginCommandBuffer(cb, &bi));
		vkCmdResetQueryPool(cb, queries, 0, 2);
		Image* inputs[4] = {color, &hudless, &depth, &motion};
		VkDeviceSize offsets[4] = {0, dbytes, 2 * dbytes, 2 * dbytes + rbytes};
		for (int i = 0; i < 4; i++) {
			transition(cb, inputs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			VkBufferImageCopy region = {.bufferOffset = offsets[i], .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			                            .imageExtent = {inputs[i]->w, inputs[i]->h, 1}};
			vkCmdCopyBufferToImage(cb, upload_buffer, inputs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			transition(cb, inputs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		}
		if (output.layout != VK_IMAGE_LAYOUT_GENERAL)
			transition(cb, &output, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);

		uint64_t frame_id = f + 1;
		struct ffxConfigureDescFrameGeneration config;
		memset(&config, 0, sizeof(config));
		config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.frameGenerationEnabled = true;
		config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		config.HUDLessColor = resource(&hudless, false);
		config.generationRect = (struct FfxApiRect2D){0, 0, (int32_t)opt.dw, (int32_t)opt.dh};
		config.frameID = frame_id;
		if ((rc = configure(&context, &config.header))) { fprintf(stderr, "configure=%u\n", rc); return 4; }
		struct ffxDispatchDescFrameGenerationPrepareV2 prepare;
		memset(&prepare, 0, sizeof(prepare));
		prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prepare.frameID = frame_id;
		prepare.commandList = cb;
		prepare.renderSize = (struct FfxApiDimensions2D){opt.rw, opt.rh};
		prepare.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
		prepare.frameTimeDelta = 16.67f;
		prepare.reset = f == 0;
		prepare.cameraNear = 0.1f;
		prepare.cameraFar = 1000.0f;
		prepare.cameraFovAngleVertical = 1.0f;
		prepare.viewSpaceToMetersFactor = 1.0f;
		prepare.depth = resource(&depth, false);
		prepare.motionVectors = resource(&motion, false);
		const float up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, forward[3] = {0, 0, -1};
		memcpy(prepare.cameraUp, up, sizeof(up));
		memcpy(prepare.cameraRight, right, sizeof(right));
		memcpy(prepare.cameraForward, forward, sizeof(forward));
		if ((rc = dispatch(&context, &prepare.header))) { fprintf(stderr, "prepare=%u\n", rc); return 4; }
		struct ffxDispatchDescFrameGeneration gen;
		memset(&gen, 0, sizeof(gen));
		gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
		gen.commandList = cb;
		gen.presentColor = resource(color, false);
		gen.outputs[0] = resource(&output, true);
		gen.numGeneratedFrames = 1;
		gen.reset = f == 0;
		gen.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		gen.minMaxLuminance[1] = 1000.0f;
		gen.generationRect = (struct FfxApiRect2D){0, 0, (int32_t)opt.dw, (int32_t)opt.dh};
		gen.frameID = frame_id;
		if ((rc = dispatch(&context, &gen.header))) { fprintf(stderr, "generate=%u\n", rc); return 4; }
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);

		transition(cb, &output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy back = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {opt.dw, opt.dh, 1}};
		vkCmdCopyImageToBuffer(cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1, &back);
		transition(cb, &output, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
		CHECK(vkResetFences(device, 1, &fence));
		stage = "waiting for the GPU (frame)";
		CHECK(vkQueueSubmit(queue, 1, &si, fence));
		VkResult wr = vkWaitForFences(device, 1, &fence, VK_TRUE, 20ull * 1000000000ull);
		if (wr != VK_SUCCESS) { fprintf(stderr, "frame %u: GPU did not finish (%d)\n", f, wr); return 4; }
		uint64_t ts[2];
		CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		gpu_ms[f] = (double)((ts[1] - ts[0]) & timestamp_mask) * props.limits.timestampPeriod / 1e6;
		if (f == 0) { printf("frame 0 (reset): GPU %.3f ms (includes first use)\n", gpu_ms[f]); fflush(stdout); continue; }
		if (!scoring) continue;
		render_color(f - 0.5, true, truth);
		for (size_t i = 0; i < dbytes; i++) blend[i] = (uint8_t)((previous[i] + current[i] + 1) / 2);
		double g[3], rpt[3], bl[3], nx[3];
		psnr(readback, truth, f - 0.5, g);
		psnr(previous, truth, f - 0.5, rpt);
		psnr(blend, truth, f - 0.5, bl);
		psnr(current, truth, f - 0.5, nx);
		fprintf(csv, "%u,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", f, gpu_ms[f], g[0], g[1], g[2], rpt[0], rpt[1], bl[0], bl[1], nx[0], nx[1]);
		printf("frame %2u: GPU %.3f ms | PSNR vs exact midpoint (all / moving / HUD): generated %.2f / %.2f / %.2f dB; "
		       "repeat %.2f / %.2f; blend %.2f / %.2f\n", f, gpu_ms[f], g[0], g[1], g[2], rpt[0], rpt[1], bl[0], bl[1]);
		fflush(stdout);
		if (f >= 2) {
			for (int k = 0; k < 3; k++) { sums[0][k] += g[k]; sums[1][k] += rpt[k]; sums[2][k] += bl[k]; sums[3][k] += nx[k]; }
			scored++;
		}
		if (f == opt.frames / 2) {
			write_ppm("generated.ppm", readback);
			write_ppm("exact-midpoint.ppm", truth);
			write_ppm("blend.ppm", blend);
			write_ppm("previous.ppm", previous);
		}
	}
	fclose(csv);
	vkDeviceWaitIdle(device);
	struct FfxApiEffectMemoryUsage usage = {0};
	struct ffxQueryDescFrameGenerationGetGPUMemoryUsage mem = {.header = {FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE, NULL},
	                                                           .gpuMemoryUsageFrameGeneration = &usage};
	if (!query(&context, &mem.header)) printf("AMD runtime video memory: %.1f MB\n", usage.totalUsageInBytes / 1e6);
	destroy(&context, NULL);
	if (scored) {
		size_t n = opt.frames - 2;
		double* t = malloc(n * sizeof(double));
		memcpy(t, gpu_ms + 2, n * sizeof(double));
		qsort(t, n, sizeof(double), compare_double);
		double mean = 0;
		for (size_t i = 0; i < n; i++) mean += t[i];
		printf("summary over frames 2..%u: generated %.2f dB (moving %.2f, HUD %.2f); repeat %.2f (%.2f); blend %.2f (%.2f); "
		       "GPU mean %.3f ms, median %.3f, max %.3f\n", opt.frames - 1, sums[0][0] / scored, sums[0][1] / scored, sums[0][2] / scored,
		       sums[1][0] / scored, sums[1][1] / scored, sums[2][0] / scored, sums[2][1] / scored, mean / n, t[n / 2], t[n - 1]);
		bool better = sums[0][1] > sums[2][1] + 1.0 && sums[0][1] > sums[1][1] + 1.0;
		printf("%s\n", better ? "RESULT: the generated frames are closer to the exact midpoint than repeating or blending frames."
		                      : "RESULT: the generated frames are NOT clearly better than repeating or blending frames; inspect the pictures.");
		free(t);
	}
	if (opt.timing_frames) {
		size_t n = opt.timing_frames;
		double* t = malloc(n * sizeof(double)), mean = 0;
		memcpy(t, gpu_ms + opt.frames, n * sizeof(double));
		qsort(t, n, sizeof(double), compare_double);
		for (size_t i = 0; i < n; i++) mean += t[i];
		printf("sustained GPU time over %zu frames in a row: mean %.3f ms, p50 %.3f, p95 %.3f, p99 %.3f, max %.3f\n", n, mean / n, t[n / 2], t[n * 95 / 100],
		       t[n * 99 / 100], t[n - 1]);
		free(t);
	}
	return 0;
}

static bool size(const char* s, uint32_t* w, uint32_t* h) { return sscanf(s, "%ux%u", w, h) == 2 && *w >= 64 && *h >= 64 && *w <= 7680 && *h <= 4320; }

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	opt = (Options){.dw = 2560, .dh = 1440, .rw = 1280, .rh = 720, .frames = 12, .hud = true, .full_subgroups = true, .provider = "4.0.2"};
	if (argc < 4) {
		fprintf(stderr, "usage: fsr4fg-gpu DLL PREPARED_DIR NEW_OUTPUT_DIR [--display WxH] [--render WxH] [--frames N] [--no-hud] "
		                "[--no-full-subgroups] [--provider 4.0.2|3.1.6] [--probe] [--organic] [--timing-frames N]\n");
		return 2;
	}
	opt.dll = argv[1];
	opt.prepared = argv[2];
	opt.out = argv[3];
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--display") && i + 1 < argc && size(argv[i + 1], &opt.dw, &opt.dh)) i++;
		else if (!strcmp(argv[i], "--render") && i + 1 < argc && size(argv[i + 1], &opt.rw, &opt.rh)) i++;
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc && (opt.frames = (uint32_t)atoi(argv[++i])) >= 4 && opt.frames <= 240) {}
		else if (!strcmp(argv[i], "--no-hud")) opt.hud = false;
		else if (!strcmp(argv[i], "--no-full-subgroups")) opt.full_subgroups = false;
		else if (!strcmp(argv[i], "--provider") && i + 1 < argc) opt.provider = argv[++i];
		else if (!strcmp(argv[i], "--probe")) opt.probe = true;
		else if (!strcmp(argv[i], "--organic")) opt.organic = true;
		else if (!strcmp(argv[i], "--timing-frames") && i + 1 < argc && (opt.timing_frames = (uint32_t)atoi(argv[++i])) >= 1 && opt.timing_frames <= 100000) {}
		else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
	}
	if (opt.rw > opt.dw || opt.rh > opt.dh) { fprintf(stderr, "render size above display size\n"); return 2; }
	if (mkdir(opt.out, 0755) && errno != EEXIST) { perror(opt.out); return 2; }
	char manifest[4096];
	snprintf(manifest, sizeof(manifest), "%s/manifest.json", opt.prepared);
	FILE* f = fopen(manifest, "r");
	if (!f) { fprintf(stderr, "%s missing: run prepare_fg.py first\n", manifest); return 2; }
	fclose(f);
	printf("FSR frame generation %s GPU test: AMD's runtime on this Linux Vulkan driver, %ux%u (render %ux%u), %u frames\n",
	       opt.provider, opt.dw, opt.dh, opt.rw, opt.rh, opt.frames);
	return PeRunOnWindowsThread(run, NULL);
}
