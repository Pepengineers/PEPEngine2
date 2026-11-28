#include "App.Base/ECS/WorldLoader.h"
#include "App.Base/ECS/World.h"

#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Types/TextureTypes.h"
#include "App.Base/Modules/RenderModule.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
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
    GPUTexture* CreateSolidGpuTexture(
        RenderModule& renderModule,
        const std::string& textureName,
        const std::uint8_t red,
        const std::uint8_t green,
        const std::uint8_t blue,
        const std::uint8_t alpha,
        const Engine::Core::ETextureType textureType)
    {
        Engine::Core::Texture texture = CreateSingleColorTexture(red, green, blue, alpha, textureType);

        GPUTexture* gpuTexture = renderModule.CreateTexture(textureName, &texture);
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
    GPUTexture* CreateDiffuseColorTexture(
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
        GPUTexture* FlatNormal = nullptr;
        GPUTexture* White = nullptr;
        GPUTexture* Black = nullptr;

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
        GPUTexture* Diffuse = nullptr;
        GPUTexture* Normal = nullptr;
        GPUTexture* Specular = nullptr;
        GPUTexture* Roughness = nullptr;
        GPUTexture* Emissive = nullptr;
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
    GPUTexture* LoadSceneGpuTexture(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& texturePath,
        const Engine::Core::ETextureType textureType,
        const std::string& sceneName,
        std::unordered_map<std::wstring, GPUTexture*>& gpuTexturesByPath)
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
        GPUTexture* gpuTexture = renderModule.CreateTexture(textureName, &typedTexture);
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
    GPUTexture* LoadSceneDiffuseGpuTexture(
        RenderModule& renderModule,
        Engine::Core::AssetManager& assetManager,
        const std::filesystem::path& diffuseTexturePath,
        const std::filesystem::path& opacityTexturePath,
        const std::string& sceneName,
        std::unordered_map<std::wstring, GPUTexture*>& gpuTexturesByPath)
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
        GPUTexture* gpuTexture = renderModule.CreateTexture(textureName, mergedTexture.get());
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

        GPUTexture* gpuTexture = renderModule.CreateTexture(textureName, fallbackTexture);
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
        std::unordered_map<std::wstring, GPUTexture*>& gpuTexturesByPath)
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

            GPUTexture* normalTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].NormalTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (normalTexture != nullptr)
            {
                textures.Normal = normalTexture;
                textures.HasNormalMap = true;
            }

            GPUTexture* specularTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].SpecularTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (specularTexture != nullptr)
            {
                textures.Specular = specularTexture;
                textures.HasSpecularMap = true;
            }

            GPUTexture* roughnessTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].RoughnessTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (roughnessTexture != nullptr)
            {
                textures.Roughness = roughnessTexture;
                textures.HasRoughnessMap = true;
            }
            
            GPUTexture* emissiveTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].EmissiveTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
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
        std::unordered_map<std::wstring, GPUTexture*>& gpuTexturesByPath,
        DirectX::BoundingBox& outBounds)
    {
        Engine::Core::MeshHandle sceneMeshHandle = {};
        const Engine::Core::Mesh* sceneMesh = assetManager.LoadMesh(objPath, sceneMeshHandle);
        if (sceneMesh == nullptr || !sceneMeshHandle.IsValid())
        {
            return false;
        }
        
        const SceneDefaultMaterialTextures defaultMaterialTextures = CreateSceneDefaultMaterialTextures(renderModule, sceneName);
        if (!defaultMaterialTextures.IsValid())
        {
            return false;
        }
        
        GDX12Material* fallbackMaterial = CreateSceneFallbackMaterial(renderModule, assetManager, sceneName, defaultMaterialTextures);
        if (fallbackMaterial == nullptr)
        {
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
            return false;
        }
        
        bool loadedAnyObject = false;
        DirectX::BoundingBox sceneBounds = {};
        std::unordered_map<std::wstring, GPUTexture*> gpuTexturesByPath;
        
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
            return false;
        }
        
        CreateCameraForBounds(world, sceneBounds);
        renderModule.SetActiveCamera(&world.GetECS().Get<CameraComponent>(world.ActiveCamera));
        return true;
    }
}

bool WorldLoader::LoadFromFile(World& world, const std::filesystem::path& path)
{
    /* todo if file not found return 0;
    rapid yaml parsing
    create entities and components
    */
    world.SetName(path.string()); 

    //
    //Loading Neccessary Assets
    //
    auto& assetManager = Engine::Core::AssetManager::GetInstance();
    auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    
    const bool isDirectory = std::filesystem::is_directory(path);
    if (isDirectory || IsObjPath(path))
    {
        const auto sceneFiles = isDirectory
            ? CollectSceneObjPaths(path)
            : std::vector<std::filesystem::path>{path};
        if (sceneFiles.empty()) { return false; }
        DirectX::BoundingBox sceneBounds = {};
        std::unordered_map<std::wstring, GPUTexture*> gpuTexturesByPath;
        for (const auto& sceneFile : sceneFiles)
        {
            if (!LoadObjSceneEntity(world, *renderModule, assetManager, sceneFile,
                BuildScenePartName(isDirectory ? path : path.parent_path(), sceneFile), gpuTexturesByPath, sceneBounds))
            {
                return false;
            }
        }
    }

    //Textures: 
    //this should probably be done via TextureHandle
    auto HeadTexture = assetManager.LoadTexture("african_head_diffuse.dds");

    //this should be automated via events
    renderModule->CreateTexture("HeadTexture", HeadTexture);

    auto SvTexture = assetManager.LoadTexture("friazino_diff.png");

    //this should be automated via events
    renderModule->CreateTexture("SvTexture", SvTexture);

    //Materials: 
    auto HeadMaterial = renderModule->CreateMaterial("HeadMaterial");
    HeadMaterial->Metallic = 0.f;
    HeadMaterial->Roughness = 0.8f;
    HeadMaterial->Diffuse = renderModule->GetTextureByName("HeadTexture");

    auto SvMaterial = renderModule->CreateMaterial("SvMaterial");
    SvMaterial->Metallic = 0.1f;
    SvMaterial->Roughness = 1.f;
    SvMaterial->Diffuse = renderModule->GetTextureByName("SvTexture");

    //Meshes: 
    Engine::Core::MeshAssetLocator locator = {};
    locator.SourcePath = "african_head.obj";

    Engine::Core::MeshHandle HeadMeshHandle;
    const Engine::Core::Mesh* HeadMesh = assetManager.Meshes().Load(locator, HeadMeshHandle);

    //this should be automated via events
    renderModule->SubmitMesh(HeadMesh, HeadMeshHandle);

    locator.SourcePath = "Svidetel.fbx";

    Engine::Core::MeshHandle SvMeshHandle;
    const Engine::Core::Mesh* SvMesh = assetManager.Meshes().Load(locator, SvMeshHandle);

    //this should be automated via events
    renderModule->SubmitMesh(SvMesh, SvMeshHandle);

    //
    // Creating Entities and components
    //
    
    WorldECS& ecs = world.GetECS();

    auto en1 = ecs.CreateEntity();
    en1.AddComponent<NameComponent>("en1");
    en1.AddComponent<TransformComponent>(Vector3(0.0f, 0.f, 0.f));
    en1.AddComponent<CircleMovementComponent>(1,1);

    std::vector<GDX12Material*> HeadMaterials = { HeadMaterial };
    en1.AddComponent<StaticMeshRenderComponent>(HeadMeshHandle, HeadMaterials);
    
    auto splineEntity = ecs.CreateEntity();
    splineEntity.AddComponent<NameComponent>("en2_spline");

    std::vector<SplinePoint> splinePoints =
    {
        {
            Vector3(10.f, -5.f, -20.f),
            Vector3(0.f, 0.f, 0.f),
            Vector3(-5.f, 0.f, 8.f)
        },
        {
            Vector3(0.f, -2.f, -12.f),
            Vector3(5.f, 0.f, -8.f),
            Vector3(-5.f, 6.f, 8.f)
        },
        {
            Vector3(-10.f, 2.f, -22.f),
            Vector3(5.f, -6.f, -8.f),
            Vector3(-5.f, 4.f, -8.f)
        },
        {
            Vector3(-20.f, -3.f, -16.f),
            Vector3(5.f, -4.f, 8.f),
            Vector3(-5.f, 0.f, 6.f)
        },
        {
            Vector3(-30.f, -5.f, -25.f),
            Vector3(5.f, 0.f, -6.f),
            Vector3(0.f, 0.f, 0.f)
        }
    };

    splineEntity.AddComponent<SplineCurveComponent>(
        false,
        splinePoints
    );

    auto en2 = ecs.CreateEntity();
    en2.AddComponent<NameComponent>("en2");
    en2.AddComponent<TransformComponent>(Vector3(10.f, -5.f, -20.f), Vector3(0.f, 0.f, 0.f),
    Vector3(0.1f, 0.1f, 0.1f));

    std::vector<GDX12Material*> SvMaterials = { SvMaterial };
    en2.AddComponent<StaticMeshRenderComponent>(SvMeshHandle, SvMaterials);

    en2.AddComponent<SplineFollowComponent>(
        splineEntity.GetId(),
        8.0f,   // duration
        false,  // bLoop
        true    // bPlaying
    );
    
    auto markerSpline = ecs.CreateEntity();
    markerSpline.AddComponent<NameComponent>("MarkerSpline");

    std::vector<SplinePoint> markerSplinePoints =
    {
        {
            Vector3(491.f, 200.f, -661.f),
            Vector3(0.f, 0.f, 0.f),
            Vector3(-127.f, -23.333f, 46.f)
        },
        {
            Vector3(110.f, 130.f, -523.f),
            Vector3(84.667f, 11.667f, -73.f),
            Vector3(-84.667f, -11.667f, 73.f)
        },
        {
            Vector3(-17.f, 130.f, -223.f),
            Vector3(-11.f, 0.f, -101.833f),
            Vector3(11.f, 0.f, 101.833f)
        },
        {
            Vector3(176.f, 130.f, 88.f),
            Vector3(-82.667f, -8.333f, -48.333f),
            Vector3(82.667f, 8.333f, 48.333f)
        },
        {
            Vector3(479.f, 180.f, 67.f),
            Vector3(-109.f, -11.667f, 11.833f),
            Vector3(109.f, 11.667f, -11.833f)
        },
        {
            Vector3(830.f, 200.f, 17.f),
            Vector3(-5.667f, 5.f, 55.833f),
            Vector3(5.667f, -5.f, -55.833f)
        },
        {
            Vector3(513.f, 150.f, -268.f),
            Vector3(105.667f, 16.667f, 95.f),
            Vector3(0.f, 0.f, 0.f)
        }
    };

    markerSpline.AddComponent<SplineCurveComponent>(
        false,
        markerSplinePoints
    );    
    
    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>("marker");
    marker.AddComponent<TransformComponent>(Vector3(491.f, 200.f, -661.f));
    marker.AddComponent<SplineFollowComponent>(
        markerSpline.GetId(),
        18.0f,  // Duration
        true,  // bLoop
        true    // bPlaying
    );

    auto cameraSpline = ecs.CreateEntity();
    cameraSpline.AddComponent<NameComponent>("MainCameraSpline");

    std::vector<SplinePoint> cameraSplinePoints =
    {
        {
            Vector3(769.f, 200.f, -914.f),
            Vector3(0.f, 0.f, 0.f),
            Vector3(-96.f, 0.f, 87.333f)
        },
        {
            Vector3(481.f, 200.f, -652.f),
            Vector3(84.5f, 0.f, -63.333f),
            Vector3(-84.5f, 0.f, 63.333f)
        },
        {
            Vector3(262.f, 200.f, -534.f),
            Vector3(57.333f, 0.f, -67.667f),
            Vector3(-57.333f, 0.f, 67.667f)
        },
        {
            Vector3(137.f, 200.f, -246.f),
            Vector3(-0.333f, 0.f, -82.333f),
            Vector3(0.333f, 0.f, 82.333f)
        },
        {
            Vector3(264.f, 200.f, -40.f),
            Vector3(-76.5f, 0.f, -60.f),
            Vector3(76.5f, 0.f, 60.f)
        },
        {
            Vector3(596.f, 200.f, 114.f),
            Vector3(-108.f, 0.f, -52.167f),
            Vector3(108.f, 0.f, 52.167f)
        },
        {
            Vector3(912.f, 200.f, 273.f),
            Vector3(-105.333f, 0.f, -53.f),
            Vector3(0.f, 0.f, 0.f)
        }
    };

    cameraSpline.AddComponent<SplineCurveComponent>(
        false,
        cameraSplinePoints
    );

    auto camera = ecs.CreateEntity();
    camera.AddComponent<NameComponent>("MainCamera");
    camera.AddComponent<TransformComponent>(Vector3(769.f, 200.f, -914.f), Vector3(0.f, 180.f, 0.f));
    camera.AddComponent<CameraComponent>();
    camera.AddComponent<LookAtTargetComponent>(marker.GetId());
    camera.AddComponent<SplineFollowComponent>(
        cameraSpline.GetId(),
        18.0f,  // Duration
        true,  // bLoop
        true    // bPlaying
    );

    world.ActiveCamera = camera;
    renderModule->SetActiveCamera(&camera.GetComponent<CameraComponent>());

    auto createLight = [&ecs](const char* name, LightType type, Vector3 position, float range, float intensity,
        Vector3 direction = Vector3(0, -1, 0)) -> LightComponent&
        {
            direction.Normalize();
            const Vector3 rotation(
                XMConvertToDegrees(std::asin(-direction.y)),
                XMConvertToDegrees(std::atan2(direction.x, direction.z)),
                0);
            auto entity = ecs.CreateEntity();
            entity.AddComponent<NameComponent>(name);
            entity.AddComponent<TransformComponent>(position, rotation);
            auto& light = entity.AddComponent<LightComponent>();
            light.Type = type;
            light.Range = range;
            light.Intensity = intensity;
            return light;
        };

    auto& sun = createLight("Sun", LightType::Directional, Vector3::Zero, 0, 4.0f, Vector3(0.35f, -1.0f, 0.25f));
    sun.Color = Vector3(1.0f, 0.95f, 0.875f);

    // Building facade.
    createLight("Paris_Building_01_paris_building_01_bottom_44", LightType::Point, { 361.331f, 410.190f, 279.902f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_45", LightType::Point, { 560.746f, 410.190f, 358.776f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_46", LightType::Point, { 778.120f, 410.190f, 444.753f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_47", LightType::Point, { -117.551f, 410.188f, -426.037f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_48", LightType::Point, { -6.615f, 410.189f, -631.797f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_49", LightType::Point, { 95.156f, 410.189f, -820.557f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_67", LightType::Point, { 131.870f, 410.190f, 189.603f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_68", LightType::Point, { 212.189f, 410.189f, -1037.624f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_158", LightType::Point, { -178.991f, 266.331f, -222.308f }, 500.000f, 150.000f);
    createLight("Paris_Building_01_paris_building_01_bottom_159", LightType::Point, { -42.617f, 267.739f, 76.785f }, 500.000f, 150.000f);

    // Streetlights.
    createLight("Paris_Streetlight_01a", LightType::Point, { -433.326f, 409.493f, -419.017f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__2_", LightType::Point, { 373.428f, 409.996f, -2125.876f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__3_", LightType::Point, { -2081.920f, 409.996f, -1823.962f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__4_", LightType::Point, { -965.267f, 409.996f, 208.816f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__5_", LightType::Point, { -208.779f, 409.996f, 465.133f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__6_", LightType::Point, { -175.615f, 409.996f, 1004.082f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__7_", LightType::Point, { 781.492f, 409.996f, 865.957f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__8_", LightType::Point, { 2158.015f, 409.996f, 1882.062f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__9_", LightType::Point, { 3330.580f, 409.996f, 2409.981f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__10_", LightType::Point, { 3879.824f, 409.996f, 3394.735f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__11_", LightType::Point, { 4932.883f, 409.996f, 3435.792f }, 650.000f, 180.000f);
    createLight("Paris_Streetlight_01a__12_", LightType::Point, { 3510.323f, 409.996f, 1832.195f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B", LightType::Point, { -1339.952f, 372.467f, 121.300f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__2_", LightType::Point, { -1725.812f, 372.467f, -378.603f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__3_", LightType::Point, { -2017.024f, 372.467f, -718.796f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__4_", LightType::Point, { -2458.580f, 371.137f, -1180.908f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__5_", LightType::Point, { -2851.466f, 298.162f, -1719.422f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__6_", LightType::Point, { -2689.168f, 298.162f, -2106.046f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__7_", LightType::Point, { -1773.059f, 372.793f, -1494.189f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__8_", LightType::Point, { -1193.675f, 372.467f, -961.885f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__9_", LightType::Point, { -843.238f, 375.138f, -1141.376f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__10_", LightType::Point, { -212.129f, 391.431f, -1828.257f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__11_", LightType::Point, { 1802.556f, 416.735f, -4156.665f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__12_", LightType::Point, { 1765.658f, 416.735f, -3440.353f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__13_", LightType::Point, { 898.219f, 423.911f, -2008.213f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__14_", LightType::Point, { 3000.853f, 374.075f, 1350.240f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__15_", LightType::Point, { 2446.246f, 429.276f, 2316.705f }, 650.000f, 180.000f);
    createLight("Paris_StreetLight_01B__16_", LightType::Point, { 2076.290f, 374.075f, 1275.618f }, 650.000f, 180.000f);

    // Spotlights.
    createLight("Spotlights_1_1", LightType::Spot, { -2189.932f, 865.291f, -878.311f }, 1100.000f, 450.000f, { -0.258f, 0.940f, 0.224f });
    createLight("Spotlights_1_2", LightType::Spot, { -2563.253f, 865.291f, -1309.126f }, 1100.000f, 450.000f, { -0.258f, 0.940f, 0.224f });
    createLight("Spotlights_1_3", LightType::Spot, { 1231.022f, 340.951f, -2508.879f }, 1100.000f, 450.000f, { 0.000f, -1.000f, 0.000f });
    createLight("Spotlights_2_1", LightType::Spot, { 983.025f, 339.634f, -2093.813f }, 1100.000f, 450.000f, { 0.000f, -1.000f, 0.000f });
    createLight("Spotlights_3_1", LightType::Spot, { -1242.096f, 866.259f, 229.179f }, 1100.000f, 450.000f, { -0.258f, 0.940f, 0.224f });
    createLight("Spotlights_4_1", LightType::Spot, { -1615.682f, 866.259f, -205.051f }, 1100.000f, 450.000f, { -0.250f, 0.940f, 0.233f });
    createLight("Spotlights_5_1", LightType::Spot, { -1841.664f, 865.291f, -585.871f }, 1100.000f, 450.000f, { -0.254f, 0.940f, 0.229f });

    // String lights.
    createLight("paris_stringlights_01a8_Red_1", LightType::Point, { 269.315f, 398.968f, -945.381f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_Blue_1", LightType::Point, { -516.465f, 570.747f, -1658.131f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_Blue_2", LightType::Point, { -76.588f, 431.524f, -1255.428f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_Green_1", LightType::Point, { 38.002f, 413.645f, -1152.029f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_Pink_1", LightType::Point, { -190.160f, 456.598f, -1358.485f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_Orange_1", LightType::Point, { -300.488f, 487.891f, -1459.170f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_White_1", LightType::Point, { -409.375f, 525.982f, -1559.197f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a8_White_2", LightType::Point, { 153.299f, 402.848f, -1048.723f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Red_1", LightType::Point, { -398.662f, 402.277f, -970.280f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Blue_1", LightType::Point, { -762.247f, 464.884f, -1234.719f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Blue_2", LightType::Point, { -149.645f, 393.773f, -786.914f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Green_1", LightType::Point, { 96.381f, 412.834f, -603.955f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Pink_1", LightType::Point, { -519.168f, 416.382f, -1058.355f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Orange_1", LightType::Point, { -641.215f, 437.353f, -1147.127f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_Orange_2", LightType::Point, { -275.176f, 394.583f, -879.577f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a7_White_1", LightType::Point, { -23.891f, 400.074f, -693.617f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Red_1", LightType::Point, { -442.248f, 376.678f, -669.502f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Blue_1", LightType::Point, { -1100.092f, 505.251f, -1029.565f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Blue_2", LightType::Point, { -841.299f, 426.313f, -890.624f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Blue_3", LightType::Point, { -175.823f, 391.055f, -517.250f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Green_1", LightType::Point, { -971.651f, 461.238f, -961.184f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Green_2", LightType::Point, { -305.973f, 379.251f, -592.081f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_Pink_1", LightType::Point, { -578.188f, 384.001f, -745.768f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a6_White_1", LightType::Point, { -711.655f, 400.800f, -819.719f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Red_1", LightType::Point, { -253.277f, 367.390f, -219.987f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Blue_1", LightType::Point, { -1334.807f, 423.281f, 156.104f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Blue_2", LightType::Point, { -724.166f, 346.979f, -63.053f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Green_1", LightType::Point, { -567.342f, 346.328f, -116.469f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Pink_1", LightType::Point, { -880.566f, 355.299f, -8.643f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_Orange_1", LightType::Point, { -1033.258f, 370.644f, 45.618f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_White_1", LightType::Point, { -1184.778f, 393.377f, 100.602f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a5_White_2", LightType::Point, { -410.368f, 353.183f, -168.821f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Red_1", LightType::Point, { -286.899f, 368.573f, 258.690f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Blue_1", LightType::Point, { -905.586f, 439.003f, 696.104f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Blue_2", LightType::Point, { -661.700f, 382.764f, 523.168f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Blue_3", LightType::Point, { -37.506f, 406.745f, 83.367f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Green_1", LightType::Point, { -784.440f, 406.229f, 610.194f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Green_2", LightType::Point, { -159.289f, 383.334f, 168.912f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_Pink_1", LightType::Point, { -414.404f, 363.773f, 348.520f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a4_White_1", LightType::Point, { -539.734f, 368.707f, 436.979f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Red_1", LightType::Point, { 88.207f, 405.515f, 171.115f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Blue_1", LightType::Point, { -535.231f, 530.034f, 1037.614f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Blue_2", LightType::Point, { -189.162f, 417.896f, 544.928f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Green_1", LightType::Point, { -97.711f, 406.800f, 419.826f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Pink_1", LightType::Point, { -279.268f, 436.140f, 670.215f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_Orange_1", LightType::Point, { -366.305f, 460.666f, 793.115f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_White_1", LightType::Point, { -451.744f, 492.010f, 915.737f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a3_White_2", LightType::Point, { -5.287f, 402.726f, 295.209f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Red_1", LightType::Point, { 290.190f, 340.401f, 600.641f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Blue_1", LightType::Point, { 32.674f, 374.829f, 1179.033f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Blue_2", LightType::Point, { 134.676f, 337.541f, 950.010f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Blue_3", LightType::Point, { 392.957f, 381.904f, 369.871f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Green_1", LightType::Point, { 83.319f, 352.265f, 1065.115f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Green_2", LightType::Point, { 342.856f, 357.688f, 482.348f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_Pink_1", LightType::Point, { 237.422f, 331.411f, 719.197f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a2_White_1", LightType::Point, { 185.397f, 330.608f, 836.014f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Red_1", LightType::Point, { 790.364f, 410.112f, 536.252f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Blue_1", LightType::Point, { 460.590f, 575.318f, 1352.414f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Blue_2", LightType::Point, { 645.596f, 446.863f, 894.465f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Green_1", LightType::Point, { 693.610f, 428.758f, 775.627f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Pink_1", LightType::Point, { 597.930f, 470.932f, 1012.440f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_Orange_1", LightType::Point, { 551.555f, 500.098f, 1127.192f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_White_1", LightType::Point, { 505.719f, 534.943f, 1240.619f }, 350.000f, 35.000f);
    createLight("paris_stringlights_01a1_White_2", LightType::Point, { 741.889f, 416.556f, 656.215f }, 350.000f, 35.000f);

    // Lanterns.
    createLight("Paris_Lantern_01A", LightType::Point, { 26.641f, 227.326f, -726.443f }, 450.000f, 120.000f);
    createLight("Paris_Lantern_01A1", LightType::Point, { -109.381f, 227.232f, -474.040f }, 450.000f, 120.000f);
    createLight("Paris_Lantern_01A2", LightType::Point, { 172.517f, 227.029f, 215.806f }, 450.000f, 120.000f);
    createLight("Paris_Lantern_01A3", LightType::Point, { 450.699f, 227.030f, 325.834f }, 450.000f, 120.000f);
    createLight("Paris_Lantern_01A4", LightType::Point, { 726.789f, 227.031f, 435.034f }, 450.000f, 120.000f);

    // Interior MASTER_Interior_01_Paris_Lantern, below each shade opening.
    createLight("paris_building_01_interior_63", LightType::Point, { 722.376f, 248.922f, -278.580f }, 450.000f, 120.000f);
    createLight("paris_building_01_interior_105", LightType::Point, { 879.741f, 248.922f, -192.013f }, 450.000f, 120.000f);
    createLight("paris_building_01_interior_106", LightType::Point, { 1037.195f, 248.922f, -105.398f }, 450.000f, 120.000f);
    createLight("paris_building_01_interior_107", LightType::Point, { 662.391f, 248.922f, -420.726f }, 450.000f, 120.000f);

    // Four Paris_Ceiling_Light map_Ke disks, point and downlight below each lower shade rim.
    createLight("Paris_Ceiling_Light_01_Bulb", LightType::Point, { 611.446f, 292.996f, -849.156f }, 550.000f, 150.000f);
    createLight("Paris_Ceiling_Light_01_Downlight", LightType::Spot, { 611.446f, 292.996f, -849.156f }, 1000.000f, 400.000f);
    createLight("Paris_Ceiling_Light_02_Bulb", LightType::Point, { 424.950f, 307.563f, -493.348f }, 550.000f, 150.000f);
    createLight("Paris_Ceiling_Light_02_Downlight", LightType::Spot, { 424.950f, 307.563f, -493.348f }, 1000.000f, 400.000f);
    createLight("Paris_Ceiling_Light_03_Bulb", LightType::Point, { 500.106f, 307.563f, -50.352f }, 550.000f, 150.000f);
    createLight("Paris_Ceiling_Light_03_Downlight", LightType::Spot, { 500.106f, 307.563f, -50.352f }, 1000.000f, 400.000f);
    createLight("Paris_Ceiling_Light_04_Bulb", LightType::Point, { 798.379f, 307.563f, 100.141f }, 550.000f, 150.000f);
    createLight("Paris_Ceiling_Light_04_Downlight", LightType::Spot, { 798.379f, 307.563f, 100.141f }, 1000.000f, 400.000f);

    // Seven Paris_Wall_Bulb_Light components, 2 units below the bulb surface.
    createLight("Paris_Wall_Light_Interior_01", LightType::Point, { 187.437f, 229.332f, -813.197f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_02", LightType::Point, { 45.800f, 229.332f, -553.251f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_03", LightType::Point, { -99.001f, 229.332f, -286.208f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_04", LightType::Point, { 36.714f, 229.332f, 57.345f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_05", LightType::Point, { 334.293f, 229.332f, 199.135f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_06", LightType::Point, { 615.823f, 229.332f, 309.739f }, 100.207f, 50.297f);
    createLight("Paris_Wall_Light_Interior_02A", LightType::Point, { 773.119f, 241.921f, -463.083f }, 100.207f, 50.297f);

    // Two ceiling fan bottom glass globes
    createLight("Paris_CeilingFan_01A_Base_mesh_01__2_", LightType::Point, { 639.938f, 343.190f, 88.496f }, 450.000f, 120.000f);
    createLight("Paris_CeilingFan_01A_Base_mesh_01", LightType::Point, { 340.091f, 343.190f, -97.993f }, 450.000f, 120.000f);

    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
