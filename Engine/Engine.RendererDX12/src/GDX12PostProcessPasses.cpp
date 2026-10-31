#include "Engine.RendererDX12/RenderPasses/GDX12SpecularResolvePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12PostProcessAuxPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ToneMappingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionLightingPass.h"
#include <array>
#include <stdexcept>

namespace
{
    void CreateTexture(GDX12DeviceResources& resources, GDX12Texture& texture,
        UINT width, UINT height, DXGI_FORMAT format)
    {
        GDX12TextureDesc desc;
        desc.Width = width; desc.Height = height;
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = format;
        desc.CreateUAV = true;
        desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
            desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVHeapIndex = texture.IsInitialized() ? texture.GetUAV()->HeapIndex :
            desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        texture.Initialize(desc);
    }
    void ValidatePostProcessInputs(const std::vector<IRenderPassLink*>& inputs, size_t count)
    {
        if (inputs.size() != count) throw std::invalid_argument("Incorrect post-process input count");
        for (auto* input : inputs)
            if (!input || !input->IsInitialized()) throw std::invalid_argument("Uninitialized post-process input");
    }
    void Compile(GDX12Device* device, GDX12RootSignature& root, const wchar_t* file,
        ComPtr<ID3D12PipelineState>& output)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device, file, nullptr, "CS", "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root.GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output)));
    }
    void CopyColor(GDX12CommandList* commands, GDX12Texture& destination, GDX12Texture& source)
    {
        commands->ResourceBarrier({ source.GetResource()->GetCopySourceBarrier(), destination.GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(destination.GetResource()->D3DResource.Get(), source.GetResource()->D3DResource.Get());
        commands->ResourceBarrier({ source.GetResource()->GetSRVBarrier(), destination.GetResource()->GetSRVBarrier() });
    }
}

struct GDX12SpecularResolvePass::Data
{
    GDX12Texture Color;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
};
GDX12SpecularResolvePass::GDX12SpecularResolvePass(GDX12SssrPass* reflections)
    : _data(std::make_unique<Data>()), _reflections(reflections)
{
    if (!reflections) throw std::invalid_argument("Specular resolve requires its SSR producer");
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 8; _numOutputs = 1; _outputs.push_back(&_data->Color);
}
GDX12SpecularResolvePass::~GDX12SpecularResolvePass() = default;
void GDX12SpecularResolvePass::Initialize()
{
    ValidatePostProcessInputs(_inputs, 8);
    auto* color = _inputs[0]->GetTexture();
    if (color->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
        throw std::invalid_argument("Specular resolve requires linear HDR RGBA16F");
    for (UINT i : { 1u, 2u, 3u, 4u, 7u })
        if (_inputs[i]->GetTexture()->GetWidth() != color->GetWidth() || _inputs[i]->GetTexture()->GetHeight() != color->GetHeight())
            throw std::invalid_argument("Specular resolve surface grids must match");
    CreateTexture(*_resources, _data->Color, color->GetWidth(), color->GetHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    GDX12RootSignatureDesc root;
    const bool probes = _commonData->ReflectionLighting != nullptr;
    root.NumSingleCBVSlots = probes ? 2 : 1; root.NumSingleSRVSlots = probes ? 9 : 8; root.NumSingleUAVSlots = 1;
    if (probes) root.SRVRanges.emplace_back(TextureCube_RangeLength);
    root.StaticSamplers = GetStaticSamplers();
    _data->Root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    D3D_SHADER_MACRO macros[] = { { "PROBE_LIGHTING", "1" }, { nullptr, nullptr } };
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
        SHADERS_FOLDER "SpecularResolve.hlsl", probes ? macros : nullptr, "CS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _data->Root->GetRootSignature().Get(); pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_data->Pipeline)));
}
void GDX12SpecularResolvePass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    commands->BeginPixEvent("Specular reflection resolve", Colors::SteelBlue);
    if (!_reflections->HasValidHistory())
        CopyColor(commands, d.Color, *_inputs[0]->GetTexture());
    else
    {
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(d.Root.get()); commands->SetPipelineState(d.Pipeline);
        const auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        commands->SetComputeRootConstantBufferView(0, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
        for (UINT i = 0; i < 8; ++i)
        {
            auto* texture = _inputs[i]->GetTexture();
            commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
            commands->SetComputeSRV(i, texture->GetSRV()->GPUHandle);
        }
        if (_commonData->ReflectionLighting) _commonData->ReflectionLighting->BindCompute(commands, 1, 8, 9);
        commands->ResourceBarrier({ d.Color.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, d.Color.GetUAV()->GPUHandle);
        commands->Dispatch((d.Color.GetWidth() + 7) / 8, (d.Color.GetHeight() + 7) / 8, 1);
        commands->ResourceBarrier({ d.Color.GetResource()->GetSRVBarrier() });
    }
    commands->EndPixEvent();
}
void GDX12SpecularResolvePass::Resize()
{ _data->Color.Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight()); }
void GDX12SpecularResolvePass::ClearDenendencies()
{ GDX12RenderPass::ClearDenendencies(); _data.reset(); _reflections = nullptr; }

struct GDX12PostProcessAuxPass::Data
{
    std::array<GDX12Texture, 2> Textures;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
};
GDX12PostProcessAuxPass::GDX12PostProcessAuxPass() : _data(std::make_unique<Data>())
{
    _numInputs = 2; _numOutputs = 2;
    for (auto& texture : _data->Textures) _outputs.push_back(&texture);
}
GDX12PostProcessAuxPass::~GDX12PostProcessAuxPass() = default;
void GDX12PostProcessAuxPass::Initialize()
{
    ValidatePostProcessInputs(_inputs, 2);
    if (_inputs[0]->GetTexture()->GetWidth() != _inputs[1]->GetTexture()->GetWidth() ||
        _inputs[0]->GetTexture()->GetHeight() != _inputs[1]->GetTexture()->GetHeight())
        throw std::invalid_argument("Post-process depth and motion grids must match");
    CreateTexture(*_resources, _data->Textures[0], _commonData->WindowWidth, _commonData->WindowHeight, DXGI_FORMAT_R32_FLOAT);
    CreateTexture(*_resources, _data->Textures[1], _commonData->WindowWidth, _commonData->WindowHeight, DXGI_FORMAT_R16G16_FLOAT);
    GDX12RootSignatureDesc root;
    root.Constants.push_back(6); root.NumSingleSRVSlots = 2; root.NumSingleUAVSlots = 2;
    _data->Root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    Compile(_resources->Device, *_data->Root, SHADERS_FOLDER "PostProcessAux.hlsl", _data->Pipeline);
}
void GDX12PostProcessAuxPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    struct Parameters { XMUINT2 OutputSize, InputSize; XMFLOAT2 Jitter; } parameters = {
        { d.Textures[0].GetWidth(), d.Textures[0].GetHeight() },
        { _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight() },
        { _commonData->ActiveCameraJitterOffsetX, _commonData->ActiveCameraJitterOffsetY } };
    static_assert(sizeof(Parameters) == 24);
    commands->BeginPixEvent("Align post-process depth and motion", Colors::CadetBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(d.Root.get()); commands->SetPipelineState(d.Pipeline);
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 6, &parameters, 0);
    for (UINT i = 0; i < 2; ++i)
    {
        auto* input = _inputs[i]->GetTexture();
        commands->ResourceBarrier({ input->GetResource()->GetSRVBarrier(), d.Textures[i].GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(i, input->GetSRV()->GPUHandle);
        commands->SetComputeUAV(i, d.Textures[i].GetUAV()->GPUHandle);
    }
    commands->Dispatch((parameters.OutputSize.x + 7) / 8, (parameters.OutputSize.y + 7) / 8, 1);
    for (auto& texture : d.Textures) commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}
void GDX12PostProcessAuxPass::Resize()
{ for (auto& texture : _data->Textures) texture.Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
void GDX12PostProcessAuxPass::ClearDenendencies()
{ GDX12RenderPass::ClearDenendencies(); _data.reset(); }

struct GDX12ToneMappingPass::Data
{
    GDX12Texture Color;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
};
GDX12ToneMappingPass::GDX12ToneMappingPass() : _data(std::make_unique<Data>())
{ _numInputs = 1; _numOutputs = 1; _outputs.push_back(&_data->Color); }
GDX12ToneMappingPass::~GDX12ToneMappingPass() = default;
void GDX12ToneMappingPass::Initialize()
{
    ValidatePostProcessInputs(_inputs, 1);
    auto* input = _inputs[0]->GetTexture();
    if (input->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
        throw std::invalid_argument("Tone mapping requires linear HDR RGBA16F");
    CreateTexture(*_resources, _data->Color, input->GetWidth(), input->GetHeight(), DXGI_FORMAT_R8G8B8A8_UNORM);
    GDX12RootSignatureDesc root;
    root.NumSingleSRVSlots = 1; root.NumSingleUAVSlots = 1;
    _data->Root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    Compile(_resources->Device, *_data->Root, SHADERS_FOLDER "ToneMapping.hlsl", _data->Pipeline);
}
void GDX12ToneMappingPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    auto* input = _inputs[0]->GetTexture();
    commands->BeginPixEvent("Tone map to display", Colors::Goldenrod);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(d.Root.get()); commands->SetPipelineState(d.Pipeline);
    commands->ResourceBarrier({ input->GetResource()->GetSRVBarrier(), d.Color.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(0, input->GetSRV()->GPUHandle); commands->SetComputeUAV(0, d.Color.GetUAV()->GPUHandle);
    commands->Dispatch((d.Color.GetWidth() + 7) / 8, (d.Color.GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ d.Color.GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}
void GDX12ToneMappingPass::Resize()
{ _data->Color.Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight()); }
void GDX12ToneMappingPass::ClearDenendencies()
{ GDX12RenderPass::ClearDenendencies(); _data.reset(); }
