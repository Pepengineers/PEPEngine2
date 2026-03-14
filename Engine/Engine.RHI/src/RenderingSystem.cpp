#include "Engine.RHI/RenderingSystem.h"

RenderingSystem::RenderingSystem() :
	_currFrameConstantsIndex(0),
	_dualGPUMode(false),
	_windowHandle(nullptr),
	_windowWidth(0),
	_windowHeight(0),
	_gameTimer(nullptr)
{

}

RenderingSystem::~RenderingSystem()
{
	_primaryDevice->GetCommandQueue()->Flush();

	GDX12ShaderCompiler::Shutdown();
}

void RenderingSystem::Initialize(ComPtr<IDXGIAdapter4> primaryDeviceAdapter, ComPtr<IDXGIAdapter4> secondaryDeviceAdapter, 
	HWND windowHandle, GameTimer* gt, UINT width, UINT height)
{
#if defined(DEBUG) || defined(_DEBUG) 
	// Enable the D3D12 debug layer.
	ComPtr<ID3D12Debug> debugController;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
	debugController->EnableDebugLayer();
#endif


	_windowHandle = windowHandle;
	_gameTimer = gt;
	_windowWidth = width;
	_windowHeight = height;

	_primaryDevice = std::make_unique<GDX12Device>();
	_primaryDevice->Initialize(primaryDeviceAdapter.Get());

	if (secondaryDeviceAdapter)
	{
		_secondaryDevice = std::make_unique<GDX12Device>();
		_secondaryDevice->Initialize(secondaryDeviceAdapter.Get());

		_dualGPUMode = true;
	}

	BuildDescHeapsAndBackBuffer();
	BuildRootSignatures();
	BuildShaders();
	BuildPSOs();
	BuildFrameConstants();
}

void RenderingSystem::OnResize()
{
	_primaryDevice->GetCommandQueue()->Flush();

	_backBuffer->Resize(_windowWidth, _windowHeight);
	_depthStencil->Resize(_windowWidth, _windowHeight);
}

void RenderingSystem::Update()
{
	_currFrameConstantsIndex = (_currFrameConstantsIndex + 1) % _numFrameConstants;

	auto cmdQueue = _primaryDevice->GetCommandQueue();
	auto& frameConsts = _frameConstants[_currFrameConstantsIndex];

	if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
	{
		cmdQueue->WaitForFenceValue(frameConsts->FenceValue);
	}

	UpdateMainCB();
}

void RenderingSystem::Render()
{
	auto cmdQueue = _primaryDevice->GetCommandQueue();

	cmdQueue->Flush();

	auto cmdList = cmdQueue->GetCommandList();
	auto CurrentBackBuffer = _backBuffer->GetCurrentBuffer();
	auto& CurrentFrameConsts = _frameConstants[_currFrameConstantsIndex];

	cmdList->SetViewport(_backBuffer->GetViewport());
	cmdList->SetScissorRect(_backBuffer->GetScissorRect());

	//TODO: Replace with enhanced barriers
	// Add easy-to-use wrapper class
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer->GetD3DResource().Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cmdList->GetCommandList()->ResourceBarrier(1, &barrier);

	cmdList->SetRenderTargets({ CurrentBackBuffer }, _depthStencil.get());
	cmdList->ClearRenderTargetView(CurrentBackBuffer);

	cmdList->SetGraphicsRootSignature(_rootSignatures["Test"].get());
	cmdList->SetGraphicsRootConstantBufferView(0, CurrentFrameConsts->MainCB->GetElementAddress(0));

	cmdList->SetPipelineState(_PSOs["Test"]);

	cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawInstanced(6, 1, 0, 0);

	//TODO: Replace with enhanced barriers
	// Add easy-to-use wrapper class
	auto counterBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer->GetD3DResource().Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	cmdList->GetCommandList()->ResourceBarrier(1, &counterBarrier);

	cmdQueue->ExecuteCommandList(cmdList);
	CurrentFrameConsts->FenceValue = cmdQueue->GetFence()->GetCompletedValue();

	_backBuffer->Present();
}

void RenderingSystem::BuildDescHeapsAndBackBuffer()
{
	_rtvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1000, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	_srvuavHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	_dsvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1000, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	_backBuffer = std::make_unique<GDX12BackBuffer>(_primaryDevice.get(), _windowHandle,
		DXGI_FORMAT_R8G8B8A8_UNORM, 2, _windowWidth, _windowHeight, _rtvHeap.get());

	GDX12TextureDesc desc;
	desc.CreateSRV = false;
	desc.DSVHeap = _dsvHeap.get();

	desc.CreateDSV = true;
	desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.Width = _windowWidth;
	desc.Height = _windowHeight;
	desc.ClearValue = { 1.f, 1.f, 1.f, 1.f };

	desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	desc.DSVDesc.Texture2D.MipSlice = 0;

	_depthStencil = std::make_unique<GDX12Texture>(desc);
}

void RenderingSystem::BuildRootSignatures()
{
	GDX12RootSignatureDesc desc;
	desc.NumCBVSlots = 1;

	_rootSignatures["Test"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc);
}

void RenderingSystem::BuildShaders()
{
	auto& Compiler = GDX12ShaderCompiler::GetInstance();

	_shaders["TestVS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "VS_FSQuad", "vs");
	_shaders["TestPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "PS", "ps");
}

void RenderingSystem::BuildPSOs()
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

void RenderingSystem::BuildFrameConstants()
{
	for (int i = 0; i < _numFrameConstants; i++)
	{
		_frameConstants[i] = std::make_unique<GDX12FrameConstants>(_primaryDevice.get(), 1, 1, 1);
	}
}

void RenderingSystem::UpdateMainCB()
{
	auto& frameRes = _frameConstants[_currFrameConstantsIndex];

	GDX12MainConstants mainConstants;

	mainConstants.RenderTargetSize = { static_cast<float>(_windowWidth), static_cast<float>(_windowHeight) };
	mainConstants.TotalTime = _gameTimer->TotalTime();
	mainConstants.DeltaTime = _gameTimer->DeltaTime();

	frameRes->MainCB->CopyData(0, mainConstants);
}

void RenderingSystem::SetWindowDimensions(UINT width, UINT height)
{
	_windowWidth = width;
	_windowHeight = height;
}
