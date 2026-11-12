#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <cmath>
#include <stdexcept>

static AutoConsoleVariable<bool> Enabled(L"Render.Clouds.Enabled", true,
    L"Allow explicitly selected cloud sources. Default true.", EConsoleVariableFlags::Default);
static AutoConsoleVariable<GDX12LegacyQuality> Quality(L"Render.Clouds.Quality", GDX12LegacyQuality::Mid,
    L"0 Off, 1 Low (3 octaves), 2 Mid (5), 3 High (8).", EConsoleVariableFlags::Default);

GDX12CloudComponent::GDX12CloudComponent()
{
    _numOutputs = 1;
    _texture = std::make_unique<GDX12Texture>();
    _outputs.push_back(_texture.get());
}

void GDX12CloudComponent::Setup(GDX12DeviceResources* resources,
    RenderPipelineCommonData* common)
{
    if (_texture && _texture->IsInitialized() && resources != _resources)
        throw std::invalid_argument("A cloud component cannot migrate between device resources");
    GDX12RenderPass::Setup(resources, common);
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

UINT GDX12CloudComponent::GetQuality() const { return static_cast<UINT>(Quality.GetInt()); }

bool GDX12CloudComponent::IsActive() const
{
    return _parameters.Enabled && Enabled.GetValue() && Quality.GetInt() != 0;
}

void GDX12CloudComponent::Advance(const double simulationDeltaSeconds)
{
    if (!std::isfinite(simulationDeltaSeconds) || simulationDeltaSeconds < 0 || simulationDeltaSeconds > 3600)
        throw std::invalid_argument("Cloud delta must be finite and within [0, 3600] seconds");
    if (!IsActive() || simulationDeltaSeconds == 0) return;
    _simulationSeconds += simulationDeltaSeconds;
    if (_parameters.Speed.x == 0 && _parameters.Speed.y == 0) return;
    // Hash is periodic in simplex lattice space, so wrapping does not jump the noise field.
    _phaseX = std::fmod(_phaseX + simulationDeltaSeconds * _parameters.Speed.x, 256.0);
    _phaseY = std::fmod(_phaseY + simulationDeltaSeconds * _parameters.Speed.y, 256.0);
    ++_lightingRevision;
}

void GDX12CloudComponent::Reset()
{
    _phaseX = _phaseY = 0;
    _simulationSeconds = 0;
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

GDX12CloudInputs GDX12CloudComponent::CaptureInputs()
{
    const int quality = Quality.GetInt();
    const bool enabled = _parameters.Enabled && Enabled.GetValue() && quality != 0;
    if (_quality != quality || _enabled != enabled)
    {
        _quality = quality;
        _enabled = enabled;
        ++_sourceRevision;
        ++_lightingRevision;
    }
    const UINT presets[] = { 0, 3, 5, 8 };
    return { _parameters, Vector2(static_cast<float>(_phaseX), static_cast<float>(_phaseY)),
        _simulationSeconds, _sourceRevision, _lightingRevision, static_cast<UINT>(quality),
        _parameters.Octaves ? _parameters.Octaves : presets[quality], enabled };
}

void GDX12CloudComponent::Execute(GDX12CommandList* commands)
{
    const auto input = CaptureInputs();
    if (_renderedRevision == input.LightingRevision) return;
    Record(commands, input);
    _renderedRevision = input.LightingRevision;
}

void GDX12CloudComponent::Record(GDX12CommandList* commands, const GDX12CloudInputs& input)
{
    if (!_resources || !_texture) throw std::logic_error("Cloud source requires Setup before recording");
    _renderedRevision = 0;
    if (!_texture->IsInitialized()) CreateResources();
    struct Constants
    {
        UINT Size, Octaves, Implementation, Enabled;
        float Scale, Coverage, Softness, Padding;
        Vector3 Horizon; float PhaseX;
        Vector3 Zenith; float PhaseY;
        Vector3 Cloud; UINT Seed;
    };
    static_assert(sizeof(Constants) == 20 * sizeof(UINT));
    const auto& parameters = input.Parameters;
    const Constants constants = { 256, input.Octaves, static_cast<UINT>(parameters.Implementation),
        input.Enabled ? 1u : 0u, parameters.Scale, parameters.Coverage, parameters.Softness, 0,
        parameters.HorizonColor, input.Phase.x, parameters.ZenithColor, input.Phase.y,
        parameters.CloudColor, parameters.Seed };
    GDX12_SCOPED_EVENT(commands, "Procedural 2D clouds", Colors::CornflowerBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_root.get());
    commands->SetPipelineState(_pipeline);
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 20, &constants, 0);
    commands->SetComputeUAV(0, _texture->GetUAV()->GPUHandle);
    commands->ResourceBarrier({ _texture->GetResource()->GetUnorderedAccessBarrier() });
    commands->Dispatch(32, 32, 6);
    commands->ResourceBarrier({ commands->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ?
        _texture->GetResource()->GetNonPixelShaderResourceBarrier() : _texture->GetResource()->GetSRVBarrier() });
}

void GDX12CloudComponent::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _pipeline.Reset();
    _root.reset();
    _texture.reset();
    _renderedRevision = 0;
}
