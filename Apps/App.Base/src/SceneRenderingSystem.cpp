#include "App.Base/Systems/SceneRenderingSystem.h"

#include "App.Base/App.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

static UINT _numFrameConstants = 3;

static AutoConsoleVariableRef NumFrameConstantVariable(
    L"Render.NumFrames",
    _numFrameConstants,
    L"How many deferred frames was rendered");

SceneRenderingSystem::SceneRenderingSystem(Window* window, GameTimer* timer) :
    _dualGPUMode(false), _window(window),
    _currFrameConstantsIndex(0), _timer(timer)
{

}

SceneRenderingSystem::~SceneRenderingSystem()
{
    _primaryDevice->GetCommandQueue()->Flush();

    GDX12ShaderCompiler::Shutdown();
}

void SceneRenderingSystem::Initialize()
{

#if defined(DEBUG) || defined(_DEBUG)
    // Enable the D3D12 debug layer.
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif

    _primaryDevice = std::make_unique<GDX12Device>();
    _primaryDevice->Initialize(GDX12DeviceFactory::GetMostPerformantAdapter().Get());

    // TODO: Add find other adapter and use cvar
    // if (secondaryDeviceAdapter)
    // {
    // 	_secondaryDevice = std::make_unique<GDX12Device>();
    // 	_secondaryDevice->Initialize(secondaryDeviceAdapter.Get());
    //
    // 	_dualGPUMode = true;
    // }

    BuildDescHeapsAndBackBuffer();
    BuildRootSignatures();
    BuildShaders();
    BuildPSOs();
    BuildFrameConstants();
}

void SceneRenderingSystem::OnResize()
{
    _primaryDevice->GetCommandQueue()->Flush();

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer->Resize(width, height);
    _depthStencil->Resize(width, height);
}

void SceneRenderingSystem::Update()
{
    _currFrameConstantsIndex = (_currFrameConstantsIndex + 1) % NumFrameConstantVariable.GetValue();

    auto cmdQueue = _primaryDevice->GetCommandQueue();
    auto& frameConsts = _frameConstants[_currFrameConstantsIndex];

    if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
    {
        cmdQueue->WaitForFenceValue(frameConsts->FenceValue);
    }

    UpdateMainCB();
}

void SceneRenderingSystem::Render()
{
    auto cmdQueue = _primaryDevice->GetCommandQueue();

    cmdQueue->Flush();

    auto cmdList = cmdQueue->GetCommandList();
    auto CurrentBackBuffer = _backBuffer->GetCurrentBuffer();
    auto& CurrentFrameConsts = _frameConstants[_currFrameConstantsIndex];

    cmdList->SetViewport(_backBuffer->GetViewport());
    cmdList->SetScissorRect(_backBuffer->GetScissorRect());

    cmdList->EnhancedTextureBarrier({
        CurrentBackBuffer->GetResource()->GetRenderTargetBarrier(),
        _depthStencil->GetResource()->GetDepthWriteBarrier()
        });

    cmdList->SetRenderTargets({ CurrentBackBuffer }, _depthStencil.get());
    cmdList->ClearRenderTargetView(CurrentBackBuffer);

    cmdList->SetGraphicsRootSignature(_rootSignatures["Test"].get());
    cmdList->SetGraphicsRootConstantBufferView(0, CurrentFrameConsts->MainCB->GetElementAddress(0));

    cmdList->SetPipelineState(_PSOs["Test"]);

    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);

    cmdList->EnhancedTextureBarrier({
        CurrentBackBuffer->GetResource()->GetPresentBarrier(),
        _depthStencil->GetResource()->GetCommonBarrier()
        });

    cmdQueue->ExecuteCommandList(cmdList);
    CurrentFrameConsts->FenceValue = cmdQueue->GetFence()->GetCompletedValue();

    _backBuffer->Present();
}

void SceneRenderingSystem::BuildDescHeapsAndBackBuffer()
{
    _rtvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    _srvuavHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

    _dsvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer = std::make_unique<GDX12SwapChain>(_primaryDevice.get(), _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, _rtvHeap.get());

    GDX12TextureDesc desc;
    desc.CreateSRV = false;
    desc.DSVHeap = _dsvHeap.get();

    desc.CreateDSV = true;
    desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.Width = width;
    desc.Height = height;
    desc.ClearValue = { 1.f, 1.f, 1.f, 1.f };

    desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    desc.DSVDesc.Texture2D.MipSlice = 0;

    _depthStencil = std::make_unique<GDX12Texture>(desc);
}

void SceneRenderingSystem::BuildRootSignatures()
{
    GDX12RootSignatureDesc desc;
    desc.NumCBVSlots = 1;

    _rootSignatures["Test"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc);
}

void SceneRenderingSystem::BuildShaders()
{
    auto& Compiler = GDX12ShaderCompiler::GetInstance();

    _shaders["TestVS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "VS_FSQuad",
        "vs");
    _shaders["TestPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "PS", "ps");
}

void SceneRenderingSystem::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};

    desc.InputLayout = { nullptr, 0 };
    desc.pRootSignature = _rootSignatures["Test"]->GetRootSignature().Get();
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable = false;
    desc.DepthStencilState.StencilEnable = false;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = _backBuffer->GetFormat();
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.DSVFormat = _depthStencil->GetFormat();

    desc.VS =
    {
        reinterpret_cast<BYTE*>(_shaders["TestVS"]->GetBufferPointer()),
        _shaders["TestVS"]->GetBufferSize()
    };
    desc.PS =
    {
        reinterpret_cast<BYTE*>(_shaders["TestPS"]->GetBufferPointer()),
        _shaders["TestPS"]->GetBufferSize()
    };
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_PSOs["Test"])));
}

void SceneRenderingSystem::BuildFrameConstants()
{
    for (int i = 0; i < NumFrameConstantVariable.GetValue(); i++)
    {
        //TODO: fix zero element upload buffer crash
        _frameConstants.emplace_back(std::make_unique<GDX12FrameConstants>(_primaryDevice.get(), 1, 1, 1));
    }
}

void SceneRenderingSystem::UpdateMainCB() const
{
    auto& frameRes = _frameConstants[_currFrameConstantsIndex];

    GDX12MainConstants mainConstants;

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    mainConstants.RenderTargetSize = { static_cast<float>(width), static_cast<float>(height) };
    mainConstants.TotalTime = _timer->TotalTime();
    mainConstants.DeltaTime = _timer->DeltaTime();

    frameRes->MainCB->CopyData(0, mainConstants);
}