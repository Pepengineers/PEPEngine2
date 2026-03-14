#pragma once

#include "Engine.Core/GameTimer.h"

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/GDX12FrameConstants.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12BackBuffer.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12Texture.h"

class RenderingSystem
{
public:
	RenderingSystem();
	~RenderingSystem();

	void Initialize(ComPtr<IDXGIAdapter4> primaryDeviceAdapter, ComPtr<IDXGIAdapter4> secondaryDeviceAdapter,
		HWND windowHandle, GameTimer* gt, UINT width, UINT height);

	void SetWindowDimensions(UINT width, UINT height);
	void OnResize();

	void Update();
	void Render();

private:
	HWND _windowHandle;
	GameTimer* _gameTimer = nullptr;

	std::shared_ptr<GDX12Device> _primaryDevice;
	std::shared_ptr<GDX12Device> _secondaryDevice;
	bool _dualGPUMode;

	UINT _windowWidth;
	UINT _windowHeight;

	std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> _inputLayouts;

	// These two resources are made on _primaryDevice only
	std::unique_ptr<GDX12BackBuffer> _backBuffer;
	std::unique_ptr<GDX12Texture> _depthStencil;

	static constexpr UINT _numFrameConstants = 3;

	// All of class members below should probably be put into DeviceResources class, and made for each device
	// Since all of these resources are currently made for _primaryDevice only
	std::array<std::unique_ptr<GDX12FrameConstants>, _numFrameConstants> _frameConstants;
	UINT _currFrameConstantsIndex;

	// All heaps created in one high-capacity instance
	std::shared_ptr<GDX12DescriptorHeap> _rtvHeap;
	std::shared_ptr<GDX12DescriptorHeap> _srvuavHeap;
	std::shared_ptr<GDX12DescriptorHeap> _dsvHeap;

	std::unordered_map<std::string, ComPtr<ID3DBlob>> _shaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> _PSOs;
	std::unordered_map<std::string, std::unique_ptr<GDX12RootSignature>> _rootSignatures;
};