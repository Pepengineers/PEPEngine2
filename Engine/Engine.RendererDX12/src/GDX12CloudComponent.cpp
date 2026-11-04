#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <stdexcept>

static AutoConsoleVariable<bool> Enabled(L"Render.Clouds.Enabled", true,
    L"Allow explicitly selected cloud sources. Default true.", EConsoleVariableFlags::Default);
static AutoConsoleVariable<int> Quality(L"Render.Clouds.Quality", 2,
    L"0 Off, 1 Low (3 octaves), 2 Mid (5), 3 High (8).", EConsoleVariableFlags::Default,
    [](const int value) { return value >= 0 && value <= 3; });

GDX12CloudComponent::GDX12CloudComponent()
{
    _numOutputs = 1;
    _texture = std::make_unique<GDX12Texture>();
    _outputs.push_back(_texture.get());
}

void GDX12CloudComponent::Setup(GDX12DeviceResources* resources, GDX12DeviceResources* other,
    RenderPipelineCommonData* common)
{
    if (_texture && _texture->IsInitialized() && resources != _resources)
        throw std::invalid_argument("A cloud component cannot migrate between device resources");
    GDX12RenderPass::Setup(resources, other, common);
}

void GDX12CloudComponent::SetParameters(const GDX12CloudParameters& parameters)
{
    const auto colorValid = [](const Vector3 color)
    {
        return std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) &&
            color.x >= 0 && color.y >= 0 && color.z >= 0 &&
            color.x <= 64 && color.y <= 64 && color.z <= 64;
    };
    if (parameters.Implementation < 0 || parameters.Implementation > 1 || parameters.Octaves > 8 ||
        !std::isfinite(parameters.Scale) || parameters.Scale < 0.01f || parameters.Scale > 64 ||
        !std::isfinite(parameters.Coverage) || parameters.Coverage < 0 || parameters.Coverage > 1 ||
        !std::isfinite(parameters.Softness) || parameters.Softness < 0.001f || parameters.Softness > 1 ||
        !std::isfinite(parameters.Speed.x) || !std::isfinite(parameters.Speed.y) ||
        std::abs(parameters.Speed.x) > 10 || std::abs(parameters.Speed.y) > 10 ||
        !colorValid(parameters.HorizonColor) || !colorValid(parameters.ZenithColor) ||
        !colorValid(parameters.CloudColor))
        throw std::invalid_argument("Invalid cloud component parameters");
    _parameters = parameters;
    ++_sourceRevision;
    ++_lightingRevision;
}

UINT GDX12CloudComponent::GetQuality() const { return static_cast<UINT>(Quality.GetValue()); }

bool GDX12CloudComponent::IsActive() const
{
    return _parameters.Enabled && Enabled.GetValue() && Quality.GetValue() != 0;
}

void GDX12CloudComponent::Advance(const double simulationDeltaSeconds)
{
    if (!std::isfinite(simulationDeltaSeconds) || simulationDeltaSeconds < 0 || simulationDeltaSeconds > 3600)
        throw std::invalid_argument("Cloud delta must be finite and within [0, 3600] seconds");
    if (!IsActive() || simulationDeltaSeconds == 0 || (_parameters.Speed.x == 0 && _parameters.Speed.y == 0)) return;
    // Hash is periodic in simplex lattice space, so wrapping does not jump the noise field.
    _phaseX = std::fmod(_phaseX + simulationDeltaSeconds * _parameters.Speed.x, 256.0);
    _phaseY = std::fmod(_phaseY + simulationDeltaSeconds * _parameters.Speed.y, 256.0);
    ++_lightingRevision;
}

void GDX12CloudComponent::Reset()
{
    _phaseX = _phaseY = 0;
    ++_sourceRevision;
    ++_lightingRevision;
}

void GDX12CloudComponent::CreateResources()
{
    GDX12RootSignatureDesc root;
    root.Constants.push_back(20);
    root.NumSingleUAVSlots = 1;
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
        SHADERS_FOLDER "Clouds.hlsl", nullptr, "CloudsCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
    // shortcut: one source mip can alias extreme noise scales; add filtered mips if GPU inspection requires it.
    const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
        256, 256, 6, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    GDX12TextureDesc desc;
    desc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
        D3D12_RESOURCE_STATE_COMMON, nullptr);
    desc.Width = desc.Height = 256;
    desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Semantic = ETextureSemantic::Color;
    desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
    desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex, TextureCube_RangeLength);
    desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    desc.SRVDesc.TextureCube.MipLevels = 1;
    desc.CreateUAV = true;
    desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    desc.UAVDesc.Texture2DArray.ArraySize = 6;
    _texture->Initialize(desc);
}

void GDX12CloudComponent::Execute(GDX12CommandList* commands)
{
    if (!_resources || !_texture) throw std::logic_error("Cloud source requires Setup before Execute");
    const int quality = Quality.GetValue();
    const bool enabled = _parameters.Enabled && Enabled.GetValue() && quality != 0;
    if (_quality != quality || _enabled != enabled)
    {
        _quality = quality;
        _enabled = enabled;
        ++_sourceRevision;
        ++_lightingRevision;
    }
    if (_renderedRevision == _lightingRevision) return;
    if (!_texture->IsInitialized()) CreateResources();
    struct Constants
    {
        UINT Size, Octaves, Implementation, Enabled;
        float Scale, Coverage, Softness, Padding;
        Vector3 Horizon; float PhaseX;
        Vector3 Zenith; float PhaseY;
        Vector3 Cloud; float Padding2;
    };
    static_assert(sizeof(Constants) == 20 * sizeof(UINT));
    const UINT presets[] = { 0, 3, 5, 8 };
    Constants constants = { 256, _parameters.Octaves ? _parameters.Octaves : presets[quality],
        static_cast<UINT>(_parameters.Implementation), enabled ? 1u : 0u,
        _parameters.Scale, _parameters.Coverage, _parameters.Softness, 0,
        _parameters.HorizonColor, static_cast<float>(_phaseX), _parameters.ZenithColor,
        static_cast<float>(_phaseY), _parameters.CloudColor, 0 };
    GDX12_SCOPED_EVENT(commands, "Procedural 2D clouds", Colors::CornflowerBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_root.get());
    commands->SetPipelineState(_pipeline);
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 20, &constants, 0);
    commands->SetComputeUAV(0, _texture->GetUAV()->GPUHandle);
    commands->ResourceBarrier({ _texture->GetResource()->GetUnorderedAccessBarrier() });
    commands->Dispatch(32, 32, 6);
    commands->ResourceBarrier({ _texture->GetResource()->GetSRVBarrier() });
    _renderedRevision = _lightingRevision;
}

void GDX12CloudComponent::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _pipeline.Reset();
    _root.reset();
    _texture.reset();
    _renderedRevision = 0;
}
