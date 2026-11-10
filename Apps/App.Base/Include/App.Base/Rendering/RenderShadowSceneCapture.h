#pragma once

#include "App.Base/Rendering/CrossAdapterShadowScene.h"

struct GPUTexture;

struct CapturedShadowScene
{
    struct PrimaryDraw
    {
        UINT Instance = UINT_MAX;
        UINT VertexStart = 0, VertexCount = 0;
        UINT IndexStart = 0, IndexCount = 0;
        const GPUTexture* Mask = nullptr;
    };

    CrossAdapterSceneReplica::Snapshot Frame;
    // Indexed by the same caster indices as Frame.Casters.
    std::vector<PrimaryDraw> Primary;
    ShadowSceneSources Sources;
    std::vector<UINT> PrimaryMaterialSlots;
};
