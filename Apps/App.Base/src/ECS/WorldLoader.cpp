// TODO move helpers to separate utility libs
#include "App.Base/ECS/WorldLoader.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoadContext.h"
#include "App.Base/Modules/RenderModule.h"
#include "Common/Logger.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Types/TextureTypes.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Windows.h>

namespace
{
    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open world file: " + path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::filesystem::path ResolveWorldResourcePath(
        const WorldLoadContext& context,
        const std::filesystem::path& sourcePath)
    {
        if (sourcePath.empty() || sourcePath.is_absolute())
        {
            return sourcePath.lexically_normal();
        }

        std::error_code errorCode;
        if (std::filesystem::exists(sourcePath, errorCode))
        {
            return std::filesystem::absolute(sourcePath, errorCode).lexically_normal();
        }

        const std::filesystem::path worldRelativePath =
            (context.WorldDirectory / sourcePath).lexically_normal();
        errorCode.clear();
        if (std::filesystem::exists(worldRelativePath, errorCode))
        {
            return std::filesystem::absolute(worldRelativePath, errorCode).lexically_normal();
        }

        // Asset registries apply their own conventional roots to unresolved relative paths.
        return sourcePath.lexically_normal();
    }

    /// Converts an ASCII string to lowercase.
    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value;
    }

    /// Returns true if the path points to an obj file.
    bool IsObjPath(const std::filesystem::path& path)
    {
        return ToLowerAscii(path.extension().string()) == ".obj";
    }

    /// Replaces characters that are unsafe for renderer resource names with underscores.
    /// Returns "scene" for an empty input string.
    std::string SanitizeResourceName(std::string value)
    {
        for (char& character : value)
        {
            const unsigned char unsignedCharacter = static_cast<unsigned char>(character);
            if (!std::isalnum(unsignedCharacter))
            {
                character = '_';
            }
        }

        if (value.empty())
        {
            return "scene";
        }

        return value;
    }

    /// Builds a stable renderer resource name for one obj file inside a scene directory.
    /// The name is based on the path relative to sceneRoot to avoid collisions between files with the same filename.
    std::string BuildScenePartName(const std::filesystem::path& sceneRoot, const std::filesystem::path& objPath)
    {
        std::filesystem::path relativePath = objPath.lexically_relative(sceneRoot);
        if (relativePath.empty() || relativePath.native().find(L"..") == 0)
        {
            relativePath = objPath.filename();
        }

        relativePath.replace_extension();
        return SanitizeResourceName(relativePath.generic_string());
    }

    /// Recursively collects all obj files under the given scene directory.
    std::vector<std::filesystem::path> CollectSceneObjPaths(const std::filesystem::path& sceneDirectory)
    {
        std::vector<std::filesystem::path> objPaths;

        std::error_code errorCode;
        std::filesystem::recursive_directory_iterator iterator(
            sceneDirectory,
            std::filesystem::directory_options::skip_permission_denied, /* so that it does not crash on folders it does not have access to */
            errorCode);

        const std::filesystem::recursive_directory_iterator endIterator;
        while (!errorCode && iterator != endIterator)
        {
            const std::filesystem::directory_entry& entry = *iterator;
            if (entry.is_regular_file(errorCode) && IsObjPath(entry.path()))
            {
                objPaths.push_back(entry.path());
            }

            iterator.increment(errorCode);
        }

        std::sort(objPaths.begin(), objPaths.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) { return left.generic_wstring() < right.generic_wstring(); });
        return objPaths;
    }

    /// Returns the number of material slots required by the mesh submeshes.
    /// At least one slot is returned so meshes without material indices still have a fallback material.
    size_t GetMaterialSlotCount(const Engine::Core::Mesh& mesh)
    {
        size_t materialSlotCount = 1;

        for (const Engine::Core::SubMesh& subMesh : mesh.GetSubMeshes())
        {
            const size_t requiredSlotCount = static_cast<size_t>(subMesh.MaterialIndex) + 1;
            if (requiredSlotCount > materialSlotCount)
            {
                materialSlotCount = requiredSlotCount;
            }
        }

        return materialSlotCount;
    }

    /// Builds a material slot array filled with the given fallback material.
    std::vector<GDX12Material*> BuildFallbackMaterialSlots(const Engine::Core::Mesh& mesh, GDX12Material* fallbackMaterial)
    {
        return std::vector<GDX12Material*>(GetMaterialSlotCount(mesh), fallbackMaterial);
    }

    /// Creates a CPU-side 1x1 RGBA texture with the given color.
    Engine::Core::Texture CreateSingleColorTexture(
        const std::uint8_t red,
        const std::uint8_t green,
        const std::uint8_t blue,
        const std::uint8_t alpha,
        const Engine::Core::ETextureType textureType)
    {
        Engine::Core::TextureDesc textureDesc = {};
        textureDesc.Dimension = Engine::Core::ETextureDimension::Texture2D;
        textureDesc.Type = textureType;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.Width = 1;
        textureDesc.Height = 1;
        textureDesc.Depth = 1;
        textureDesc.ArraySize = 1;
        textureDesc.MipLevels = 1;

        Engine::Core::SubTexture subTexture = {};
        subTexture.Width = 1;
        subTexture.Height = 1;
        subTexture.Depth = 1;
        subTexture.RowPitch = 4;
        subTexture.SlicePitch = 4;
        subTexture.Data =
        {
            static_cast<std::byte>(red),
            static_cast<std::byte>(green),
            static_cast<std::byte>(blue),
            static_cast<std::byte>(alpha)
        };

        std::vector<Engine::Core::SubTexture> subresources;
        subresources.push_back(std::move(subTexture));
        return Engine::Core::Texture(textureDesc, std::move(subresources));
    }

    /// Creates or retrieves a GPU texture backed by a generated solid-color CPU texture.
    GDX12Texture* CreateSolidGpuTexture(
        RenderModule& renderModule,
        const std::string& textureName,
        const std::uint8_t red,
        const std::uint8_t green,
        const std::uint8_t blue,
        const std::uint8_t alpha,
        const Engine::Core::ETextureType textureType)
    {
        Engine::Core::Texture texture = CreateSingleColorTexture(red, green, blue, alpha, textureType);

        GDX12Texture* gpuTexture = renderModule.CreateTexture(textureName, &texture);
        if (gpuTexture == nullptr)
        {
            gpuTexture = renderModule.GetTextureByName(textureName);
        }

        return gpuTexture;
    }

    /// Converts a normalized color component to an 8-bit texture channel value.
    std::uint8_t ColorComponentToByte(const float value)
    {
        const float clampedValue = (std::max)(0.0f, (std::min)(value, 1.0f));
        return static_cast<std::uint8_t>(clampedValue * 255.0f + 0.5f);
    }

    /// Creates or retrieves a generated diffuse texture from the imported material diffuse color.
    GDX12Texture* CreateDiffuseColorTexture(
        RenderModule& renderModule,
        const std::string& sceneName,
        const Engine::Core::MeshMaterial& material)
    {
        const std::uint8_t red = ColorComponentToByte(material.DiffuseColor.x);
        const std::uint8_t green = ColorComponentToByte(material.DiffuseColor.y);
        const std::uint8_t blue = ColorComponentToByte(material.DiffuseColor.z);

        const std::string textureName = sceneName + "_DiffuseColor_" + std::to_string(red) + "_" + std::to_string(green) + "_" + std::to_string(blue);

        return CreateSolidGpuTexture(renderModule, textureName, red, green, blue, 255, Engine::Core::ETextureType::Color);
    }

    struct SceneDefaultMaterialTextures
    {
        GDX12Texture* FlatNormal = nullptr;
        GDX12Texture* White = nullptr;
        GDX12Texture* Black = nullptr;

        [[nodiscard]] bool IsValid() const
        {
            return FlatNormal != nullptr && White != nullptr && Black != nullptr;
        }
    };

    /// Creates shared default GPU textures used by imported scene materials.
    /// These textures provide neutral values for missing normal, specular, roughness, and emissive maps.
    SceneDefaultMaterialTextures CreateSceneDefaultMaterialTextures(RenderModule& renderModule, const std::string& sceneName)
    {
        SceneDefaultMaterialTextures textures = {};
        textures.FlatNormal = CreateSolidGpuTexture(renderModule, sceneName + "_DefaultFlatNormal", 128, 128, 255, 255, Engine::Core::ETextureType::Data);
        textures.White = CreateSolidGpuTexture(renderModule, sceneName + "_DefaultWhite", 255, 255, 255, 255, Engine::Core::ETextureType::Data);
        textures.Black = CreateSolidGpuTexture(renderModule, sceneName + "_DefaultBlack", 0, 0, 0, 255, Engine::Core::ETextureType::Data);
        return textures;
    }

    struct MaterialTextureSet
    {
        GDX12Texture* Diffuse = nullptr;
        GDX12Texture* Normal = nullptr;
        GDX12Texture* Specular = nullptr;
        GDX12Texture* Roughness = nullptr;
        GDX12Texture* Emissive = nullptr;
        bool HasNormalMap = false;
        bool HasSpecularMap = false;
        bool HasRoughnessMap = false;
        bool HasEmissiveMap = false;
    };

    /// Builds a GPU texture cache key from source path and texture semantic type.
    std::wstring BuildTextureCacheKey(const std::filesystem::path& texturePath, const Engine::Core::ETextureType textureType)
    {
        std::wstring textureKey = texturePath.lexically_normal().generic_wstring();
        textureKey += L"#type:";
        textureKey += std::to_wstring(static_cast<std::uint32_t>(textureType));
        return textureKey;
    }

    /// Builds a GPU texture cache key for a diffuse texture with an opacity mask packed into its alpha channel.
    std::wstring BuildMergedOpacityTextureCacheKey(
        const std::filesystem::path& diffuseTexturePath,
        const std::filesystem::path& opacityTexturePath)
    {
        std::wstring textureKey = BuildTextureCacheKey(diffuseTexturePath, Engine::Core::ETextureType::Color);
        textureKey += L"#opacity:";
        textureKey += opacityTexturePath.lexically_normal().generic_wstring();
        return textureKey;
    }

    /// Returns true if both textures can be merged as CPU-side RGBA8 images.
    bool CanMergeOpacityIntoDiffuse(const Engine::Core::Texture& diffuseTexture, const Engine::Core::Texture& opacityTexture)
    {
        return diffuseTexture.GetDimension() == Engine::Core::ETextureDimension::Texture2D &&
            opacityTexture.GetDimension() == Engine::Core::ETextureDimension::Texture2D &&
            diffuseTexture.GetFormat() == DXGI_FORMAT_R8G8B8A8_UNORM &&
            opacityTexture.GetFormat() == DXGI_FORMAT_R8G8B8A8_UNORM &&
            diffuseTexture.GetDepth() == 1 &&
            opacityTexture.GetDepth() == 1 &&
            diffuseTexture.GetArraySize() >= 1 &&
            opacityTexture.GetArraySize() >= 1 &&
            diffuseTexture.GetMipLevels() >= 1 &&
            opacityTexture.GetMipLevels() >= 1;
    }

    /// Creates a copy of diffuseTexture with opacityTexture packed into the alpha channel.
    /// The opacity texture is sampled from its red channel and multiplied with the existing diffuse alpha.
    std::unique_ptr<Engine::Core::Texture> TryCreateDiffuseTextureWithOpacityAlpha(
        const Engine::Core::Texture& diffuseTexture,
        const Engine::Core::Texture& opacityTexture)
    {
        if (!CanMergeOpacityIntoDiffuse(diffuseTexture, opacityTexture))
        {
            return nullptr;
        }

        Engine::Core::TextureDesc textureDesc = {};
        textureDesc.Dimension = Engine::Core::ETextureDimension::Texture2D;
        textureDesc.Type = Engine::Core::ETextureType::Color;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.Width = diffuseTexture.GetWidth();
        textureDesc.Height = diffuseTexture.GetHeight();
        textureDesc.Depth = 1;
        textureDesc.ArraySize = diffuseTexture.GetArraySize();
        textureDesc.MipLevels = diffuseTexture.GetMipLevels();

        std::vector<Engine::Core::SubTexture> subresources;
        subresources.reserve(diffuseTexture.GetSubresourceCount());

        for (std::uint32_t arraySlice = 0; arraySlice < diffuseTexture.GetArraySize(); ++arraySlice)
        {
            const std::uint32_t opacityArraySlice = (std::min)(arraySlice, opacityTexture.GetArraySize() - 1);
            for (std::uint32_t mipLevel = 0; mipLevel < diffuseTexture.GetMipLevels(); ++mipLevel)
            {
                const Engine::Core::SubTexture& diffuseSubresource = diffuseTexture.GetSubresource(mipLevel, arraySlice);
                const std::uint32_t opacityMipLevel = (std::min)(mipLevel, opacityTexture.GetMipLevels() - 1);
                const Engine::Core::SubTexture& opacitySubresource = opacityTexture.GetSubresource(opacityMipLevel, opacityArraySlice);

                if (diffuseSubresource.Width == 0 ||
                    diffuseSubresource.Height == 0 ||
                    opacitySubresource.Width == 0 ||
                    opacitySubresource.Height == 0 ||
                    diffuseSubresource.RowPitch < static_cast<size_t>(diffuseSubresource.Width) * 4u ||
                    opacitySubresource.RowPitch < static_cast<size_t>(opacitySubresource.Width) * 4u)
                {
                    return nullptr;
                }

                Engine::Core::SubTexture mergedSubresource = diffuseSubresource;
                for (std::uint32_t y = 0; y < diffuseSubresource.Height; ++y)
                {
                    const std::uint32_t opacityY = (std::min)(
                        static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * opacitySubresource.Height) / diffuseSubresource.Height),
                        opacitySubresource.Height - 1);

                    for (std::uint32_t x = 0; x < diffuseSubresource.Width; ++x)
                    {
                        const std::uint32_t opacityX = (std::min)(
                            static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * opacitySubresource.Width) / diffuseSubresource.Width),
                            opacitySubresource.Width - 1);

                        const size_t diffusePixelOffset = static_cast<size_t>(y) * diffuseSubresource.RowPitch + static_cast<size_t>(x) * 4u;
                        const size_t opacityPixelOffset = static_cast<size_t>(opacityY) * opacitySubresource.RowPitch + static_cast<size_t>(opacityX) * 4u;
                        if (diffusePixelOffset + 3 >= mergedSubresource.Data.size() ||
                            opacityPixelOffset >= opacitySubresource.Data.size())
                        {
                            return nullptr;
                        }

                        const std::uint8_t diffuseAlpha = std::to_integer<std::uint8_t>(mergedSubresource.Data[diffusePixelOffset + 3]);
                        const std::uint8_t opacityAlpha = std::to_integer<std::uint8_t>(opacitySubresource.Data[opacityPixelOffset]);
                        const std::uint8_t mergedAlpha = static_cast<std::uint8_t>(
                            (static_cast<std::uint32_t>(diffuseAlpha) * opacityAlpha + 127u) / 255u);
                        mergedSubresource.Data[diffusePixelOffset + 3] = static_cast<std::byte>(mergedAlpha);
                    }
                }

                subresources.push_back(std::move(mergedSubresource));
            }
        }

        return std::make_unique<Engine::Core::Texture>(textureDesc, std::move(subresources));
    }

    /// Loads a texture through AssetManager, creates the corresponding GPU texture and caches it by normalized path and semantic type.
    /// Returns an already created GPU texture when the same source texture is requested again.
    GDX12Texture* LoadSceneGpuTexture(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& texturePath,
        const Engine::Core::ETextureType textureType,
        const std::string& sceneName,
        std::unordered_map<std::wstring, GDX12Texture*>& gpuTexturesByPath)
    {
        if (texturePath.empty())
        {
            return nullptr;
        }

        const std::wstring textureKey = BuildTextureCacheKey(texturePath, textureType);
        const auto cachedTextureIterator = gpuTexturesByPath.find(textureKey);
        if (cachedTextureIterator != gpuTexturesByPath.end())
        {
            return cachedTextureIterator->second;
        }

        const Engine::Core::Texture* texture = assetManager.LoadTexture(texturePath);
        if (texture == nullptr)
        {
            return nullptr;
        }

        const std::string textureName = sceneName + "_Texture_" + std::to_string(gpuTexturesByPath.size());
        const Engine::Core::Texture typedTexture = texture->WithType(textureType);
        GDX12Texture* gpuTexture = renderModule.CreateTexture(textureName, &typedTexture);
        if (gpuTexture == nullptr)
        {
            gpuTexture = renderModule.GetTextureByName(textureName);
        }

        if (gpuTexture != nullptr)
        {
            gpuTexturesByPath.emplace(textureKey, gpuTexture);
        }

        return gpuTexture;
    }

    /// Loads a diffuse texture and packs an optional opacity mask into its alpha channel before GPU upload.
    /// Falls back to the original diffuse texture if the opacity texture is missing or cannot be merged.
    GDX12Texture* LoadSceneDiffuseGpuTexture(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& diffuseTexturePath,
        const std::filesystem::path& opacityTexturePath,
        const std::string& sceneName,
        std::unordered_map<std::wstring, GDX12Texture*>& gpuTexturesByPath)
    {
        if (diffuseTexturePath.empty())
        {
            return nullptr;
        }

        if (opacityTexturePath.empty())
        {
            return LoadSceneGpuTexture(renderModule, assetManager, diffuseTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
        }

        const std::wstring textureKey = BuildMergedOpacityTextureCacheKey(diffuseTexturePath, opacityTexturePath);
        const auto cachedTextureIterator = gpuTexturesByPath.find(textureKey);
        if (cachedTextureIterator != gpuTexturesByPath.end())
        {
            return cachedTextureIterator->second;
        }

        const Engine::Core::Texture* diffuseTexture = assetManager.LoadTexture(diffuseTexturePath);
        const Engine::Core::Texture* opacityTexture = assetManager.LoadTexture(opacityTexturePath);
        if (diffuseTexture == nullptr || opacityTexture == nullptr)
        {
            return LoadSceneGpuTexture(renderModule, assetManager, diffuseTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
        }

        std::unique_ptr<Engine::Core::Texture> mergedTexture = TryCreateDiffuseTextureWithOpacityAlpha(*diffuseTexture, *opacityTexture);
        if (mergedTexture == nullptr)
        {
            return LoadSceneGpuTexture(renderModule, assetManager, diffuseTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
        }

        const std::string textureName = sceneName + "_Texture_" + std::to_string(gpuTexturesByPath.size());
        GDX12Texture* gpuTexture = renderModule.CreateTexture(textureName, mergedTexture.get());
        if (gpuTexture == nullptr)
        {
            gpuTexture = renderModule.GetTextureByName(textureName);
        }

        if (gpuTexture != nullptr)
        {
            gpuTexturesByPath.emplace(textureKey, gpuTexture);
        }

        return gpuTexture;
    }

    /// Creates the renderer fallback material used when an imported material cannot be built.
    GDX12Material* CreateSceneFallbackMaterial(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::string& sceneName,
        const SceneDefaultMaterialTextures& defaultTextures)
    {
        const Engine::Core::Texture* fallbackTexture = assetManager.LoadTextureOrDefault(L"missing_texture.dds");
        if (fallbackTexture == nullptr)
        {
            return nullptr;
        }

        const std::string textureName = sceneName + "_FallbackTexture";
        const std::string materialName = sceneName + "_FallbackMaterial";

        GDX12Texture* gpuTexture = renderModule.CreateTexture(textureName, fallbackTexture);
        if (gpuTexture == nullptr)
        {
            gpuTexture = renderModule.GetTextureByName(textureName);
        }

        GDX12Material* material = renderModule.CreateMaterial(materialName);
        if (material == nullptr)
        {
            material = renderModule.GetMaterialByName(materialName);
        }

        if (gpuTexture == nullptr || material == nullptr)
        {
            return nullptr;
        }

        material->Metallic = 0.0f;
        material->Roughness = 1.0f;
        material->Opacity = 1.0f;
        material->SpecularColor = Vector3(0.0f, 0.0f, 0.0f);
        material->EmissiveColor = Vector3(0.0f, 0.0f, 0.0f);
        material->Diffuse = gpuTexture;
        material->Normal = defaultTextures.FlatNormal;
        material->Specular = defaultTextures.White;
        material->RoughnessMap = defaultTextures.White;
        material->Emissive = defaultTextures.Black;
        material->HasNormalMap = false;
		material->HasSpecularMap = false;
		material->HasRoughnessMap = false;
		material->HasEmissiveMap = false;
		material->UseBakedLighting = false;
		material->Type = MaterialType::Opaque;
		material->DirtyFlag = true;
		return material;
	}

    /// Creates a renderer material from imported mesh material metadata and resolved GPU textures.
    GDX12Material* CreateSceneMaterial(
        RenderModule& renderModule,
        const std::string& materialName,
        const Engine::Core::MeshMaterial& sourceMaterial,
        const MaterialTextureSet& textures)
    {
        GDX12Material* material = renderModule.CreateMaterial(materialName);
        if (material == nullptr)
        {
            material = renderModule.GetMaterialByName(materialName);
        }

        if (material == nullptr ||
            textures.Diffuse == nullptr ||
            textures.Normal == nullptr ||
            textures.Specular == nullptr ||
            textures.Roughness == nullptr ||
            textures.Emissive == nullptr)
        {
            return nullptr;
        }

        material->Metallic = 0.0f;
        material->Roughness = (std::max)(0.04f, (std::min)(sourceMaterial.Roughness, 1.0f));
        material->Opacity = (std::max)(0.0f, (std::min)(sourceMaterial.Opacity, 1.0f));
        material->SpecularColor = Vector3(sourceMaterial.SpecularColor.x, sourceMaterial.SpecularColor.y, sourceMaterial.SpecularColor.z);
        Vector3 emissiveColor(sourceMaterial.EmissiveColor.x, sourceMaterial.EmissiveColor.y, sourceMaterial.EmissiveColor.z);
        if (textures.HasEmissiveMap && emissiveColor.LengthSquared() < 0.000001f)
        {
            emissiveColor = Vector3(1.0f, 1.0f, 1.0f);
        }
        material->EmissiveColor = emissiveColor;
        material->Diffuse = textures.Diffuse;
        material->Normal = textures.Normal;
        material->Specular = textures.Specular;
        material->RoughnessMap = textures.Roughness;
        material->Emissive = textures.Emissive;
        material->HasNormalMap = textures.HasNormalMap;
		material->HasSpecularMap = textures.HasSpecularMap;
		material->HasRoughnessMap = textures.HasRoughnessMap;
		material->HasEmissiveMap = textures.HasEmissiveMap;
		material->UseBakedLighting = sourceMaterial.UseBakedLighting;
		if (sourceMaterial.Type == Engine::Core::EMaterialType::Transparent)
		{
			material->Type = MaterialType::Transparent;
		}
		else if (sourceMaterial.Type == Engine::Core::EMaterialType::Masked)
		{
			material->Type = MaterialType::Masked;
		}
		else
		{
			material->Type = MaterialType::Opaque;
		}
		material->DirtyFlag = true;
		return material;
	}

    /// Builds renderer material slots for every material referenced by an obj mesh.
    /// Missing material textures are replaced with scene defaults or solid-color fallbacks.
    std::vector<GDX12Material*> BuildObjMaterialSlots(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const Engine::Core::Mesh& mesh,
        const std::string& sceneName,
        GDX12Material* fallbackMaterial,
        const SceneDefaultMaterialTextures& defaultTextures,
        std::unordered_map<std::wstring, GDX12Texture*>& gpuTexturesByPath)
    {
        std::vector<GDX12Material*> materialSlots = BuildFallbackMaterialSlots(mesh, fallbackMaterial);
        const std::vector<Engine::Core::MeshMaterial>& materials = mesh.GetMaterials();

        const size_t materialCount = (std::min)(materialSlots.size(), materials.size());
        for (size_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
        {
            const std::filesystem::path diffuseTexturePath = materials[materialIndex].DiffuseTexturePath;
            
            MaterialTextureSet textures = {};
            if (!diffuseTexturePath.empty())
            {
                textures.Diffuse = LoadSceneDiffuseGpuTexture(
                    renderModule,
                    assetManager,
                    diffuseTexturePath,
                    materials[materialIndex].OpacityTexturePath,
                    sceneName,
                    gpuTexturesByPath);
            }

            if (textures.Diffuse == nullptr)
            {
                textures.Diffuse = CreateDiffuseColorTexture(renderModule, sceneName, materials[materialIndex]);
            }
            
            textures.Normal = defaultTextures.FlatNormal;
            textures.Specular = defaultTextures.White;
            textures.Roughness = defaultTextures.White;
            textures.Emissive = defaultTextures.Black;

            GDX12Texture* normalTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].NormalTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (normalTexture != nullptr)
            {
                textures.Normal = normalTexture;
                textures.HasNormalMap = true;
            }

            GDX12Texture* specularTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].SpecularTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (specularTexture != nullptr)
            {
                textures.Specular = specularTexture;
                textures.HasSpecularMap = true;
            }

            GDX12Texture* roughnessTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].RoughnessTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (roughnessTexture != nullptr)
            {
                textures.Roughness = roughnessTexture;
                textures.HasRoughnessMap = true;
            }
            
            GDX12Texture* emissiveTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].EmissiveTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
            if (emissiveTexture != nullptr)
            {
                textures.Emissive = emissiveTexture;
                textures.HasEmissiveMap = true;
            }
            
            if (textures.Diffuse == nullptr)
            {
                continue;
            }
            
            GDX12Material* material = CreateSceneMaterial(renderModule, sceneName + "_Material_" + std::to_string(materialIndex), materials[materialIndex], textures);
            
            if (material != nullptr)
            {
                materialSlots[materialIndex] = material;
            }
        }
        
        return materialSlots;
    }

    /// Loads one obj file as a renderable world entity.
    bool LoadObjSceneEntity(
        World& world,
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& objPath,
        const std::string& sceneName,
        std::unordered_map<std::wstring, GDX12Texture*>& gpuTexturesByPath,
        DirectX::BoundingBox& outBounds)
    {
        Engine::Core::MeshHandle sceneMeshHandle = {};
        const Engine::Core::Mesh* sceneMesh = assetManager.LoadMesh(objPath, sceneMeshHandle);
        if (sceneMesh == nullptr || !sceneMeshHandle.IsValid())
        {
            Logger::Error(
                "LoadObjSceneEntity failed.\n"
                "AssetManager::LoadMesh() failed for path:\n{}\n"
                "Current working directory:\n{}\n"
                "Mesh pointer is null: {}\n"
                "Mesh handle is valid: {}",
                objPath.string(),
                std::filesystem::current_path().string(),
                sceneMesh == nullptr ? "true" : "false",
                sceneMeshHandle.IsValid() ? "true" : "false");
            return false;
        }
        
        const SceneDefaultMaterialTextures defaultMaterialTextures = CreateSceneDefaultMaterialTextures(renderModule, sceneName);
        if (!defaultMaterialTextures.IsValid())
        {
            Logger::Error(
                "LoadObjSceneEntity failed.\n"
                "CreateSceneDefaultMaterialTextures() returned incomplete defaults for scene '{}'.\n"
                "FlatNormal: {}\n"
                "White: {}\n"
                "Black: {}",
                sceneName,
                defaultMaterialTextures.FlatNormal != nullptr ? "true" : "false",
                defaultMaterialTextures.White != nullptr ? "true" : "false",
                defaultMaterialTextures.Black != nullptr ? "true" : "false");
            return false;
        }
        
        GDX12Material* fallbackMaterial = CreateSceneFallbackMaterial(renderModule, assetManager, sceneName, defaultMaterialTextures);
        if (fallbackMaterial == nullptr)
        {
            Logger::Error(
                "LoadObjSceneEntity failed.\n"
                "CreateSceneFallbackMaterial() failed for scene '{}'.\n"
                "OBJ path:\n{}",
                sceneName,
                objPath.string());
            return false;
        }
        
        renderModule.SubmitMesh(sceneMesh, sceneMeshHandle);
        
        WorldECS& ecs = world.GetECS();
        auto sceneEntity = ecs.CreateEntity();
        sceneEntity.AddComponent<NameComponent>(sceneName);
        sceneEntity.AddComponent<TransformComponent>(
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.0f, 1.0f, 1.0f));
        sceneEntity.AddComponent<StaticMeshRenderComponent>(
            sceneMeshHandle,
            BuildObjMaterialSlots(
                renderModule,
                assetManager,
                *sceneMesh,
                sceneName,
                fallbackMaterial,
                defaultMaterialTextures,
                gpuTexturesByPath));
        
        outBounds = sceneMesh->GetBounds();
        return true;
    }

    /// Creates the active camera from scene bounds so the loaded scene is visible by default.
    void CreateCameraForBounds(World& world, const DirectX::BoundingBox& sceneBounds)
    {
        const Vector3 sceneCenter =
        {
            sceneBounds.Center.x,
            sceneBounds.Center.y,
            sceneBounds.Center.z
        };
        
        const Vector3 sceneExtents =
        {
            sceneBounds.Extents.x,
            sceneBounds.Extents.y,
            sceneBounds.Extents.z
        };
        
        float maxHorizontalExtent = sceneExtents.x;
        maxHorizontalExtent = (std::max)(sceneExtents.z, maxHorizontalExtent);
        maxHorizontalExtent = (std::max)(maxHorizontalExtent, 10.0f);
        
        const float cameraDistance = maxHorizontalExtent * 1.75f;
        const Vector3 cameraLocation = sceneCenter + Vector3(0.0f, sceneExtents.y * 0.35f, -cameraDistance);
        
        WorldECS& ecs = world.GetECS();
        auto camera = ecs.CreateEntity();
        camera.AddComponent<NameComponent>("MainCamera");
        camera.AddComponent<TransformComponent>(cameraLocation, Vector3(0.0f, 0.0f, 0.0f));
        
        auto& cameraComponent = camera.AddComponent<CameraComponent>();
        cameraComponent.FOV = 60.0f;
        cameraComponent.NearPlane = 0.1f;
        cameraComponent.FarPlane = 10000.0f;
        cameraComponent.DirtyFlag = true;
        
        world.ActiveCamera = camera;
    }

    /// Loads a set of obj files as one scene.
    bool LoadObjSceneFiles(
        World& world,
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& sceneRoot,
        const std::vector<std::filesystem::path>& objPaths)
    {
        if (objPaths.empty())
        {
            Logger::Error("LoadObjSceneFiles failed.\nNo .obj files found under directory:\n{}", sceneRoot.string());
            return false;
        }
        
        bool loadedAnyObject = false;
        DirectX::BoundingBox sceneBounds = {};
        std::unordered_map<std::wstring, GDX12Texture*> gpuTexturesByPath;
        
        for (const std::filesystem::path& objPath : objPaths)
        {
            DirectX::BoundingBox objBounds = {};
            if (!LoadObjSceneEntity(
                world,
                renderModule,
                assetManager,
                objPath,
                BuildScenePartName(sceneRoot, objPath),
                gpuTexturesByPath,
                objBounds))
            {
                continue;
            }
            
            if (!loadedAnyObject)
            {
                sceneBounds = objBounds;
                loadedAnyObject = true;
            }
            else
            {
                DirectX::BoundingBox::CreateMerged(sceneBounds, sceneBounds, objBounds);
            }
        }
        
        if (!loadedAnyObject)
        {
            Logger::Error(
                "LoadObjSceneFiles failed.\n"
                "Found .obj files, but none of them loaded successfully under directory:\n{}",
                sceneRoot.string());
            return false;
        }
        
        CreateCameraForBounds(world, sceneBounds);
        return true;
    }
}

static bool LoadWorldResources(WorldLoadContext& context, ryml::NodeRef resourcesNode);
static bool CreateWorldEntities(WorldLoadContext& context, ryml::NodeRef entitiesNode);
static bool LoadEntityComponentsWithoutRefs(WorldLoadContext& context, ryml::NodeRef entitiesNode);
static bool LoadEntityComponentsWithRefs(WorldLoadContext& context, ryml::NodeRef entitiesNode);
static Vector3 ReadVector3(ryml::NodeRef node);

static bool LoadYamlWorld(World& world, const std::filesystem::path& path)
{
    auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    if (!renderModule)
    {
        return false;
    }

    auto& assetManager = Engine::Core::AssetManager::GetInstance();

    WorldLoadContext context;
    context.World = &world;
    context.Render = renderModule.get();
    context.AssetManager = &assetManager;
    context.WorldFilePath = std::filesystem::absolute(path).lexically_normal();
    context.WorldDirectory = context.WorldFilePath.parent_path();

    ryml::Tree tree;

    try
    {
        const std::string yamlText = ReadTextFile(context.WorldFilePath);
        tree = ryml::parse_in_arena(
            ryml::to_csubstr(context.WorldFilePath.string()),
            ryml::to_csubstr(yamlText));
    }
    catch (const std::exception& exception)
    {
        Logger::Error(
            "Failed to load world yaml '{}': {}",
            context.WorldFilePath.string(),
            exception.what());
        return false;
    }

    ryml::NodeRef root = tree.rootref();

    if (!root.has_child("World"))
    {
        Logger::Error(
            "World yaml does not contain root node 'World': {}",
            context.WorldFilePath.string());
        return false;
    }

    ryml::NodeRef worldNode = root["World"];

    if (worldNode.has_child("Name"))
    {
        std::string worldName;
        worldNode["Name"] >> worldName;
        world.SetName(worldName);
    }

    if (worldNode.has_child("Resources"))
    {
        if (!LoadWorldResources(context, worldNode["Resources"]))
        {
            return false;
        }
    }

    if (worldNode.has_child("Entities"))
    {
        if (!CreateWorldEntities(context, worldNode["Entities"]))
        {
            return false;
        }

        if (!LoadEntityComponentsWithoutRefs(context, worldNode["Entities"]))
        {
            return false;
        }

        if (!LoadEntityComponentsWithRefs(context, worldNode["Entities"]))
        {
            return false;
        }
    }

    if (worldNode.has_child("ActiveCamera"))
    {
        std::string activeCameraId;
        worldNode["ActiveCamera"] >> activeCameraId;

        const Entity activeCamera = context.ResolveEntity(activeCameraId);
        if (activeCamera == InvalidEntity)
        {
            Logger::Error(
                "World ActiveCamera references unknown entity '{}': {}",
                activeCameraId,
                context.WorldFilePath.string());
            return false;
        }

        world.ActiveCamera = activeCamera;
    }

    return true;
}


bool WorldLoader::LoadFromFile(World& world, const std::filesystem::path& path)
{
    if (path.empty())
    {
        Logger::Error("WorldLoader::LoadFromFile failed: World file path is empty.");
        return false;
    }

    if (!std::filesystem::exists(path))
    {
        Logger::Error(
            "World source path not found.\n"
            "Path: {}\n"
            "Current working directory: {}",
            path.string(),
            std::filesystem::current_path().string());
        return false;
    }

    const std::string extension = ToLowerAscii(path.extension().string());

    if (extension == ".yaml" || extension == ".world")
    {
        return LoadYamlWorld(world, path);
    }

    if (std::filesystem::is_directory(path))
    {
        auto& assetManager = Engine::Core::AssetManager::GetInstance();
        auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

        if (!renderModule)
        {
            return false;
        }

        return LoadObjSceneFiles(world, *renderModule, assetManager, path, CollectSceneObjPaths(path));
    }

    if (IsObjPath(path))
    {
        auto& assetManager = Engine::Core::AssetManager::GetInstance();
        auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

        if (!renderModule)
        {
            return false;
        }

        DirectX::BoundingBox sceneBounds = {};
        std::unordered_map<std::wstring, GDX12Texture*> gpuTexturesByPath;

        if (!LoadObjSceneEntity(
            world,
            *renderModule,
            assetManager,
            std::filesystem::absolute(path),
            BuildScenePartName(path.parent_path(), path),
            gpuTexturesByPath,
            sceneBounds))
        {
            return false;
        }

        CreateCameraForBounds(world, sceneBounds);
        return true;
    }

    return false;
}

static bool LoadTextures(WorldLoadContext& context, ryml::NodeRef texturesNode)
{
    for (ryml::NodeRef textureNode : texturesNode.children())
    {
        if (!textureNode.has_child("Id") || !textureNode.has_child("Source"))
        {
            Logger::Error("World texture resource requires Id and Source.");
            return false;
        }

        std::string id;
        std::string source;

        textureNode["Id"] >> id;
        textureNode["Source"] >> source;

        if (id.empty() || context.TexturesById.find(id) != context.TexturesById.end())
        {
            Logger::Error("Invalid or duplicate world texture resource id: '{}'", id);
            return false;
        }

        const std::filesystem::path texturePath =
            ResolveWorldResourcePath(context, source);
        const Engine::Core::Texture* cpuTexture =
            context.AssetManager->LoadTexture(texturePath);

        if (!cpuTexture)
        {
            Logger::Error(
                "Failed to load world texture resource '{}': {}",
                id,
                texturePath.string());
            return false;
        }

        GDX12Texture* gpuTexture = context.Render->CreateTexture(id, cpuTexture);
        if (!gpuTexture)
        {
            gpuTexture = context.Render->GetTextureByName(id);
        }

        if (!gpuTexture)
        {
            return false;
        }

        context.TexturesById.emplace(id, gpuTexture);
    }

    return true;
}

static bool LoadMaterials(WorldLoadContext& context, ryml::NodeRef materialsNode)
{
    for (ryml::NodeRef materialNode : materialsNode.children())
    {
        if (!materialNode.has_child("Id"))
        {
            Logger::Error("World material resource requires Id.");
            return false;
        }

        std::string id;
        materialNode["Id"] >> id;

        if (id.empty() || context.MaterialsById.find(id) != context.MaterialsById.end())
        {
            Logger::Error("Invalid or duplicate world material resource id: '{}'", id);
            return false;
        }

        GDX12Material* material = context.Render->CreateMaterial(id);
        if (!material)
        {
            material = context.Render->GetMaterialByName(id);
        }

        if (!material)
        {
            return false;
        }

        if (materialNode.has_child("Metallic"))
        {
            materialNode["Metallic"] >> material->Metallic;
        }

        if (materialNode.has_child("Roughness"))
        {
            materialNode["Roughness"] >> material->Roughness;
        }

        if (materialNode.has_child("Diffuse"))
        {
            std::string textureId;
            materialNode["Diffuse"] >> textureId;
            material->Diffuse = context.ResolveTexture(textureId);
            if (!material->Diffuse)
            {
                Logger::Error(
                    "World material '{}' references unknown diffuse texture '{}'.",
                    id,
                    textureId);
                return false;
            }
        }

        material->DirtyFlag = true;

        context.MaterialsById.emplace(id, material);
    }

    return true;
}

static bool LoadMeshes(WorldLoadContext& context, ryml::NodeRef meshesNode)
{
    for (ryml::NodeRef meshNode : meshesNode.children())
    {
        if (!meshNode.has_child("Id") || !meshNode.has_child("Source"))
        {
            Logger::Error("World mesh resource requires Id and Source.");
            return false;
        }

        std::string id;
        std::string source;

        meshNode["Id"] >> id;
        meshNode["Source"] >> source;

        if (id.empty() || context.MeshesById.find(id) != context.MeshesById.end())
        {
            Logger::Error("Invalid or duplicate world mesh resource id: '{}'", id);
            return false;
        }

        const std::filesystem::path meshPath =
            ResolveWorldResourcePath(context, source);
        Engine::Core::MeshHandle meshHandle = {};
        const Engine::Core::Mesh* cpuMesh =
            context.AssetManager->LoadMesh(meshPath, meshHandle);

        if (!cpuMesh || !meshHandle.IsValid())
        {
            Logger::Error(
                "Failed to load world mesh resource '{}': {}",
                id,
                meshPath.string());
            return false;
        }

        bool submitToRenderer = true;
        if (meshNode.has_child("SubmitToRenderer"))
        {
            meshNode["SubmitToRenderer"] >> submitToRenderer;
        }

        bool importMaterials = false;
        if (meshNode.has_child("ImportMaterials"))
        {
            meshNode["ImportMaterials"] >> importMaterials;
        }

        if (submitToRenderer)
        {
            context.Render->SubmitMesh(cpuMesh, meshHandle);
        }

        context.MeshesById.emplace(id, meshHandle);
        context.CpuMeshesById.emplace(id, cpuMesh);

        if (importMaterials)
        {
            const SceneDefaultMaterialTextures defaultTextures =
                CreateSceneDefaultMaterialTextures(*context.Render, id);
            if (!defaultTextures.IsValid())
            {
                Logger::Error(
                    "Failed to create default textures for imported mesh materials '{}'.",
                    id);
                return false;
            }

            GDX12Material* fallbackMaterial = CreateSceneFallbackMaterial(
                *context.Render,
                *context.AssetManager,
                id,
                defaultTextures);
            if (!fallbackMaterial)
            {
                Logger::Error(
                    "Failed to create fallback material for imported mesh '{}'.",
                    id);
                return false;
            }

            std::vector<GDX12Material*> importedMaterials = BuildObjMaterialSlots(
                *context.Render,
                *context.AssetManager,
                *cpuMesh,
                id,
                fallbackMaterial,
                defaultTextures,
                context.ImportedTexturesByPath);

            if (importedMaterials.empty())
            {
                Logger::Error("Imported mesh '{}' produced no material slots.", id);
                return false;
            }

            context.MeshMaterialsById.emplace(id, std::move(importedMaterials));
        }
    }

    return true;
}

static bool LoadWorldResources(WorldLoadContext& context, ryml::NodeRef resourcesNode)
{
    if (resourcesNode.has_child("Textures"))
    {
        if (!LoadTextures(context, resourcesNode["Textures"]))
        {
            return false;
        }
    }

    if (resourcesNode.has_child("Materials"))
    {
        if (!LoadMaterials(context, resourcesNode["Materials"]))
        {
            return false;
        }
    }

    if (resourcesNode.has_child("Meshes"))
    {
        if (!LoadMeshes(context, resourcesNode["Meshes"]))
        {
            return false;
        }
    }

    return true;
}

static bool CreateWorldEntities(WorldLoadContext& context, ryml::NodeRef entitiesNode)
{
    WorldECS& ecs = context.World->GetECS();

    for (ryml::NodeRef entityNode : entitiesNode.children())
    {
        if (!entityNode.has_child("Id"))
        {
            Logger::Error("World entity requires Id.");
            return false;
        }

        std::string id;
        entityNode["Id"] >> id;

        if (id.empty() || context.EntitiesById.find(id) != context.EntitiesById.end())
        {
            Logger::Error("Invalid or duplicate world entity id: '{}'", id);
            return false;
        }

        auto entity = ecs.CreateEntity();

        context.EntitiesById.emplace(id, entity.GetId());
    }

    return true;
}

static bool ParseTransform(WorldECS::EntityHandle& entity, ryml::NodeRef node)
{
    Vector3 location = Vector3(0.0f, 0.0f, 0.0f);
    Vector3 rotation = Vector3(0.0f, 0.0f, 0.0f);
    Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);

    if (node.has_child("Location"))
    {
        location = ReadVector3(node["Location"]);
    }

    if (node.has_child("Rotation"))
    {
        rotation = ReadVector3(node["Rotation"]);
    }

    if (node.has_child("Scale"))
    {
        scale = ReadVector3(node["Scale"]);
    }

    entity.AddComponent<TransformComponent>(location, rotation, scale);
    return true;
}

static bool ParseStaticMeshRender(
    WorldLoadContext& context,
    WorldECS::EntityHandle& entity,
    ryml::NodeRef node)
{
    std::string meshId;
    node["Mesh"] >> meshId;

    Engine::Core::MeshHandle meshHandle = context.ResolveMesh(meshId);
    if (!meshHandle.IsValid())
    {
        return false;
    }

    std::vector<GDX12Material*> materials;

    if (node.has_child("Materials"))
    {
        ryml::NodeRef materialsNode = node["Materials"];

        for (ryml::NodeRef materialNode : materialsNode.children())
        {
            std::string materialId;
            materialNode >> materialId;

            GDX12Material* material = context.ResolveMaterial(materialId);
            if (!material)
            {
                return false;
            }

            materials.push_back(material);
        }
    }
    else if (const std::vector<GDX12Material*>* importedMaterials =
        context.ResolveMeshMaterials(meshId))
    {
        materials = *importedMaterials;
    }

    if (materials.empty())
    {
        return false;
    }

    if (const Engine::Core::Mesh* cpuMesh = context.ResolveCpuMesh(meshId))
    {
        const size_t requiredMaterialCount = GetMaterialSlotCount(*cpuMesh);
        if (materials.size() < requiredMaterialCount)
        {
            materials.resize(requiredMaterialCount, materials.back());
        }
    }

    auto& component = entity.AddComponent<StaticMeshRenderComponent>(
        meshHandle,
        materials
    );

    if (const Engine::Core::Mesh* cpuMesh = context.ResolveCpuMesh(meshId))
    {
        component.Bounds = cpuMesh->GetBounds();
    }

    return true;
}

static bool LoadEntityComponentsWithoutRefs(
    WorldLoadContext& context,
    ryml::NodeRef entitiesNode)
{
    WorldECS& ecs = context.World->GetECS();

    for (ryml::NodeRef entityNode : entitiesNode.children())
    {
        std::string id;
        entityNode["Id"] >> id;

        const Entity runtimeEntity = context.ResolveEntity(id);
        if (runtimeEntity == InvalidEntity)
        {
            return false;
        }

        auto entity = ecs.GetEntityHandle(runtimeEntity);

        if (!entityNode.has_child("Components"))
        {
            continue;
        }

        ryml::NodeRef components = entityNode["Components"];

        if (components.has_child("Name"))
        {
            std::string name = id;

            ryml::NodeRef nameNode = components["Name"];
            if (nameNode.has_child("Value"))
            {
                nameNode["Value"] >> name;
            }

            entity.AddComponent<NameComponent>(name);
        }

        if (components.has_child("Transform"))
        {
            if (!ParseTransform(entity, components["Transform"]))
            {
                return false;
            }
        }

        if (components.has_child("Camera"))
        {
            auto& camera = entity.AddComponent<CameraComponent>();

            ryml::NodeRef cameraNode = components["Camera"];

            if (cameraNode.has_child("FOV"))
            {
                cameraNode["FOV"] >> camera.FOV;
            }

            if (cameraNode.has_child("NearPlane"))
            {
                cameraNode["NearPlane"] >> camera.NearPlane;
            }

            if (cameraNode.has_child("FarPlane"))
            {
                cameraNode["FarPlane"] >> camera.FarPlane;
            }

            camera.DirtyFlag = true;
        }

        if (components.has_child("Velocity"))
        {
            Vector3 velocity = ReadVector3(components["Velocity"]);
            entity.AddComponent<VelocityComponent>(velocity);
        }

        if (components.has_child("CircleMovement"))
        {
            ryml::NodeRef node = components["CircleMovement"];

            float radius = 100.0f;
            float speed = 100.0f;

            if (node.has_child("Radius"))
            {
                node["Radius"] >> radius;
            }

            if (node.has_child("Speed"))
            {
                node["Speed"] >> speed;
            }

            entity.AddComponent<CircleMovementComponent>(radius, speed);
        }

        if (components.has_child("StaticMeshRender"))
        {
            if (!ParseStaticMeshRender(context, entity, components["StaticMeshRender"]))
            {
                return false;
            }
        }

        if (components.has_child("SplineCurve"))
        {
            ryml::NodeRef node = components["SplineCurve"];

            bool loop = false;
            if (node.has_child("Loop"))
            {
                node["Loop"] >> loop;
            }

            std::vector<SplinePoint> points;

            if (node.has_child("Points"))
            {
                for (ryml::NodeRef pointNode : node["Points"].children())
                {
                    SplinePoint point;
                    point.Position = ReadVector3(pointNode["Position"]);
                    point.ArriveTangent = ReadVector3(pointNode["ArriveTangent"]);
                    point.LeaveTangent = ReadVector3(pointNode["LeaveTangent"]);
                    points.push_back(point);
                }
            }

            entity.AddComponent<SplineCurveComponent>(loop, points);
        }
    }

    return true;
}

static Vector3 ReadVector3(ryml::NodeRef node)
{
    if (!node.is_seq() || node.num_children() < 3)
    {
        return Vector3::Zero;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    node[0] >> x;
    node[1] >> y;
    node[2] >> z;

    return Vector3(x, y, z);
}

static bool LoadEntityComponentsWithRefs(
    WorldLoadContext& context,
    ryml::NodeRef entitiesNode)
{
    WorldECS& ecs = context.World->GetECS();

    for (ryml::NodeRef entityNode : entitiesNode.children())
    {
        std::string id;
        entityNode["Id"] >> id;

        const Entity runtimeEntity = context.ResolveEntity(id);
        if (runtimeEntity == InvalidEntity)
        {
            return false;
        }

        auto entity = ecs.GetEntityHandle(runtimeEntity);

        if (!entityNode.has_child("Components"))
        {
            continue;
        }

        ryml::NodeRef components = entityNode["Components"];

        if (components.has_child("LookAtTarget"))
        {
            ryml::NodeRef node = components["LookAtTarget"];

            std::string targetId;
            node["Target"] >> targetId;

            const Entity targetEntity = context.ResolveEntity(targetId);
            if (targetEntity == InvalidEntity)
            {
                return false;
            }

            Vector3 targetOffset = Vector3(0.0f, 0.0f, 0.0f);
            Vector3 worldUp = Vector3(0.0f, 1.0f, 0.0f);
            bool enabled = true;

            if (node.has_child("TargetOffset"))
            {
                targetOffset = ReadVector3(node["TargetOffset"]);
            }

            if (node.has_child("WorldUp"))
            {
                worldUp = ReadVector3(node["WorldUp"]);
            }

            if (node.has_child("Enabled"))
            {
                node["Enabled"] >> enabled;
            }

            auto& component = entity.AddComponent<LookAtTargetComponent>(
                targetEntity,
                targetOffset,
                worldUp,
                enabled
            );

            if (node.has_child("LocalForward"))
            {
                component.LocalForward = ReadVector3(node["LocalForward"]);
            }
        }

        if (components.has_child("SplineFollow"))
        {
            ryml::NodeRef node = components["SplineFollow"];

            std::string curveId;
            node["Curve"] >> curveId;

            const Entity curveEntity = context.ResolveEntity(curveId);
            if (curveEntity == InvalidEntity)
            {
                return false;
            }

            float duration = 5.0f;
            bool loop = false;
            bool playing = true;
            float time = 0.0f;

            if (node.has_child("Duration"))
            {
                node["Duration"] >> duration;
            }

            if (node.has_child("Loop"))
            {
                node["Loop"] >> loop;
            }

            if (node.has_child("Playing"))
            {
                node["Playing"] >> playing;
            }

            if (node.has_child("Time"))
            {
                node["Time"] >> time;
            }

            entity.AddComponent<SplineFollowComponent>(
                curveEntity,
                duration,
                loop,
                playing,
                time
            );
        }
    }

    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
