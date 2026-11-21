#include "Engine.RendererDX12/GDX12DeviceResources.h"

#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include "Common/GameTimer.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"

static UINT _numFrameConstants = 3;

static AutoConsoleVariableRef NumFrameConstantVariable(
    L"Render.NumFrames",
    _numFrameConstants,
    L"How many deferred frames was rendered");

void GDX12DeviceResources::Initialize(GDX12Device* device)
{
    Device = device;
    InputLayouts["Default"] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    RTVHeap = std::make_unique<GDX12DescriptorHeap>(device,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(device,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

    DSVHeap = std::make_unique<GDX12DescriptorHeap>(device,
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    GeometryBuffer = std::make_unique<GDX12GeometryBuffer>(device);

    for (int i = 0; i < NumFrameConstantVariable.GetValue(); i++)
    {
        FrameConstants.push_back(std::make_unique<GDX12FrameConstants>(device));

        FrameConstants[i]->ShadowViews->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->LightsCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->ClusterLightMaskBuffer->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->MaterialCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->TransformCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->InstanceCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }

    IndirectCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(device, 0, EBufferType::Upload, false);
    IndirectCommandsCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
}

void GDX12DeviceResources::SetMeshDrawCommands(const std::vector<UINT>& instances, const GPUMesh* mesh)
{
    if (instances.empty() || !IndirectCommandsCache || !IndirectCommandsCache->GetElementCount()) return;
    GDX12CommandQueue::CPUwait(Device->GetCommandQueue()->GetCompletionPoint());
    for (size_t i = 0; i < instances.size(); ++i)
    {
        GDX12IndirectDrawArgs command{};
        if (mesh)
        {
            const auto& subMesh = mesh->SubMeshes[i];
            command.InstanceID = instances[i];
            command.IndexCountPerInstance = subMesh.IndexCount;
            command.InstanceCount = 1;
            command.StartIndexLocation = subMesh.StartIndexLocation;
            command.BaseVertexLocation = subMesh.StartVertexLocation;
            command.StartInstanceLocation = instances[i];
        }
        IndirectCommandsCache->CopyData(instances[i], command);
    }
}

void GDX12DeviceResources::UpdateMainCB(UINT width, UINT height, GameTimer* timer)
{
    auto& frameRes = FrameConstants[CurrFrameConstantsIndex];

    GDX12MainConstants mainConstants;

    mainConstants.RenderTargetSize = { static_cast<float>(width), static_cast<float>(height) };
    mainConstants.TotalTime = timer->TotalTime();
    mainConstants.DeltaTime = timer->DeltaTime();

    frameRes->MainCB->CopyData(0, mainConstants);
}

void GDX12DeviceResources::UpdateMaterialCB(const GDX12Material& material)
{
    GDX12MaterialConstants materialConstants;
    materialConstants.Roughness = material.Roughness;
    materialConstants.Metallic = material.Metallic;
    materialConstants.Opacity = material.Opacity;
    materialConstants.RenderLayer = UINT(material.Type);

    const auto textureIndex = [this](const GPUTexture* texture, UINT fallback = UINT_MAX)
    {
        if (!texture) { return fallback; }
        auto* local = Device->Role == DEVICE_ROLE_PRIMARY ? texture->PrimaryDeviceTexture : texture->SecondaryDeviceTexture;
        return local ? local->GetSRV()->HeapIndex - Texture2D_StartIndex : fallback;
    };
    materialConstants.DiffuseMapIndex = textureIndex(material.Diffuse, 0);
    materialConstants.DisplacementMapIndex = textureIndex(material.Displacement, 0);
    materialConstants.NormalMapIndex = textureIndex(material.HasNormalMap ? material.Normal : nullptr);
    materialConstants.RoughnessMapIndex = textureIndex(material.HasRoughnessMap ? material.RoughnessMap : nullptr);
    materialConstants.SpecularMapIndex = textureIndex(material.HasSpecularMap ? material.Specular : nullptr);
    materialConstants.EmissiveMapIndex = textureIndex(material.HasEmissiveMap ? material.Emissive : nullptr);
    materialConstants.SpecularColor = material.SpecularColor;
    materialConstants.EmissiveColor = material.EmissiveColor;
    materialConstants.UseBakedLighting = material.UseBakedLighting;
    FrameConstants[CurrFrameConstantsIndex]->MaterialCache->CopyData(material._CBufferIndex, materialConstants);
}
