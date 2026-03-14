#include "Engine.RHI/RenderingSystem.h"

RenderingSystem::RenderingSystem() :
	_currFrameConstantsIndex(0),
	_dualGPUMode(false),
	_windowHandle(nullptr),
	_windowWidth(0),
	_windowHeight(0)
{

}

RenderingSystem::~RenderingSystem()
{
	GDX12ShaderCompiler::Shutdown();
}

void RenderingSystem::Initialize(ComPtr<IDXGIAdapter4> primaryDeviceAdapter, ComPtr<IDXGIAdapter4> secondaryDeviceAdapter, 
	HWND windowHandle, GameTimer* gt, UINT width, UINT height)
{
	_windowHandle = windowHandle;
	_gameTimer = gt;
	_windowWidth = width;
	_windowHeight = height;

	_primaryDevice = std::make_shared<GDX12Device>();
	_primaryDevice->Initialize(primaryDeviceAdapter.Get());

	if (secondaryDeviceAdapter)
	{
		_secondaryDevice = std::make_shared<GDX12Device>();
		_secondaryDevice->Initialize(secondaryDeviceAdapter.Get());

		_dualGPUMode = true;
	}

	_rtvHeap = std::make_shared<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1000, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	_srvuavHeap = std::make_shared<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	_dsvHeap = std::make_shared<GDX12DescriptorHeap>(_primaryDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1000, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	_backBuffer = std::make_unique<GDX12BackBuffer>(_primaryDevice, _windowHandle,
		DXGI_FORMAT_R8G8B8A8_UNORM, 2, _windowWidth, _windowHeight, _rtvHeap);

	GDX12TextureDesc desc;
	desc.CreateSRV = false;
	desc.DSVHeap = _dsvHeap;
	
	desc.CreateDSV = true;
	desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.Width = _windowWidth;
	desc.Height = _windowHeight;
	desc.ClearValue = { 1.f, 1.f, 1.f, 1.f };

	desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	desc.DSVDesc.Texture2D.MipSlice = 0;

	_depthStencil = std::make_unique<GDX12Texture>(desc);
}

void RenderingSystem::OnResize()
{
	_backBuffer->Resize(_windowWidth, _windowHeight);
	_depthStencil->Resize(_windowWidth, _windowHeight);
}

void RenderingSystem::Update()
{

}

void RenderingSystem::Render()
{
	auto cmdQueue = _primaryDevice->GetCommandQueue();

	cmdQueue->Flush();

	auto cmdList = cmdQueue->GetCommandList();
	auto backBuffer = _backBuffer->GetCurrentBuffer();

	float clearColor[] = { 0.5 + 0.5 * cos(_gameTimer->TotalTime()), 0.5 + 0.5 * sin(_gameTimer->TotalTime()),
		0.5 + 0.5 * cos(_gameTimer->TotalTime()), 1.0f };

	cmdList->GetCommandList()->ClearRenderTargetView(backBuffer->GetRTV()->CPUHandle, clearColor, 0, nullptr);

	cmdQueue->ExecuteCommandList(cmdList);

	_backBuffer->Present();
}

void RenderingSystem::SetWindowDimensions(UINT width, UINT height)
{
	_windowWidth = width;
	_windowHeight = height;
}
