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
// The provider also asks Windows GDI for AMD ASIC family/revision. Redirect ONLY
// these two hardware queries inside the inspected DLL to our synthetic profile.
// No AMD instructions or files are patched; this is host-call instrumentation.
static LONG WINAPI enum_adapters(void *out) {
    unsigned char *p=out;memset(p,0,324);*(uint32_t*)p=1;*(uint32_t*)(p+4)=1;memset(p+8,3,8);return 0;
}
static LONG WINAPI adapter_info(void *query) {
    unsigned char *p=query;void *data=*(void**)(p+8);uint32_t size=*(uint32_t*)(p+16);
    if(*(uint32_t*)(p+4)!=0 || size<0x38 || size>0x100000 || !data)return (LONG)0xc000000d;
    memset(data,0,size);*(uint32_t*)((unsigned char*)data+0x30)=0x91;*(uint32_t*)((unsigned char*)data+0x34)=1;
    printf("SIMULATED Windows ASIC query: family=145 revision=1 (no physical GPU)\n");return 0;
}
static FARPROC WINAPI lookup(HMODULE module,LPCSTR name) {
    if((uintptr_t)name>65535) {
        if(!strcmp(name,"D3DKMTEnumAdapters"))return (FARPROC)enum_adapters;
        if(!strcmp(name,"D3DKMTQueryAdapterInfo"))return (FARPROC)adapter_info;
    }
    return GetProcAddress(module,name);
}
static int instrument_adapter_queries(HMODULE module) {
    unsigned char *base=(void*)module;IMAGE_DOS_HEADER *dos=(void*)base;
    IMAGE_NT_HEADERS64 *nt=(void*)(base+dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *imp=(void*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imp->Name;imp++) {
        if(!imp->OriginalFirstThunk)continue;
        IMAGE_THUNK_DATA64 *names=(void*)(base+imp->OriginalFirstThunk),*iat=(void*)(base+imp->FirstThunk);
        for(;names->u1.AddressOfData;names++,iat++) {
            if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))continue;
            IMAGE_IMPORT_BY_NAME *symbol=(void*)(base+names->u1.AddressOfData);
            if(!strcmp((const char*)symbol->Name,"GetProcAddress")) {
                DWORD old,ignored;if(!VirtualProtect(&iat->u1.Function,sizeof(uintptr_t),PAGE_READWRITE,&old))return 0;
                iat->u1.Function=(uintptr_t)lookup;
                if(!VirtualProtect(&iat->u1.Function,sizeof(uintptr_t),old,&ignored))return 0;
                return 1;
            }
        }
    }
    return 0;
}
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
    if(!strcmp(argv[2],"fp8")){fprintf(stderr,"FP8 ASIC profile is not implemented; refusing mislabeled capture\n");return 2;}
    capProfile(0);
    printf("OFFLINE synthetic device; no GPU execution.\n");
    HMODULE dll=LoadLibraryExA(argv[1],NULL,LOAD_WITH_ALTERED_SEARCH_PATH);if(!dll){fprintf(stderr,"DLL load failed: %lu\n",GetLastError());return 1;}
    if(!instrument_adapter_queries(dll)){fprintf(stderr,"Cannot instrument synthetic adapter queries\n");return 1;}
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
