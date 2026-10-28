// SPDX-License-Identifier: GPL-2.0-or-later
// Offline Vulkan recorder for the Windows FSR provider. NO GPU work is performed.
// Handles and capabilities are synthetic. A successful trace is not a rendered frame.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 16384
#define BINDINGS 512
typedef struct Object {
    uint64_t size, memory, offset;
    unsigned char *data;
    uint32_t width, height, depth, format, usage, mips, stride;
    uint64_t resource;
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    char name[128];
} Object;
static Object objects[LIMIT];
static uint64_t serial = 1, pipeline, sets[8];
static uint32_t frame;
static FILE *trace;
static int fp8;
static uint64_t allocated;

static void fail(const char *why) { fprintf(stderr,"offline recorder: %s\n",why); exit(2); }
static uint64_t id(const void *handle) { return (uint64_t)(uintptr_t)handle; }
#define ID(h) id((const void *)(uintptr_t)(h))
#define OBJ(h) objects[checked(ID(h))]
static uint64_t checked(uint64_t h) { if (!h || h >= serial) fail("unknown synthetic handle"); return h; }
static uint64_t make(void) { if (serial == LIMIT) fail("object limit"); return serial++; }
static FILE *logfile(void) { if (!trace) { trace=fopen("trace.jsonl","wb"); if (!trace) fail("trace open"); setvbuf(trace,NULL,_IONBF,0); } return trace; }
#define LOG(...) fprintf(logfile(), __VA_ARGS__)
static void bytes(const char *name,const void *data,size_t size) {
    FILE *f=fopen(name,"wb"); if (!f) fail("binary output open");
    if (fwrite(data,1,size,f)!=size || fclose(f)) fail("binary output write");
}
static uint64_t hash(const void *data,size_t size) {
    uint64_t h=14695981039346656037ull;const unsigned char *p=data;
    for(size_t i=0;i<size;i++) h=(h^p[i])*1099511628211ull;
    return h;
}
static void dump(uint64_t buffer,uint64_t offset,uint64_t size,char *name,size_t capacity) {
    Object *b=&objects[checked(buffer)];
    if (!b->memory) fail("unbound buffer");
    Object *m=&objects[checked(b->memory)];
    if (offset>b->size) fail("buffer offset");
    if (size==VK_WHOLE_SIZE) size=b->size-offset;
    if(size>b->size-offset || b->offset>m->size || offset>m->size-b->offset || size>m->size-b->offset-offset) fail("buffer range");
    const unsigned char *data=m->data+b->offset+offset;
    snprintf(name,capacity,"data_%016"PRIx64".bin",hash(data,(size_t)size));
    bytes(name,data,(size_t)size);
}
__declspec(dllexport) void capProfile(int use_fp8) { fp8=use_fp8; LOG("{\"op\":\"profile\",\"synthetic\":true,\"gpu_executed\":false,\"fp8\":%s}\n",fp8?"true":"false"); }
__declspec(dllexport) void capFrame(uint32_t number) { frame=number; LOG("{\"op\":\"frame\",\"index\":%u}\n",frame); }
__declspec(dllexport) void capNote(VkImage image,const char *name) { LOG("{\"op\":\"input\",\"image\":%"PRIu64",\"name\":\"%s\"}\n",ID(image),name); }

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice d,VkPhysicalDeviceProperties *p) {
    (void)d;memset(p,0,sizeof(*p)); p->apiVersion=VK_API_VERSION_1_3;p->vendorID=0x1002;p->deviceID=fp8?0x7550:0x744c;p->deviceType=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    strcpy(p->deviceName,"FSR offline synthetic recorder - no GPU");
    p->limits.maxImageDimension1D=p->limits.maxImageDimension2D=16384;p->limits.maxImageDimension3D=2048;p->limits.maxImageArrayLayers=2048;
    p->limits.maxUniformBufferRange=65536;p->limits.maxStorageBufferRange=1u<<30;p->limits.maxMemoryAllocationCount=4096;
    p->limits.maxBoundDescriptorSets=8;p->limits.maxPerStageDescriptorSamplers=1024;p->limits.maxPerStageDescriptorStorageBuffers=1024;p->limits.maxPerStageDescriptorStorageImages=1024;p->limits.maxPerStageDescriptorSampledImages=1024;
    p->limits.maxComputeSharedMemorySize=65536;p->limits.maxComputeWorkGroupInvocations=1024;
    for(int i=0;i<3;i++){p->limits.maxComputeWorkGroupCount[i]=65535;p->limits.maxComputeWorkGroupSize[i]=1024;}
    p->limits.minUniformBufferOffsetAlignment=256;p->limits.minStorageBufferOffsetAlignment=16;p->limits.minTexelBufferOffsetAlignment=16;p->limits.nonCoherentAtomSize=64;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d,VkPhysicalDeviceProperties2 *p) {
    vkGetPhysicalDeviceProperties(d,&p->properties);
    for(VkBaseOutStructure *s=p->pNext;s;s=s->pNext) {
        LOG("{\"op\":\"property_query\",\"type\":%u}\n",s->sType);
        switch(s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
            VkPhysicalDeviceIDProperties *v=(void*)s;memset(v->deviceUUID,1,VK_UUID_SIZE);memset(v->driverUUID,2,VK_UUID_SIZE);
            // The Windows provider refuses a device without a valid Windows LUID before
            // asking for any shader features. This identity is explicitly SYNTHETIC.
            memset(v->deviceLUID,3,VK_LUID_SIZE);v->deviceLUIDValid=1;v->deviceNodeMask=1;break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            VkPhysicalDeviceDriverProperties *v=(void*)s;v->driverID=VK_DRIVER_ID_MESA_RADV;strcpy(v->driverName,"synthetic offline recorder");break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            VkPhysicalDeviceSubgroupProperties *v=(void*)s;v->subgroupSize=32;v->supportedStages=VK_SHADER_STAGE_COMPUTE_BIT;v->supportedOperations=0xff;v->quadOperationsInAllStages=1;break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
            VkPhysicalDeviceSubgroupSizeControlProperties *v=(void*)s;v->minSubgroupSize=32;v->maxSubgroupSize=64;v->maxComputeWorkgroupSubgroups=32;v->requiredSubgroupSizeStages=VK_SHADER_STAGE_COMPUTE_BIT;break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES: {
            unsigned char *a=(void*)&((VkPhysicalDeviceShaderIntegerDotProductProperties*)s)->integerDotProduct8BitUnsignedAccelerated;
            for(size_t i=0;i<sizeof(VkPhysicalDeviceShaderIntegerDotProductProperties)-offsetof(VkPhysicalDeviceShaderIntegerDotProductProperties,integerDotProduct8BitUnsignedAccelerated);i+=4) *(VkBool32*)(a+i)=1;
            break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR:
            ((VkPhysicalDeviceCooperativeMatrixPropertiesKHR*)s)->cooperativeMatrixSupportedStages=VK_SHADER_STAGE_COMPUTE_BIT;break;
        // Caller initialized the remaining fields. Unsupported optional properties stay zero.
        default: LOG("{\"op\":\"unsupported_property\",\"type\":%u}\n",s->sType);break;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d,VkPhysicalDeviceFeatures2 *p) {
    (void)d;VkBool32 *v=(void*)&p->features;for(size_t i=0;i<sizeof(p->features)/4;i++)v[i]=1;
    for(VkBaseOutStructure *s=p->pNext;s;s=s->pNext) {
        LOG("{\"op\":\"feature_query\",\"type\":%u}\n",s->sType);
        switch(s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: { VkPhysicalDeviceShaderFloat16Int8Features *v=(void*)s;v->shaderFloat16=v->shaderInt8=1;break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: ((VkPhysicalDeviceShaderIntegerDotProductFeatures*)s)->shaderIntegerDotProduct=1;break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR: { VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR *v=(void*)s;v->computeDerivativeGroupQuads=v->computeDerivativeGroupLinear=1;break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: { VkPhysicalDeviceSubgroupSizeControlFeatures *v=(void*)s;v->subgroupSizeControl=v->computeFullSubgroups=1;break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT: { VkPhysicalDeviceShaderFloat8FeaturesEXT *v=(void*)s;v->shaderFloat8=v->shaderFloat8CooperativeMatrix=fp8;break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR: { VkPhysicalDeviceCooperativeMatrixFeaturesKHR *v=(void*)s;v->cooperativeMatrix=v->cooperativeMatrixRobustBufferAccess=fp8;break; }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE: { VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE *v=(void*)s;v->shaderMixedFloatDotProductFloat16AccFloat32=v->shaderMixedFloatDotProductFloat16AccFloat16=1;v->shaderMixedFloatDotProductFloat8AccFloat32=fp8;break; }
        default: LOG("{\"op\":\"unsupported_feature\",\"type\":%u}\n",s->sType);break;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d,VkPhysicalDeviceMemoryProperties *p) {
    (void)d;memset(p,0,sizeof(*p));p->memoryTypeCount=1;p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    p->memoryHeapCount=1;p->memoryHeaps[0].size=8ull<<30;p->memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}
static const char *extensions[]={"VK_EXT_subgroup_size_control","VK_KHR_shader_float16_int8","VK_KHR_shader_integer_dot_product","VK_KHR_compute_shader_derivatives","VK_VALVE_shader_mixed_float_dot_product","VK_KHR_storage_buffer_storage_class","VK_KHR_16bit_storage","VK_KHR_8bit_storage","VK_KHR_maintenance1","VK_KHR_maintenance2","VK_EXT_shader_float8","VK_KHR_cooperative_matrix"};
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d,const char *layer,uint32_t *n,VkExtensionProperties *p) {
    (void)d;(void)layer;uint32_t count=(uint32_t)(sizeof(extensions)/sizeof(*extensions))-(fp8?0:2);
    if(!p){*n=count;return VK_SUCCESS;}uint32_t used=*n<count?*n:count;for(uint32_t i=0;i<used;i++){memset(p+i,0,sizeof(*p));strcpy(p[i].extensionName,extensions[i]);p[i].specVersion=1;}*n=used;return used<count?VK_INCOMPLETE:VK_SUCCESS;
}

#define CREATE(T,NAME,INFO) VKAPI_ATTR VkResult VKAPI_CALL NAME(VkDevice d,const INFO *p,const VkAllocationCallbacks *a,T *out) { (void)d;(void)p;(void)a;*out=(T)(uintptr_t)make();return VK_SUCCESS; }
#define DESTROY(T,NAME) VKAPI_ATTR void VKAPI_CALL NAME(VkDevice d,T h,const VkAllocationCallbacks *a) { (void)d;(void)h;(void)a; }
CREATE(VkSampler,vkCreateSampler,VkSamplerCreateInfo)
CREATE(VkDescriptorPool,vkCreateDescriptorPool,VkDescriptorPoolCreateInfo)
CREATE(VkPipelineLayout,vkCreatePipelineLayout,VkPipelineLayoutCreateInfo)
CREATE(VkDescriptorSetLayout,vkCreateDescriptorSetLayout,VkDescriptorSetLayoutCreateInfo)
DESTROY(VkSampler,vkDestroySampler)
DESTROY(VkDescriptorPool,vkDestroyDescriptorPool)
DESTROY(VkPipelineLayout,vkDestroyPipelineLayout)
DESTROY(VkDescriptorSetLayout,vkDestroyDescriptorSetLayout)
DESTROY(VkShaderModule,vkDestroyShaderModule)
DESTROY(VkPipeline,vkDestroyPipeline)
DESTROY(VkBuffer,vkDestroyBuffer)
DESTROY(VkImage,vkDestroyImage)
DESTROY(VkImageView,vkDestroyImageView)
DESTROY(VkBufferView,vkDestroyBufferView)
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *p,const VkAllocationCallbacks *a,VkBuffer *out) {
    (void)d;(void)a;uint64_t h=make();*out=(VkBuffer)(uintptr_t)h;objects[h].size=p->size;objects[h].usage=p->usage;
    LOG("{\"op\":\"buffer\",\"id\":%"PRIu64",\"size\":%"PRIu64",\"usage\":%u}\n",h,(uint64_t)p->size,p->usage);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d,const VkImageCreateInfo *p,const VkAllocationCallbacks *a,VkImage *out) {
    (void)d;(void)a;uint64_t h=make();*out=(VkImage)(uintptr_t)h;Object *o=objects+h;o->width=p->extent.width;o->height=p->extent.height;o->depth=p->extent.depth;o->format=p->format;o->mips=p->mipLevels;o->usage=p->usage;o->size=(uint64_t)o->width*o->height*o->depth*16;
    LOG("{\"op\":\"image\",\"id\":%"PRIu64",\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"mips\":%u,\"usage\":%u}\n",h,o->format,o->width,o->height,o->depth,o->mips,o->usage);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *p) { (void)d;p->size=(OBJ(b).size+255)&~255ull;p->alignment=256;p->memoryTypeBits=1; }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d,VkImage b,VkMemoryRequirements *p) { vkGetBufferMemoryRequirements(d,(VkBuffer)b,p); }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice d,const VkBufferMemoryRequirementsInfo2 *b,VkMemoryRequirements2 *p) { vkGetBufferMemoryRequirements(d,b->buffer,&p->memoryRequirements); }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *p,const VkAllocationCallbacks *a,VkDeviceMemory *out) {
    (void)d;(void)a;if(p->allocationSize>(1ull<<30) || allocated+p->allocationSize>(3ull<<30))fail("allocation limit");allocated+=p->allocationSize;
    uint64_t h=make();objects[h].size=p->allocationSize;objects[h].data=calloc(1,(size_t)p->allocationSize);if(!objects[h].data)fail("allocation failed");*out=(VkDeviceMemory)(uintptr_t)h;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a) { (void)d;(void)a;if(m){Object *o=&OBJ(m);free(o->data);o->data=NULL;allocated-=o->size;} }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize offset) { (void)d;Object *o=&OBJ(b),*mem=&OBJ(m);if(offset>mem->size || o->size>mem->size-offset)fail("bind range");o->memory=ID(m);o->offset=offset;return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d,VkImage b,VkDeviceMemory m,VkDeviceSize offset) { return vkBindBufferMemory(d,(VkBuffer)b,m,offset); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void **out) {
    (void)d;(void)flags;Object *o=&OBJ(m);if(offset>o->size || (size!=VK_WHOLE_SIZE && size>o->size-offset))fail("map range");*out=o->data+offset;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory m) { (void)d;Object *o=&OBJ(m);if(o->size<=16*1024*1024){char n[80];snprintf(n,sizeof(n),"unmap_%016"PRIx64".bin",hash(o->data,(size_t)o->size));bytes(n,o->data,(size_t)o->size);LOG("{\"op\":\"unmap\",\"memory\":%"PRIu64",\"size\":%"PRIu64",\"file\":\"%s\"}\n",ID(m),o->size,n);} }
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *ranges) { (void)d;(void)n;(void)ranges;return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *p,const VkAllocationCallbacks *a,VkImageView *out) {
    (void)d;(void)a;uint64_t h=make();*out=(VkImageView)(uintptr_t)h;objects[h].resource=ID(p->image);
    LOG("{\"op\":\"image_view\",\"id\":%"PRIu64",\"image\":%"PRIu64",\"format\":%u,\"base_mip\":%u,\"mips\":%u}\n",h,ID(p->image),p->format,p->subresourceRange.baseMipLevel,p->subresourceRange.levelCount);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice d,const VkBufferViewCreateInfo *p,const VkAllocationCallbacks *a,VkBufferView *out) {
    (void)d;(void)a;uint64_t h=make();*out=(VkBufferView)(uintptr_t)h;objects[h].resource=ID(p->buffer);objects[h].offset=p->offset;objects[h].size=p->range;objects[h].format=p->format;
    LOG("{\"op\":\"buffer_view\",\"id\":%"PRIu64",\"buffer\":%"PRIu64",\"offset\":%"PRIu64",\"range\":%"PRIu64",\"format\":%u}\n",h,ID(p->buffer),(uint64_t)p->offset,(uint64_t)p->range,p->format);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *p,const VkAllocationCallbacks *a,VkShaderModule *out) {
    (void)d;(void)a;uint64_t h=make();*out=(VkShaderModule)(uintptr_t)h;snprintf(objects[h].name,sizeof(objects[h].name),"shader_%016"PRIx64".spv",hash(p->pCode,p->codeSize));bytes(objects[h].name,p->pCode,p->codeSize);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *p,const VkAllocationCallbacks *a,VkPipeline *out) {
    (void)d;(void)c;(void)a;for(uint32_t i=0;i<n;i++){uint64_t h=make();out[i]=(VkPipeline)(uintptr_t)h;
        snprintf(objects[h].name,sizeof(objects[h].name),"%s",p[i].stage.pName);
        LOG("{\"op\":\"pipeline\",\"id\":%"PRIu64",\"entry\":\"%s\",\"shader\":\"%s\",\"layout\":%"PRIu64"}\n",h,p[i].stage.pName,OBJ(p[i].stage.module).name,ID(p[i].layout));
    }return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *p,VkDescriptorSet *out) {
    (void)d;for(uint32_t i=0;i<p->descriptorSetCount;i++){uint64_t h=make();out[i]=(VkDescriptorSet)(uintptr_t)h;objects[h].data=calloc(BINDINGS,sizeof(Object));if(!objects[h].data)fail("descriptor allocation");}return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice d,VkDescriptorPool pool,uint32_t n,const VkDescriptorSet *sets) { (void)d;(void)pool;for(uint32_t i=0;i<n;i++){free(OBJ(sets[i]).data);OBJ(sets[i]).data=NULL;}return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *p,uint32_t copies,const VkCopyDescriptorSet *copy) {
    (void)d;(void)copy;if(copies)fail("descriptor copies unsupported");for(uint32_t i=0;i<n;i++) {
        if(p[i].dstBinding>=BINDINGS || p[i].dstArrayElement || p[i].descriptorCount!=1)fail("descriptor array unsupported");
        Object *o=(Object*)OBJ(p[i].dstSet).data+p[i].dstBinding;o->usage=p[i].descriptorType;o->stride=1;
        switch(p[i].descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: o->buffer=*p[i].pBufferInfo;break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:case VK_DESCRIPTOR_TYPE_SAMPLER:case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:o->image=*p[i].pImageInfo;break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:o->resource=ID(*p[i].pTexelBufferView);break;
        default:fail("unsupported descriptor type");
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p) { (void)c;(void)b;pipeline=ID(p); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout layout,uint32_t first,uint32_t n,const VkDescriptorSet *p,uint32_t dyn,const uint32_t *offsets) {
    (void)c;(void)b;(void)layout;(void)offsets;if(first+n>8 || dyn)fail("descriptor set binding unsupported");for(uint32_t i=0;i<n;i++)sets[first+i]=ID(p[i]);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z) {
    (void)c;checked(pipeline);LOG("{\"op\":\"dispatch\",\"frame\":%u,\"pipeline\":%"PRIu64",\"groups\":[%u,%u,%u],\"bindings\":[",frame,pipeline,x,y,z);int comma=0;
    for(uint32_t set=0;set<8;set++)if(sets[set])for(uint32_t binding=0;binding<BINDINGS;binding++) {
        Object *o=(Object*)objects[checked(sets[set])].data+binding;if(!o->stride)continue;
        LOG("%s{\"set\":%u,\"binding\":%u,\"type\":%u",comma++?",":"",set,binding,o->usage);
        if(o->usage==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || o->usage==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            LOG(",\"buffer\":%"PRIu64",\"offset\":%"PRIu64",\"range\":%"PRIu64,ID(o->buffer.buffer),(uint64_t)o->buffer.offset,(uint64_t)o->buffer.range);
            uint64_t remaining=OBJ(o->buffer.buffer).size-o->buffer.offset;
            if(o->usage==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || remaining<=1024*1024) {char name[80];dump(ID(o->buffer.buffer),o->buffer.offset,o->buffer.range,name,sizeof(name));LOG(",\"data\":\"%s\"",name);}
        } else if(o->usage==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || o->usage==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            Object *v=&objects[checked(o->resource)];LOG(",\"view\":%"PRIu64,o->resource);
            if(objects[checked(v->resource)].size<=1024*1024) {char name[80];dump(v->resource,v->offset,v->size,name,sizeof(name));LOG(",\"data\":\"%s\"",name);}
        }
        else LOG(",\"view\":%"PRIu64",\"sampler\":%"PRIu64",\"layout\":%u",ID(o->image.imageView),ID(o->image.sampler),o->image.imageLayout);
        LOG("}");
    }LOG("]}\n");
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy *p) {
    (void)c;for(uint32_t i=0;i<n;i++){char name[80];dump(ID(src),p[i].srcOffset,p[i].size,name,sizeof(name));Object *s=&OBJ(src),*d=&OBJ(dst),*sm=&objects[checked(s->memory)],*dm=&objects[checked(d->memory)];
        if(p[i].dstOffset>d->size || p[i].size>d->size-p[i].dstOffset)fail("copy destination range");
        memcpy(dm->data+d->offset+p[i].dstOffset,sm->data+s->offset+p[i].srcOffset,(size_t)p[i].size);
        LOG("{\"op\":\"copy_buffer\",\"src\":%"PRIu64",\"dst\":%"PRIu64",\"src_offset\":%"PRIu64",\"dst_offset\":%"PRIu64",\"size\":%"PRIu64",\"data\":\"%s\"}\n",ID(src),ID(dst),(uint64_t)p[i].srcOffset,(uint64_t)p[i].dstOffset,(uint64_t)p[i].size,name);
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,uint32_t value) {
    (void)c;Object *b=&OBJ(buffer);if(offset>b->size)fail("fill offset");if(size==VK_WHOLE_SIZE)size=b->size-offset;if(size>b->size-offset || size%4)fail("fill range");Object *m=&objects[checked(b->memory)];for(uint64_t i=0;i<size;i+=4)memcpy(m->data+b->offset+offset+i,&value,4);
    LOG("{\"op\":\"fill\",\"buffer\":%"PRIu64",\"offset\":%"PRIu64",\"size\":%"PRIu64",\"value\":%u}\n",ID(buffer),(uint64_t)offset,(uint64_t)size,value);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
    (void)c;(void)dep;(void)nm;(void)m;LOG("{\"op\":\"barrier\",\"src_stage\":%u,\"dst_stage\":%u,\"buffers\":[",src,dst);
    for(uint32_t i=0;i<nb;i++)LOG("%s{\"id\":%"PRIu64",\"src\":%u,\"dst\":%u}",i?",":"",ID(b[i].buffer),b[i].srcAccessMask,b[i].dstAccessMask);
    LOG("],\"images\":[");for(uint32_t i=0;i<ni;i++)LOG("%s{\"id\":%"PRIu64",\"old\":%u,\"new\":%u,\"src\":%u,\"dst\":%u}",i?",":"",ID(im[i].image),im[i].oldLayout,im[i].newLayout,im[i].srcAccessMask,im[i].dstAccessMask);LOG("]}\n");
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer c,VkImage image,VkImageLayout layout,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *ranges) {
    (void)c;(void)n;(void)ranges;LOG("{\"op\":\"clear_image\",\"image\":%"PRIu64",\"layout\":%u,\"value\":[%u,%u,%u,%u]}\n",ID(image),layout,v->uint32[0],v->uint32[1],v->uint32[2],v->uint32[3]);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer src,VkImage dst,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *p) {
    (void)c;(void)p;LOG("{\"op\":\"copy_buffer_image\",\"buffer\":%"PRIu64",\"image\":%"PRIu64",\"layout\":%u,\"regions\":%u}\n",ID(src),ID(dst),layout,n);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy *p) { (void)c;(void)p;LOG("{\"op\":\"copy_image\",\"src\":%"PRIu64",\"dst\":%"PRIu64",\"src_layout\":%u,\"dst_layout\":%u,\"regions\":%u}\n",ID(src),ID(dst),sl,dl,n); }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o) { (void)c;(void)b;(void)o;fail("indirect dispatch requires GPU results"); }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    (void)device;HMODULE self=NULL;GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const char*)&vkGetDeviceProcAddr,&self);
    if(!strcmp(name,"vkGetBufferMemoryRequirements2KHR"))name="vkGetBufferMemoryRequirements2";
    FARPROC p=GetProcAddress(self,name);if(!p)LOG("{\"op\":\"missing_optional_proc\",\"name\":\"%s\"}\n",name);return (PFN_vkVoidFunction)p;
}
