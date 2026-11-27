#pragma once

#include "Engine.RendererDX12/GDX12UploadBuffer.h"

class GDX12RayTracingPipelineState;

struct GDX12ShaderTableDesc
{
    std::wstring RayGeneration;
    std::vector<std::wstring> Miss;
    std::vector<std::wstring> HitGroups;
};

// Immutable shader records in one upload buffer, no per-frame updates or copies.
class GDX12ShaderTable
{
public:
    GDX12ShaderTable(const GDX12RayTracingPipelineState& pipeline, const GDX12ShaderTableDesc& description);

    // Use with the pipeline that created this table, starts align to 64 bytes and strides to 32.
    D3D12_DISPATCH_RAYS_DESC GetDispatchDescription(UINT width, UINT height = 1, UINT depth = 1) const;
    GDX12Resource& GetResource() const { return _buffer->GetResource(); }
    GDX12Device* GetDevice() const { return _device; }

private:
    GDX12Device* _device;
    std::unique_ptr<GDX12UploadBuffer<BYTE>> _buffer;
    D3D12_DISPATCH_RAYS_DESC _description{};
};
