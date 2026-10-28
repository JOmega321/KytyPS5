// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the AMD provider's CPU code under Wine with a deliberately fake Vulkan device.
// It records host decisions only. It cannot validate GPU support or output pixels.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffx/api/include/vk/ffx_api_vk.h"
#include "ffx/upscalers/include/ffx_upscale.h"
__declspec(dllimport) void capProfile(int);
__declspec(dllimport) void capFrame(uint32_t);
__declspec(dllimport) void capNote(VkImage,const char*);
static void message(uint32_t type,const wchar_t *text) { fprintf(stderr,"FFX %u: %ls\n",type,text); }
static struct FfxApiResource resource(VkFormat vkfmt,uint32_t fmt,uint32_t w,uint32_t h,int output,const char *name) {
    VkImage image;
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=vkfmt,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    vkCreateImage((VkDevice)(uintptr_t)0x100000,&ci,NULL,&image);capNote(image,name);
    struct FfxApiResource r={0};r.resource=(void*)image;r.description.type=FFX_API_RESOURCE_TYPE_TEXTURE2D;r.description.format=fmt;r.description.width=w;r.description.height=h;r.description.depth=1;r.description.mipCount=1;r.description.usage=output?FFX_API_RESOURCE_USAGE_UAV:FFX_API_RESOURCE_USAGE_READ_ONLY;r.state=output?FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:FFX_API_RESOURCE_STATE_COMPUTE_READ;return r;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc!=6){fprintf(stderr,"usage: capture.exe DLL int8|fp8 RENDER_WxH OUTPUT_WxH FRAMES\nRun in a NEW empty working directory. NO GPU execution.\n");return 2;}
    uint32_t rw=0,rh=0,ow=0,oh=0;int frames=atoi(argv[5]);
    if((strcmp(argv[2],"int8") && strcmp(argv[2],"fp8")) || sscanf(argv[3],"%ux%u",&rw,&rh)!=2 || sscanf(argv[4],"%ux%u",&ow,&oh)!=2 || !rw || !rh || rw>ow || rh>oh || ow>4096 || oh>4096 || frames<1 || frames>16)return 2;
    capProfile(!strcmp(argv[2],"fp8"));
    printf("OFFLINE synthetic device; no GPU execution.\n");
    HMODULE dll=LoadLibraryExA(argv[1],NULL,LOAD_WITH_ALTERED_SEARCH_PATH);if(!dll){fprintf(stderr,"DLL load failed: %lu\n",GetLastError());return 1;}
    PfnFfxCreateContext create=(PfnFfxCreateContext)GetProcAddress(dll,"ffxCreateContext");
    PfnFfxDestroyContext destroy=(PfnFfxDestroyContext)GetProcAddress(dll,"ffxDestroyContext");
    PfnFfxQuery query=(PfnFfxQuery)GetProcAddress(dll,"ffxQuery");
    PfnFfxDispatch dispatch=(PfnFfxDispatch)GetProcAddress(dll,"ffxDispatch");
    if(!create || !destroy || !query || !dispatch)return 1;
    VkDeviceContext device={(VkDevice)(uintptr_t)0x100000,(VkPhysicalDevice)(uintptr_t)0x200000,vkGetDeviceProcAddr};
    uint64_t count=32,ids[32]={0};const char *names[32]={0};
    struct ffxQueryDescGetVersions versions={.header={FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,NULL},.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,.device=&device,.outputCount=&count,.versionIds=ids,.versionNames=names};
    uint32_t rc=query(NULL,&versions.header);printf("query=%u count=%llu\n",rc,(unsigned long long)count);if(rc || count>32)return 1;
    uint64_t chosen=0;for(uint64_t i=0;i<count;i++){printf("provider %016llx %s\n",(unsigned long long)ids[i],names[i]?names[i]:"?");if(!chosen && names[i] && (strstr(names[i],"4.1.2") || strstr(names[i],"FSR4")))chosen=ids[i];}
    if(!chosen){fprintf(stderr,"No FSR4 provider; refusing FSR3 fallback\n");return 1;}
    struct ffxCreateContextDescUpscaleVersion version={.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,NULL},.version=FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc backend={.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,&version.header},.deviceContext=device};
    struct ffxOverrideVersion override={.header={FFX_API_DESC_TYPE_OVERRIDE_VERSION,&backend.header},.versionId=chosen};
    struct ffxCreateContextDescUpscale desc={.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,&override.header},.flags=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE|FFX_UPSCALE_ENABLE_AUTO_EXPOSURE,.maxRenderSize={ow,oh},.maxUpscaleSize={ow,oh},.fpMessage=message};
    ffxContext context=NULL;rc=create(&context,&desc.header,NULL);printf("create=%u\n",rc);if(rc || !context)return 1;
    struct ffxQueryGetProviderVersion selected={.header={FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION,NULL}};rc=query(&context,&selected.header);printf("selected=%016llx %s\n",(unsigned long long)selected.versionId,selected.versionName?selected.versionName:"?");if(rc || selected.versionId!=chosen){destroy(&context,NULL);return 1;}
    struct ffxDispatchDescUpscale d={0};d.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;d.commandList=(void*)(uintptr_t)0x300000;
    d.color=resource(VK_FORMAT_R16G16B16A16_SFLOAT,FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,rw,rh,0,"color");
    d.depth=resource(VK_FORMAT_R32_SFLOAT,FFX_API_SURFACE_FORMAT_R32_FLOAT,rw,rh,0,"depth");
    d.motionVectors=resource(VK_FORMAT_R16G16_SFLOAT,FFX_API_SURFACE_FORMAT_R16G16_FLOAT,rw,rh,0,"motion");
    d.output=resource(VK_FORMAT_R16G16B16A16_SFLOAT,FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,ow,oh,1,"output");
    d.motionVectorScale.x=d.motionVectorScale.y=1;d.renderSize.width=rw;d.renderSize.height=rh;d.upscaleSize.width=ow;d.upscaleSize.height=oh;d.enableSharpening=true;d.sharpness=0.5f;d.frameTimeDelta=10;d.preExposure=1;d.cameraNear=0.05f;d.cameraFar=3000;d.cameraFovAngleVertical=0.75f;d.viewSpaceToMetersFactor=1;
    for(int i=0;i<frames;i++){capFrame((uint32_t)i);d.reset=i==0;d.jitterOffset.x=0.25f*(i%4)-0.375f;d.jitterOffset.y=0.125f;rc=dispatch(&context,&d.header);printf("frame %d dispatch=%u (recorded, NOT executed)\n",i,rc);if(rc)return 1;}
    rc=destroy(&context,NULL);printf("destroy=%u\n",rc);return rc?1:0;
}
