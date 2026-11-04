// SPDX-License-Identifier: GPL-2.0-or-later
// fsr4fg-gpu: runs AMD's FSR Frame Generation 4.0.2 runtime (amd_fidelityfx_framegeneration_vk.dll, SDK 2.4.0) on
// the real Vulkan device of this Linux machine, through pehost and Windows-ABI thunks, with AMD's shaders replaced by
// the prepared ones (prepare_fg.py) where RADV needs them. It renders a synthetic scene whose objects move by known
// amounts, generates the frame between each pair, reads it back and compares it with the exact midpoint picture and
// with simple baselines (repeating a frame, blending two). A test tool: it changes nothing in the emulator.
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dirent.h>
#include <dlfcn.h>
#include <asm/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include "fg_runtime.h"
#include "pehost.h"
#include "vk_thunks.h"

#define sha256_hex FgRtSha256Hex
#define GUEST_END 0xFC00000000ull // (above the emulator's guest range: see reserve_guest_layout)
#define CHECK(x)                                                                                                       \
	do {                                                                                                               \
		VkResult r_ = (x);                                                                                             \
		if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(4); }           \
	} while (0)

// ------------------------------------------------------------------------------------------------ options/state
typedef struct Options {
	const char *dll, *prepared, *out, *provider;
	uint32_t    dw, dh, rw, rh, frames, timing_frames, self_test_timeout;
	bool        hud, full_subgroups, probe, organic, guest_layout, no_pictures, identity, self_test, force;
	bool        game_inputs, game_device, hudless_image, hudless_desc, pq;
	VkFormat    color_format, depth_format;
	uint32_t    resize_at, resize_w, resize_h;
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
	const char* doing = fg_rt_stage;
	snprintf(line, sizeof(line), "\n[FG] STOPPED by signal %d (%s) while: %s %s\n  last Vulkan call from AMD's code: %s (%lu calls)\n",
	         sig, strsignal(sig), doing ? doing : stage, doing ? fg_rt_stage_detail : stage_detail, vk_thunk_last ? vk_thunk_last : "none", vk_thunk_calls);
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
	stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16}; // (per thread: the test's and AMD's)
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

// ------------------------------------------------------------------------------------------------ device
typedef struct Feature { const char* name; VkBool32* value; } Feature;

static bool has_extension(const VkExtensionProperties* e, uint32_t n, const char* name) {
	for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
	return false;
}

// tests/fsr4fg-self-test.py: what a driver can do to this process, without needing a driver that does it.
static void inject_fault(void) {
	const char* fault = getenv("FSR4FG_INJECT");
	if (!fault) return;
	stage = "an injected fault";
	if (!strcmp(fault, "crash")) {
		int* volatile null_page = NULL;
		*null_page = 1;
	}
	if (!strcmp(fault, "fatal")) PeFatal("injected");
	if (!strcmp(fault, "hang-hard")) signal(SIGTERM, SIG_IGN);
	if (!strncmp(fault, "hang", 4)) for (;;) pause();
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
	if (opt.identity) {
		printf("identity: %04x:%04x:%u:%s:%s\n", props.properties.vendorID, props.properties.deviceID, props.properties.driverVersion,
		       driver.driverName, driver.driverInfo);
		fflush(stdout);
		_exit(0);
	}
	inject_fault();

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
	const char* enabled_ext[6];
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
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
	if (opt.game_device) {
		// The device the emulator makes (vulkanWindow.cpp): robust buffers and images on, and of everything else only what
		// AMD's programs declare (the emulator enables these for FSR 4 frame generation).
		VkPhysicalDeviceRobustness2FeaturesEXT have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
		VkPhysicalDeviceFeatures2 ask = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have};
		if (has_extension(ext, next, "VK_EXT_robustness2")) {
			vkGetPhysicalDeviceFeatures2(gpu, &ask);
			enabled_ext[enabled++] = "VK_EXT_robustness2";
			robustness2.robustBufferAccess2 = have.robustBufferAccess2;
			robustness2.robustImageAccess2 = have.robustImageAccess2;
			robustness2.nullDescriptor = have.nullDescriptor;
			robustness2.pNext = features.pNext;
			features.pNext = &robustness2;
		}
		VkPhysicalDeviceFeatures base = {.robustBufferAccess = VK_TRUE, .shaderInt16 = VK_TRUE, .shaderInt64 = VK_TRUE, .shaderStorageImageExtendedFormats = VK_TRUE,
		                                 .shaderStorageImageReadWithoutFormat = VK_TRUE, .shaderStorageImageWriteWithoutFormat = VK_TRUE};
		features.features = base;
		void* next11 = v11.pNext;
		memset(&v11, 0, sizeof(v11));
		v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		v11.pNext = next11;
		v11.storageBuffer16BitAccess = v11.variablePointers = v11.variablePointersStorageBuffer = VK_TRUE;
		void* next12 = v12.pNext;
		memset(&v12, 0, sizeof(v12));
		v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		v12.pNext = next12;
		v12.shaderFloat16 = v12.shaderInt8 = v12.vulkanMemoryModel = v12.vulkanMemoryModelDeviceScope = v12.timelineSemaphore = VK_TRUE;
		void* next13 = v13.pNext;
		VkBool32 robust_image = v13.robustImageAccess;
		memset(&v13, 0, sizeof(v13));
		v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		v13.pNext = next13;
		v13.robustImageAccess = robust_image;
		v13.subgroupSizeControl = v13.computeFullSubgroups = v13.synchronization2 = VK_TRUE;
		printf("device as the emulator makes it: robustBufferAccess on, robustImageAccess %u, robustBufferAccess2 %u, robustImageAccess2 %u\n", robust_image,
		       robustness2.robustBufferAccess2, robustness2.robustImageAccess2);
	} else {
		features.features.robustBufferAccess = VK_FALSE;
		v13.robustImageAccess = VK_FALSE;
	}
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
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return i;
	fprintf(stderr, "no memory type\n");
	exit(4);
}

typedef struct Image { VkImage image; VkFormat format; uint32_t w, h, bpp; VkImageLayout layout; VkImageUsageFlags usage; } Image;
static VkImageAspectFlags barrier_aspect(VkFormat f) {
	return f == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
	       : f == VK_FORMAT_D32_SFLOAT       ? VK_IMAGE_ASPECT_DEPTH_BIT
	                                         : VK_IMAGE_ASPECT_COLOR_BIT;
}
#define DISPLAY_USAGE (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
static Image make_image(VkFormat format, uint32_t w, uint32_t h, uint32_t bpp, VkImageUsageFlags usage) {
	Image im = {.format = format, .w = w, .h = h, .bpp = bpp, .layout = VK_IMAGE_LAYOUT_UNDEFINED, .usage = usage};
	VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
	                        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
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
	                          .subresourceRange = {barrier_aspect(im->format), 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	im->layout = to;
}
static FgRtImage rt_image(const Image* im) { return (FgRtImage){im->image, im->format, im->w, im->h, im->usage, im->layout}; }
// The scene is drawn as RGBA8; the device's image may be laid out otherwise.
static void to_device(VkFormat format, uint8_t* pixels, size_t bytes) {
	for (size_t i = 0; i < bytes; i += 4) {
		uint8_t* p = pixels + i;
		if (format == VK_FORMAT_B8G8R8A8_UNORM) {
			uint8_t r = p[0];
			p[0] = p[2];
			p[2] = r;
		} else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
			uint32_t v = (uint32_t)p[0] * 1023 / 255 | (uint32_t)p[1] * 1023 / 255 << 10 | (uint32_t)p[2] * 1023 / 255 << 20 | 3u << 30;
			memcpy(p, &v, 4);
		}
	}
}
static void from_device(VkFormat format, const uint8_t* in, uint8_t* out, size_t bytes) {
	for (size_t i = 0; i < bytes; i += 4) {
		const uint8_t* p = in + i;
		uint8_t*       q = out + i;
		if (format == VK_FORMAT_B8G8R8A8_UNORM) {
			q[0] = p[2];
			q[1] = p[1];
			q[2] = p[0];
			q[3] = p[3];
		} else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
			uint32_t v;
			memcpy(&v, p, 4);
			q[0] = (uint8_t)((v & 1023) * 255 / 1023);
			q[1] = (uint8_t)((v >> 10 & 1023) * 255 / 1023);
			q[2] = (uint8_t)((v >> 20 & 1023) * 255 / 1023);
			q[3] = 255;
		} else {
			memcpy(q, p, 4);
		}
	}
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
				// (--game-inputs: as the game's, UV units along the motion. Otherwise render pixels towards the previous position.)
				mx = opt.game_inputs ? objects[k].vx / (float)opt.dw : -objects[k].vx * (float)opt.rw / (float)opt.dw;
				my = opt.game_inputs ? objects[k].vy / (float)opt.dh : -objects[k].vy * (float)opt.rh / (float)opt.dh;
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
static void wait_idle(void) { vkDeviceWaitIdle(device); }

static int failed_status(void) {
	fprintf(stderr, "AMD's runtime is unavailable: %s\n", FgRtReason());
	return FgRtStopped() ? PE_EXIT_FATAL : FgRtNotOffered() ? 5 : 4;
}

static int run(void) {
	install_crash_reports();
	stage = "creating the Vulkan device";
	create_device();
	FgRtConfig config = {.dll = opt.dll, .prepared = opt.prepared, .provider = opt.provider, .instance = instance, .physical_device = gpu, .device = device,
	                     .get_instance_proc_addr = gipa, .get_device_proc_addr = gdpa, .minimum_address = opt.guest_layout ? GUEST_END : 0,
	                     .full_subgroups = opt.full_subgroups, .hudless_desc = opt.hudless_desc, .verbose = true, .probe = opt.probe,
	                     .on_thread_start = install_crash_reports, .wait_idle = wait_idle};
	FgRtStart(&config);
	FgRtWaitLoaded();
	if (FgRtGetState() == FG_RT_FAILED) return failed_status();
	FgRtStats loaded = FgRtGetStats();
	dll_base = loaded.image_base;
	dll_size = loaded.image_size;
	printf("Vulkan thunks bound: %u of %u commands\n", loaded.thunks_bound, loaded.thunks_total);
	const char* names[8];
	unsigned    name_count = 8;
	FgRtProviders(names, &name_count);
	for (unsigned i = 0; i < name_count; i++) printf("AMD provider offered: %s\n", names[i] ? names[i] : "?");
	if (opt.probe) return 0;

	const VkImageUsageFlags color_usage = opt.game_inputs ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
	                                                       : DISPLAY_USAGE;
	const VkImageUsageFlags depth_usage = opt.game_inputs ? VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT : DISPLAY_USAGE;
	const VkImageUsageFlags motion_usage = opt.game_inputs ? VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : DISPLAY_USAGE;
	Image present[2] = {make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage), make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage)};
	Image hudless = make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage);
	Image output = make_image(opt.color_format, opt.dw, opt.dh, 4, DISPLAY_USAGE);
	Image depth = make_image(opt.depth_format, opt.rw, opt.rh, 4, depth_usage);
	Image motion = make_image(VK_FORMAT_R16G16_SFLOAT, opt.rw, opt.rh, 4, motion_usage);
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

	uint8_t *truth = malloc(dbytes), *previous = malloc(dbytes), *current = malloc(dbytes), *blend = malloc(dbytes), *generated = malloc(dbytes);
	bool     reset_next = true, runtime_gone = false;
	double* gpu_ms = calloc(opt.frames + opt.timing_frames, sizeof(double));
	double* record_ms = calloc(opt.frames + opt.timing_frames, sizeof(double));
	double sums[4][3] = {{0}};
	double margin_sum = 0, margin_worst = INFINITY;
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
			if (opt.resize_at && f == opt.resize_at) {
				// A new display size, as a window resize gives: AMD's context has to be made again, on a running device.
				printf("frame %u: display %ux%u -> %ux%u\n", f, opt.dw, opt.dh, opt.resize_w, opt.resize_h);
				opt.dw = opt.resize_w;
				opt.dh = opt.resize_h;
				dbytes = (size_t)opt.dw * opt.dh * 4;
				present[0] = make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage);
				present[1] = make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage);
				hudless = make_image(opt.color_format, opt.dw, opt.dh, 4, color_usage);
				output = make_image(opt.color_format, opt.dw, opt.dh, 4, DISPLAY_USAGE);
				reset_next = true;
			}
			render_color(f, true, upload);
			render_color(f, false, upload + dbytes);
			render_depth_motion(f, (float*)(upload + 2 * dbytes), (uint16_t*)(upload + 2 * dbytes + rbytes));
			memcpy(previous, current, dbytes);
			memcpy(current, upload, dbytes);
			to_device(opt.color_format, upload, 2 * dbytes);
		}
		VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		CHECK(vkResetCommandBuffer(cb, 0));
		CHECK(vkBeginCommandBuffer(cb, &bi));
		vkCmdResetQueryPool(cb, queries, 0, 2);
		Image* inputs[4] = {color, &hudless, &depth, &motion};
		VkDeviceSize offsets[4] = {0, dbytes, 2 * dbytes, 2 * dbytes + rbytes};
		for (int i = 0; i < 4; i++) {
			transition(cb, inputs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			VkImageAspectFlags copy_aspect = barrier_aspect(inputs[i]->format) & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			VkBufferImageCopy region = {.bufferOffset = offsets[i], .imageSubresource = {copy_aspect, 0, 0, 1},
			                            .imageExtent = {inputs[i]->w, inputs[i]->h, 1}};
			vkCmdCopyBufferToImage(cb, upload_buffer, inputs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			// (--game-inputs: the finished frame is general, as the presenter leaves it.)
			transition(cb, inputs[i], opt.game_inputs && i == 0 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		}
		if (output.layout != VK_IMAGE_LAYOUT_GENERAL)
			transition(cb, &output, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);

		FgRtFrame frame = {.command = cb, .color = rt_image(color), .depth = rt_image(&depth), .motion = rt_image(&motion), .output = rt_image(&output),
		                   .rect = {0, 0, (int32_t)opt.dw, (int32_t)opt.dh},
		                   .motion_scale = {opt.game_inputs ? -(float)opt.rw : 1.0f, opt.game_inputs ? -(float)opt.rh : 1.0f},
		                   .frame_ms = 16.67f, .near_plane = 0.1f, .fov_y = 1.0f, .up = {0, 1, 0}, .right = {1, 0, 0}, .forward = {0, 0, -1},
		                   .reset = reset_next, .pq = opt.pq};
		if (opt.hudless_image) frame.hudless = rt_image(&hudless);
		const char* fault = getenv("FSR4FG_INJECT");
		if (f == 2 && fault && !strcmp(fault, "fatal-frame")) FgRtInjectStop();
		FgRtResult made;
		// (The first frames find the context still being made: the game shows them without a generated one.)
		double record_start = now_ms();
		while ((made = FgRtGenerate(&frame)) == FG_RT_NOT_YET) {
			struct timespec tick = {0, 5000000};
			nanosleep(&tick, NULL);
			record_start = now_ms();
		}
		record_ms[f] = now_ms() - record_start;
		if (made == FG_RT_UNAVAILABLE) {
			// As the game goes on without AMD's runtime: this thread is unharmed and the device still works.
			printf("frame %u: AMD's runtime is gone (%s), going on without it\n", f, FgRtReason());
			fflush(stdout);
			runtime_gone = true;
		}
		reset_next = false;
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
		if (f == 0) {
			FgRtStats made_stats = FgRtGetStats();
			printf("AMD context creation took %.1f s (%u pipelines, %.1f s compiling)\n", made_stats.create_seconds, made_stats.pipelines, made_stats.compile_seconds);
			printf("provider %s; AMD shaders: %u replaced by prepared ones, %u used as shipped; %u matrix pipelines with full subgroups; %u pipeline failures\n",
			       FgRtActiveProvider(), made_stats.replaced, made_stats.as_shipped, made_stats.matrix_pipelines, made_stats.pipeline_failures);
			printf("frame 0 (reset): GPU %.3f ms (includes first use)\n", gpu_ms[f]);
			fflush(stdout);
			continue;
		}
		if (runtime_gone) continue;
		if (opt.resize_at && (f == opt.resize_at || f == opt.resize_at + 1)) continue; // (before and at the reset: nothing to score)
		if (!scoring) continue;
		render_color(f - 0.5, true, truth);
		for (size_t i = 0; i < dbytes; i++) blend[i] = (uint8_t)((previous[i] + current[i] + 1) / 2);
		from_device(opt.color_format, readback, generated, dbytes);
		double g[3], rpt[3], bl[3], nx[3];
		psnr(generated, truth, f - 0.5, g);
		psnr(previous, truth, f - 0.5, rpt);
		psnr(blend, truth, f - 0.5, bl);
		psnr(current, truth, f - 0.5, nx);
		fprintf(csv, "%u,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", f, gpu_ms[f], g[0], g[1], g[2], rpt[0], rpt[1], bl[0], bl[1], nx[0], nx[1]);
		printf("frame %2u: GPU %.3f ms | PSNR vs exact midpoint (all / moving / HUD): generated %.2f / %.2f / %.2f dB; "
		       "repeat %.2f / %.2f; blend %.2f / %.2f\n", f, gpu_ms[f], g[0], g[1], g[2], rpt[0], rpt[1], bl[0], bl[1]);
		fflush(stdout);
		if (f >= 2) {
			for (int k = 0; k < 3; k++) { sums[0][k] += g[k]; sums[1][k] += rpt[k]; sums[2][k] += bl[k]; sums[3][k] += nx[k]; }
			double margin = g[1] - fmax(rpt[1], bl[1]);
			margin_sum += margin;
			margin_worst = fmin(margin_worst, margin);
			scored++;
		}
		if (f == opt.frames / 2 && !opt.no_pictures) {
			write_ppm("generated.ppm", generated);
			write_ppm("exact-midpoint.ppm", truth);
			write_ppm("blend.ppm", blend);
			write_ppm("previous.ppm", previous);
		}
	}
	fclose(csv);
	vkDeviceWaitIdle(device);
	printf("AMD runtime video memory: %.1f MB\n", FgRtGetStats().memory_bytes / 1e6);
	unsigned long gs = 0;
	syscall(SYS_arch_prctl, ARCH_GET_GS, &gs);
	printf("this thread's GS base: %#lx (AMD's code runs on its own thread)\n", gs);
	printf("queue calls by AMD's code: %u\n", FgRtGetStats().queue_calls);
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
		printf("margin: moving areas over the better of repeat and blend: mean %.2f dB, worst frame %.2f dB\n", margin_sum / scored, margin_worst);
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
		// What the caller's thread waits for AMD's thread to record (the emulator holds the renderer's lock meanwhile).
		memcpy(t, record_ms + opt.frames, n * sizeof(double));
		qsort(t, n, sizeof(double), compare_double);
		mean = 0;
		for (size_t i = 0; i < n; i++) mean += t[i];
		printf("CPU time recording AMD's frame, over the same frames: mean %.3f ms, p50 %.3f, p95 %.3f, p99 %.3f, max %.3f\n", mean / n, t[n / 2], t[n * 95 / 100],
		       t[n * 99 / 100], t[n - 1]);
		free(t);
	}
	if (runtime_gone) return failed_status();
	return 0;
}

// ------------------------------------------------------------------------------------------------ guest layout
// What the emulator does on Linux at startup (src/kernel/memoryAddressSpace.inc): every free address of the guest's
// range is taken, PROT_NONE. AMD's preferred base (0x180000000) is inside it, so the image gets relocated above it.
#define GUEST_MIN 0x40000ull
static void reserve_gap(uint64_t start, uint64_t end) {
	if (start < GUEST_MIN) start = GUEST_MIN;
	if (end > GUEST_END) end = GUEST_END;
	if (end <= start) return;
	void* p = mmap((void*)(uintptr_t)start, end - start, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (p != (void*)(uintptr_t)start) { fprintf(stderr, "cannot reserve the guest range at %#llx: %s\n", (unsigned long long)start, strerror(errno)); exit(4); }
}
static void reserve_guest_layout(void) {
	FILE* maps = fopen("/proc/self/maps", "r");
	if (!maps) { perror("/proc/self/maps"); exit(4); }
	char line[512];
	uint64_t at = GUEST_MIN;
	// (Read first: mapping while reading would change the list.)
	static struct { uint64_t start, end; } taken[4096];
	size_t n = 0;
	while (fgets(line, sizeof(line), maps) && n < 4096) {
		unsigned long long a, b;
		if (sscanf(line, "%llx-%llx", &a, &b) == 2) { taken[n].start = a; taken[n].end = b; n++; }
	}
	fclose(maps);
	for (size_t i = 0; i < n; i++) {
		reserve_gap(at, taken[i].start);
		if (taken[i].end > at) at = taken[i].end;
	}
	reserve_gap(at, GUEST_END);
	printf("guest layout: %#llx..%#llx reserved, as the emulator does\n", GUEST_MIN, GUEST_END);
}

// ------------------------------------------------------------------------------------------------ self-test
// --self-test: can AMD's runtime run on this machine? The answer has to survive the driver crashing or hanging, which
// no fallback inside the game could catch, so this process never touches Vulkan: it runs itself as a child, reads what
// the child says and writes the verdict to STATE_DIR/self-test.json (0: pass, 1: fail, 2: could not test). A verdict
// holds until the driver, GPU, AMD's DLL, the prepared programs or these rules change (--force tests anyway).
#define SELF_TEST_RULES 3
#define SELF_TEST_FRAMES "6"
#define SELF_TEST_DISPLAY "1280x720"
#define SELF_TEST_RENDER "640x360"
// Moving areas, in dB above the better of repeating and blending, on the worst frame. See FG-TEST.md for how it was set.
#define SELF_TEST_MARGIN 12.0
#define DLL_SHA256 "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"

static void* read_all(const char* path, size_t* size_out) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* data = n >= 0 ? malloc((size_t)n + 1) : NULL;
	if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
	fclose(f);
	if (data) { data[n] = 0; *size_out = (size_t)n; }
	return data;
}
static bool file_sha256(const char* path, char hex[65]) {
	size_t n;
	void* data = read_all(path, &n);
	if (!data) return false;
	sha256_hex(data, n, hex);
	free(data);
	return true;
}
// One hash for everything in the prepared folder: its files' contents, not what the manifest says they are.
static bool programs_sha256(const char* dir, char hex[65]) {
	struct dirent** names;
	int count = scandir(dir, &names, NULL, alphasort);
	if (count < 0) return false;
	char*  list = malloc((size_t)count * 340 + 1);
	size_t used = 0;
	bool   ok = list != NULL;
	for (int i = 0; i < count; i++) {
		char path[4096 + 256], file_hex[65];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]->d_name);
		struct stat info;
		if (ok && !stat(path, &info) && S_ISREG(info.st_mode)) {
			if (file_sha256(path, file_hex)) used += (size_t)sprintf(list + used, "%.255s:%s\n", names[i]->d_name, file_hex);
			else ok = false;
		}
		free(names[i]);
	}
	free(names);
	if (ok) sha256_hex(list, used, hex);
	free(list);
	return ok;
}
static void json_string(FILE* f, const char* name, const char* value, const char* tail) {
	fprintf(f, " \"%s\": \"", name);
	for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
		if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
		else if (*c < 32) fprintf(f, "\\u%04x", *c);
		else fputc(*c < 127 ? *c : '?', f); // (child output need not be valid UTF-8; the file must stay valid JSON)
	}
	fprintf(f, "\"%s\n", tail);
}

typedef struct ChildOutput {
	FILE*  log;
	char   identity[512], stopped[512], where[512], fatal[512], last[512], missing[1024], progress[512];
	double margin_worst, margin_mean;
	bool   has_margin;
} ChildOutput;

static void child_line(ChildOutput* c, const char* line) {
	if (c->log) fprintf(c->log, "%s\n", line);
	double mean, worst;
	size_t length = strlen(line);
	if (!strncmp(line, "identity: ", 10)) snprintf(c->identity, sizeof(c->identity), "%s", line + 10);
	else if (!strncmp(line, "[FG] STOPPED", 12)) snprintf(c->stopped, sizeof(c->stopped), "%s", line + 5);
	else if (!strncmp(line, "  instruction ", 14)) snprintf(c->where, sizeof(c->where), "%s", line + 2);
	else if (!strncmp(line, "pehost: ", 8) && strstr(line, "provider read") == NULL && strstr(line, "loaded ") == NULL &&
	         strstr(line, "unresolved import") == NULL && strstr(line, "LoadLibrary") == NULL) snprintf(c->fatal, sizeof(c->fatal), "%s", line + 8);
	else if (!strncmp(line, "compiling pipeline ", 19)) snprintf(c->progress, sizeof(c->progress), "%s", line);
	else if (sscanf(line, "margin: moving areas over the better of repeat and blend: mean %lf dB, worst frame %lf dB", &mean, &worst) == 2) {
		c->margin_mean = mean;
		c->margin_worst = worst;
		c->has_margin = true;
	} else if (length > 8 && !strcmp(line + length - 7, "MISSING")) {
		const char* name = line + strspn(line, " ");
		const char* end  = strstr(name, "  "); // the status column
		size_t      used = strlen(c->missing);
		if (end) snprintf(c->missing + used, sizeof(c->missing) - used, "%s%.*s", used ? ", " : "", (int)(end - name), name);
	}
	if (length && strncmp(line, "RESULT", 6)) snprintf(c->last, sizeof(c->last), "%s", line);
}

typedef enum ChildEnd { CHILD_EXITED, CHILD_SIGNALED, CHILD_TIMED_OUT, CHILD_STUCK, CHILD_NOT_STARTED } ChildEnd;

// Runs this program with `args`; every line goes to child_line. A child past `seconds` gets SIGTERM (its crash report
// says where it was), then SIGKILL. One stuck in the kernel (a GPU reset) is left behind instead of waited for.
static ChildEnd run_child(char** args, uint32_t seconds, ChildOutput* out, int* status) {
	int fds[2];
	if (pipe(fds)) return CHILD_NOT_STARTED;
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return CHILD_NOT_STARTED; }
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		struct rlimit no_core = {0, 0};
		setrlimit(RLIMIT_CORE, &no_core); // an expected outcome here, not one to keep a dump of
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		execv("/proc/self/exe", args);
		_exit(127);
	}
	close(fds[1]);
	char   buffer[8192], line[4096];
	size_t used = 0;
	double start = now_ms(), deadline = start + seconds * 1000.0;
	bool   timed_out = false, killed = false, open_pipe = true;
	while (open_pipe) {
		double remaining = deadline - now_ms();
		if (remaining <= 0) {
			if (!timed_out) { timed_out = true; kill(pid, SIGTERM); deadline = now_ms() + 5000; continue; }
			if (killed) break; // not even SIGKILL ended it: stuck in the kernel
			killed = true;
			kill(pid, SIGKILL);
			deadline = now_ms() + 10000;
			continue;
		}
		struct pollfd p = {fds[0], POLLIN, 0};
		int r = poll(&p, 1, remaining > 1000 ? 1000 : (int)remaining + 1);
		if (r < 0 && errno != EINTR) break;
		if (r <= 0) continue;
		ssize_t n = read(fds[0], buffer, sizeof(buffer));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { open_pipe = false; break; }
		for (ssize_t i = 0; i < n; i++) {
			if (buffer[i] == '\n' || used + 1 == sizeof(line)) {
				line[used] = 0;
				child_line(out, line);
				used = 0;
			} else {
				line[used++] = buffer[i];
			}
		}
	}
	if (used) { line[used] = 0; child_line(out, line); }
	close(fds[0]);
	// (The pipe also ends when the child dies; give that a moment, then give up on one stuck in the kernel.)
	for (double until = now_ms() + (killed ? 1000 : 15000);;) {
		pid_t w = waitpid(pid, status, WNOHANG);
		if (w == pid) break;
		if (now_ms() > until) {
			if (!killed) { killed = true; kill(pid, SIGKILL); until = now_ms() + 10000; continue; }
			return CHILD_STUCK;
		}
		struct timespec tick = {0, 20000000};
		nanosleep(&tick, NULL);
	}
	if (timed_out) return CHILD_TIMED_OUT;
	return WIFSIGNALED(*status) ? CHILD_SIGNALED : CHILD_EXITED;
}

static int self_test(char* self) {
	char path[4096], dll_hex[65], programs_hex[65], asset[128] = "?";
	if (!file_sha256(opt.dll, dll_hex)) { fprintf(stderr, "self-test: cannot read %s\n", opt.dll); return 2; }
	if (strcmp(dll_hex, DLL_SHA256)) { fprintf(stderr, "self-test: that is not the SDK 2.4.0 frame generation DLL\n"); return 2; }
	if (!programs_sha256(opt.prepared, programs_hex)) { fprintf(stderr, "self-test: cannot read the prepared programs in %s\n", opt.prepared); return 2; }
	snprintf(path, sizeof(path), "%s/ASSET_VERSION", opt.prepared);
	size_t asset_size;
	char*  asset_text = read_all(path, &asset_size);
	if (asset_text) {
		snprintf(asset, sizeof(asset), "%s", asset_text);
		asset[strcspn(asset, "\r\n")] = 0;
		free(asset_text);
	}
	// RADV exposes VK_NV_cooperative_matrix2 only with this (as run-fg-test.sh and the launcher set it); a value the
	// user set stays.
	setenv("radv_cooperative_matrix2_nv", "true", 0);

	// What is being tested. Asking the driver can crash too, so a child does that as well.
	ChildOutput who;
	memset(&who, 0, sizeof(who));
	int   status = 0;
	char* ask[] = {self, (char*)opt.dll, (char*)opt.prepared, (char*)opt.out, "--identity", NULL};
	ChildEnd asked = run_child(ask, 60, &who, &status);
	char key[1024];
	const char* verdict;
	char   reason[1600] = "";
	double started = now_ms();
	int    code;
	bool   transient = false;
	ChildOutput run;
	memset(&run, 0, sizeof(run));
	if (asked != CHILD_EXITED || WEXITSTATUS(status) != 0 || !who.identity[0]) {
		// No key can be made without the driver's answer, so nothing is cached: the next start asks again.
		snprintf(key, sizeof(key), "no driver answer");
		verdict = "fail";
		code    = 1;
		snprintf(reason, sizeof(reason), "the Vulkan driver did not answer: %s", who.stopped[0] ? who.stopped : who.last[0] ? who.last : "no output");
	} else {
		snprintf(key, sizeof(key), "rules %d|%s|dll %.16s|programs %.16s|%s|matrix2 %s", SELF_TEST_RULES, who.identity, dll_hex, programs_hex, asset,
		         getenv("radv_cooperative_matrix2_nv"));
		// A verdict for exactly this?
		snprintf(path, sizeof(path), "%s/self-test.json", opt.out);
		size_t stored_size = 0;
		char*  stored = opt.force ? NULL : read_all(path, &stored_size);
		if (stored) {
			char wanted[1200];
			snprintf(wanted, sizeof(wanted), "\"key\": \"%s\"", key);
			// (A failure that may not happen again, such as a timeout on a busy machine, is tested again.)
			if (strstr(stored, wanted) && !strstr(stored, "\"transient\": true")) {
				bool pass = strstr(stored, "\"verdict\": \"pass\"") != NULL;
				printf("self-test: %s (kept verdict for %s)\n", pass ? "PASS" : "FAIL", key);
				const char* said = strstr(stored, "\"reason\": \"");
				if (said && !pass) {
					said += 11;
					printf("self-test: reason: %.*s\n", (int)strcspn(said, "\n") - 2, said);
				}
				free(stored);
				return pass ? 0 : 1;
			}
			free(stored);
		}
		char dir[4096], log_path[4096];
		snprintf(dir, sizeof(dir), "%s/self-test-run", opt.out);
		snprintf(log_path, sizeof(log_path), "%s/self-test.log", opt.out);
		run.log = fopen(log_path, "w");
		char seconds[16];
		snprintf(seconds, sizeof(seconds), "%u", opt.self_test_timeout);
		char* args[] = {self, (char*)opt.dll, (char*)opt.prepared, dir, "--organic", "--frames", SELF_TEST_FRAMES, "--display", SELF_TEST_DISPLAY,
		                "--render", SELF_TEST_RENDER, "--guest-layout", "--game-inputs", "--game-device", "--no-pictures", NULL};
		printf("self-test: running AMD's runtime on %s\n", who.identity);
		fflush(stdout);
		ChildEnd end = run_child(args, opt.self_test_timeout, &run, &status);
		if (run.log) fclose(run.log);
		verdict = "fail";
		code    = 1;
		if (end == CHILD_NOT_STARTED) {
			fprintf(stderr, "self-test: cannot start a child: %s\n", strerror(errno));
			return 2;
		} else if (end == CHILD_TIMED_OUT || end == CHILD_STUCK) {
			transient = true;
			snprintf(reason, sizeof(reason), "no result within %u s%s; last: %s", opt.self_test_timeout, end == CHILD_STUCK ? " and it could not be stopped" : "",
			         run.stopped[0] ? run.stopped : run.progress[0] ? run.progress : run.last);
		} else if (end == CHILD_SIGNALED) {
			transient = WTERMSIG(status) == SIGKILL || WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGINT; // out of memory, or stopped from outside
			snprintf(reason, sizeof(reason), "crashed: %s%s%s", run.stopped[0] ? run.stopped : strsignal(WTERMSIG(status)), run.where[0] ? "; " : "", run.where);
		} else if (WEXITSTATUS(status) == 3) {
			snprintf(reason, sizeof(reason), "the driver lacks: %s", run.missing[0] ? run.missing : run.last);
		} else if (WEXITSTATUS(status) == 5) {
			snprintf(reason, sizeof(reason), "AMD's runtime does not offer FSR 4.0.2 on this device");
		} else if (WEXITSTATUS(status) == PE_EXIT_FATAL) {
			snprintf(reason, sizeof(reason), "AMD's runtime stopped: %s", run.fatal[0] ? run.fatal : run.last);
		} else if (WEXITSTATUS(status) != 0) {
			transient = true; // a Vulkan call failed: out of memory or a lost device can be passing conditions
			snprintf(reason, sizeof(reason), "failed (status %d): %s", WEXITSTATUS(status), run.last);
		} else if (!run.has_margin) {
			snprintf(reason, sizeof(reason), "finished without a score");
		} else if (!(run.margin_worst >= SELF_TEST_MARGIN)) {
			snprintf(reason, sizeof(reason), "generated frames are wrong: worst frame %.2f dB above repeat and blend, %.1f needed", run.margin_worst, SELF_TEST_MARGIN);
		} else {
			verdict = "pass";
			code    = 0;
		}
	}
	double seconds_taken = (now_ms() - started) / 1000;
	char temp[4200], final_path[4096];
	snprintf(final_path, sizeof(final_path), "%s/self-test.json", opt.out);
	snprintf(temp, sizeof(temp), "%s.tmp", final_path);
	FILE* f = fopen(temp, "w");
	if (!f) { perror(temp); return 2; }
	fprintf(f, "{\n");
	json_string(f, "verdict", verdict, ",");
	json_string(f, "reason", reason, ",");
	json_string(f, "key", key, ",");
	fprintf(f, " \"transient\": %s,\n", transient ? "true" : "false");
	if (run.has_margin) fprintf(f, " \"margin_db\": {\"mean\": %.2f, \"worst\": %.2f, \"needed\": %.1f},\n", run.margin_mean, run.margin_worst, SELF_TEST_MARGIN);
	fprintf(f, " \"seconds\": %.1f\n}\n", seconds_taken);
	if (fclose(f) || rename(temp, final_path)) { perror(final_path); return 2; }
	if (code == 0) printf("self-test: PASS in %.1f s, worst frame %.2f dB above repeat and blend (%.1f needed)\n", seconds_taken, run.margin_worst, SELF_TEST_MARGIN);
	else printf("self-test: FAIL in %.1f s: %s\n", seconds_taken, reason);
	return code;
}

static bool size(const char* s, uint32_t* w, uint32_t* h) { return sscanf(s, "%ux%u", w, h) == 2 && *w >= 64 && *h >= 64 && *w <= 7680 && *h <= 4320; }

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	opt = (Options){.dw = 2560, .dh = 1440, .rw = 1280, .rh = 720, .frames = 12, .hud = true, .full_subgroups = true, .provider = "4.0.2", .self_test_timeout = 120,
	               .hudless_image = true, .hudless_desc = true, .color_format = VK_FORMAT_R8G8B8A8_UNORM, .depth_format = VK_FORMAT_R32_SFLOAT};
	if (argc < 4) {
		fprintf(stderr, "usage: fsr4fg-gpu DLL PREPARED_DIR NEW_OUTPUT_DIR [--display WxH] [--render WxH] [--frames N] [--no-hud] "
		                "[--no-full-subgroups] [--provider 4.0.2|3.1.6] [--probe] [--organic] [--timing-frames N] [--guest-layout] [--no-pictures]\n"
		                "       [--game-inputs] [--game-device] [--format rgba8|bgra8|a2b10g10r10] [--depth r32|d32|d32s8] [--pq] [--no-hudless] [--no-hudless-desc] [--resize-at N:WxH]\n"
		                "       fsr4fg-gpu DLL PREPARED_DIR STATE_DIR --self-test [--force] [--self-test-timeout SECONDS]\n");
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
		else if (!strcmp(argv[i], "--guest-layout")) opt.guest_layout = true;
		else if (!strcmp(argv[i], "--game-inputs")) {
			// What the emulator's presenter hands over: BGRA8, a D32S8 depth copy, a general color image, UV motion vectors.
			opt.game_inputs = true;
			opt.color_format = VK_FORMAT_B8G8R8A8_UNORM;
			opt.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
		}
		else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
			const char* v = argv[++i];
			opt.color_format = !strcmp(v, "rgba8") ? VK_FORMAT_R8G8B8A8_UNORM : !strcmp(v, "bgra8") ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		}
		else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
			const char* v = argv[++i];
			opt.depth_format = !strcmp(v, "r32") ? VK_FORMAT_R32_SFLOAT : !strcmp(v, "d32") ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D32_SFLOAT_S8_UINT;
		}
		else if (!strcmp(argv[i], "--game-device")) opt.game_device = true;
		else if (!strcmp(argv[i], "--pq")) opt.pq = true;
		else if (!strcmp(argv[i], "--no-hudless")) opt.hudless_image = false;
		else if (!strcmp(argv[i], "--no-hudless-desc")) opt.hudless_desc = false;
		else if (!strcmp(argv[i], "--resize-at") && i + 1 < argc && sscanf(argv[i + 1], "%u:%ux%u", &opt.resize_at, &opt.resize_w, &opt.resize_h) == 3) i++;
		else if (!strcmp(argv[i], "--no-pictures")) opt.no_pictures = true;
		else if (!strcmp(argv[i], "--identity")) opt.identity = true;
		else if (!strcmp(argv[i], "--self-test")) opt.self_test = true;
		else if (!strcmp(argv[i], "--force")) opt.force = true;
		else if (!strcmp(argv[i], "--self-test-timeout") && i + 1 < argc && (opt.self_test_timeout = (uint32_t)atoi(argv[++i])) >= 1 && opt.self_test_timeout <= 3600) {}
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
	if (opt.self_test) return self_test(argv[0]);
	if (opt.guest_layout) reserve_guest_layout();
	printf("FSR frame generation %s GPU test: AMD's runtime on this Linux Vulkan driver, %ux%u (render %ux%u), %u frames\n",
	       opt.provider, opt.dw, opt.dh, opt.rw, opt.rh, opt.frames);
	return run();
}
