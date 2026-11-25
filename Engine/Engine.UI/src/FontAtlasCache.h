#pragma once

#include <memory>

struct ImDrawData;
struct ImTextureData;

namespace Engine::UI
{
    struct AtlasSnapshot;
    class DrawData;

    // Each ImGui context owns its font atlas and updates it independently.
    struct FontAtlasCache
    {
        const ImTextureData* Texture = nullptr;
        int UniqueId = 0;
        int Frame = -1;
        std::shared_ptr<const AtlasSnapshot> Snapshot;
        void BeginFrame();
    };

    std::shared_ptr<const DrawData> CaptureDrawData(const ImDrawData& source, FontAtlasCache& cache);
}
