#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12RootSignature;

struct GDX12RayTracingHitGroupDesc
{
    std::wstring Name;
    D3D12_HIT_GROUP_TYPE Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    std::wstring ClosestHitFunctionName;
    std::wstring AnyHitFunctionName;
    std::wstring IntersectionFunctionName;
};

struct GDX12RayTracingPipelineStateDesc
{
    D3D12_SHADER_BYTECODE Library{};
    // An empty list makes all shader functions in the library available.
    std::vector<std::wstring> ShaderFunctionsNames;
    std::vector<GDX12RayTracingHitGroupDesc> HitGroups;
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
