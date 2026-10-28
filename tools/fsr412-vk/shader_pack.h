#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Native asset-access API, NOT AMD's ffx API. This library does not perform upscaling.
void* fsr412_spirv_open(const char* path);
void fsr412_spirv_close(void* pack);
size_t fsr412_spirv_count(const void* pack);
const uint32_t* fsr412_spirv_module(const void* pack, size_t index, size_t* bytes, const char** entry_point);
const char* fsr412_spirv_error(void);
#ifdef __cplusplus
}
#endif
