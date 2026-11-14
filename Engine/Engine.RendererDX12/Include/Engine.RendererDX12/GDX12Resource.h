#pragma once
#include <vector>
#include <utility>

#include "Engine.RendererDX12/GDX12ResourceHandle.h"

#include "Engine.RendererDX12/D3DHelpers.h"

// A recording-local tracker redirects legacy barrier helpers without changing published state.
class GDX12RecordingStates;
class GDX12Resource
{
public:
	GDX12Resource();
	~GDX12Resource() = default;

    CD3DX12_RESOURCE_BARRIER GetRenderTargetBarrier();
    CD3DX12_RESOURCE_BARRIER GetPixelShaderResourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetNonPixelShaderResourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetUnorderedAccessBarrier();
    CD3DX12_RESOURCE_BARRIER GetCopyDestBarrier();
    CD3DX12_RESOURCE_BARRIER GetCopySourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetDepthWriteBarrier();
    CD3DX12_RESOURCE_BARRIER GetDepthReadBarrier();
    CD3DX12_RESOURCE_BARRIER GetResolveSourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetResolveDestBarrier();
    CD3DX12_RESOURCE_BARRIER GetGenericReadBarrier();
    CD3DX12_RESOURCE_BARRIER GetCommonBarrier();
    CD3DX12_RESOURCE_BARRIER GetPresentBarrier();
    CD3DX12_RESOURCE_BARRIER GetIndirectArgsBarrier();
    CD3DX12_RESOURCE_BARRIER GetUAVBarrier();
    CD3DX12_RESOURCE_BARRIER GetSRVBarrier();
    CD3DX12_RESOURCE_BARRIER GetTransitionBarrier(D3D12_RESOURCE_STATES state);

    // Keep the DIRECT-published read union immutable until the COMPUTE join.
    void BeginConcurrentRead();
    void EndConcurrentRead() { _concurrentRead = false; }

    void SetCurrentState(D3D12_RESOURCE_STATES newState);
    D3D12_RESOURCE_STATES GetCurrentState();

    void Reset();

    virtual void ResetState();

	GDX12ResourceHandle D3DResource;

private:
	D3D12_RESOURCE_STATES _currentState;
    bool _concurrentRead = false;
};

// Opt-in, thread-local and strict: every physical resource must be declared by
// the packet. Boundary states are restored before the list closes.
class GDX12RecordingStates
{
public:
    using Binding = std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>;
    explicit GDX12RecordingStates(const std::vector<Binding>&);
    ~GDX12RecordingStates();
    GDX12RecordingStates(const GDX12RecordingStates&) = delete;
    GDX12RecordingStates& operator=(const GDX12RecordingStates&) = delete;
    void Finish(ID3D12GraphicsCommandList*);
    static D3D12_RESOURCE_STATES* Find(ID3D12Resource*);
private:
    std::vector<Binding> _states, _boundary;
    static thread_local GDX12RecordingStates* _active;
};
