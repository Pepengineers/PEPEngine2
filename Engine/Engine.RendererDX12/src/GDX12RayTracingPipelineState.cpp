#include "Engine.RendererDX12/GDX12RayTracingPipelineState.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include <stdexcept>

GDX12RayTracingPipelineState::GDX12RayTracingPipelineState(GDX12Device* device, const GDX12RayTracingPipelineStateDesc& description)
    : _device(device)
{
    if (!device || device->GetDeviceFeatures().Features.RaytracingTier() == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    { throw std::invalid_argument("ERROR: Ray tracing pipeline requires a device with DXR support."); }
    if (!description.Library.pShaderBytecode || !description.Library.BytecodeLength || !description.GlobalRootSignature)
    { throw std::invalid_argument("ERROR: Ray tracing pipeline requires a shader library and a global root signature."); }
    if (description.MaxRecursionDepth > D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH
        || description.MaxAttributeSize > D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES)
    { throw std::invalid_argument("ERROR: Ray tracing pipeline configuration exceeds DXR limits."); }
    if (description.GlobalRootSignature->GetDesc().Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE)
    { throw std::invalid_argument("ERROR: Ray tracing pipeline requires a global root signature."); }

    CD3DX12_STATE_OBJECT_DESC state(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    auto* library = state.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    library->SetDXILLibrary(&description.Library);
    for (const auto& name : description.ShaderFunctionsNames) { library->DefineExport(name.c_str()); }
    for (const auto& group : description.HitGroups)
    {
        auto* hitGroup = state.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
        hitGroup->SetHitGroupExport(group.Name.c_str());
        hitGroup->SetHitGroupType(group.Type);
        hitGroup->SetClosestHitShaderImport(group.ClosestHitFunctionName.empty() ? nullptr : group.ClosestHitFunctionName.c_str());
        hitGroup->SetAnyHitShaderImport(group.AnyHitFunctionName.empty() ? nullptr : group.AnyHitFunctionName.c_str());
        hitGroup->SetIntersectionShaderImport(group.IntersectionFunctionName.empty() ? nullptr : group.IntersectionFunctionName.c_str());
    }
    state.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>()->SetRootSignature(
        description.GlobalRootSignature->GetRootSignature().Get());
    state.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>()->Config(
        description.MaxPayloadSize, description.MaxAttributeSize);
    state.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>()->Config(description.MaxRecursionDepth);
    ThrowIfFailed(device->GetDevice()->CreateStateObject(state, IID_PPV_ARGS(&_stateObject)));
    ThrowIfFailed(_stateObject.As(&_properties));
}

const void* GDX12RayTracingPipelineState::GetShaderIdentifier(const std::wstring& name) const
{
    const auto* identifier = _properties->GetShaderIdentifier(name.c_str());
    if (!identifier) { throw std::invalid_argument("ERROR: Shader table entry is missing from the ray tracing pipeline."); }
    return identifier;
}
