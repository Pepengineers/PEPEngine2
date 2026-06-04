#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Texture;

class GDX12Material
{
public:
	std::string Name;

	GDX12Texture* Diffuse;
	GDX12Texture* Normal;
	GDX12Texture* Specular;
	GDX12Texture* RoughnessMap;
	GDX12Texture* Emissive;
	GDX12Texture* Displacement;

	float Metallic;
	float Roughness;
	Vector3 SpecularColor;
	Vector3 EmissiveColor;
	bool HasNormalMap;
	bool HasSpecularMap;
	bool HasRoughnessMap;
	bool HasEmissiveMap;
	bool UseBakedLighting;

	bool DirtyFlag;
	UINT _CBufferIndex;

private:

	friend class RenderModule;

	GDX12Material() : Name(""), Diffuse(nullptr), Normal(nullptr), Specular(nullptr), RoughnessMap(nullptr),
		Emissive(nullptr), Metallic(0.f), Roughness(1.f),
		SpecularColor(0.0f, 0.0f, 0.0f), EmissiveColor(0.0f, 0.0f, 0.0f),
		HasNormalMap(false), HasSpecularMap(false), HasRoughnessMap(false), HasEmissiveMap(false),
		UseBakedLighting(false), DirtyFlag(true), _CBufferIndex(0), _numFramesDirty(0)
	{

	}
	UINT _numFramesDirty;
};