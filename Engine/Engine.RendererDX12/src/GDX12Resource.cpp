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
	if (_concurrentRead) throw std::logic_error("UAV barrier during concurrent texture reads");
	auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(D3DResource.Get());
	return barrier;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetSRVBarrier()
{
	return GetTransitionBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetTransitionBarrier(D3D12_RESOURCE_STATES state)
{
    if (_concurrentRead)
    {
        if (state == D3D12_RESOURCE_STATE_COMMON || (state & _currentState) != state)
            throw std::logic_error("Concurrent texture reader attempted to change its published state");
        state = _currentState;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(), _currentState, state);
    _currentState = state;
    return barrier;
}

void GDX12Resource::BeginConcurrentRead()
{
    constexpr auto readStates = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (_concurrentRead || _currentState == D3D12_RESOURCE_STATE_COMMON || (_currentState & ~readStates))
        throw std::logic_error("Concurrent texture access requires a published read-only state");
    _concurrentRead = true;
}

void GDX12Resource::SetCurrentState(const D3D12_RESOURCE_STATES newState)
{
	if (_concurrentRead && newState != _currentState)
        throw std::logic_error("Concurrent texture state changed outside its queue join");
	_currentState = newState;
}

D3D12_RESOURCE_STATES GDX12Resource::GetCurrentState()
{
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
