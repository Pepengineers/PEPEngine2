#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include <D3D12MemAlloc.h>
#include <utility>

// Copies retain the allocation as well as the resource, including imported textures and upload batches.
class GDX12ResourceHandle
{
public:
    GDX12ResourceHandle() = default;
    GDX12ResourceHandle(ComPtr<ID3D12Resource> external) : _external(std::move(external)) {}
    GDX12ResourceHandle(const GDX12ResourceHandle&) = default;
    GDX12ResourceHandle(GDX12ResourceHandle&&) noexcept = default;

    GDX12ResourceHandle& operator=(GDX12ResourceHandle other) noexcept
    {
        _allocator.Swap(other._allocator);
        _allocation.Swap(other._allocation);
        _external.Swap(other._external);
        return *this;
    }

    ID3D12Resource* Get() const noexcept { return _allocation ? _allocation->GetResource() : _external.Get(); }
    ID3D12Resource* operator->() const noexcept { return Get(); }
    explicit operator bool() const noexcept { return Get() != nullptr; }
    D3D12MA::Allocation* GetAllocation() const noexcept { return _allocation.Get(); }
    void Reset() noexcept { *this = GDX12ResourceHandle(); }

private:
    friend class GDX12Device;
    GDX12ResourceHandle(ComPtr<D3D12MA::Allocator> allocator, ComPtr<D3D12MA::Allocation> allocation)
        : _allocator(std::move(allocator)), _allocation(std::move(allocation)) {}

    // Member order keeps the allocator alive until its last allocation is released.
    ComPtr<D3D12MA::Allocator> _allocator;
    ComPtr<D3D12MA::Allocation> _allocation;
    ComPtr<ID3D12Resource> _external;
};
