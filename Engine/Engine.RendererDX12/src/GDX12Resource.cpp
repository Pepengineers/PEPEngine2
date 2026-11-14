#include "Engine.RendererDX12/GDX12Resource.h"
#include <stdexcept>

GDX12Resource::GDX12Resource() : _currentState(D3D12_RESOURCE_STATE_COMMON)
{

}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetRenderTargetBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPixelShaderResourceBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetNonPixelShaderResourceBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetUnorderedAccessBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopyDestBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_COPY_DEST);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopySourceBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_COPY_SOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthWriteBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthReadBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_DEPTH_READ);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveSourceBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveDestBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_RESOLVE_DEST);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetGenericReadBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_GENERIC_READ);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCommonBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPresentBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_PRESENT);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetIndirectArgsBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetUAVBarrier()
{
    GDX12RecordingStates::Find(D3DResource.Get()); // validate the native packet allow-list
	if (_concurrentRead) throw std::logic_error("UAV barrier during concurrent resource reads");
	auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(D3DResource.Get());
	return barrier;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetSRVBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetTransitionBarrier(D3D12_RESOURCE_STATES state)
{
    if (auto* local = GDX12RecordingStates::Find(D3DResource.Get()))
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(), *local, state);
        *local = state;
        return barrier;
    }
    if (_concurrentRead)
    {
        if (state == D3D12_RESOURCE_STATE_COMMON || (state & _currentState) != state)
            throw std::logic_error("Concurrent resource reader attempted to change its published state");
        state = _currentState;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(), _currentState, state);
    _currentState = state;
    return barrier;
}

void GDX12Resource::BeginConcurrentRead()
{
    auto readStates = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (D3DResource && D3DResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        readStates |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (_concurrentRead || _currentState == D3D12_RESOURCE_STATE_COMMON || (_currentState & ~readStates))
        throw std::logic_error("Concurrent resource access requires a published read-only state");
    _concurrentRead = true;
}

void GDX12Resource::SetCurrentState(const D3D12_RESOURCE_STATES newState)
{
    if (auto* local = GDX12RecordingStates::Find(D3DResource.Get())) { *local = newState; return; }
	if (_concurrentRead && newState != _currentState)
        throw std::logic_error("Concurrent resource state changed outside its queue join");
	_currentState = newState;
}

D3D12_RESOURCE_STATES GDX12Resource::GetCurrentState()
{
    if (auto* local = GDX12RecordingStates::Find(D3DResource.Get())) return *local;
	return _currentState;
}

void GDX12Resource::Reset()
{
	D3DResource.Reset();
	_currentState = D3D12_RESOURCE_STATE_COMMON;
    _concurrentRead = false;
}

void GDX12Resource::ResetState()
{
	_currentState = D3D12_RESOURCE_STATE_COMMON;
    _concurrentRead = false;
}

thread_local GDX12RecordingStates* GDX12RecordingStates::_active = nullptr;
GDX12RecordingStates::GDX12RecordingStates(const std::vector<Binding>& states) : _states(states), _boundary(states)
{
    if (_active) throw std::logic_error("Nested recording state scope");
    _active = this;
}
GDX12RecordingStates::~GDX12RecordingStates() { _active = nullptr; }
D3D12_RESOURCE_STATES* GDX12RecordingStates::Find(ID3D12Resource* resource)
{
    if (!_active) return nullptr;
    for (auto& entry : _active->_states) if (entry.first == resource) return &entry.second;
    throw std::logic_error("Undeclared resource in native recording packet");
}
void GDX12RecordingStates::Finish(ID3D12GraphicsCommandList* commands)
{
    for (size_t i = 0; i < _states.size(); ++i)
        if (_states[i].second != _boundary[i].second)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_states[i].first,
                _states[i].second, _boundary[i].second);
            commands->ResourceBarrier(1, &barrier);
        }
}
