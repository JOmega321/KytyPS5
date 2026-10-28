// SPDX-License-Identifier: GPL-2.0-or-later
// Native FSR 4.1.2 INT8 replay investigation; this is not a game integration.
// Vulkan setup/resource helpers adapted from bbport gpu/tools/fsr4_bench.cpp,
// commit 8185b88d80573fa300595d5c54450183b4109935 (GPL-2.0-or-later).
// Uses Kyty's existing replay to TEST the recovered 4.1.2 execution contract.
// Completion and finite pixels do not establish equivalence with AMD's output.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "../../src/local/fsr411/fsr411.h"
#include "../../src/local/fsr4-formats.h"

namespace {
std::atomic_uint validation_errors{0};
VkDebugUtilsMessengerEXT debug_messenger{};
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++validation_errors;
    std::fprintf(stderr,"Vulkan validation: %s\n",data->pMessage);return VK_FALSE;
}

#define CHECK(call)                                                                            \
    do {                                                                                       \
        const VkResult r_ = (call);                                                            \
        if (r_ != VK_SUCCESS) {                                                                \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(r_));                           \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)

struct Gpu {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
};

uint32_t MemoryType(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

Gpu CreateGpu(bool fp8=false) {
    Gpu gpu;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "FSR 4.1.2 native experimental frame test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=Debug;
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const bool validate=std::getenv("FSR412_VALIDATE") && !std::strcmp(std::getenv("FSR412_VALIDATE"),"1");
    std::printf("Khronos validation: %s\n",validate?"enabled":"not requested");
    if(validate){ici.pNext=&debug;ici.enabledLayerCount=1;ici.ppEnabledLayerNames=&layer;ici.enabledExtensionCount=1;ici.ppEnabledExtensionNames=&extension;}
    CHECK(vkCreateInstance(&ici, nullptr, &gpu.instance));
    if(validate) {
        auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(gpu.instance,"vkCreateDebugUtilsMessengerEXT"));
        if(!create){std::fprintf(stderr,"Debug messenger unavailable\n");std::exit(1);}
        CHECK(create(gpu.instance,&debug,nullptr,&debug_messenger));
    }
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data()));
    if(devices.empty()){std::fprintf(stderr,"No Vulkan device\n");std::exit(1);}
    gpu.physical = devices.at(0);
    for (const auto device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu.physical = device;
            break;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu.physical, &props);
    std::printf("GPU: %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, family_props.data());
    while (gpu.family < families && !(family_props[gpu.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        ++gpu.family;
    }

    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR matrix{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrix2FeaturesNV matrix2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
    if(fp8){derivatives.pNext=&f8;f8.pNext=&matrix;matrix.pNext=&matrix2;}
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext=&f11;f11.pNext=&f12;f12.pNext=&f13;f13.pNext=&derivatives;
    vkGetPhysicalDeviceFeatures2(gpu.physical,&features);
    if(!f12.shaderFloat16 || !f12.shaderInt8 || !f13.shaderIntegerDotProduct ||
       !derivatives.computeDerivativeGroupLinear || !features.features.shaderInt16 ||
       !features.features.shaderStorageImageExtendedFormats) {
        std::fprintf(stderr,"Required native INT8 Vulkan features are unavailable\n");std::exit(1);
    }
    std::vector<const char*> extensions{VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME,
                                       VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if(fp8)for(const char *name:{VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME})extensions.push_back(name);
    uint32_t available_count=0;
    CHECK(vkEnumerateDeviceExtensionProperties(gpu.physical,nullptr,&available_count,nullptr));
    std::vector<VkExtensionProperties> available(available_count);
    CHECK(vkEnumerateDeviceExtensionProperties(gpu.physical,nullptr,&available_count,available.data()));
    for(const char *required:extensions) {
        if(!std::any_of(available.begin(),available.end(),[&](const auto& e){return !std::strcmp(e.extensionName,required);})) {
            std::fprintf(stderr,"Missing device extension: %s\n",required);std::exit(1);
        }
    }
    if(fp8) {
        std::printf("FP8 features: float8=%u matrixFloat8=%u cooperativeMatrix=%u conversions=%u memoryModel=%u variablePointers=%u storage16=%u subgroupSizeControl=%u fullSubgroups=%u\n",
            f8.shaderFloat8,f8.shaderFloat8CooperativeMatrix,matrix.cooperativeMatrix,matrix2.cooperativeMatrixConversions,
            f12.vulkanMemoryModel,f11.variablePointers,f11.storageBuffer16BitAccess,f13.subgroupSizeControl,f13.computeFullSubgroups);
        if(!f8.shaderFloat8 || !f8.shaderFloat8CooperativeMatrix || !matrix.cooperativeMatrix || !matrix2.cooperativeMatrixConversions ||
           !f12.vulkanMemoryModel || !f11.variablePointers || !f11.variablePointersStorageBuffer || !f11.storageBuffer16BitAccess || !f13.subgroupSizeControl || !f13.computeFullSubgroups) {
            std::fprintf(stderr,"Required native FP8 matrix features are unavailable; no INT8 fallback is used by this test\n");std::exit(1);
        }
    }
    if(gpu.family==families){std::fprintf(stderr,"No compute queue\n");std::exit(1);}
    std::printf("Device type: %s\n",props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU?"CPU software Vulkan (not Radeon hardware)":"hardware Vulkan");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gpu.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    CHECK(vkCreateDevice(gpu.physical, &dci, nullptr, &gpu.device));
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    return gpu;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data = nullptr;
};

Buffer CreateHostBuffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    CHECK(vkCreateBuffer(gpu.device, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(gpu, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(gpu.device, &ai, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(gpu.device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(gpu.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data));
    return b;
}

uint16_t Half(float f) {
    // Inputs in [0, 2): exact enough for test data (truncating conversion).
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) {
        return uint16_t(sign);
    }
    return uint16_t(sign | (uint32_t(exp) << 10) | ((x >> 13) & 0x3ffu));
}

struct Image {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    uint32_t width = 0, height = 0;
};

Image CreateImage(const Gpu& gpu, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect) {
    Image image;
    image.width = w;
    image.height = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    CHECK(vkCreateImage(gpu.device, &ci, nullptr, &image.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, image.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(gpu.device, &ai, nullptr, &image.memory));
    CHECK(vkBindImageMemory(gpu.device, image.image, image.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    CHECK(vkCreateImageView(gpu.device, &vi, nullptr, &image.view));
    return image;
}


} // namespace

int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==2 && !std::strcmp(argv[1],"--fp8-check")) {
        const Gpu gpu=CreateGpu(true);
        vkDestroyDevice(gpu.device,nullptr);
        if(debug_messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(gpu.instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(gpu.instance,debug_messenger,nullptr);}
        vkDestroyInstance(gpu.instance,nullptr);
        std::puts("Native FP8 feature prerequisites: OK; shader execution remains untested");return 0;
    }
    if(argc<3 || argc>9){std::fprintf(stderr,"usage: native-frame-test ASSETS NEW_OUTPUT.raw [RENDER_WxH] [OUTPUT_WxH] [FRAMES] [rgba16f|r11g11b10] [sharpen|no-sharpen] [int8|fp8]\n");return 2;}
    uint32_t rw=320,rh=180,ow=640,oh=360;int frames=argc>5?std::atoi(argv[5]):3;
    if((argc>3 && std::sscanf(argv[3],"%ux%u",&rw,&rh)!=2) ||
       (argc>4 && std::sscanf(argv[4],"%ux%u",&ow,&oh)!=2) || !rw || !rh || rw>ow || rh>oh || ow>3840 || oh>2160 || frames<1 || frames>8){std::fprintf(stderr,"Supported output is up to 3840x2160, with 1 to 8 frames\n");return 2;}
    if(double(ow)/rw>3.01 || double(oh)/rh>3.01){std::fprintf(stderr,"Upscale ratio exceeds the audited 3x model\n");return 2;}
    const std::string precision=argc>8?argv[8]:"int8";
    if(precision!="int8" && precision!="fp8"){std::fprintf(stderr,"Unknown precision\n");return 2;}
    const bool fp8=precision=="fp8";
    const std::string format=argc>6?argv[6]:"rgba16f";
    const std::string sharpening=argc>7?argv[7]:"sharpen";
    if((format!="rgba16f" && format!="r11g11b10") || (sharpening!="sharpen" && sharpening!="no-sharpen")) {
        std::fprintf(stderr,"Unknown output format or sharpening mode\n");return 2;
    }
    const bool packed=format=="r11g11b10";
    const VkFormat output_format=packed?VK_FORMAT_B10G11R11_UFLOAT_PACK32:VK_FORMAT_R16G16B16A16_SFLOAT;
    if(!Libs::Graphics::Fsr4OutputFormatSupported(output_format)) {
        std::fprintf(stderr,"Game integration rejects the selected output format\n");return 1;
    }
    std::printf("Output format: %s; sharpening: %s\n",format.c_str(),sharpening.c_str());
    FILE *file=std::fopen(argv[2],"wbx");if(!file){std::perror("new output file");return 2;}
    setenv("BB_FSR411_VARIANT",fp8?"fp8":"int8",1);
    std::printf("EXPERIMENTAL native FSR 4.1.2 %s frame test; no AMD reference comparison yet.\n",fp8?"FP8":"INT8");
    const Gpu gpu=CreateGpu(fp8);
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Image color=CreateImage(gpu,VK_FORMAT_R16G16B16A16_SFLOAT,rw,rh,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth=CreateImage(gpu,VK_FORMAT_R32_SFLOAT,rw,rh,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    Image motion=CreateImage(gpu,VK_FORMAT_R16G16_SFLOAT,rw,rh,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    Image output=CreateImage(gpu,output_format,ow,oh,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pci.queueFamilyIndex=gpu.family;
    VkCommandPool pool;CHECK(vkCreateCommandPool(gpu.device,&pci,nullptr,&pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    VkCommandBuffer cmd;CHECK(vkAllocateCommandBuffers(gpu.device,&cai,&cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;CHECK(vkCreateFence(gpu.device,&fci,nullptr,&fence));
    const auto submit=[&]{CHECK(vkEndCommandBuffer(cmd));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;CHECK(vkQueueSubmit(gpu.queue,1,&si,fence));CHECK(vkWaitForFences(gpu.device,1,&fence,VK_TRUE,60000000000ull));CHECK(vkResetFences(gpu.device,1,&fence));CHECK(vkResetCommandBuffer(cmd,0));};
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd,&begin));
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for(Image *image:{&color,&depth,&motion,&output}) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image->image;b.subresourceRange=range;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
    const VkClearColorValue grey{{0.4f,0.35f,0.3f,1.0f}},far{{0.5f,0,0,0}},zero{},sentinel{{17,17,17,17}};
    vkCmdClearColorImage(cmd,color.image,VK_IMAGE_LAYOUT_GENERAL,&grey,1,&range);
    vkCmdClearColorImage(cmd,depth.image,VK_IMAGE_LAYOUT_GENERAL,&far,1,&range);
    vkCmdClearColorImage(cmd,motion.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
    vkCmdClearColorImage(cmd,output.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);
    // A nonuniform deterministic color pattern exercises the neural passes instead
    // of checking only a flat image. The same data is used for reference comparisons.
    Buffer input=CreateHostBuffer(gpu,VkDeviceSize(rw)*rh*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* color_data=static_cast<uint16_t*>(input.data);
    for(uint32_t y=0;y<rh;y++)for(uint32_t x=0;x<rw;x++) {
        const size_t pixel=(size_t(y)*rw+x)*4;
        color_data[pixel]=Half(0.05f+0.9f*float(x)/float(rw));
        color_data[pixel+1]=Half(0.05f+0.8f*float(y)/float(rh));
        color_data[pixel+2]=Half(((x/8+y/8)%2)?0.25f:0.85f);
        color_data[pixel+3]=Half(1.0f);
    }
    VkMemoryBarrier upload_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    upload_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;upload_barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&upload_barrier,0,nullptr,0,nullptr);
    VkBufferImageCopy input_copy{};input_copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};input_copy.imageExtent={rw,rh,1};
    vkCmdCopyBufferToImage(cmd,input.buffer,color.image,VK_IMAGE_LAYOUT_GENERAL,1,&input_copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);submit();
    {
        Fsr411::Features enabled{};enabled.fp8_matrices=fp8;
        Fsr411::Upscaler upscaler(gpu.physical,gpu.device,argv[1],enabled);
        if(fp8 && upscaler.Describe().rfind("FP8 ",0)!=0){std::fprintf(stderr,"FP8 assets were not selected; refusing INT8 fallback\n");return 1;}
        std::printf("Replay precision: %s\n",fp8?"FP8 (native cooperative matrices)":"INT8");
        for(int i=0;i<frames;i++) {
            CHECK(vkBeginCommandBuffer(cmd,&begin));Fsr411::Frame f;f.cmdbuf=cmd;f.color={color.image,color.view,rw,rh};f.depth={depth.image,depth.view,rw,rh};f.motion={motion.image,motion.view,rw,rh};f.output={output.image,output.view,ow,oh};f.render_width=rw;f.render_height=rh;f.ultra_performance=double(ow)/rw>2.5;f.jitter[0]=0.25f*float(i%4)-0.375f;f.jitter[1]=0.125f;f.sharpen=sharpening=="sharpen";f.sharpness=f.sharpen?0.5f:0.0f;f.reset=i==0;f.auto_exposure=true;
            if(!upscaler.Record(f)){std::fprintf(stderr,"Native replay failed: %s\n",upscaler.Error().c_str());std::fclose(file);return 1;}
            submit();std::printf("Native Vulkan frame %d completed (%ux%u -> %ux%u)\n",i,rw,rh,ow,oh);
        }
    }
    const VkDeviceSize size=VkDeviceSize(ow)*oh*(packed?4:8);
    Buffer readback=CreateHostBuffer(gpu,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CHECK(vkBeginCommandBuffer(cmd,&begin));barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={ow,oh,1};
    vkCmdCopyImageToBuffer(cmd,output.image,VK_IMAGE_LAYOUT_GENERAL,readback.buffer,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);submit();
    const auto *pixels=static_cast<const uint16_t*>(readback.data);size_t bad=0,nonzero=0,unchanged=0,rgb_nonzero=0;
    if(packed) {
        const auto decode=[](uint32_t bits, unsigned mantissa_bits) {
            const unsigned exponent=bits>>mantissa_bits;
            const float fraction=float(bits&((1u<<mantissa_bits)-1))/float(1u<<mantissa_bits);
            if(exponent==31)return INFINITY;
            return exponent?std::ldexp(1.0f+fraction,int(exponent)-15):std::ldexp(fraction,-14);
        };
        const auto *words=static_cast<const uint32_t*>(readback.data);
        for(size_t i=0;i<size_t(ow)*oh;i++) {
            const float rgb[]={decode(words[i]&2047u,6),decode((words[i]>>11)&2047u,6),decode(words[i]>>22,5)};
            for(float value:rgb){bad+=!std::isfinite(value);nonzero+=value!=0;rgb_nonzero+=value!=0;unchanged+=value==17.0f;}
        }
    } else {
        for(size_t i=0;i<size_t(ow)*oh*4;i++){bad+=(pixels[i]&0x7c00u)==0x7c00u;nonzero+=(pixels[i]&0x7fffu)!=0;unchanged+=pixels[i]==Half(17.0f);rgb_nonzero+=(i%4!=3 && (pixels[i]&0x7fffu)!=0);}
    }
    const bool written=std::fwrite(readback.data,1,size_t(size),file)==size_t(size);const bool closed=std::fclose(file)==0;
    std::printf("Readback: %llu bytes, nonfinite=%zu, nonzero=%zu, untouched sentinel components=%zu\n",static_cast<unsigned long long>(size),bad,nonzero,unchanged);
    vkUnmapMemory(gpu.device,input.memory);vkDestroyBuffer(gpu.device,input.buffer,nullptr);vkFreeMemory(gpu.device,input.memory,nullptr);
    vkUnmapMemory(gpu.device,readback.memory);vkDestroyBuffer(gpu.device,readback.buffer,nullptr);vkFreeMemory(gpu.device,readback.memory,nullptr);
    for(Image *image:{&color,&depth,&motion,&output}){vkDestroyImageView(gpu.device,image->view,nullptr);vkDestroyImage(gpu.device,image->image,nullptr);vkFreeMemory(gpu.device,image->memory,nullptr);}
    vkDestroyFence(gpu.device,fence,nullptr);vkDestroyCommandPool(gpu.device,pool,nullptr);vkDestroyDevice(gpu.device,nullptr);
    if(debug_messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(gpu.instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(gpu.instance,debug_messenger,nullptr);}
    vkDestroyInstance(gpu.instance,nullptr);
    if(!written || !closed || bad || rgb_nonzero<size_t(ow)*oh || unchanged || validation_errors){std::fprintf(stderr,"Readback or validation check failed (validation errors: %u)\n",validation_errors.load());return 1;}
    std::printf("Native frame execution and readback sanity passed. Image equivalence and game integration remain unverified.\n");return 0;
}
