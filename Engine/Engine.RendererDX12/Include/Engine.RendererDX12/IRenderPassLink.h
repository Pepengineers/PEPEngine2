#pragma once

class GDX12Texture;

class IRenderPassLink
{
public:
	virtual GDX12Texture* GetTexture() = 0;
};