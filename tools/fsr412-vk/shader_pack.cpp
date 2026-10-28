#include "shader_pack.h"
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
thread_local std::string error;
struct Pack {
    unsigned char* memory = nullptr;
    size_t size = 0;
    uint32_t count = 0;
    ~Pack() { if (memory) munmap(memory, size); }
};
uint32_t read32(const unsigned char* p) { uint32_t n; std::memcpy(&n,p,4); return n; }
uint64_t read64(const unsigned char* p) { uint64_t n; std::memcpy(&n,p,8); return n; }
constexpr size_t record_size = 144;
}
extern "C" {
void* fsr412_spirv_open(const char* path) {
    error.clear();
    try {
        if (!path) { error="null path"; return nullptr; }
        int fd=open(path,O_RDONLY|O_CLOEXEC);
        if (fd<0) { error="cannot open shader pack"; return nullptr; }
        struct stat info{};
        if (fstat(fd,&info) || info.st_size<16 || uint64_t(info.st_size)>uint64_t(1024)*1024*1024) {
            close(fd); error="invalid shader pack size"; return nullptr;
        }
        auto p=std::make_unique<Pack>();
        p->size=static_cast<size_t>(info.st_size);
        void* mapping=mmap(nullptr,p->size,PROT_READ,MAP_PRIVATE,fd,0);
        close(fd);
        if (mapping==MAP_FAILED) { error="cannot map shader pack"; return nullptr; }
        p->memory=static_cast<unsigned char*>(mapping);
        if (std::memcmp(p->memory,"FSR4SPV\0",8) || read32(p->memory+8)!=1) {
            error="unsupported shader pack format"; return nullptr;
        }
        p->count=read32(p->memory+12);
        if (!p->count || p->count>10000 || 16+size_t(p->count)*record_size>p->size) {
            error="invalid shader index"; return nullptr;
        }
        uint64_t end=16+uint64_t(p->count)*record_size;
        for (size_t i=0;i<p->count;i++) {
            auto* r=p->memory+16+i*record_size;
            uint64_t offset=read64(r);
            uint32_t bytes=read32(r+8);
            if (offset!=end || offset%4 || bytes<20 || bytes%4 || offset>p->size || bytes>p->size-offset ||
                read32(r+12)!=0 || !std::memchr(r+16,0,128)) {
                error="invalid shader record bounds"; return nullptr;
            }
            auto* code=p->memory+offset;
            if (read32(code)!=0x07230203 || read32(code+16)!=0) {
                error="invalid SPIR-V header"; return nullptr;
            }
            for (uint32_t at=20;at<bytes;) {
                uint32_t count=read32(code+at)>>16;
                if (!count || count>(bytes-at)/4) { error="invalid SPIR-V instruction"; return nullptr; }
                at+=count*4;
            }
            end=offset+bytes;
        }
        if (end!=p->size) { error="unexpected trailing shader pack data"; return nullptr; }
        return p.release();
    } catch (...) { error="shader pack allocation failed"; return nullptr; }
}
void fsr412_spirv_close(void* pack) { delete static_cast<Pack*>(pack); }
size_t fsr412_spirv_count(const void* pack) { return pack ? static_cast<const Pack*>(pack)->count : 0; }
const uint32_t* fsr412_spirv_module(const void* pack, size_t index, size_t* bytes, const char** entry) {
    if (bytes) *bytes=0;
    if (entry) *entry=nullptr;
    auto* p=static_cast<const Pack*>(pack);
    if (!p || index>=p->count) { error="module index out of bounds"; return nullptr; }
    auto* record=p->memory+16+index*record_size;
    if (bytes) *bytes=read32(record+8);
    if (entry) *entry=reinterpret_cast<const char*>(record+16);
    return reinterpret_cast<const uint32_t*>(p->memory+read64(record));
}
const char* fsr412_spirv_error(void) { return error.c_str(); }
}
