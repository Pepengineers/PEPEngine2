#include "Engine.UI/UIScreenComposite.h"
#include "Engine.UI/OffscreenUI.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <stdexcept>

namespace Engine::UI
{
    struct ScreenComposite::Impl
    {
        struct Recording
        {
            explicit Recording(std::shared_ptr<Impl> compositor)
                : Compositor(std::move(compositor)),
                  Srv(&Compositor->Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
                  Rtv(&Compositor->Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE) {}

            std::shared_ptr<Impl> Compositor;
            GDX12DescriptorHeap Srv, Rtv;
            GDX12ResourceHandle Input, Target;
        };

        explicit Impl(GDX12Device& device) : Device(device)
        {
            ThrowIfFailed(Device.GetDevice().As(&Identity));
            GDX12RootSignatureDesc root;
            root.NumSingleSRVSlots = 1;
            root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
            Root = std::make_unique<GDX12RootSignature>(&device, root);
            constexpr wchar_t shader[] = SHADERS_FOLDER L"UIScreenComposite.hlsl";
            auto& compiler = GDX12ShaderCompiler::GetInstance();
            const auto vertex = compiler.CompileShader(&device, shader, nullptr, "VSMain", "vs");
            const auto pixel = compiler.CompileShader(&device, shader, nullptr, "PSMain", "ps");

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = Root->GetRootSignature().Get();
            pso.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
            pso.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
            pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pso.DepthStencilState.DepthEnable = FALSE;
            pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            auto& blend = pso.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            pso.SampleMask = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = OffscreenFormat;
            pso.SampleDesc.Count = 1;
            ThrowIfFailed(Device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&Pipeline)));
        }

        void CheckDevice(ID3D12DeviceChild& object) const
        {
            ComPtr<IUnknown> owner;
            ThrowIfFailed(object.GetDevice(IID_PPV_ARGS(&owner)));
            if (owner.Get() != Identity.Get())
                throw std::invalid_argument("Screen composite requires device-local commands and textures");
        }

        GDX12Device& Device;
        ComPtr<IUnknown> Identity;
        std::unique_ptr<GDX12RootSignature> Root;
        ComPtr<ID3D12PipelineState> Pipeline;
    };

    ScreenComposite::ScreenComposite(GDX12Device& device)
    {
        if (!device.IsInitialized()) throw std::invalid_argument("Screen composite requires a live device");
        _impl = std::make_shared<Impl>(device);
    }

    ScreenComposite::~ScreenComposite() = default;

    std::shared_ptr<const void> ScreenComposite::Record(GDX12CommandList& commands,
        GDX12ResourceHandle input, GDX12ResourceHandle target) const
    {
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Screen composite belongs to a retired device incarnation");
        if (commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !input || !target || input.Get() == target.Get())
            throw std::invalid_argument("Screen composite requires a DIRECT list and distinct source and target");
        auto* native = commands.GetCommandList().Get();
        state.CheckDevice(*native);
        state.CheckDevice(*input.Get());
        state.CheckDevice(*target.Get());
        const auto sourceDesc = input->GetDesc();
        const auto targetDesc = target->GetDesc();
        const auto valid = [](const D3D12_RESOURCE_DESC& desc)
        {
            return desc.Format == OffscreenFormat && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1;
        };
        if (!valid(sourceDesc) || !valid(targetDesc) || (sourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
            !(targetDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
            throw std::invalid_argument("Screen composite requires RGBA8 UNORM texture views");

        auto recording = std::make_shared<Impl::Recording>(_impl);
        recording->Input = std::move(input);
        recording->Target = std::move(target);
        auto& device = state.Device.GetDevice();
        device->CreateShaderResourceView(recording->Input.Get(), nullptr, recording->Srv.GetCPUHandle());
        const auto rtv = recording->Rtv.GetCPUHandle();
        device->CreateRenderTargetView(recording->Target.Get(), nullptr, rtv);

        commands.SetDescriptorHeaps({&recording->Srv});
        commands.SetGraphicsRootSignature(state.Root.get());
        commands.SetGraphicsSRV(0, recording->Srv.GetGPUHandle());
        commands.SetPipelineState(state.Pipeline);
        commands.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands.SetViewport({0, 0, static_cast<float>(targetDesc.Width), static_cast<float>(targetDesc.Height), 0, 1});
        commands.SetScissorRect({0, 0, static_cast<LONG>(targetDesc.Width), static_cast<LONG>(targetDesc.Height)});
        native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands.DrawInstanced(3, 1, 0, 0);
        return recording;
    }
}
