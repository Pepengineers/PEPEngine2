#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12Texture;
class GDX12DescriptorHeap;

class GDX12BackBuffer
{
public:
    GDX12BackBuffer(GDX12Device* device, HWND hwnd,
        DXGI_FORMAT format, UINT bufferCount, UINT width, UINT height, 
        GDX12DescriptorHeap* rtvHeap);
    ~GDX12BackBuffer();

    void Resize(UINT width, UINT height);
    void Present();

    DXGI_FORMAT GetFormat();
    UINT GetBufferCount();
    UINT GetCurrentBufferIndex();

    GDX12Texture* GetCurrentBuffer();
    GDX12Texture* GetBuffer(UINT index);
    D3D12_VIEWPORT GetViewport();
    D3D12_RECT GetScissorRect();

    ComPtr<IDXGISwapChain4> GetSwapChain();

    void Reset();

private:
    void CreateBuffers();

    GDX12Device* _device;
    ComPtr<IDXGISwapChain4> _swapChain;
    GDX12DescriptorHeap* _rtvHeap;

    DXGI_FORMAT _format;
    UINT _bufferCount;
    UINT _currentBufferIndex;
    HWND _hwnd;
    UINT _width;
    UINT _height;

    std::vector<std::unique_ptr<GDX12Texture>> _buffers;

    D3D12_VIEWPORT _screenViewport;
    D3D12_RECT _screenScissorRect;
};
