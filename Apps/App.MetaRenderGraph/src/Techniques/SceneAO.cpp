#include "SceneAO.h"
#include "../../../../External/External.Libraries/XeGTAO/XeGTAO.h"
#define FFX_CACAO_ENABLE_D3D12
#include "../../../../External/External.Libraries/cacao/inc/ffx_cacao_impl.h"
#include <cstring>
namespace Meta
{
namespace
{
void CheckCACAO(FFX_CACAO_Status result)
{
    if (result != FFX_CACAO_STATUS_OK)
        throw std::runtime_error("CACAO preparation/recording failed");
}
void Expand(Matrix &p, float x, float y)
{
    p._11 *= x;
    p._22 *= y;
    p._31 = x * p._31 + (1 - x);
    p._32 = y * p._32 - (1 - y);
}
} // namespace
SceneAO::SceneAO(GDX12DeviceResources &r, RenderPipelineCommonData &c, GDX12Texture *depth, GDX12Texture *normals)
    : _resources(r), _common(c)
{
    _pass.Setup(&r, &c);
    _pass.SetInputs({depth, normals});
    _pass.Initialize();
}
SceneAO::~SceneAO()
{
    _pass.ClearDenendencies();
}
void SceneAO::Freeze(GDX12Texture *depth, GDX12Texture *normals, UINT &temporalIndex)
{
    auto &p = _pass;
    p.SetInputs({depth, normals});
    if (p._output.GetWidth() != depth->GetWidth() || p._output.GetHeight() != depth->GetHeight())
        p.Resize();
    _settings = GetAmbientOcclusionSettings();
    const auto &s = _settings;
    if (!p._hasPrevious || std::memcmp(&p._previous, &s, sizeof(s)))
        _common.ResetUpscalerHistory = true;
    p._previous = s;
    p._hasPrevious = true;
    _half = s.Resolution == GDX12AOResolution::Half;
    _width = _half ? (depth->GetWidth() + 1) / 2 : depth->GetWidth();
    _height = _half ? (depth->GetHeight() + 1) / 2 : depth->GetHeight();
    auto &grid = p._grids[_half];
    if (!s.IsActive() || (_width == 1 && _height == 1))
        return;
    if (!grid.Normals.IsInitialized())
        p.CreateGrid(grid, _width, _height, _half);
    const UINT slot = _resources.CurrFrameConstantsIndex;
    GDX12AmbientOcclusionPass::ShaderConstants constants = {s.RadiusCm,
                                                            s.Intensity,
                                                            s.BiasCm,
                                                            s.HorizonBias,
                                                            s.FadeStartCm,
                                                            s.FadeEndCm,
                                                            s.Sharpness,
                                                            UINT(s.Samples),
                                                            UINT(s.Directions),
                                                            UINT(s.Steps),
                                                            UINT(s.DenoisePasses),
                                                            UINT(s.Implementation),
                                                            float(s.GTAOSlices),
                                                            float(s.GTAOSteps),
                                                            _half,
                                                            0};
    p._constants->CopyData(slot, constants);
    _projection = _common.CameraViewToClip;
    _projection._31 -= _common.ActiveCameraJitterOffsetX * 2 / depth->GetWidth();
    _projection._32 += _common.ActiveCameraJitterOffsetY * 2 / depth->GetHeight();
    if (_half)
        Expand(_projection, depth->GetWidth() / (2.f * _width), depth->GetHeight() / (2.f * _height));
    XeGTAO::GTAOSettings gtao;
    gtao.Radius = s.RadiusCm;
    gtao.DenoisePasses = s.DenoisePasses;
    gtao.FalloffRange = s.GTAOFalloff;
    gtao.ThinOccluderCompensation = s.GTAOThinOccluder;
    XeGTAO::GTAOConstants xe{};
    XeGTAO::GTAOUpdateConstants(xe, _width, _height, gtao, &_projection._11, true,
                                _common.Upscaler ? temporalIndex++ : 0);
    xe.NDCToViewAdd.x += _projection._31 / _projection._11;
    xe.NDCToViewAdd.y += _projection._32 / _projection._22;
    xe.DenoiseBlurBeta = s.DenoisePasses ? .2f + s.Sharpness : 1e4f;
    p._xeConstants->CopyData(slot, xe);
    // SDK context creation and settings updates are owner-only prerequisites.
    // Native CACAO keeps its private descriptor/scratch ownership; the graph
    // owns imported textures and the entire dispatch is one exclusive packet.
    if (s.Implementation == 3)
    {
        auto &inputDepth = _half ? grid.Depth : *depth;
        const bool pad = (std::max)(_width, _height) < 16;
        auto &cacaoDepth = pad ? grid.CacaoDepth : inputDepth;
        auto &cacaoNormals = pad ? grid.CacaoNormals : grid.Normals;
        auto &output = pad ? grid.CacaoVisibility : grid.Scratch[0];
        if (!grid.Cacao)
        {
            grid.CacaoMemory.resize(FFX_CACAO_D3D12GetContextSize());
            grid.Cacao = reinterpret_cast<FFX_CACAO_D3D12Context *>(grid.CacaoMemory.data());
            CheckCACAO(FFX_CACAO_D3D12InitContext(grid.Cacao, _resources.Device->GetDevice().Get(),
                                                  UINT(_resources.FrameConstants.size())));
        }
        if (!grid.CacaoScreenReady)
        {
            FFX_CACAO_D3D12ScreenSizeInfo info{};
            info.width = cacaoDepth.GetWidth();
            info.height = cacaoDepth.GetHeight();
            info.depthBufferResource = cacaoDepth.GetResource()->D3DResource.Get();
            info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
            info.normalBufferResource = cacaoNormals.GetResource()->D3DResource.Get();
            info.normalBufferSrvDesc = info.depthBufferSrvDesc;
            info.normalBufferSrvDesc.Format = cacaoNormals.GetFormat();
            info.outputResource = output.GetResource()->D3DResource.Get();
            info.outputUavDesc.Format = output.GetFormat();
            info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(grid.Cacao, &info));
            grid.CacaoScreenReady = true;
        }
        auto settings = FFX_CACAO_DEFAULT_SETTINGS;
        settings.radius = s.RadiusCm * .01f;
        settings.shadowMultiplier = s.Intensity;
        settings.shadowPower = s.CACAOPower;
        settings.shadowClamp = s.CACAOClamp;
        settings.horizonAngleThreshold = s.HorizonBias;
        settings.fadeOutFrom = s.FadeStartCm * .01f;
        settings.fadeOutTo = s.FadeEndCm * .01f;
        settings.qualityLevel = FFX_CACAO_Quality(s.CACAOQuality);
        settings.adaptiveQualityLimit = s.CACAOAdaptiveLimit;
        settings.blurPassCount = s.DenoisePasses;
        settings.sharpness = s.Sharpness;
        settings.detailShadowStrength = s.CACAODetail;
        CheckCACAO(FFX_CACAO_D3D12UpdateSettings(grid.Cacao, &settings));
    }
}
void SceneAO::Append(Graph &graph, std::vector<ID3D12Resource *> &bindings)
{
    _work.Commands.clear();
    auto &p = _pass;
    auto &grid = p._grids[_half];
    auto *fullDepth = p._inputs[0]->GetTexture();
    auto *fullNormals = p._inputs[1]->GetTexture();
    const auto id = [&](GDX12Resource *resource) {
        return NativeWork::ResourceFor(graph, bindings, resource->D3DResource.Get(), "AO resource");
    };
    const auto access = [&](GDX12Texture *texture, Mode mode, State state) {
        return Access{id(texture->GetResource()), mode, state};
    };
    const UINT slot = _resources.CurrFrameConstantsIndex;
    auto *root = p._root.get();
    auto *heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    const auto camera = _resources.FrameConstants[slot]->CameraCB->GetElementAddress(_common.ActiveCameraCBufferIndex),
               constants = p._constants->GetElementAddress(slot), xe = p._xeConstants->GetElementAddress(slot);
    const auto dispatch = [&](const std::string &name, UINT pipeline, UINT width, UINT height,
                              std::vector<Access> accesses, std::array<GDX12Texture *, 7> srvs,
                              std::array<GDX12Descriptor *, 10> uavs, UINT axis = 0, UINT final = 0,
                              float scale = 1.f) {
        accesses.push_back({id(&_resources.FrameConstants[slot]->CameraCB->GetResource()), Mode::Read,
                            D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&p._constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&p._xeConstants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 7> reads{};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 10> writes{};
        for (UINT i = 0; i < 7; ++i)
            if (srvs[i])
                reads[i] = srvs[i]->GetSRV()->GPUHandle;
        for (UINT i = 0; i < 10; ++i)
            if (uavs[i])
                writes[i] = uavs[i]->GPUHandle;
        auto *pso = p._pipelines[pipeline].Get();
        return _work.Add(graph, name, std::move(accesses), [=](auto *cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(root->GetRootSignature().Get());
            cmd->SetPipelineState(pso);
            const struct
            {
                UINT Axis, Final;
                float Scale;
                UINT Padding;
            } c{axis, final, scale, 0};
            cmd->SetComputeRoot32BitConstants(0, 4, &c, 0);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(2), constants);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(3), xe);
            for (UINT i = 0; i < 7; ++i)
                if (reads[i].ptr)
                    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), reads[i]);
            for (UINT i = 0; i < 10; ++i)
                if (writes[i].ptr)
                    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(i), writes[i]);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });
    };
    if (!_settings.IsActive() || (_width == 1 && _height == 1))
    {
        dispatch("AO neutral fallback", p.Neutral, p._output.GetWidth(), p._output.GetHeight(),
                 {access(&p._output, Mode::Write, UnorderedAccess)}, {}, {p._output.GetUAV()});
        return;
    }
    auto *depth = _half ? &grid.Depth : fullDepth;
    const State read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::vector<Access> prepare{access(fullDepth, Mode::Read, read), access(fullNormals, Mode::Read, read),
                                access(&grid.Normals, Mode::Write, UnorderedAccess)};
    if (_half)
        prepare.push_back(access(depth, Mode::Write, UnorderedAccess));
    dispatch("AO prepare normals/depth", _half ? p.PrepareHalf : p.PrepareNormals, _width, _height, prepare,
             {fullDepth, fullNormals},
             {nullptr, grid.Normals.GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              _half ? depth->GetUAV() : nullptr});
    GDX12Texture *visibility = nullptr;
    GDX12Texture *terms = nullptr;
    float scale = 1.f;
    if (_settings.Implementation == 3)
    {
        const bool pad = (std::max)(_width, _height) < 16;
        auto *inputDepth = pad ? &grid.CacaoDepth : depth;
        auto *inputNormals = pad ? &grid.CacaoNormals : &grid.Normals;
        visibility = pad ? &grid.CacaoVisibility : &grid.Scratch[0];
        if (pad)
            dispatch("CACAO pad small grid", p.PadCACAO, inputDepth->GetWidth(), inputDepth->GetHeight(),
                     {access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                      access(inputDepth, Mode::Write, UnorderedAccess),
                      access(inputNormals, Mode::Write, UnorderedAccess)},
                     {depth, &grid.Normals},
                     {nullptr, inputNormals->GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                      inputDepth->GetUAV()});
        Matrix projection = _projection;
        if (pad)
            Expand(projection, float(depth->GetWidth()) / inputDepth->GetWidth(), 1);
        FFX_CACAO_Matrix4x4 proj{}, identity{};
        std::memcpy(&proj, &projection, sizeof(proj));
        proj.elements[3][2] *= .01f;
        for (UINT i = 0; i < 4; ++i)
            identity.elements[i][i] = 1;
        auto *context = grid.Cacao;
        auto *output = visibility->GetResource()->D3DResource.Get();
        _work.Add(graph, "CACAO SDK dispatch (exclusive persistent context)",
                  {access(inputDepth, Mode::Read, read), access(inputNormals, Mode::Read, read),
                   access(visibility, Mode::Write, UnorderedAccess)},
                  [context, proj, identity, output](auto *cmd) {
                      CheckCACAO(FFX_CACAO_D3D12Draw(context, cmd, &proj, &identity));
                      const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                          output, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      cmd->ResourceBarrier(1, &barrier);
                  });
    }
    else if (_settings.Implementation == 2)
    {
        std::array<GDX12Descriptor *, 10> uavs{};
        for (UINT mip = 0; mip < 5; ++mip)
            uavs[2 + mip] = &grid.DepthUAVs[mip];
        dispatch("XeGTAO depth pyramid", p.XePrefilter, grid.XeDepth.GetWidth() / 2, grid.XeDepth.GetHeight() / 2,
                 {access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                  access(&grid.XeDepth, Mode::Write, UnorderedAccess)},
                 {depth, &grid.Normals}, uavs);
        dispatch("XeGTAO main", p.XeMain, _width, _height,
                 {access(&grid.XeDepth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                  access(&grid.XeTerms[0], Mode::Write, UnorderedAccess),
                  access(&grid.XeEdges, Mode::Write, UnorderedAccess)},
                 {depth, &grid.Normals, &grid.XeDepth},
                 {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, grid.XeTerms[0].GetUAV(),
                  grid.XeEdges.GetUAV()});
        UINT source = 0;
        for (int pass = 0; pass < _settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            dispatch("XeGTAO denoise", p.XeDenoise, (_width + 1) / 2, _height,
                     {access(&grid.XeTerms[source], Mode::Read, read), access(&grid.XeEdges, Mode::Read, read),
                      access(&grid.XeTerms[target], Mode::Write, UnorderedAccess)},
                     {depth, &grid.Normals, &grid.XeDepth, &grid.XeTerms[source], &grid.XeEdges},
                     {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, grid.XeTerms[target].GetUAV()}, 0,
                     pass + 1 == _settings.DenoisePasses);
            source = target;
        }
        terms = &grid.XeTerms[source];
        visibility = &grid.XeDepth;
        scale = _settings.DenoisePasses ? 1.f : XE_GTAO_OCCLUSION_TERM_SCALE;
    }
    else
    {
        dispatch("SSAO/HBAO visibility", _settings.Implementation == 0 ? p.SSAO : p.HBAO, _width, _height,
                 {access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                  access(&grid.Scratch[0], Mode::Write, UnorderedAccess)},
                 {depth, &grid.Normals}, {grid.Scratch[0].GetUAV()});
        UINT source = 0;
        for (int pass = 0; pass < _settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            dispatch("AO bilateral blur", p.Blur, _width, _height,
                     {access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                      access(&grid.Scratch[source], Mode::Read, read),
                      access(&grid.Scratch[target], Mode::Write, UnorderedAccess)},
                     {depth, &grid.Normals, &grid.Scratch[source]}, {grid.Scratch[target].GetUAV()}, pass % 2);
            source = target;
        }
        visibility = &grid.Scratch[source];
    }
    if (!terms)
        terms = &grid.XeTerms[0];
    auto *resolved = _half ? &grid.Visibility : &p._output;
    std::vector<Access> resolve{access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                                access(visibility, Mode::Read, read), access(resolved, Mode::Write, UnorderedAccess)};
    if (terms)
        resolve.push_back(access(terms, Mode::Read, read));
    dispatch("AO resolve", p.Resolve, _width, _height, resolve, {depth, &grid.Normals, visibility, terms},
             {resolved->GetUAV()}, 0, 0, scale);
    if (_half)
        dispatch("AO depth/normal aware upsample", p.Upsample, p._output.GetWidth(), p._output.GetHeight(),
                 {access(depth, Mode::Read, read), access(&grid.Normals, Mode::Read, read),
                  access(resolved, Mode::Read, read), access(fullDepth, Mode::Read, read),
                  access(fullNormals, Mode::Read, read), access(&p._output, Mode::Write, UnorderedAccess)},
                 {depth, &grid.Normals, resolved, nullptr, nullptr, fullDepth, fullNormals}, {p._output.GetUAV()});
}
} // namespace Meta
