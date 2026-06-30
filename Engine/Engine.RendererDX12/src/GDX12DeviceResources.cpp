#include "Engine.RendererDX12/GDX12DeviceResources.h"

#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include "Common/GameTimer.h"

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

        FrameConstants[i]->MaterialCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->TransformCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->InstanceCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }

    IndirectCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(device, 0, EBufferType::Upload, false);
    IndirectCommandsCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
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

void GDX12DeviceResources::UpdateMaterialCB(std::unordered_map<std::string, std::unique_ptr<GDX12Material>>& materials)
{
    auto currMaterialCB = FrameConstants[CurrFrameConstantsIndex]->MaterialCache.get();
    for (auto& i : materials)
    {
        GDX12Material* material = i.second.get();

        if (material->DirtyFlag)
        {
            material->DirtyFlag = false;
            material->_numFramesDirty = _numFrameConstants;
        }

        if (material->_numFramesDirty > 0)
        {
            GDX12MaterialConstants materialConstants;
            materialConstants.Roughness = material->Roughness;
            materialConstants.Metallic = material->Metallic;
            materialConstants.Opacity = material->Opacity;
            materialConstants.RenderLayer = UINT(material->Type);

            if (Device->Role == DEVICE_ROLE_PRIMARY)
            {
                if (material->Diffuse) { materialConstants.DiffuseIndex = material->Diffuse->PrimaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                if (material->Normal) { materialConstants.NormalIndex = material->Normal->PrimaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                if (material->Displacement) { materialConstants.DisplacementIndex = material->Displacement->PrimaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                currMaterialCB->CopyData(material->_CBufferIndex, materialConstants);
            }
            else
            {
                if (material->Diffuse) { materialConstants.DiffuseIndex = material->Diffuse->SecondaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                if (material->Normal) { materialConstants.NormalIndex = material->Normal->SecondaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                if (material->Displacement) { materialConstants.DisplacementIndex = material->Displacement->SecondaryDeviceTexture->GetSRV()->HeapIndex - Texture2D_StartIndex; }
                currMaterialCB->CopyData(material->_CBufferIndex, materialConstants);
            }
            material->_numFramesDirty--;
        }
    }
}
