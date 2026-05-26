#include "Engine.RendererDX12/GDX12Resource.h"

GDX12Resource::GDX12Resource() : _currentState(D3D12_RESOURCE_STATE_COMMON)
{

}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetRenderTargetBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPixelShaderResourceBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetNonPixelShaderResourceBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	_currentState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetUnorderedAccessBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	_currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopyDestBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_COPY_DEST);
	_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCopySourceBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	_currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthWriteBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetDepthReadBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_DEPTH_READ);
	_currentState = D3D12_RESOURCE_STATE_DEPTH_READ;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveSourceBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	_currentState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetResolveDestBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_RESOLVE_DEST);
	_currentState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetGenericReadBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_GENERIC_READ);
	_currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetCommonBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_COMMON);
	_currentState = D3D12_RESOURCE_STATE_COMMON;
	return transition;
}

CD3DX12_RESOURCE_BARRIER GDX12Resource::GetPresentBarrier()
{
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(D3DResource.Get(),
		_currentState, D3D12_RESOURCE_STATE_PRESENT);
	_currentState = D3D12_RESOURCE_STATE_PRESENT;
	return transition;
}

void GDX12Resource::SetCurrentState(D3D12_RESOURCE_STATES newState)
{
	_currentState = newState;
}

D3D12_RESOURCE_STATES GDX12Resource::GetCurrentState()
{
	return _currentState;
}
