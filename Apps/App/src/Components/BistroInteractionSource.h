#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"

// Visual proxy published after character movement; never a mesh or a physics collider.
struct BistroInteractionSource
{
    XMFLOAT4 PositionRadius = {0, 0, 0, 65};
    XMFLOAT4 PreviousHeight = {0, 0, 0, 190};
};
static_assert(sizeof(BistroInteractionSource) == 32);
