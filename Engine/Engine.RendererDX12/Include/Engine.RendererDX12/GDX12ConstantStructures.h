#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

struct GDX12MainConstants
{
    DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;
};

struct GDX12MeshConstants
{
    DirectX::XMFLOAT4X4 WorldMatrix = Identity4x4();
};

struct GDX12MaterialConstants
{
    DirectX::XMFLOAT3 Color = { 1.f, 1.f, 1.f };
    float Roughness = 0.5f;
    float Metallic = 0.5f;
    float _pad1;
    float _pad2;
    float _pad3;
};