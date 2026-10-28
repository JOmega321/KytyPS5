#pragma once
#include "local/frame-gen.h"
namespace Libs::Graphics { struct GraphicContext;
bool Fsr4Upscale(GraphicContext&, vk::CommandBuffer, const FrameGen::UpscaleInputs&);
}
