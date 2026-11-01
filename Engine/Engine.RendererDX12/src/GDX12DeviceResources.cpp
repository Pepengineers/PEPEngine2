#include "Engine.RendererDX12/GDX12DeviceResources.h"

#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include "Common/GameTimer.h"

static AutoConsoleVariable<UINT> NumFrameConstantVariable(
    L"Render.NumFrames",
    3,
    L"Number of buffered frames. Positive integer; set before GPU initialization.",
    EConsoleVariableFlags::ReadOnly,
    [](UINT value) { return value > 0 && value <= static_cast<UINT>((std::numeric_limits<int>::max)()); });

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
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4096,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    D3D12_SHADER_RESOURCE_VIEW_DESC reflectionView = {};
    reflectionView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    reflectionView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    reflectionView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    reflectionView.TextureCubeArray.NumCubes = 1;
    reflectionView.TextureCubeArray.MipLevels = 1;
    ReflectionRadianceSRV.InitAsSRV(nullptr, &reflectionView, SRV_UAV_Heap.get(),
        SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));

    D3D12_SHADER_RESOURCE_VIEW_DESC skyView = {};
    skyView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    skyView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    skyView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    skyView.Texture2D.MipLevels = 1;
    SkyTransmittanceSRV.InitAsSRV(nullptr, &skyView, SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    SkyViewSRV.InitAsSRV(nullptr, &skyView, SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    skyView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    skyView.Texture3D.MipLevels = 1;
    SkyAerialRadianceSRV.InitAsSRV(nullptr, &skyView, SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    SkyAerialTransmittanceSRV.InitAsSRV(nullptr, &skyView, SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));

    GeometryBuffer = std::make_unique<GDX12GeometryBuffer>(device);

    for (int i = 0; i < NumFrameConstantVariable.GetValue(); i++)
    {
        FrameConstants.push_back(std::make_unique<GDX12FrameConstants>(device));

        FrameConstants[i]->MaterialCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->TransformCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->InstanceCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->ReflectionProbes->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        FrameConstants[i]->LocalLightCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }

    IndirectCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(device, 0, EBufferType::Upload, false);
    IndirectCommandsCache->CreateSRV(SRV_UAV_Heap.get(), SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
}

void GDX12DeviceResources::UpdateMainCB(UINT width, UINT height, GameTimer* timer)
{
    auto& frameRes = FrameConstants[CurrFrameConstantsIndex];

    GDX12MainConstants mainConstants = Lighting;

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
            ++material->_revision;
            material->_numPrimaryFramesDirty = NumFrameConstantVariable.GetValue();
            material->_numSecondaryFramesDirty = NumFrameConstantVariable.GetValue();
        }

        auto& numFramesDirty = Device->Role == DEVICE_ROLE_PRIMARY
            ? material->_numPrimaryFramesDirty
            : material->_numSecondaryFramesDirty;

        if (numFramesDirty > 0)
        {
            GDX12MaterialConstants materialConstants;
            materialConstants.Roughness = material->Roughness;
            materialConstants.Metallic = material->Metallic;
            materialConstants.Opacity = material->Opacity;
            materialConstants.RenderLayer = UINT(material->Type);
            materialConstants.SpecularColor = material->SpecularColor;
            materialConstants.EmissiveColor = material->EmissiveColor;
            materialConstants.Flags = (material->HasNormalMap ? 1u : 0u) |
                (material->HasSpecularMap ? 2u : 0u) | (material->HasRoughnessMap ? 4u : 0u) |
                (material->HasEmissiveMap ? 8u : 0u) | (material->UseBakedLighting ? 16u : 0u);

            const auto textureIndex = [this](const GPUTexture* texture)
            {
                if (!texture) return 0u;
                auto* gpu = Device->Role == DEVICE_ROLE_PRIMARY ? texture->PrimaryDeviceTexture : texture->SecondaryDeviceTexture;
                return gpu ? gpu->GetSRV()->HeapIndex - Texture2D_StartIndex : 0u;
            };
            materialConstants.DiffuseIndex = textureIndex(material->Diffuse);
            materialConstants.NormalIndex = textureIndex(material->Normal);
            materialConstants.DisplacementIndex = textureIndex(material->Displacement);
            materialConstants.SpecularIndex = textureIndex(material->Specular);
            materialConstants.RoughnessIndex = textureIndex(material->RoughnessMap);
            materialConstants.EmissiveIndex = textureIndex(material->Emissive);
            currMaterialCB->CopyData(material->_CBufferIndex, materialConstants);
            numFramesDirty--;
        }
    }
}
