#pragma once

#include "App.Base/App.h"

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12FrameConstants.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12Texture.h"

class RenderModule final : public Module
{
public:
    RenderModule(Window* window);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize() const;


protected:
    void OnUpdate() override;
    void OnRender() override;

private:
    void BuildDescHeapsAndBackBuffer();
    void BuildRootSignatures();
    void BuildShaders();
    void BuildPSOs();
    void BuildFrameConstants();

    void UpdateMainCB() const;

protected:
    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    GameTimer timer;

    // Make sure that the declaration order is Lower level(basic structures) structures to higher(made of lower)
    // This defines destruction order and may break if done incorrectly

    std::unique_ptr<GDX12Device> _primaryDevice;
    std::unique_ptr<GDX12Device> _secondaryDevice;
    bool _dualGPUMode;

    std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> _inputLayouts;
    Window* window;

    // All of class members below should probably be put into DeviceResources class, and made for each device
    // Since all of these resources are currently existing on _primaryDevice only
    std::vector<std::unique_ptr<GDX12FrameConstants>> _frameConstants;
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
