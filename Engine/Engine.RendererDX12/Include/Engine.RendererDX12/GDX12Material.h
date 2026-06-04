#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Texture;

class GDX12Material
{
public:
	std::string Name;

	GDX12Texture* Diffuse;
	GDX12Texture* Normal;
	GDX12Texture* Displacement;

	float Metallic;
	float Roughness;

	bool DirtyFlag;
	UINT _CBufferIndex;

private:

	friend class RenderModule;

	GDX12Material() : Name(""), Diffuse(nullptr), Normal(nullptr), Displacement(nullptr),
		Metallic(0.f), Roughness(0.f), DirtyFlag(true), _CBufferIndex(0), _numFramesDirty(0)
	{

	}
	UINT _numFramesDirty;
};