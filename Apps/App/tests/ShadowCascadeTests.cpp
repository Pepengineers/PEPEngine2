#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include <cassert>
#include <iostream>
#include <limits>

int main()
{
    const float fov = XMConvertToRadians(60.0f);
    const float aspect = 16.0f / 9.0f;
    const Vector3 sun(0.35f, -1.0f, 0.25f);
    for (float farPlane : { 1000.0f, 10000.0f })
    {
        const Matrix inverseView = Matrix::CreateFromYawPitchRoll(0.4f, -0.2f, 0.0f) *
            Matrix::CreateTranslation(300.0f, 150.0f, -900.0f);
        const auto cascades = BuildShadowCascades(inverseView, fov, aspect, 0.1f, farPlane, sun);
        const float splits[] = { cascades.SplitDepths.x, cascades.SplitDepths.y, cascades.SplitDepths.z, cascades.SplitDepths.w };
        const float texels[] = { cascades.TexelWorldSize.x, cascades.TexelWorldSize.y, cascades.TexelWorldSize.z, cascades.TexelWorldSize.w };
        assert(std::abs(splits[3] - (std::min)(farPlane, 6000.0f)) < 0.01f);
        float previous = 0.1f;
        for (int cascade = 0; cascade < 4; ++cascade)
        {
            assert(splits[cascade] > previous && texels[cascade] > 0.0f);
            const Matrix lightViewProj = Matrix(cascades.ViewProj[cascade]).Transpose();
            for (float depth : { previous, splits[cascade] })
            {
                const float height = depth * std::tan(fov * 0.5f);
                for (float y : { -height, height })
                    for (float x : { -height * aspect, height * aspect })
                    {
                        const auto world = Vector3::Transform(Vector3(x, y, -depth), inverseView);
                        const auto clip = Vector3::Transform(world, lightViewProj);
                        assert(std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z));
                        assert(std::abs(clip.x) <= 1.001f && std::abs(clip.y) <= 1.001f);
                        assert(clip.z >= 0.0f && clip.z <= 1.0f);
                    }
            }
            previous = splits[cascade] - (splits[cascade] - previous) * 0.1f;
        }
    }

    float previousTexelSize = (std::numeric_limits<float>::max)();
    for (UINT resolution : { 512u, 1024u, 2048u, 4096u })
    {
        const auto first = BuildShadowCascades(Matrix::Identity, fov, aspect, 0.1f, 10000.0f, sun, nullptr, resolution);
        const auto shifted = BuildShadowCascades(Matrix::CreateTranslation(0.00001f, 0, 0),
            fov, aspect, 0.1f, 10000.0f, sun, nullptr, resolution);
        assert(first.TexelWorldSize.x < previousTexelSize);
        previousTexelSize = first.TexelWorldSize.x;
        for (int cascade = 0; cascade < 4; ++cascade)
        {
            // Crossing a grid boundary may move one texel; fractional texel motion must not occur.
            const float dx = (first.ViewProj[cascade]._14 - shifted.ViewProj[cascade]._14) * (0.5f * resolution);
            const float dy = (first.ViewProj[cascade]._24 - shifted.ViewProj[cascade]._24) * (0.5f * resolution);
            assert(std::abs(dx - std::round(dx)) < 0.001f);
            assert(std::abs(dy - std::round(dy)) < 0.001f);
        }
    }
    std::cout << "CSM: ordered splits, receiver coverage, finite depth, and texel stabilization passed.\n";
}
