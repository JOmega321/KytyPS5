#include "local/fsr4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "local/fsr411/fsr411.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace Libs::Graphics {
namespace {
std::atomic<int64_t> last_fsr4 {0};
std::atomic<bool> fsr4_fallback {false};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
const char* Fsr4Watermark() {
    const char* enabled = std::getenv("KYTY_FSR4_WATERMARK");
    const char* selected = std::getenv("KYTY_UPSCALER");
    if (!enabled || std::string_view(enabled) != "1" || !selected || std::string_view(selected) != "fsr4") return "";
    if (fsr4_fallback.load()) return "FSR 3.1 fallback (FSR 4.1.2 unavailable)";
    if (last_fsr4.load() && NowMs() - last_fsr4.load() < 2000)
        return "FSR 4.1.2 INT8 | PS5 Pro path | experimental";
    return "FSR 4.1.2 INT8 requested | awaiting upscale";
}

#if defined(__linux__)
bool Fsr4Upscale(GraphicContext& graphics, vk::CommandBuffer command, const FrameGen::UpscaleInputs& in) {
    // Kept until process exit, like the FSR3 contexts. No teardown while GPU commands reference it.
    struct State { std::mutex mutex; std::unique_ptr<Fsr411::Upscaler> upscaler; std::string error; bool first = true; };
    static auto* state = new State;
    std::lock_guard lock(state->mutex);
    const auto fail = [&](const std::string& reason) {
        fsr4_fallback.store(true);
        if (state->error != reason) {
            std::printf("FSR 4.1.2 unavailable: %s; falling back to FSR 3\n", reason.c_str());
            state->error = reason;
        }
        return false;
    };
    if (!graphics.fsr412_enabled) return fail("required Vulkan features were not enabled");
    for (const auto* image : {&in.color, &in.depth, &in.motion, &in.output})
        if (!image->view || image->base_level || image->base_layer)
            return fail("whole images with valid views are required");
    if (in.color.format != vk::Format::eR16G16B16A16Sfloat ||
        in.output.format != vk::Format::eR16G16B16A16Sfloat || in.motion.format != vk::Format::eR16G16Sfloat)
        return fail("expected RGBA16F scene/output and RG16F motion vectors");
    if (!state->upscaler) {
        const char* dir = std::getenv("KYTY_FSR412_DIR");
        const std::string folder = dir && *dir ? dir : "fsr4_412";
        std::ifstream marker(folder + "/ASSET_VERSION");
        std::string version;
        std::getline(marker, version);
        if (version != "FSR412_VULKAN_INT8_4.1.2.4555")
            return fail("native Vulkan 4.1.2 assets are missing or from another version; use FSR4 setup");
        state->upscaler = std::make_unique<Fsr411::Upscaler>(graphics.physical_device, graphics.device,
                                                          folder);
    }
    const auto image = [](const FrameGen::UpscaleImage& i) {
        return Fsr411::Image{static_cast<VkImage>(i.image), static_cast<VkImageView>(i.view), i.extent.width, i.extent.height};
    };
    Fsr411::Frame frame{};
    frame.cmdbuf = command;
    frame.color = image(in.color); frame.depth = image(in.depth);
    frame.motion = image(in.motion); frame.output = image(in.output);
    frame.render_width = in.color.extent.width; frame.render_height = in.color.extent.height;
    frame.ultra_performance = in.color.extent.width * 5 < in.output.extent.width * 2;
    frame.jitter[0] = in.jitter_x; frame.jitter[1] = in.jitter_y;
    frame.motion_scale[0] = in.motion_scale_x; frame.motion_scale[1] = in.motion_scale_y;
    frame.reset = in.reset || state->first;
    if (const char* sharp = std::getenv("KYTY_FSR_SHARPNESS"))
        frame.sharpness = std::clamp(std::strtof(sharp, nullptr), 0.0f, 1.0f);
    frame.sharpen = frame.sharpness > 0;
    // Restore the caller's declared layouts, including on an asset-load failure.
    const auto barrier = [&](bool restore) {
        std::array<vk::ImageMemoryBarrier,4> barriers{};
        const FrameGen::UpscaleImage* images[] = {&in.color, &in.depth, &in.motion, &in.output};
        for (size_t n = 0; n < 4; ++n) {
            auto& b = barriers[n]; const auto& i = *images[n];
            b.image = i.image; b.oldLayout = restore ? vk::ImageLayout::eGeneral : i.layout;
            b.newLayout = restore ? i.layout : vk::ImageLayout::eGeneral;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            b.dstAccessMask = b.srcAccessMask;
            const auto format = i.image_format == vk::Format::eUndefined ? i.format : i.image_format;
            const bool depth = format == vk::Format::eD32Sfloat || format == vk::Format::eD32SfloatS8Uint;
            b.subresourceRange = {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            if (format == vk::Format::eD32SfloatS8Uint) b.subresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
        }
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                                {}, {}, {}, barriers);
    };
    barrier(false);
    const bool ok = state->upscaler->Record(frame);
    barrier(true);
    if (!ok) return fail(state->upscaler->Error());
    if (state->first || !state->error.empty())
        std::printf("Super resolution: FSR 4.1.2 experimental %s\n", state->upscaler->Describe().c_str());
    last_fsr4.store(NowMs()); fsr4_fallback.store(false);
    state->first = false; state->error.clear();
    return true;
}
#else
bool Fsr4Upscale(GraphicContext&, vk::CommandBuffer, const FrameGen::UpscaleInputs&) { return false; }
#endif
}
