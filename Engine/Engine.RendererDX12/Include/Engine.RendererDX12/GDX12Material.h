#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.Core/Types/TextureTypes.h"

class GDX12Texture;

enum class MaterialType
{
	Opaque = 0,
	Masked = 1,
	Transparent = 2,
};

struct GPUTexture
{
	std::string Name;
	GDX12Texture* PrimaryDeviceTexture;
	GDX12Texture* SecondaryDeviceTexture;
};

class GDX12Material
{
public:
	std::string Name;

	GPUTexture* Diffuse;
	GPUTexture* Normal;
	GPUTexture* Specular;
	GPUTexture* RoughnessMap;
	GPUTexture* Emissive;
	GPUTexture* Displacement;

	float Metallic;
	float Roughness;
	float Opacity;
	Vector3 SpecularColor;
	Vector3 EmissiveColor;
	bool HasNormalMap;
	bool HasSpecularMap;
	bool HasRoughnessMap;
	bool HasEmissiveMap;
	bool UseBakedLighting;
	MaterialType Type;
	bool DirtyFlag;
	UINT _PrimaryCBufferIndex;
	UINT _SecondaryCBufferIndex;

private:
	friend class RenderModule;
	friend class GDX12DeviceResources;

	GDX12Material() : Name(""), Diffuse(nullptr), Normal(nullptr), Specular(nullptr), RoughnessMap(nullptr),
		Emissive(nullptr), Displacement(nullptr), Metallic(0.f), Roughness(1.f), Opacity(1.f),
		SpecularColor(0.0f, 0.0f, 0.0f), EmissiveColor(0.0f, 0.0f, 0.0f),
		HasNormalMap(false), HasSpecularMap(false), HasRoughnessMap(false), HasEmissiveMap(false),
		UseBakedLighting(false), DirtyFlag(true), _PrimaryCBufferIndex(0), _SecondaryCBufferIndex(0), _numFramesDirty(0), Type(MaterialType::Opaque)
	{

	}
	UINT _numFramesDirty;
};
