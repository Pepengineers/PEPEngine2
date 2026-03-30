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
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

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
	void BuildDescHeapsAndBackBuffer();
	void BuildRootSignatures();
	void BuildShaders();
	void BuildPSOs();
	void BuildFrameConstants();

	void UpdateMainCB();

	HWND _windowHandle;
	GameTimer* _gameTimer;

	// Make sure that the declaration order is Lower level(basic structures) structures to higher(made of lower)
	// This defines destruction order and may break if done incorrectly

	std::unique_ptr<GDX12Device> _primaryDevice;
	std::unique_ptr<GDX12Device> _secondaryDevice;
	bool _dualGPUMode;

	UINT _windowWidth;
	UINT _windowHeight;

	std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> _inputLayouts;

	static constexpr UINT _numFrameConstants = 3;

	// All of class members below should probably be put into DeviceResources class, and made for each device
	// Since all of these resources are currently existing on _primaryDevice only
	std::array<std::unique_ptr<GDX12FrameConstants>, _numFrameConstants> _frameConstants;
	UINT _currFrameConstantsIndex;

	// All heaps created in one high-capacity instance
	std::unique_ptr<GDX12DescriptorHeap> _rtvHeap;
	std::unique_ptr<GDX12DescriptorHeap> _srvuavHeap;
	std::unique_ptr<GDX12DescriptorHeap> _dsvHeap;

	std::unordered_map<std::string, ComPtr<ID3DBlob>> _shaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> _PSOs;
	std::unordered_map<std::string, std::unique_ptr<GDX12RootSignature>> _rootSignatures;

	// These two resources are made on _primaryDevice only
	std::unique_ptr<GDX12SwapChain> _backBuffer;
	std::unique_ptr<GDX12Texture> _depthStencil;
};