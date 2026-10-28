#pragma once
#include "local/frame-gen.h"
namespace Libs::Graphics { struct GraphicContext;
const char* Fsr4Watermark();
bool Fsr4Upscale(GraphicContext&, vk::CommandBuffer, const FrameGen::UpscaleInputs&);
}
