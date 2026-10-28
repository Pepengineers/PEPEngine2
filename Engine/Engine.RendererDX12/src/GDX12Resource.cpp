#include "Engine.RendererDX12/GDX12Resource.h"

#include <mutex>
#include <stdexcept>

struct GDX12Resource::ResourceState
{
    ComPtr<ID3D12Resource> Resource;
    D3D12_RESOURCE_STATES Current = D3D12_RESOURCE_STATE_COMMON;
};

GDX12Resource::GDX12Resource() = default;

GDX12Resource::ResourceState& GDX12Resource::State()
{
    if (_state && _state->Resource.Get() == D3DResource.Get()) { return *_state; }
    static std::mutex registryMutex;
    static std::unordered_map<ID3D12Resource*, std::weak_ptr<ResourceState>> registry;
    std::lock_guard<std::mutex> lock(registryMutex);
    _state.reset();
    if (D3DResource)
    {
        auto found = registry.find(D3DResource.Get());
        if (found != registry.end()) { _state = found->second.lock(); }
    }
    if (!_state)
    {
        for (auto it = registry.begin(); it != registry.end();)
        {
            if (it->second.expired()) { it = registry.erase(it); }
            else { ++it; }
        }
        _state = std::make_shared<ResourceState>();
        _state->Resource = D3DResource;
        if (D3DResource) { registry[D3DResource.Get()] = _state; }
    }
    return *_state;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetTransitionBarrier(D3D12_RESOURCE_STATES newState)
{
    if (!D3DResource) { throw std::runtime_error("ERROR: Given GPU resource has no native allocation to transition. Initialize the GDX12Resource before "
        "requesting a state transition."); }
    auto& state = State();
    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(), state.Current, newState);
    state.Current = newState;
    return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetRenderTargetBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPixelShaderResourceBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetNonPixelShaderResourceBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetUnorderedAccessBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopyDestBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_COPY_DEST); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopySourceBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_COPY_SOURCE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthWriteBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_DEPTH_WRITE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthReadBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_DEPTH_READ); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveSourceBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_RESOLVE_SOURCE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveDestBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_RESOLVE_DEST); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetGenericReadBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_GENERIC_READ); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCommonBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPresentBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_PRESENT); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetIndirectArgsBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetSRVBarrier() { return GetTransitionBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
CD3DX12_RESOURCE_BARRIER GDX12Resource::GetUAVBarrier() { return CD3DX12_RESOURCE_BARRIER::UAV(D3DResource.Get()); }

void GDX12Resource::SetCurrentState(D3D12_RESOURCE_STATES newState) { State().Current = newState; }
D3D12_RESOURCE_STATES GDX12Resource::GetCurrentState() { return State().Current; }
void GDX12Resource::ResetState() { State().Current = D3D12_RESOURCE_STATE_COMMON; }
void GDX12Resource::Reset()
{
    D3DResource.Reset();
    _state.reset();
}
