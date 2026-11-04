// SPDX-License-Identifier: GPL-2.0-or-later
// pehost: runs the CPU code of a 64-bit Windows DLL (AMD's FidelityFX Vulkan provider) inside a Linux process, with
// a small subset of the Win32 API it imports. No Wine. The DLL's own code runs natively; imported functions are
// served by this host with the Windows x64 calling convention. Only what AMD's provider needs is implemented;
// anything else stops the process, with exit status PE_EXIT_FATAL and the function's name, rather than guessing.
#ifndef KYTY_TOOLS_FSR4FG_PEHOST_H_
#define KYTY_TOOLS_FSR4FG_PEHOST_H_
#include <stddef.h>
#include <stdint.h>

#define WINAPI_ABI __attribute__((ms_abi))
#define PE_EXIT_FATAL 6

typedef struct PeModule PeModule;

// Resolves imports of `dll` (lowercase, e.g. "vulkan-1.dll") that the host does not serve; NULL: unresolved.
typedef void* (*PeImportResolver)(const char* dll, const char* name);

// Optional synthetic Windows GDI adapter queries (D3DKMTEnumAdapters / D3DKMTQueryAdapterInfo), which AMD's
// providers ask through LoadLibraryA("gdi32.dll") + GetProcAddress.
typedef struct PeGdiAdapter {
	uint32_t asic_family, asic_revision;
} PeGdiAdapter;

void      PeSetImportResolver(PeImportResolver resolver);
void      PeSetGdiAdapter(const PeGdiAdapter* adapter);
// Before PeLoad: the image is never mapped below `address`, so it is relocated when its preferred base is lower. A
// process that keeps low addresses for itself (the emulator's guest) needs this: the mapping could otherwise succeed
// before that range is taken.
void      PeSetMinimumAddress(uint64_t address);
PeModule* PeLoad(const char* path);
// Process attach: TLS callbacks then DllMain. Must run on a thread prepared by PeRunOnWindowsThread.
int   PeAttach(PeModule* module);
void* PeExport(PeModule* module, const char* name);
// The module's mapped image (for crash reports).
void PeImageRange(const PeModule* module, uintptr_t* base, size_t* size);
// Runs fn(arg) on a new thread with a Windows TEB, TLS and a 64 MiB stack; returns fn's result.
int PeRunOnWindowsThread(int (*fn)(void*), void* arg);

#endif
