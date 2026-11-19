#pragma once

#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"

#include <array>

// One instance per device for shader exports and imports, prepared when the graph is built.
class ComputeCopyResources
{
public:
    static bool SupportsFormat(GDX12Device* device, DXGI_FORMAT format);
    void Prepare(GDX12Device* device, DXGI_FORMAT format);
    FrameGraphCopyStatistics Dispatch(GDX12CommandList* commands, FrameGraphPhysicalResource& source,
        FrameGraphPhysicalResource& destination) const;

private:
    enum TexelType { Float4, Uint4, Int4, Count };
    static TexelType GetTexelType(DXGI_FORMAT format);

    std::unique_ptr<GDX12RootSignature> _root;
    std::array<ComPtr<ID3D12PipelineState>, TexelType::Count> _pipelines{};
};
