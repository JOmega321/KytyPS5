// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_TOOLS_FSR4FG_FAKEVK_H_
#define KYTY_TOOLS_FSR4FG_FAKEVK_H_
#include <stdint.h>

typedef struct FakeVkProfile {
	uint32_t vendor_id, device_id, driver_id;
	int      fp8;
} FakeVkProfile;

void FakeVkSetProfile(const FakeVkProfile* profile);
void FakeVkFrame(uint32_t index);
void FakeVkNote(const char* json_members); // e.g. "\"stage\":\"configure\",\"result\":0"
void FakeVkFlush(void);
// The synthetic device's functions (Windows x64 calling convention), by Vulkan name; NULL when not implemented.
void* FakeVkLookup(const char* name);
__attribute__((ms_abi)) void* FakeVkGetDeviceProcAddr(void* device, const char* name);

#endif
