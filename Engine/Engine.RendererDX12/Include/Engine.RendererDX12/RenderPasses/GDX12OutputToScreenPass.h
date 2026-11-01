#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
    GDX12OutputToScreenPass() : IN_Texture(nullptr), IN_BackBuffer(nullptr)
    {
        _flags = RENDER_PASS_FLAG_NONE;
        _numInputs = 2;
        _numOutputs = 0;
    }

