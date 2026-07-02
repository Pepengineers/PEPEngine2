#pragma once

class GDX12Texture;
class GDX12SharedTexture;
class RenderPipelineCommonData;

class IRenderPassLink
{
public:
	virtual GDX12Texture* GetTexture() 
	{ 
		OutputDebugStringA("ERROR: Forbidden interface call\n");
		return nullptr; 
	}
	virtual GDX12SharedTexture* GetSharedTexture() 
	{
		OutputDebugStringA("ERROR: Forbidden interface call\n");
		return nullptr;
	}
	virtual RenderPipelineCommonData* GetCommonData() 
	{
		OutputDebugStringA("ERROR: Forbidden interface call\n");
		return nullptr;
	}
};