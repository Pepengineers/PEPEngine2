#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionPreviewPass.h"

GDX12ReflectionPreviewPass::GDX12ReflectionPreviewPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
        RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
    _numInputs = 5;
}

void GDX12ReflectionPreviewPass::Initialize()
{
    GDX12RootSignatureDesc root;
    root.Constants = {1};
    root.NumSingleCBVSlots = 2;
    root.NumSingleSRVSlots = 5;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
    auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ReflectionPreview.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _root->GetRootSignature().Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.InputLayout = {_resources->InputLayouts["Default"].data(), (UINT)_resources->InputLayouts["Default"].size()};
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 4;
    for (UINT i = 0; i < 4; ++i) desc.RTVFormats[i] = _inputs[i ? i + 1 : 0]->GetTexture()->GetFormat();
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
}

void GDX12ReflectionPreviewPass::Execute(GDX12CommandList* commands)
{
    if (_resources->ReflectionPreviewDraws.empty()) return;
    GDX12_SCOPED_EVENT(commands, "Reflection probe meshes", Colors::Cyan);
    auto& frame = *_resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    for (size_t i = 0; i < _inputs.size(); ++i)
        commands->ResourceBarrier({i == 1 ? _inputs[i]->GetTexture()->GetResource()->GetDepthWriteBarrier() :
            _inputs[i]->GetTexture()->GetResource()->GetRenderTargetBarrier()});
    commands->SetGraphicsRootSignature(_root.get());
    commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()});
    commands->SetGraphicsRootConstantBufferView(1, frame.MainCB->GetElementAddress(0));
    commands->SetGraphicsRootConstantBufferView(2, frame.CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetGraphicsSRV(0, frame.ShadowMaterials->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(1, frame.ShadowTransforms->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(2, frame.ShadowInstances->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(3, _resources->ReflectionRadianceSRV.GPUHandle);
    commands->SetGraphicsSRV(4, frame.ReflectionProbes->GetSRV()->GPUHandle);
    commands->SetPipelineState(_pipeline);
    auto* color = _inputs[0]->GetTexture();
    commands->SetViewport(color->GetViewport());
    commands->SetScissorRect(color->GetScissorRect());
    commands->SetGeometryBuffer(_resources->GeometryBuffer.get());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetRenderTargets({color, _inputs[2]->GetTexture(), _inputs[3]->GetTexture(), _inputs[4]->GetTexture()}, _inputs[1]->GetTexture());
    for (const auto& draw : _resources->ReflectionPreviewDraws)
    {
        commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 1, &draw.InstanceID, 0);
        commands->DrawIndexedInstanced(draw.IndexCountPerInstance, 1, draw.StartIndexLocation,
            draw.BaseVertexLocation, draw.StartInstanceLocation);
    }
    for (auto* input : _inputs) commands->ResourceBarrier({input->GetTexture()->GetResource()->GetSRVBarrier()});
}
