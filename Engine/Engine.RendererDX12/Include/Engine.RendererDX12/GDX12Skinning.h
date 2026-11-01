#pragma once

#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"

struct GDX12SkinPalette
{
    DirectX::SimpleMath::Matrix Current;
    DirectX::SimpleMath::Matrix Previous;
};

// One attachment owns its source buffers and one palette upload per queued frame slot.
struct GDX12SkinMesh
{
    std::unique_ptr<GDX12UploadBuffer<Vertex>> Vertices;
    std::unique_ptr<GDX12UploadBuffer<SkinVertexRange>> Ranges;
    std::unique_ptr<GDX12UploadBuffer<SkinInfluence>> Influences;
    std::vector<std::unique_ptr<GDX12UploadBuffer<GDX12SkinPalette>>> Palettes;
    UINT OutputVertex = 0;
    UINT VertexCount = 0;
};

class GDX12Skinning
{
public:
    explicit GDX12Skinning(GDX12Device* device);
    std::unique_ptr<GDX12SkinMesh> CreateMesh(const SubMesh& source, UINT frameCount);
    void Dispatch(GDX12CommandList* commands, GDX12GeometryBuffer& geometry,
        GDX12SkinMesh& mesh, UINT frameSlot, const std::vector<GDX12SkinPalette>& palette);

private:
    GDX12Device* _device;
    ComPtr<ID3D12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
};
