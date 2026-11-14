#pragma once
#ifdef _DEBUG
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"

    class BistroCollisionPass final : public GDX12RenderPass
    {
    public:
        explicit BistroCollisionPass(SceneManagerModule& scenes) : _scenes(scenes) { _numInputs = 1; }

        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName("Jolt collision view");
            graph.Write(_inputs[0], "Swapchain", Lifetime::Dynamic, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        void Initialize() override
        {
            GDX12RootSignatureDesc root;
            root.Constants = {20};
            _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
            auto& compiler = GDX12ShaderCompiler::GetInstance();
            auto vs = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "CollisionView.hlsl", nullptr, "VS", "vs");
            auto ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "CollisionView.hlsl", nullptr, "PS", "ps");
            const D3D12_INPUT_ELEMENT_DESC position = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = _root->GetRootSignature().Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.InputLayout = {&position, 1};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = true;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthEnable = false;
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = _inputs[0]->GetTexture()->GetFormat();
            desc.SampleDesc.Count = 1;
            auto device = _resources->Device->GetDevice();
            ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_fill)));
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
            ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_wire)));
            _uploads.resize(_resources->FrameConstants.size());
        }

        void PrepareRecording()
        {
            _statics.clear();
            _moving.clear();
            auto physics = BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
            if (auto* world = _scenes.GetWorld(); world && physics)
                physics->CollectCollisionTriangles(*world, _statics, _moving);

            const auto count = _statics.size() + _moving.size();
            if (count != 0)
            {
                if (count > UINT_MAX / sizeof(Vector3)) throw std::length_error("Collision view vertex buffer is too large");
                // RenderModule has already waited for the current frame slot's fence.
                auto& upload = _uploads.at(_resources->CurrFrameConstantsIndex);
                if (!upload || upload->GetElementCount() < count)
                    upload = std::make_unique<GDX12UploadBuffer<Vector3>>(_resources->Device, static_cast<UINT>(count), EBufferType::Upload, false);
                const auto staticCount = static_cast<UINT>(_statics.size());
                const auto movingCount = static_cast<UINT>(_moving.size());
                if (staticCount) upload->CopyData(0, _statics.data(), staticCount);
                if (movingCount) upload->CopyData(staticCount, _moving.data(), movingCount);
            }
        }
        GDX12Resource* UploadResource() const
        {
            const auto& upload = _uploads.at(_resources->CurrFrameConstantsIndex);
            return upload && (!_statics.empty() || !_moving.empty()) ? &upload->GetResource() : nullptr;
        }
        void Execute(GDX12CommandList* commands) override
        {
            PrepareRecording();
            RecordCommands(commands);
        }
        void RecordCommands(GDX12CommandList* commands)
        {
            auto* target = _inputs[0]->GetTexture();
            GDX12_SCOPED_EVENT(commands, "Jolt collision view", Colors::Lime);
            commands->ResourceBarrier({target->GetResource()->GetRenderTargetBarrier()});
            commands->ClearRenderTargetView(target);
            const auto staticCount = static_cast<UINT>(_statics.size());
            const auto movingCount = static_cast<UINT>(_moving.size());
            const auto count = staticCount + movingCount;
            if (count != 0)
            {
                auto& upload = _uploads.at(_resources->CurrFrameConstantsIndex);
                commands->SetRenderTargets({target}, nullptr);
                commands->SetViewport(target->GetViewport());
                commands->SetScissorRect(target->GetScissorRect());
                commands->SetGraphicsRootSignature(_root.get());
                commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                auto list = commands->GetCommandList();
                const D3D12_VERTEX_BUFFER_VIEW buffer = {upload->GetElementAddress(0),
                    static_cast<UINT>(count * sizeof(Vector3)), sizeof(Vector3)};
                list->IASetVertexBuffers(0, 1, &buffer);
                commands->InvalidateGeometryBuffer();
                const auto viewProjection = _commonData->CameraView * _commonData->CameraViewToClip;
                list->SetGraphicsRoot32BitConstants(0, 16, &viewProjection, 0);
                const auto draw = [&](const Vector4& tint, UINT start, UINT size)
                {
                    list->SetGraphicsRoot32BitConstants(0, 4, &tint, 16);
                    commands->DrawInstanced(size, 1, start, 0);
                };
                commands->SetPipelineState(_fill);
                if (staticCount) draw({0.10f, 1.0f, 0.22f, 0.27f}, 0, staticCount);
                if (movingCount)
                {
                    draw({1.0f, 0.06f, 0.08f, 0.12f}, staticCount, movingCount);
                    commands->SetPipelineState(_wire);
                    draw({1.0f, 0.13f, 0.16f, 0.50f}, staticCount, movingCount);
                }
            }
            commands->ResourceBarrier({target->GetResource()->GetPresentBarrier()});
        }

    private:
        SceneManagerModule& _scenes;
        std::unique_ptr<GDX12RootSignature> _root;
        ComPtr<ID3D12PipelineState> _fill, _wire;
        std::vector<std::unique_ptr<GDX12UploadBuffer<Vector3>>> _uploads;
        std::vector<Vector3> _statics, _moving;
    };
#endif
