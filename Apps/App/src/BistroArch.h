#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"

namespace BistroArch
{
    // Exterior building_02 tunnel: 11 m between portals, crown at 688 cm, paving at 23 cm.
    inline constexpr float YawDegrees = 69;
    inline constexpr float HalfWidthCm = 200;
    inline constexpr float HalfLengthCm = 530;
    inline const Vector3 GrassPosition(1399.6f, 23.3f, 1369.0f);
    // Leave the lower 80 cm clear; the top is 75% of the opening height above the paving.
    inline const BoundingBox SwarmBounds({1399.6f, 300, 1369}, {HalfWidthCm, 220, HalfLengthCm});
}
