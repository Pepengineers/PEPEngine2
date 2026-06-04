#include <Engine.RendererDX12/GDX12SwapChain.h>

#include <Engine.RendererDX12/GDX12DeviceFactory.h>
#include <Engine.RendererDX12/GDX12Device.h>
#include <Engine.RendererDX12/GDX12Texture.h>
#include <Engine.RendererDX12/GDX12DescriptorHeap.h>

GDX12SwapChain::GDX12SwapChain(GDX12Device* device, HWND hwnd,
    DXGI_FORMAT format, UINT bufferCount, UINT width, UINT height, GDX12DescriptorHeap* rtvHeap)
    : _device(device)
    , _hwnd(hwnd)
    , _format(format)
    , _bufferCount(bufferCount)
    , _currentBufferIndex(0)
    , _width(width)
    , _height(height)
    , _rtvHeap(rtvHeap)
{
    Reset();

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

    _swapChain = GDX12DeviceFactory::CreateSwapChain(_device, swapChainDesc, _hwnd);

    CreateBuffers();

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
    Reset();
}

void GDX12SwapChain::CreateBuffers()
{
    _buffers.clear();
    _buffers.reserve(_bufferCount);

    for (UINT i = 0; i < _bufferCount; i++)
    {
        ComPtr<ID3D12Resource> backBuffer;
       _swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

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

        _buffers.push_back(std::make_unique<GDX12Texture>(textureDesc));
    }
}

void GDX12SwapChain::Resize(UINT width, UINT height)
{
    if (_width == width && _height == height) { return; }

    _width = width;
    _height = height;

    _buffers.clear();

    DXGI_SWAP_CHAIN_DESC desc;
    _swapChain->GetDesc(&desc);

    _swapChain->ResizeBuffers(
        _bufferCount, _width, _height,
        desc.BufferDesc.Format, desc.Flags);

    CreateBuffers();
    _currentBufferIndex = _swapChain->GetCurrentBackBufferIndex();

    _screenViewport.TopLeftX = 0;
    _screenViewport.TopLeftY = 0;
    _screenViewport.Width = static_cast<FLOAT>(_width);
    _screenViewport.Height = static_cast<FLOAT>(_height);
    _screenViewport.MinDepth = 0.0f;
    _screenViewport.MaxDepth = 1.0f;

    _screenScissorRect = { 0, 0, static_cast<int>(_width), static_cast<int>(_height) };
}

void GDX12SwapChain::Present()
{
    _swapChain->Present(0u, DXGI_PRESENT_ALLOW_TEARING);
    _currentBufferIndex = (_currentBufferIndex + 1) % _bufferCount;
}

D3D12_VIEWPORT GDX12SwapChain::GetViewport()
{
    return _screenViewport;
}

D3D12_RECT GDX12SwapChain::GetScissorRect()
{
    return _screenScissorRect;
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

GDX12Texture* GDX12SwapChain::GetBuffer(UINT index)
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
    _buffers.clear();
    _swapChain.Reset();
    _currentBufferIndex = 0;
}
