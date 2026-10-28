// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vulkan/vulkan.h>

namespace Libs::Graphics {
// The native Vulkan postpass and RCAS use formatless float storage writes.
// Demon's Souls supplies packed HDR RGB output; internal history stays RGBA16F.
constexpr bool Fsr4OutputFormatSupported(VkFormat format) {
    return format == VK_FORMAT_R16G16B16A16_SFLOAT ||
           format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}
} // namespace Libs::Graphics
