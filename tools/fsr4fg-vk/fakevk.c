// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic Vulkan device for recording AMD's frame generation provider on Linux (pehost). NO GPU work is done:
// handles and capabilities are synthetic and command buffers are only logged. A trace is not a rendered frame.
// Functions use the Windows x64 calling convention, as the provider calls them. Adapted from tools/fsr412-vk/capture_vk.c.
#define VK_NO_PROTOTYPES
#include "fakevk.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#define W __attribute__((ms_abi))
#define MAX_OBJECTS 262144

typedef struct Descriptor {
	uint32_t binding, element, type, used;
	VkDescriptorImageInfo  image;
	VkDescriptorBufferInfo buffer;
	uint64_t texel_view;
} Descriptor;

typedef struct Object {
	int      kind;
	uint64_t size, memory, offset;
	uint8_t* data;
	uint32_t width, height, depth, format, usage, mips, layers;
	uint64_t resource;
	uint64_t value; // semaphore payload
	char     name[160];
	Descriptor* set; // descriptor set: its written descriptors, in binding order of first write
	uint32_t    set_count, set_cap;
} Object;
enum { K_NONE, K_BUFFER, K_IMAGE, K_MEMORY, K_VIEW, K_BUFFER_VIEW, K_SHADER, K_PIPELINE, K_SET, K_SEMAPHORE, K_OTHER };

static Object*  objects;
static uint64_t serial = 1, bound_pipeline, bound_sets[8], allocated;
static uint32_t frame_index;
static FILE*    trace;
static FakeVkProfile profile;
static uint8_t  push_constants[256];
static uint32_t push_constant_size;

static void fail(const char* why) { fprintf(stderr, "fake Vulkan device: %s\n", why); fflush(stderr); exit(2); }
static uint64_t id_of(const void* h) { return (uint64_t)(uintptr_t)h; }
#define ID(h) id_of((const void*)(uintptr_t)(h))
static uint64_t checked(uint64_t h) { if (!h || h >= serial) fail("unknown synthetic handle"); return h; }
#define OBJ(h) objects[checked(ID(h))]
static uint64_t make(int kind) {
	if (!objects) objects = calloc(MAX_OBJECTS, sizeof(Object));
	if (serial == MAX_OBJECTS) fail("object limit");
	objects[serial].kind = kind;
	return serial++;
}
static FILE* out(void) {
	if (!trace) {
		trace = fopen("trace.jsonl", "wb");
		if (!trace) fail("trace open");
	}
	return trace;
}
#define LOG(...) fprintf(out(), __VA_ARGS__)
static uint64_t fnv(const void* data, size_t size) {
	uint64_t h = 14695981039346656037ull;
	const uint8_t* p = data;
	for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}
static void write_file(const char* name, const void* data, size_t size) {
	FILE* f = fopen(name, "rb");
	if (f) { fclose(f); return; } // content-addressed
	f = fopen(name, "wb");
	if (!f || fwrite(data, 1, size, f) != size || fclose(f)) fail("binary output");
}
static void dump_bytes(const void* data, size_t size, char* name, size_t cap) {
	snprintf(name, cap, "data_%016" PRIx64 ".bin", fnv(data, size));
	write_file(name, data, size);
}
static uint8_t* buffer_bytes(uint64_t buffer, uint64_t offset, uint64_t* size) {
	Object* b = &objects[checked(buffer)];
	if (!b->memory) fail("unbound buffer");
	Object* m = &objects[checked(b->memory)];
	if (offset > b->size) fail("buffer offset");
	if (*size == VK_WHOLE_SIZE) *size = b->size - offset;
	if (*size > b->size - offset || b->offset + offset + *size > m->size) fail("buffer range");
	return m->data + b->offset + offset;
}

void FakeVkSetProfile(const FakeVkProfile* p) {
	profile = *p;
	LOG("{\"op\":\"profile\",\"synthetic\":true,\"gpu_executed\":false,\"vendor\":%u,\"device\":%u,\"driver_id\":%u,\"fp8\":%s}\n",
	    p->vendor_id, p->device_id, p->driver_id, p->fp8 ? "true" : "false");
}
void FakeVkFrame(uint32_t index) { frame_index = index; LOG("{\"op\":\"frame\",\"index\":%u}\n", index); fflush(out()); }
void FakeVkNote(const char* json) { LOG("{\"op\":\"note\",%s}\n", json); fflush(out()); }
void FakeVkFlush(void) { if (trace) fflush(trace); }

// ------------------------------------------------------------------------------------------------ physical device
W static void vkGetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties* p) {
	(void)d;
	memset(p, 0, sizeof(*p));
	p->apiVersion = VK_API_VERSION_1_3;
	p->driverVersion = VK_MAKE_VERSION(26, 2, 5);
	p->vendorID = profile.vendor_id;
	p->deviceID = profile.device_id;
	p->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	snprintf(p->deviceName, sizeof(p->deviceName), "FSR offline synthetic recorder - no GPU");
	VkPhysicalDeviceLimits* l = &p->limits;
	l->maxImageDimension1D = l->maxImageDimension2D = 16384;
	l->maxImageDimension3D = 2048;
	l->maxImageArrayLayers = 2048;
	l->maxUniformBufferRange = 65536;
	l->maxStorageBufferRange = 1u << 30;
	l->maxPushConstantsSize = 256;
	l->maxMemoryAllocationCount = 4096;
	l->maxSamplerAllocationCount = 4000;
	l->maxBoundDescriptorSets = 32;
	l->maxPerStageDescriptorSamplers = l->maxPerStageDescriptorUniformBuffers = 1u << 20;
	l->maxPerStageDescriptorStorageBuffers = l->maxPerStageDescriptorSampledImages = l->maxPerStageDescriptorStorageImages = 1u << 20;
	l->maxPerStageResources = 1u << 20;
	l->maxDescriptorSetSamplers = l->maxDescriptorSetUniformBuffers = l->maxDescriptorSetStorageBuffers = 1u << 20;
	l->maxDescriptorSetSampledImages = l->maxDescriptorSetStorageImages = 1u << 20;
	l->maxComputeSharedMemorySize = 65536;
	l->maxComputeWorkGroupInvocations = 1024;
	for (int i = 0; i < 3; i++) { l->maxComputeWorkGroupCount[i] = 65535; l->maxComputeWorkGroupSize[i] = 1024; }
	l->maxSamplerAnisotropy = 16;
	l->timestampComputeAndGraphics = 1;
	l->timestampPeriod = 10;
	l->minUniformBufferOffsetAlignment = 64;
	l->minStorageBufferOffsetAlignment = 4;
	l->minTexelBufferOffsetAlignment = 4;
	l->nonCoherentAtomSize = 64;
	l->optimalBufferCopyOffsetAlignment = l->optimalBufferCopyRowPitchAlignment = 1;
	l->maxViewports = 16;
	l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
	l->maxFramebufferWidth = l->maxFramebufferHeight = 16384;
	l->maxFramebufferLayers = 2048;
	l->maxColorAttachments = 8;
}
W static void vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
	vkGetPhysicalDeviceProperties(d, &p->properties);
	for (VkBaseOutStructure* s = p->pNext; s; s = s->pNext) {
		LOG("{\"op\":\"property_query\",\"type\":%u}\n", s->sType);
		switch ((int)s->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
			VkPhysicalDeviceIDProperties* v = (void*)s;
			memset(v->deviceUUID, 1, VK_UUID_SIZE);
			memset(v->driverUUID, 2, VK_UUID_SIZE);
			memset(v->deviceLUID, 3, VK_LUID_SIZE); // SYNTHETIC LUID: the Windows provider refuses a device without one
			v->deviceLUIDValid = 1;
			v->deviceNodeMask = 1;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
			VkPhysicalDeviceDriverProperties* v = (void*)s;
			v->driverID = (VkDriverId)profile.driver_id;
			snprintf(v->driverName, sizeof(v->driverName), "synthetic offline recorder");
			v->conformanceVersion = (VkConformanceVersion){1, 4, 0, 0};
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
			VkPhysicalDeviceSubgroupProperties* v = (void*)s;
			v->subgroupSize = 64;
			v->supportedStages = VK_SHADER_STAGE_ALL;
			v->supportedOperations = 0x1ff;
			v->quadOperationsInAllStages = 1;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
			VkPhysicalDeviceSubgroupSizeControlProperties* v = (void*)s;
			v->minSubgroupSize = 32;
			v->maxSubgroupSize = 64;
			v->maxComputeWorkgroupSubgroups = 32;
			v->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
			VkPhysicalDeviceVulkan11Properties* v = (void*)s;
			v->subgroupSize = 64;
			v->subgroupSupportedStages = VK_SHADER_STAGE_ALL;
			v->subgroupSupportedOperations = 0x1ff;
			v->subgroupQuadOperationsInAllStages = 1;
			v->maxMemoryAllocationSize = 1ull << 32;
			v->maxPerSetDescriptors = 1u << 20;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
			VkPhysicalDeviceVulkan13Properties* v = (void*)s;
			v->minSubgroupSize = 32;
			v->maxSubgroupSize = 64;
			v->maxComputeWorkgroupSubgroups = 32;
			v->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR:
			((VkPhysicalDeviceCooperativeMatrixPropertiesKHR*)s)->cooperativeMatrixSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		default: LOG("{\"op\":\"unanswered_property\",\"type\":%u}\n", s->sType); break;
		}
	}
}
// Features: every VkBool32 of every queried structure is reported supported (FP8 ones only with the FP8 profile),
// and each queried structure type is logged, so the trace says what the provider looked at.
static bool fp8_feature(uint32_t type) {
	return type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT ||
	       type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR ||
	       type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV;
}
static void all_true(void* s, size_t size) {
	VkBool32* b = (VkBool32*)((uint8_t*)s + sizeof(VkBaseOutStructure));
	for (size_t i = 0; i < (size - sizeof(VkBaseOutStructure)) / 4; i++) b[i] = 1;
}
static size_t feature_size(uint32_t type) {
	switch ((int)type) {
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: return sizeof(VkPhysicalDeviceVulkan11Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: return sizeof(VkPhysicalDeviceVulkan12Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: return sizeof(VkPhysicalDeviceVulkan13Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: return sizeof(VkPhysicalDeviceShaderFloat16Int8Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: return sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: return sizeof(VkPhysicalDeviceSynchronization2Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT: return sizeof(VkPhysicalDeviceShaderFloat8FeaturesEXT);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR: return sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV: return sizeof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES: return sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: return sizeof(VkPhysicalDeviceVariablePointersFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: return sizeof(VkPhysicalDevice16BitStorageFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: return sizeof(VkPhysicalDevice8BitStorageFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: return sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR: return sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT: return sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES: return sizeof(VkPhysicalDeviceShaderAtomicInt64Features);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD: return sizeof(VkPhysicalDeviceCoherentMemoryFeaturesAMD);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR: return sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR);
	default: return 0;
	}
}
W static void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* p) {
	(void)d;
	VkBool32* core = (VkBool32*)&p->features;
	for (size_t i = 0; i < sizeof(p->features) / 4; i++) core[i] = 1;
	for (VkBaseOutStructure* s = p->pNext; s; s = s->pNext) {
		size_t size = feature_size(s->sType);
		LOG("{\"op\":\"feature_query\",\"type\":%u,\"answered\":%s}\n", s->sType, size ? "true" : "false");
		if (!size) continue;
		VkBaseOutStructure* next = s->pNext;
		if (fp8_feature(s->sType) && !profile.fp8) memset((uint8_t*)s + sizeof(VkBaseOutStructure), 0, size - sizeof(VkBaseOutStructure));
		else all_true(s, size);
		s->pNext = next;
	}
}
W static void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties* p) {
	(void)d;
	memset(p, 0, sizeof(*p));
	p->memoryTypeCount = 3;
	p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	p->memoryTypes[1].heapIndex = 1;
	p->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	p->memoryHeapCount = 2;
	p->memoryHeaps[0].size = 16ull << 30;
	p->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	p->memoryHeaps[1].size = 16ull << 30;
}
W static void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t* n, VkQueueFamilyProperties* p) {
	(void)d;
	if (!p) { *n = 2; return; }
	uint32_t c = *n < 2 ? *n : 2;
	VkQueueFamilyProperties fam[2] = {
	    {VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, 64, {1, 1, 1}},
	    {VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 4, 64, {1, 1, 1}},
	};
	memcpy(p, fam, c * sizeof(*p));
	*n = c;
}
W static VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice d, uint32_t q, VkSurfaceKHR s, VkBool32* out_supported) {
	(void)d; (void)q; (void)s;
	*out_supported = 1;
	return VK_SUCCESS;
}
static const char* const extensions[] = {
    "VK_KHR_synchronization2", "VK_KHR_dedicated_allocation", "VK_EXT_descriptor_indexing", "VK_EXT_subgroup_size_control",
    "VK_KHR_shader_float16_int8", "VK_KHR_timeline_semaphore", "VK_KHR_16bit_storage", "VK_KHR_8bit_storage",
    "VK_KHR_shader_integer_dot_product", "VK_KHR_compute_shader_derivatives", "VK_KHR_push_descriptor",
    "VK_KHR_swapchain", "VK_EXT_shader_image_atomic_int64", "VK_KHR_shader_atomic_int64",
    "VK_KHR_acceleration_structure", "VK_AMD_buffer_marker", "VK_AMD_device_coherent_memory",
    // FP8 profile only (the last three):
    "VK_EXT_shader_float8", "VK_KHR_cooperative_matrix", "VK_NV_cooperative_matrix2"};
W static VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char* layer, uint32_t* n, VkExtensionProperties* p) {
	(void)d; (void)layer;
	uint32_t count = (uint32_t)(sizeof(extensions) / sizeof(*extensions)) - (profile.fp8 ? 0 : 3);
	if (!p) { *n = count; return VK_SUCCESS; }
	uint32_t used = *n < count ? *n : count;
	for (uint32_t i = 0; i < used; i++) {
		memset(p + i, 0, sizeof(*p));
		snprintf(p[i].extensionName, sizeof(p[i].extensionName), "%s", extensions[i]);
		p[i].specVersion = 1;
	}
	*n = used;
	return used < count ? VK_INCOMPLETE : VK_SUCCESS;
}

// ------------------------------------------------------------------------------------------------ objects
#define CREATE(T, NAME, INFO, KIND, WHAT)                                                                              \
	W static VkResult NAME(VkDevice d, const INFO* p, const VkAllocationCallbacks* a, T* o) {                          \
		(void)d; (void)p; (void)a;                                                                                     \
		uint64_t h = make(KIND);                                                                                       \
		*o = (T)(uintptr_t)h;                                                                                          \
		if (WHAT) LOG("{\"op\":\"%s\",\"id\":%" PRIu64 "}\n", WHAT ? WHAT : "", h);                                    \
		return VK_SUCCESS;                                                                                             \
	}
#define DESTROY(T, NAME) W static void NAME(VkDevice d, T h, const VkAllocationCallbacks* a) { (void)d; (void)h; (void)a; }
CREATE(VkDescriptorPool, vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, K_OTHER, (const char*)NULL)
CREATE(VkCommandPool, vkCreateCommandPool, VkCommandPoolCreateInfo, K_OTHER, (const char*)NULL)
CREATE(VkRenderPass, vkCreateRenderPass, VkRenderPassCreateInfo, K_OTHER, "render_pass")
CREATE(VkFramebuffer, vkCreateFramebuffer, VkFramebufferCreateInfo, K_OTHER, (const char*)NULL)
DESTROY(VkSampler, vkDestroySampler)
DESTROY(VkDescriptorPool, vkDestroyDescriptorPool)
DESTROY(VkPipelineLayout, vkDestroyPipelineLayout)
DESTROY(VkDescriptorSetLayout, vkDestroyDescriptorSetLayout)
DESTROY(VkShaderModule, vkDestroyShaderModule)
DESTROY(VkPipeline, vkDestroyPipeline)
DESTROY(VkBuffer, vkDestroyBuffer)
DESTROY(VkImage, vkDestroyImage)
DESTROY(VkImageView, vkDestroyImageView)
DESTROY(VkBufferView, vkDestroyBufferView)
DESTROY(VkCommandPool, vkDestroyCommandPool)
DESTROY(VkRenderPass, vkDestroyRenderPass)
DESTROY(VkFramebuffer, vkDestroyFramebuffer)
DESTROY(VkSemaphore, vkDestroySemaphore)

W static VkResult vkCreateSampler(VkDevice d, const VkSamplerCreateInfo* p, const VkAllocationCallbacks* a, VkSampler* o) {
	(void)d; (void)a;
	uint64_t h = make(K_OTHER);
	*o = (VkSampler)(uintptr_t)h;
	LOG("{\"op\":\"sampler\",\"id\":%" PRIu64 ",\"mag\":%u,\"min\":%u,\"mipmap\":%u,\"address\":[%u,%u,%u],\"lod\":[%g,%g,%g],\"compare\":%u,\"border\":%u,\"unnormalized\":%u}\n",
	    h, p->magFilter, p->minFilter, p->mipmapMode, p->addressModeU, p->addressModeV, p->addressModeW, p->mipLodBias, p->minLod, p->maxLod,
	    p->compareEnable ? p->compareOp + 1 : 0, p->borderColor, p->unnormalizedCoordinates);
	return VK_SUCCESS;
}
W static VkResult vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo* p, const VkAllocationCallbacks* a, VkSemaphore* o) {
	(void)d; (void)a;
	uint64_t h = make(K_SEMAPHORE);
	for (const VkBaseInStructure* s = p->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) objects[h].value = ((const VkSemaphoreTypeCreateInfo*)s)->initialValue;
	*o = (VkSemaphore)(uintptr_t)h;
	return VK_SUCCESS;
}
W static VkResult vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* p, const VkAllocationCallbacks* a, VkDescriptorSetLayout* o) {
	(void)d; (void)a;
	uint64_t h = make(K_OTHER);
	*o = (VkDescriptorSetLayout)(uintptr_t)h;
	LOG("{\"op\":\"set_layout\",\"id\":%" PRIu64 ",\"flags\":%u,\"bindings\":[", h, p->flags);
	for (uint32_t i = 0; i < p->bindingCount; i++)
	{
		LOG("%s{\"binding\":%u,\"type\":%u,\"count\":%u,\"stages\":%u,\"immutable_samplers\":[", i ? "," : "", p->pBindings[i].binding,
		    p->pBindings[i].descriptorType, p->pBindings[i].descriptorCount, p->pBindings[i].stageFlags);
		for (uint32_t k = 0; p->pBindings[i].pImmutableSamplers && k < p->pBindings[i].descriptorCount; k++)
			LOG("%s%" PRIu64, k ? "," : "", ID(p->pBindings[i].pImmutableSamplers[k]));
		LOG("]}");
	}
	LOG("]}\n");
	return VK_SUCCESS;
}
W static VkResult vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo* p, const VkAllocationCallbacks* a, VkPipelineLayout* o) {
	(void)d; (void)a;
	uint64_t h = make(K_OTHER);
	*o = (VkPipelineLayout)(uintptr_t)h;
	LOG("{\"op\":\"pipeline_layout\",\"id\":%" PRIu64 ",\"set_layouts\":[", h);
	for (uint32_t i = 0; i < p->setLayoutCount; i++) LOG("%s%" PRIu64, i ? "," : "", ID(p->pSetLayouts[i]));
	LOG("],\"push_constants\":[");
	for (uint32_t i = 0; i < p->pushConstantRangeCount; i++)
		LOG("%s[%u,%u,%u]", i ? "," : "", p->pPushConstantRanges[i].stageFlags, p->pPushConstantRanges[i].offset, p->pPushConstantRanges[i].size);
	LOG("]}\n");
	return VK_SUCCESS;
}
W static VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* p, const VkAllocationCallbacks* a, VkBuffer* o) {
	(void)d; (void)a;
	uint64_t h = make(K_BUFFER);
	*o = (VkBuffer)(uintptr_t)h;
	objects[h].size = p->size;
	objects[h].usage = p->usage;
	LOG("{\"op\":\"buffer\",\"id\":%" PRIu64 ",\"size\":%" PRIu64 ",\"usage\":%u}\n", h, (uint64_t)p->size, p->usage);
	return VK_SUCCESS;
}
W static VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo* p, const VkAllocationCallbacks* a, VkImage* o) {
	(void)d; (void)a;
	uint64_t h = make(K_IMAGE);
	*o = (VkImage)(uintptr_t)h;
	Object* x = objects + h;
	x->width = p->extent.width; x->height = p->extent.height; x->depth = p->extent.depth;
	x->format = p->format; x->mips = p->mipLevels; x->layers = p->arrayLayers; x->usage = p->usage;
	x->size = (uint64_t)x->width * x->height * x->depth * x->layers * 16;
	LOG("{\"op\":\"image\",\"id\":%" PRIu64 ",\"type\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"mips\":%u,\"layers\":%u,\"usage\":%u,\"flags\":%u}\n",
	    h, p->imageType, x->format, x->width, x->height, x->depth, x->mips, x->layers, x->usage, p->flags);
	return VK_SUCCESS;
}
W static void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements* p) {
	(void)d;
	p->size = (OBJ(b).size + 255) & ~255ull;
	p->alignment = 256;
	p->memoryTypeBits = 7;
}
W static void vkGetImageMemoryRequirements(VkDevice d, VkImage b, VkMemoryRequirements* p) {
	(void)d;
	p->size = (OBJ(b).size + 65535) & ~65535ull;
	p->alignment = 65536;
	p->memoryTypeBits = 5;
}
W static void vkGetBufferMemoryRequirements2(VkDevice d, const VkBufferMemoryRequirementsInfo2* b, VkMemoryRequirements2* p) {
	vkGetBufferMemoryRequirements(d, b->buffer, &p->memoryRequirements);
	for (VkBaseOutStructure* s = p->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
			((VkMemoryDedicatedRequirements*)s)->prefersDedicatedAllocation = 0;
			((VkMemoryDedicatedRequirements*)s)->requiresDedicatedAllocation = 0;
		}
}
W static VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* p, const VkAllocationCallbacks* a, VkDeviceMemory* o) {
	(void)d; (void)a;
	if (p->allocationSize > (4ull << 30) || allocated + p->allocationSize > (6ull << 30)) fail("allocation limit");
	allocated += p->allocationSize;
	uint64_t h = make(K_MEMORY);
	objects[h].size = p->allocationSize;
	objects[h].data = calloc(1, (size_t)p->allocationSize);
	if (!objects[h].data) fail("allocation failed");
	*o = (VkDeviceMemory)(uintptr_t)h;
	LOG("{\"op\":\"memory\",\"id\":%" PRIu64 ",\"size\":%" PRIu64 ",\"type\":%u}\n", h, (uint64_t)p->allocationSize, p->memoryTypeIndex);
	return VK_SUCCESS;
}
W static void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a) {
	(void)d; (void)a;
	if (!m) return;
	Object* x = &OBJ(m);
	free(x->data);
	x->data = NULL;
	allocated -= x->size;
}
W static VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
	(void)d;
	Object *x = &OBJ(b), *mem = &OBJ(m);
	if (offset > mem->size || (x->kind == K_BUFFER && x->size > mem->size - offset)) fail("bind range");
	x->memory = ID(m);
	x->offset = offset;
	LOG("{\"op\":\"bind\",\"resource\":%" PRIu64 ",\"memory\":%" PRIu64 ",\"offset\":%" PRIu64 "}\n", ID(b), ID(m), (uint64_t)offset);
	return VK_SUCCESS;
}
W static VkResult vkBindImageMemory(VkDevice d, VkImage b, VkDeviceMemory m, VkDeviceSize offset) {
	(void)d;
	OBJ(b).memory = ID(m);
	OBJ(b).offset = offset;
	LOG("{\"op\":\"bind\",\"resource\":%" PRIu64 ",\"memory\":%" PRIu64 ",\"offset\":%" PRIu64 "}\n", ID(b), ID(m), (uint64_t)offset);
	return VK_SUCCESS;
}
W static VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags f, void** o) {
	(void)d; (void)f;
	Object* x = &OBJ(m);
	if (offset > x->size || (size != VK_WHOLE_SIZE && size > x->size - offset)) fail("map range");
	*o = x->data + offset;
	return VK_SUCCESS;
}
W static void vkUnmapMemory(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
W static VkResult vkFlushMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange* r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }
W static VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo* p, const VkAllocationCallbacks* a, VkImageView* o) {
	(void)d; (void)a;
	uint64_t h = make(K_VIEW);
	*o = (VkImageView)(uintptr_t)h;
	objects[h].resource = ID(p->image);
	LOG("{\"op\":\"image_view\",\"id\":%" PRIu64 ",\"image\":%" PRIu64 ",\"view_type\":%u,\"format\":%u,\"base_mip\":%u,\"mips\":%u,\"base_layer\":%u,\"layers\":%u}\n",
	    h, ID(p->image), p->viewType, p->format, p->subresourceRange.baseMipLevel, p->subresourceRange.levelCount,
	    p->subresourceRange.baseArrayLayer, p->subresourceRange.layerCount);
	return VK_SUCCESS;
}
W static VkResult vkCreateBufferView(VkDevice d, const VkBufferViewCreateInfo* p, const VkAllocationCallbacks* a, VkBufferView* o) {
	(void)d; (void)a;
	uint64_t h = make(K_BUFFER_VIEW);
	*o = (VkBufferView)(uintptr_t)h;
	objects[h].resource = ID(p->buffer); objects[h].offset = p->offset; objects[h].size = p->range; objects[h].format = p->format;
	LOG("{\"op\":\"buffer_view\",\"id\":%" PRIu64 ",\"buffer\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"range\":%" PRIu64 ",\"format\":%u}\n",
	    h, ID(p->buffer), (uint64_t)p->offset, (uint64_t)p->range, p->format);
	return VK_SUCCESS;
}
W static VkResult vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* p, const VkAllocationCallbacks* a, VkShaderModule* o) {
	(void)d; (void)a;
	uint64_t h = make(K_SHADER);
	*o = (VkShaderModule)(uintptr_t)h;
	snprintf(objects[h].name, sizeof(objects[h].name), "shader_%016" PRIx64 ".spv", fnv(p->pCode, p->codeSize));
	write_file(objects[h].name, p->pCode, p->codeSize);
	return VK_SUCCESS;
}
static void log_stage(const VkPipelineShaderStageCreateInfo* s) {
	uint32_t required = 0;
	for (const VkBaseInStructure* x = s->pNext; x; x = x->pNext)
		if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO)
			required = ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*)x)->requiredSubgroupSize;
	LOG("{\"stage\":%u,\"entry\":\"%s\",\"shader\":\"%s\",\"flags\":%u,\"required_subgroup_size\":%u,\"specialization\":[", s->stage,
	    s->pName, OBJ(s->module).name, s->flags, required);
	if (s->pSpecializationInfo)
		for (uint32_t i = 0; i < s->pSpecializationInfo->mapEntryCount; i++) {
			const VkSpecializationMapEntry* e = s->pSpecializationInfo->pMapEntries + i;
			uint64_t v = 0;
			memcpy(&v, (const uint8_t*)s->pSpecializationInfo->pData + e->offset, e->size < 8 ? e->size : 8);
			LOG("%s[%u,%" PRIu64 "]", i ? "," : "", e->constantID, v);
		}
	LOG("]}");
}
W static VkResult vkCreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* p, const VkAllocationCallbacks* a, VkPipeline* o) {
	(void)d; (void)c; (void)a;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t h = make(K_PIPELINE);
		o[i] = (VkPipeline)(uintptr_t)h;
		snprintf(objects[h].name, sizeof(objects[h].name), "%s", p[i].stage.pName);
		LOG("{\"op\":\"pipeline\",\"id\":%" PRIu64 ",\"bind_point\":1,\"layout\":%" PRIu64 ",\"flags\":%u,\"stages\":[", h, ID(p[i].layout), p[i].flags);
		log_stage(&p[i].stage);
		LOG("]}\n");
	}
	return VK_SUCCESS;
}
W static VkResult vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* p, const VkAllocationCallbacks* a, VkPipeline* o) {
	(void)d; (void)c; (void)a;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t h = make(K_PIPELINE);
		o[i] = (VkPipeline)(uintptr_t)h;
		LOG("{\"op\":\"pipeline\",\"id\":%" PRIu64 ",\"bind_point\":0,\"layout\":%" PRIu64 ",\"stages\":[", h, ID(p[i].layout));
		for (uint32_t k = 0; k < p[i].stageCount; k++) { if (k) LOG(","); log_stage(p[i].pStages + k); }
		LOG("]}\n");
	}
	return VK_SUCCESS;
}
W static VkResult vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* p, VkDescriptorSet* o) {
	(void)d;
	for (uint32_t i = 0; i < p->descriptorSetCount; i++) {
		uint64_t h = make(K_SET);
		o[i] = (VkDescriptorSet)(uintptr_t)h;
		objects[h].set_cap = 16;
		objects[h].set = calloc(objects[h].set_cap, sizeof(Descriptor));
		objects[h].resource = ID(p->pSetLayouts[i]);
	}
	return VK_SUCCESS;
}
W static VkResult vkFreeDescriptorSets(VkDevice d, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* s) {
	(void)d; (void)pool;
	for (uint32_t i = 0; i < n; i++) if (s[i]) { free(OBJ(s[i]).set); OBJ(s[i]).set = NULL; }
	return VK_SUCCESS;
}
W static void vkUpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet* p, uint32_t copies, const VkCopyDescriptorSet* c) {
	(void)d; (void)c;
	if (copies) fail("descriptor copies unsupported");
	for (uint32_t i = 0; i < n; i++)
		for (uint32_t e = 0; e < p[i].descriptorCount; e++) {
			uint32_t binding = p[i].dstBinding, element = p[i].dstArrayElement + e;
			Object* set = &OBJ(p[i].dstSet);
			if (!set->set) fail("write to a freed descriptor set");
			Descriptor* x = NULL;
			for (uint32_t k = 0; k < set->set_count && !x; k++)
				if (set->set[k].binding == binding && set->set[k].element == element) x = set->set + k;
			if (!x) {
				if (set->set_count == set->set_cap) {
					set->set_cap *= 2;
					set->set = realloc(set->set, set->set_cap * sizeof(Descriptor));
				}
				x = set->set + set->set_count++;
				memset(x, 0, sizeof(*x));
				x->binding = binding;
				x->element = element;
			}
			x->type = p[i].descriptorType;
			x->used = 1;
			switch (p[i].descriptorType) {
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				x->buffer = p[i].pBufferInfo[e]; break;
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				x->image = p[i].pImageInfo[e]; break;
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
				x->texel_view = ID(p[i].pTexelBufferView[e]); break;
			default: fail("unsupported descriptor type");
			}
		}
}

// ------------------------------------------------------------------------------------------------ commands
W static VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* p, VkCommandBuffer* o) {
	(void)d;
	for (uint32_t i = 0; i < p->commandBufferCount; i++) o[i] = (VkCommandBuffer)(uintptr_t)make(K_OTHER);
	return VK_SUCCESS;
}
W static void vkFreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer* c) { (void)d; (void)pool; (void)n; (void)c; }
W static VkResult vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* i) { (void)i; LOG("{\"op\":\"begin\",\"cmd\":%" PRIu64 "}\n", ID(c)); return VK_SUCCESS; }
W static VkResult vkEndCommandBuffer(VkCommandBuffer c) { LOG("{\"op\":\"end\",\"cmd\":%" PRIu64 "}\n", ID(c)); return VK_SUCCESS; }
W static VkResult vkResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) { (void)c; (void)f; return VK_SUCCESS; }
W static VkResult vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; (void)p; (void)f; return VK_SUCCESS; }
W static void vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p) { (void)c; (void)b; bound_pipeline = ID(p); }
W static void vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint b, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* s, uint32_t dyn, const uint32_t* offs) {
	(void)c; (void)b; (void)l; (void)offs;
	if (first + n > 8 || dyn) fail("descriptor set binding unsupported");
	for (uint32_t i = 0; i < n; i++) bound_sets[first + i] = ID(s[i]);
}
W static void vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags s, uint32_t offset, uint32_t size, const void* v) {
	(void)c; (void)l; (void)s;
	if (offset + size > sizeof(push_constants)) fail("push constant range");
	memcpy(push_constants + offset, v, size);
	if (offset + size > push_constant_size) push_constant_size = offset + size;
}
static void log_bindings(void) {
	int comma = 0;
	for (uint32_t set = 0; set < 8; set++) {
		if (!bound_sets[set]) continue;
		Object* table = &objects[checked(bound_sets[set])];
		if (!table->set) fail("dispatch with a freed descriptor set");
		for (uint32_t k = 0; k < table->set_count; k++) {
				Descriptor* x = table->set + k;
				LOG("%s{\"set\":%u,\"binding\":%u,\"element\":%u,\"type\":%u", comma++ ? "," : "", set, x->binding, x->element, x->type);
				if (x->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
					LOG(",\"buffer\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"range\":%" PRIu64, ID(x->buffer.buffer), (uint64_t)x->buffer.offset,
					    (uint64_t)x->buffer.range);
					uint64_t size = x->buffer.range;
					uint8_t* data = buffer_bytes(ID(x->buffer.buffer), x->buffer.offset, &size);
					if (x->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && size <= 65536) {
						char name[64];
						dump_bytes(data, (size_t)size, name, sizeof(name));
						LOG(",\"data\":\"%s\"", name);
					}
				} else if (x->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || x->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
					LOG(",\"view\":%" PRIu64, x->texel_view);
				} else {
					uint64_t image = x->image.imageView ? objects[checked(ID(x->image.imageView))].resource : 0;
					LOG(",\"view\":%" PRIu64 ",\"image\":%" PRIu64 ",\"sampler\":%" PRIu64 ",\"layout\":%u", ID(x->image.imageView), image,
					    ID(x->image.sampler), x->image.imageLayout);
				}
				LOG("}");
		}
	}
}
W static void vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
	(void)c;
	checked(bound_pipeline);
	LOG("{\"op\":\"dispatch\",\"frame\":%u,\"pipeline\":%" PRIu64 ",\"entry\":\"%s\",\"groups\":[%u,%u,%u],\"bindings\":[", frame_index,
	    bound_pipeline, objects[bound_pipeline].name, x, y, z);
	log_bindings();
	LOG("]");
	if (push_constant_size) {
		char name[64];
		dump_bytes(push_constants, push_constant_size, name, sizeof(name));
		LOG(",\"push_constants\":\"%s\"", name);
	}
	LOG("}\n");
}
W static void vkCmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o) {
	(void)c;
	LOG("{\"op\":\"dispatch_indirect\",\"frame\":%u,\"pipeline\":%" PRIu64 ",\"entry\":\"%s\",\"buffer\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"bindings\":[",
	    frame_index, bound_pipeline, bound_pipeline ? objects[bound_pipeline].name : "", ID(b), (uint64_t)o);
	log_bindings();
	LOG("]}\n");
}
W static void vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* p) {
	(void)c;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t size = p[i].size, dsize = p[i].size;
		uint8_t* s = buffer_bytes(ID(src), p[i].srcOffset, &size);
		uint8_t* d = buffer_bytes(ID(dst), p[i].dstOffset, &dsize);
		memcpy(d, s, (size_t)size);
		char name[64];
		dump_bytes(s, (size_t)size, name, sizeof(name));
		LOG("{\"op\":\"copy_buffer\",\"frame\":%u,\"src\":%" PRIu64 ",\"dst\":%" PRIu64 ",\"src_offset\":%" PRIu64 ",\"dst_offset\":%" PRIu64 ",\"size\":%" PRIu64 ",\"data\":\"%s\"}\n",
		    frame_index, ID(src), ID(dst), (uint64_t)p[i].srcOffset, (uint64_t)p[i].dstOffset, size, name);
	}
}
W static void vkCmdFillBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, uint32_t value) {
	(void)c;
	uint64_t s = size;
	uint8_t* d = buffer_bytes(ID(buffer), offset, &s);
	for (uint64_t i = 0; i + 4 <= s; i += 4) memcpy(d + i, &value, 4);
	LOG("{\"op\":\"fill\",\"frame\":%u,\"buffer\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"size\":%" PRIu64 ",\"value\":%u}\n", frame_index, ID(buffer),
	    (uint64_t)offset, s, value);
}
W static void vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* v) {
	(void)c;
	uint64_t s = size;
	uint8_t* d = buffer_bytes(ID(buffer), offset, &s);
	memcpy(d, v, (size_t)s);
	char name[64];
	dump_bytes(v, (size_t)s, name, sizeof(name));
	LOG("{\"op\":\"update_buffer\",\"frame\":%u,\"buffer\":%" PRIu64 ",\"offset\":%" PRIu64 ",\"size\":%" PRIu64 ",\"data\":\"%s\"}\n", frame_index,
	    ID(buffer), (uint64_t)offset, s, name);
}
W static void vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm,
                                   const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* im) {
	(void)c; (void)dep;
	LOG("{\"op\":\"barrier\",\"frame\":%u,\"src_stage\":%u,\"dst_stage\":%u,\"memory\":[", frame_index, src, dst);
	for (uint32_t i = 0; i < nm; i++) LOG("%s[%u,%u]", i ? "," : "", m[i].srcAccessMask, m[i].dstAccessMask);
	LOG("],\"buffers\":[");
	for (uint32_t i = 0; i < nb; i++) LOG("%s{\"id\":%" PRIu64 ",\"src\":%u,\"dst\":%u}", i ? "," : "", ID(b[i].buffer), b[i].srcAccessMask, b[i].dstAccessMask);
	LOG("],\"images\":[");
	for (uint32_t i = 0; i < ni; i++)
		LOG("%s{\"id\":%" PRIu64 ",\"old\":%u,\"new\":%u,\"src\":%u,\"dst\":%u,\"src_family\":%u,\"dst_family\":%u}", i ? "," : "", ID(im[i].image),
		    im[i].oldLayout, im[i].newLayout, im[i].srcAccessMask, im[i].dstAccessMask, im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex);
	LOG("]}\n");
}
W static void vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) {
	(void)c;
	LOG("{\"op\":\"barrier2\",\"frame\":%u,\"memory\":[", frame_index);
	for (uint32_t i = 0; i < d->memoryBarrierCount; i++) {
		const VkMemoryBarrier2* m = d->pMemoryBarriers + i;
		LOG("%s[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", i ? "," : "", m->srcStageMask, m->srcAccessMask, m->dstStageMask, m->dstAccessMask);
	}
	LOG("],\"buffers\":[");
	for (uint32_t i = 0; i < d->bufferMemoryBarrierCount; i++) {
		const VkBufferMemoryBarrier2* b = d->pBufferMemoryBarriers + i;
		LOG("%s{\"id\":%" PRIu64 ",\"src_stage\":%" PRIu64 ",\"src\":%" PRIu64 ",\"dst_stage\":%" PRIu64 ",\"dst\":%" PRIu64 "}", i ? "," : "", ID(b->buffer),
		    b->srcStageMask, b->srcAccessMask, b->dstStageMask, b->dstAccessMask);
	}
	LOG("],\"images\":[");
	for (uint32_t i = 0; i < d->imageMemoryBarrierCount; i++) {
		const VkImageMemoryBarrier2* b = d->pImageMemoryBarriers + i;
		LOG("%s{\"id\":%" PRIu64 ",\"old\":%u,\"new\":%u,\"src_stage\":%" PRIu64 ",\"src\":%" PRIu64 ",\"dst_stage\":%" PRIu64 ",\"dst\":%" PRIu64 "}",
		    i ? "," : "", ID(b->image), b->oldLayout, b->newLayout, b->srcStageMask, b->srcAccessMask, b->dstStageMask, b->dstAccessMask);
	}
	LOG("]}\n");
}
W static void vkCmdClearColorImage(VkCommandBuffer c, VkImage image, VkImageLayout layout, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r) {
	(void)c; (void)n; (void)r;
	LOG("{\"op\":\"clear_image\",\"frame\":%u,\"image\":%" PRIu64 ",\"layout\":%u,\"value\":[%u,%u,%u,%u]}\n", frame_index, ID(image), layout,
	    v->uint32[0], v->uint32[1], v->uint32[2], v->uint32[3]);
}
W static void vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t n, const VkBufferImageCopy* p) {
	(void)c;
	LOG("{\"op\":\"copy_buffer_image\",\"frame\":%u,\"buffer\":%" PRIu64 ",\"image\":%" PRIu64 ",\"layout\":%u,\"regions\":%u", frame_index, ID(src),
	    ID(dst), layout, n);
	if (n == 1) LOG(",\"buffer_offset\":%" PRIu64 ",\"extent\":[%u,%u,%u]", (uint64_t)p->bufferOffset, p->imageExtent.width, p->imageExtent.height,
	                p->imageExtent.depth);
	LOG("}\n");
}
W static void vkCmdCopyImage(VkCommandBuffer c, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy* p) {
	(void)c; (void)p;
	LOG("{\"op\":\"copy_image\",\"frame\":%u,\"src\":%" PRIu64 ",\"dst\":%" PRIu64 ",\"src_layout\":%u,\"dst_layout\":%u,\"regions\":%u}\n", frame_index,
	    ID(src), ID(dst), sl, dl, n);
}
W static void vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* b, VkSubpassContents s) {
	(void)c; (void)s;
	LOG("{\"op\":\"begin_render_pass\",\"frame\":%u,\"render_pass\":%" PRIu64 "}\n", frame_index, ID(b->renderPass));
}
W static void vkCmdEndRenderPass(VkCommandBuffer c) { (void)c; LOG("{\"op\":\"end_render_pass\",\"frame\":%u}\n", frame_index); }
W static void vkCmdSetViewport(VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport* v) { (void)c; (void)f; (void)n; (void)v; }
W static void vkCmdSetScissor(VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D* r) { (void)c; (void)f; (void)n; (void)r; }
W static void vkCmdDraw(VkCommandBuffer c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
	(void)c; (void)fv; (void)fi;
	LOG("{\"op\":\"draw\",\"frame\":%u,\"pipeline\":%" PRIu64 ",\"vertices\":%u,\"instances\":%u,\"bindings\":[", frame_index, bound_pipeline, vc, ic);
	log_bindings();
	LOG("]}\n");
}
W static void vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer c, const VkDebugUtilsLabelEXT* l) {
	(void)c;
	LOG("{\"op\":\"label\",\"frame\":%u,\"name\":\"%s\"}\n", frame_index, l && l->pLabelName ? l->pLabelName : "");
}
W static void vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer c) { (void)c; LOG("{\"op\":\"label_end\",\"frame\":%u}\n", frame_index); }
W static VkResult vkSetDebugUtilsObjectNameEXT(VkDevice d, const VkDebugUtilsObjectNameInfoEXT* n) {
	(void)d;
	if (n->objectHandle && n->objectHandle < serial && n->pObjectName) {
		LOG("{\"op\":\"name\",\"id\":%" PRIu64 ",\"type\":%u,\"name\":\"%s\"}\n", n->objectHandle, n->objectType, n->pObjectName);
		if (objects[n->objectHandle].kind != K_SHADER && objects[n->objectHandle].kind != K_PIPELINE)
			snprintf(objects[n->objectHandle].name, sizeof(objects[n->objectHandle].name), "%s", n->pObjectName);
	}
	return VK_SUCCESS;
}

// ------------------------------------------------------------------------------------------------ queues and sync
W static void vkGetDeviceQueue(VkDevice d, uint32_t f, uint32_t i, VkQueue* q) { (void)d; *q = (VkQueue)(uintptr_t)(0x9000 + f * 16 + i); }
W static VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
	(void)f;
	for (uint32_t i = 0; i < n; i++) {
		LOG("{\"op\":\"submit\",\"frame\":%u,\"queue\":%" PRIu64 ",\"command_buffers\":%u,\"waits\":%u,\"signals\":%u}\n", frame_index, ID(q),
		    s[i].commandBufferCount, s[i].waitSemaphoreCount, s[i].signalSemaphoreCount);
		const VkTimelineSemaphoreSubmitInfo* t = NULL;
		for (const VkBaseInStructure* x = s[i].pNext; x; x = x->pNext)
			if (x->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) t = (const void*)x;
		// The synthetic GPU completes everything at once: timeline semaphores take their signal values.
		for (uint32_t k = 0; k < s[i].signalSemaphoreCount; k++)
			if (t && k < t->signalSemaphoreValueCount) OBJ(s[i].pSignalSemaphores[k]).value = t->pSignalSemaphoreValues[k];
	}
	return VK_SUCCESS;
}
W static VkResult vkQueueWaitIdle(VkQueue q) { (void)q; return VK_SUCCESS; }
W static VkResult vkDeviceWaitIdle(VkDevice d) { (void)d; return VK_SUCCESS; }
W static VkResult vkGetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t* v) { (void)d; *v = OBJ(s).value; return VK_SUCCESS; }
W static VkResult vkWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* w, uint64_t t) {
	(void)d; (void)t;
	for (uint32_t i = 0; i < w->semaphoreCount; i++)
		if (OBJ(w->pSemaphores[i]).value < w->pValues[i]) {
			LOG("{\"op\":\"wait_unsignaled\",\"semaphore\":%" PRIu64 ",\"value\":%" PRIu64 ",\"have\":%" PRIu64 "}\n", ID(w->pSemaphores[i]), w->pValues[i],
			    OBJ(w->pSemaphores[i]).value);
			OBJ(w->pSemaphores[i]).value = w->pValues[i];
		}
	return VK_SUCCESS;
}
W static VkResult vkCreateSwapchainKHR(VkDevice d, const void* p, const VkAllocationCallbacks* a, VkSwapchainKHR* o) {
	(void)d; (void)p; (void)a; (void)o;
	fail("the provider tried to create a swapchain (the recorder runs frame generation without one)");
	return VK_ERROR_INITIALIZATION_FAILED;
}
W static void vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a) { (void)d; (void)s; (void)a; }
W static VkResult vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t* n, VkImage* i) { (void)d; (void)s; (void)n; (void)i; fail("swapchain images"); return 0; }
W static VkResult vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t* i) {
	(void)d; (void)s; (void)t; (void)se; (void)f; (void)i;
	fail("swapchain acquire");
	return 0;
}
W static VkResult vkQueuePresentKHR(VkQueue q, const void* p) { (void)q; (void)p; fail("swapchain present"); return 0; }

// ------------------------------------------------------------------------------------------------ lookup
typedef struct Entry { const char* name; void* fn; } Entry;
#define F(n) {#n, (void*)n}
static const Entry table[] = {
    F(vkGetPhysicalDeviceProperties), F(vkGetPhysicalDeviceProperties2), F(vkGetPhysicalDeviceFeatures2),
    F(vkGetPhysicalDeviceMemoryProperties), F(vkGetPhysicalDeviceQueueFamilyProperties), F(vkGetPhysicalDeviceSurfaceSupportKHR),
    F(vkEnumerateDeviceExtensionProperties), F(vkCreateSampler), F(vkCreateDescriptorPool), F(vkCreateCommandPool),
    F(vkCreateRenderPass), F(vkCreateFramebuffer), F(vkDestroySampler), F(vkDestroyDescriptorPool), F(vkDestroyPipelineLayout),
    F(vkDestroyDescriptorSetLayout), F(vkDestroyShaderModule), F(vkDestroyPipeline), F(vkDestroyBuffer), F(vkDestroyImage),
    F(vkDestroyImageView), F(vkDestroyBufferView), F(vkDestroyCommandPool), F(vkDestroyRenderPass), F(vkDestroyFramebuffer),
    F(vkDestroySemaphore), F(vkCreateSemaphore), F(vkCreateDescriptorSetLayout), F(vkCreatePipelineLayout), F(vkCreateBuffer),
    F(vkCreateImage), F(vkGetBufferMemoryRequirements), F(vkGetImageMemoryRequirements), F(vkGetBufferMemoryRequirements2),
    F(vkAllocateMemory), F(vkFreeMemory), F(vkBindBufferMemory), F(vkBindImageMemory), F(vkMapMemory), F(vkUnmapMemory),
    F(vkFlushMappedMemoryRanges), F(vkCreateImageView), F(vkCreateBufferView), F(vkCreateShaderModule),
    F(vkCreateComputePipelines), F(vkCreateGraphicsPipelines), F(vkAllocateDescriptorSets), F(vkFreeDescriptorSets),
    F(vkUpdateDescriptorSets), F(vkAllocateCommandBuffers), F(vkFreeCommandBuffers), F(vkBeginCommandBuffer),
    F(vkEndCommandBuffer), F(vkResetCommandBuffer), F(vkResetCommandPool), F(vkCmdBindPipeline), F(vkCmdBindDescriptorSets),
    F(vkCmdPushConstants), F(vkCmdDispatch), F(vkCmdDispatchIndirect), F(vkCmdCopyBuffer), F(vkCmdFillBuffer),
    F(vkCmdUpdateBuffer), F(vkCmdPipelineBarrier), F(vkCmdPipelineBarrier2), F(vkCmdClearColorImage),
    F(vkCmdCopyBufferToImage), F(vkCmdCopyImage), F(vkCmdBeginRenderPass), F(vkCmdEndRenderPass), F(vkCmdSetViewport),
    F(vkCmdSetScissor), F(vkCmdDraw), F(vkCmdBeginDebugUtilsLabelEXT), F(vkCmdEndDebugUtilsLabelEXT),
    F(vkSetDebugUtilsObjectNameEXT), F(vkGetDeviceQueue), F(vkQueueSubmit), F(vkQueueWaitIdle), F(vkDeviceWaitIdle),
    F(vkGetSemaphoreCounterValue), F(vkWaitSemaphores), F(vkCreateSwapchainKHR), F(vkDestroySwapchainKHR),
    F(vkGetSwapchainImagesKHR), F(vkAcquireNextImageKHR), F(vkQueuePresentKHR),
    {"vkGetDeviceProcAddr", (void*)FakeVkGetDeviceProcAddr},
};
static const char* const aliases[][2] = {
    {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
    {"vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2"},
    {"vkGetSemaphoreCounterValueKHR", "vkGetSemaphoreCounterValue"},
    {"vkWaitSemaphoresKHR", "vkWaitSemaphores"},
};

void* FakeVkLookup(const char* name) {
	for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); i++)
		if (!strcmp(name, aliases[i][0])) name = aliases[i][1];
	for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++)
		if (!strcmp(table[i].name, name)) return table[i].fn;
	return NULL;
}
W void* FakeVkGetDeviceProcAddr(void* d, const char* name) {
	(void)d;
	void* fn = FakeVkLookup(name);
	if (!fn) LOG("{\"op\":\"missing_optional_proc\",\"name\":\"%s\"}\n", name);
	return fn;
}
