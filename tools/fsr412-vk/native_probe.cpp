#include "shader_pack.h"
#include <cstdio>
#include <dlfcn.h>
int main(int argc,char** argv) {
    if (argc!=3) { std::fprintf(stderr,"usage: native-probe /path/libfsr412_spirv.so /path/shaders.spvpack\n"); return 2; }
    void* library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if (!library) { std::fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
    auto open=reinterpret_cast<decltype(&fsr412_spirv_open)>(dlsym(library,"fsr412_spirv_open"));
    auto close=reinterpret_cast<decltype(&fsr412_spirv_close)>(dlsym(library,"fsr412_spirv_close"));
    auto count=reinterpret_cast<decltype(&fsr412_spirv_count)>(dlsym(library,"fsr412_spirv_count"));
    auto module=reinterpret_cast<decltype(&fsr412_spirv_module)>(dlsym(library,"fsr412_spirv_module"));
    auto error=reinterpret_cast<decltype(&fsr412_spirv_error)>(dlsym(library,"fsr412_spirv_error"));
    if (!open||!close||!count||!module||!error) { std::fprintf(stderr,"missing native shader-pack API\n"); return 1; }
    auto pack=open(argv[2]);
    if (!pack) { std::fprintf(stderr,"open: %s\n",error()); return 1; }
    size_t total=0, n=count(pack);
    for (size_t i=0;i<n;i++) {
        size_t bytes=0; const char* entry=nullptr;
        auto words=module(pack,i,&bytes,&entry);
        if (!words||words[0]!=0x07230203||!entry||!*entry) { std::fprintf(stderr,"invalid module %zu\n",i); close(pack); return 1; }
        total+=bytes;
    }
    close(pack);dlclose(library);
    std::printf("Linux ELF loader: %zu SPIR-V modules, %zu bytes read successfully.\n",n,total);
    std::puts("CPU-side asset compatibility only. No GPU dispatch or image-quality validation was performed.");
}
