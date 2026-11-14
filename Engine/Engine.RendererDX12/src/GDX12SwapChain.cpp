#include <Engine.RendererDX12/GDX12SwapChain.h>

#include <Engine.RendererDX12/GDX12DeviceFactory.h>
#include <Engine.RendererDX12/GDX12Device.h>
#include <Engine.RendererDX12/GDX12Texture.h>
#include <Engine.RendererDX12/GDX12DescriptorHeap.h>
#include <Engine.RendererDX12/GDX12StreamlineSDK.h>
#include <Engine.RendererDX12/GDX12FrameGeneration.h>
#include <Engine.RendererDX12/RenderPasses/GDX12RenderPass.h>
#include <Engine.Core/Logging/Logger.h>

GDX12SwapChain::GDX12SwapChain(GDX12Device* device, const HWND hwnd,
    const DXGI_FORMAT format, const UINT bufferCount, const UINT width, const UINT height, GDX12DescriptorHeap* rtvHeap)
    : _device(device), _hwnd(hwnd),
    _format(format), _bufferCount(bufferCount), _currentBufferIndex(0),
    _width(width), _height(height), _rtvHeap(rtvHeap), bVSyncEnabled(false)
{
    _frameGeneration = std::make_unique<GDX12FrameGeneration>(_device);
    CreatePresentation(false);
}

void GDX12SwapChain::CreatePresentation(const bool applySettings)
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = _width;
    swapChainDesc.Height = _height;
    swapChainDesc.Format = _format;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = _bufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    _proxySwapChain = _frameGeneration->CreateSwapChain(_hwnd, swapChainDesc, applySettings);
    _swapChain = GDX12StreamlineSDK::Get().GetNativeSwapChain(_proxySwapChain);
    CreateBuffers();
    _currentBufferIndex = GetPresentationSwapChain()->GetCurrentBackBufferIndex();

    _screenViewport.TopLeftX = 0;
    _screenViewport.TopLeftY = 0;
    _screenViewport.Width = static_cast<FLOAT>(_width);
    _screenViewport.Height = static_cast<FLOAT>(_height);
    _screenViewport.MinDepth = 0.0f;
    _screenViewport.MaxDepth = 1.0f;

    _screenScissorRect = { 0, 0, static_cast<int>(_width), static_cast<int>(_height) };
}

GDX12SwapChain::~GDX12SwapChain()
{
    try { Reset(); }
    catch (const std::exception& e) { Log::Error("FrameGeneration", "Swapchain shutdown: ", e.what()); }
}

void GDX12SwapChain::CreateBuffers()
{
    _buffers.clear();
    _buffers.reserve(_bufferCount);

    for (UINT i = 0; i < _bufferCount; i++)
    {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(GetPresentationSwapChain()->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
        backBuffer = GDX12StreamlineSDK::Get().GetNativeResource(backBuffer);

        GDX12TextureDesc textureDesc;
        textureDesc.RTVHeap = _rtvHeap;
        textureDesc.Width = _width;
        textureDesc.Height = _height;
        textureDesc.Format = _format;
        textureDesc.CreateSRV = false;
        textureDesc.ClearValue = { 0.f, 0.f, 0.f, 1.f };

        textureDesc.CreateRTV = true;
        textureDesc.RTVHeapIndex = _rtvHeap->GetAvailableIndex();
        textureDesc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        textureDesc.RTVDesc.Format = _format;
        textureDesc.RTVDesc.Texture2D.PlaneSlice = 0;
        textureDesc.RTVDesc.Texture2D.MipSlice = 0;

        textureDesc.ExternalResource = backBuffer;

        _buffers.push_back(std::make_unique<GDX12Texture>());
        _buffers[i]->Initialize(textureDesc);
    }
}

void GDX12SwapChain::Resize(const UINT width, const UINT height)
{
    if (!width || !height || (_width == width && _height == height)) return;

    Reset();

    _width = width;
    _height = height;

    CreatePresentation(true);
}

void GDX12SwapChain::Present()
{
    UINT syncInterval = bVSyncEnabled ? 1 : 0;
    UINT presentFlags = bVSyncEnabled ? 0 : DXGI_PRESENT_ALLOW_TEARING;
    _frameGeneration->BeforePresent();
    ThrowIfFailed(GetPresentationSwapChain()->Present(syncInterval, presentFlags));
    _currentBufferIndex = GetPresentationSwapChain()->GetCurrentBackBufferIndex();
    _frameGeneration->AfterPresent();
}

bool GDX12SwapChain::NeedsPresentationReset() const { return _frameGeneration->NeedsRecreate(); }

void GDX12SwapChain::BeginFrame(RenderPipelineCommonData& common)
{
    if (_frameGeneration->NeedsRecreate())
    {
        Reset(); CreatePresentation(true);
        common.RequestHistoryReset = common.ResetUpscalerHistory = true;
    }
    _frameGeneration->BeginFrame(common);
}

void GDX12SwapChain::SetFrameGenerationSuspended(const bool suspended)
{
    if (_frameGeneration->GetStatus().Suspended == suspended) return;
    _device->GetCommandQueue()->Flush();
    _frameGeneration->SetSuspended(suspended);
}

GDX12FrameGeneration& GDX12SwapChain::GetFrameGeneration() { return *_frameGeneration; }

D3D12_VIEWPORT GDX12SwapChain::GetViewport()
{
    return _screenViewport;
}

D3D12_RECT GDX12SwapChain::GetScissorRect()
{
    return _screenScissorRect;
}

GDX12Texture* GDX12SwapChain::GetTexture()
{
    return GetCurrentBuffer();
}

bool GDX12SwapChain::IsInitialized()
{
    return true;
}

DXGI_FORMAT GDX12SwapChain::GetFormat()
{
    return _format;
}

UINT GDX12SwapChain::GetBufferCount()
{
    return _bufferCount;
}

UINT GDX12SwapChain::GetCurrentBufferIndex()
{
    return _currentBufferIndex;
}

GDX12Texture* GDX12SwapChain::GetCurrentBuffer()
{
    return _buffers[_currentBufferIndex].get();
}

GDX12Texture* GDX12SwapChain::GetBuffer(const UINT index)
{
    if (index >= _buffers.size())
    {
        OutputDebugStringA("ERROR: Unexisting back buffer index provided");
        return nullptr;
    }

    return _buffers[index].get();
}

const ComPtr<IDXGISwapChain4>& GDX12SwapChain::GetSwapChain()
{
    return _swapChain;
}

void GDX12SwapChain::Reset()
{
    _device->GetCommandQueue()->Flush();
    if (_frameGeneration) _frameGeneration->WaitForIdle();
    _buffers.clear();
    if (_frameGeneration) _frameGeneration->DestroySwapChain(_proxySwapChain, _swapChain);
    else { _proxySwapChain.Reset(); _swapChain.Reset(); }
    _currentBufferIndex = 0;
}

IDXGISwapChain4* GDX12SwapChain::GetPresentationSwapChain() const
{
    return (_proxySwapChain ? _proxySwapChain : _swapChain).Get();
}
