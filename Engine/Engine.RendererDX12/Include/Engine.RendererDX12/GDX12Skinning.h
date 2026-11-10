#pragma once

#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include <tuple>

struct GDX12SkinPalette
{
    DirectX::SimpleMath::Matrix Current;
    DirectX::SimpleMath::Matrix Previous;
};

struct GDX12SkinSource
{
    std::unique_ptr<GDX12UploadBuffer<Vertex>> Vertices;
    std::unique_ptr<GDX12UploadBuffer<SkinVertexRange>> Ranges;
    std::unique_ptr<GDX12UploadBuffer<SkinInfluence>> Influences;
};

struct GDX12SkinMesh
{
    std::shared_ptr<const GDX12SkinSource> Source;
    std::vector<std::unique_ptr<GDX12UploadBuffer<GDX12SkinPalette>>> Palettes;
    std::vector<std::unique_ptr<GDX12UploadBuffer<GDX12SkinPalette>>> PaletteUploads;
    UINT OutputVertex = 0;
    UINT VertexCount = 0;
};

bool GDX12SkeletalInstancingEnabled();

class GDX12Skinning
{
public:
    explicit GDX12Skinning(GDX12Device* device);
    std::unique_ptr<GDX12SkinMesh> CreateMesh(const SubMesh& source, UINT frameCount, MeshHandle handle = {}, std::uint64_t revision = 0);
    void Dispatch(GDX12CommandList* commands, GDX12GeometryBuffer& geometry,
        GDX12SkinMesh& mesh, UINT frameSlot, const std::vector<GDX12SkinPalette>& palette);

    static std::vector<SkinVertexRange> ValidateSource(const SubMesh& source);
    void RecordDispatch(GDX12CommandList& commands, UINT vertexCount, UINT outputVertex, UINT boneCount,
        D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_GPU_VIRTUAL_ADDRESS ranges, D3D12_GPU_VIRTUAL_ADDRESS influences,
        D3D12_GPU_VIRTUAL_ADDRESS palette, D3D12_GPU_VIRTUAL_ADDRESS output) const;

private:
    std::map<std::tuple<std::uintptr_t, UINT, UINT, std::uint64_t>, std::weak_ptr<const GDX12SkinSource>> _sources;
    GDX12Device* _device;
    ComPtr<ID3D12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
};
