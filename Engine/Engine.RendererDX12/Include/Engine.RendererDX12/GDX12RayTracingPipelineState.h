#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12RootSignature;

struct GDX12RayTracingPipelineStateDesc
{
    D3D12_SHADER_BYTECODE Library{};
    // An empty list exports every shader in the library.
    std::vector<std::wstring> Exports;
    std::vector<D3D12_HIT_GROUP_DESC> HitGroups;
    GDX12RootSignature* GlobalRootSignature = nullptr;
    UINT MaxPayloadSize = 0;
    UINT MaxAttributeSize = sizeof(float) * 2;
    UINT MaxRecursionDepth = 1;
};

// One device-local DXR pipeline, shader tables use identifiers from this state object.
class GDX12RayTracingPipelineState
{
public:
    GDX12RayTracingPipelineState(GDX12Device* device, const GDX12RayTracingPipelineStateDesc& description);

    const ComPtr<ID3D12StateObject>& GetStateObject() const { return _stateObject; }
    GDX12Device* GetDevice() const { return _device; }

    // Returns the 32-byte identifier, valid while this pipeline exists.
    const void* GetShaderIdentifier(const std::wstring& name) const;

private:
    GDX12Device* _device;
    ComPtr<ID3D12StateObject> _stateObject;
    ComPtr<ID3D12StateObjectProperties> _properties;
};
