#pragma once

#include <cstdint>
#include <memory>
#include "Engine.RendererDX12/GDX12Submission.h"
#include "Engine.RendererDX12\D3DHelpers.h"

#include "Engine.RendererDX12/GDX12ResourceHandle.h"

class GDX12CommandQueue;

//info on device capabilities
struct DeviceSpecs
{
	D3D_FEATURE_LEVEL MaxFeatureLevel;
	std::string Name;
	size_t DedicatedVideoMemory;  // in bytes
	size_t DedicatedSystemMemory; // in bytes
	size_t SharedSystemMemory;    // in bytes
	CD3DX12FeatureSupport Features;
};

enum EDeviceRole : std::uint8_t
{
	DEVICE_ROLE_PRIMARY,
	DEVICE_ROLE_SECONDARY
};

// Initialization, queue API and reset require one externally serialized device owner.
class GDX12Device : public std::enable_shared_from_this<GDX12Device>
{
public:
	GDX12Device();
	~GDX12Device();

	HRESULT Initialize(ComPtr<IDXGIAdapter4> adapter);
	void Reset();
	// Keep the returned handle alive until the GPU finishes; a raw resource reference does not retain its allocation.
	GDX12ResourceHandle CreateResource(D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
		D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue = nullptr);
	D3D12MA::Allocator* GetMemoryAllocator() const { return _allocator.Get(); }

	const ComPtr<ID3D12Device14>& GetDevice();
	const ComPtr<ID3D12Device14>& GetCommandQueueCreationDevice();
	GDX12CommandQueue* GetCommandQueue();
    // Lazily created on opt-in. Queue creation failure keeps the DIRECT path available.
    GDX12CommandQueue* TryGetComputeQueue();
    GDX12CommandQueue* TryGetCopyQueue();
    GDX12Submission::AdapterId GetSubmissionAdapterId() const { return _submissionAdapterId; }
	const DeviceSpecs& GetDeviceFeatures();
	LUID GetAdapterLuid() const;
	const bool IsInitialized();

	EDeviceRole Role;
private:
    friend class GDX12CommandQueue;
    GDX12CommandQueue* FindSubmissionQueue(GDX12Submission::TimelineId timeline) const;
	void CollectDeviceFeatures();

	ComPtr<IDXGIAdapter4> _adapter;
	ComPtr<ID3D12Device14> _device;
	ComPtr<ID3D12Device14> _proxyDevice;
	ComPtr<D3D12MA::Allocator> _allocator;

	std::unique_ptr<GDX12CommandQueue> _commandQueue;
    std::unique_ptr<GDX12CommandQueue> _computeQueue;
    bool _computeQueueAttempted = false;
    std::unique_ptr<GDX12CommandQueue> _copyQueue;
    bool _copyQueueAttempted = false;
    GDX12Submission::AdapterId _submissionAdapterId = GDX12Submission::AdapterId::Invalid;
    std::vector<GDX12CommandQueue*> _submissionQueues;

	bool _isInitialized;

	UINT _rtvDescriptorSize;
	UINT _dsvDescriptorSize;
	UINT _cbvSrvUavDescriptorSize;

	DeviceSpecs _specs;
};
