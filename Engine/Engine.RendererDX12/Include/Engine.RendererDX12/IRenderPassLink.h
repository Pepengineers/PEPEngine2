#pragma once

class GDX12Texture;
class RenderPipelineCommonData;

class IRenderPassLink
{
public:
	virtual GDX12Texture* GetTexture() 
	{ 
		OutputDebugStringA("ERROR: Forbidden interface call\n");
		return nullptr; 
	}

	virtual bool IsInitialized() { return false; }
};