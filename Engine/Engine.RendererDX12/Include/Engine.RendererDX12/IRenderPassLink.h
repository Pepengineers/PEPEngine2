#pragma once

class GDX12Texture;
class GDX12SharedTexture;

class IRenderPassLink
{
public:
	virtual GDX12Texture* GetTexture() = 0;
	virtual GDX12SharedTexture* GetSharedTexture() { return nullptr; }
};