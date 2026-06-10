#include "App.Base/WorldLoader.h"
#include "App.Base/World.h"

#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Types/TextureTypes.h"
#include "App.Base/Modules/RenderModule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
        GDX12Texture* Opacity = nullptr;
        bool HasNormalMap = false;
        bool HasSpecularMap = false;
        bool HasRoughnessMap = false;
        bool HasEmissiveMap = false;
        bool HasOpacityMap = false;
    };

    /// Builds a GPU texture cache key from source path and texture semantic type.
    std::wstring BuildTextureCacheKey(const std::filesystem::path& texturePath, const Engine::Core::ETextureType textureType)
    {
        std::wstring textureKey = texturePath.lexically_normal().generic_wstring();
        textureKey += L"#type:";
        textureKey += std::to_wstring(static_cast<std::uint32_t>(textureType));
        return textureKey;
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
        material->OpacityMap = defaultTextures.White;
        material->HasNormalMap = false;
		material->HasSpecularMap = false;
		material->HasRoughnessMap = false;
		material->HasEmissiveMap = false;
		material->HasOpacityMap = false;
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
            textures.Emissive == nullptr ||
            textures.Opacity == nullptr)
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
        material->OpacityMap = textures.Opacity;
        material->HasNormalMap = textures.HasNormalMap;
		material->HasSpecularMap = textures.HasSpecularMap;
		material->HasRoughnessMap = textures.HasRoughnessMap;
		material->HasEmissiveMap = textures.HasEmissiveMap;
		material->HasOpacityMap = textures.HasOpacityMap;
		material->UseBakedLighting = sourceMaterial.UseBakedLighting;
		material->Type = sourceMaterial.Type == Engine::Core::EMaterialType::Transparent ? MaterialType::Transparent : MaterialType::Opaque;
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
                textures.Diffuse = LoadSceneGpuTexture(renderModule, assetManager, diffuseTexturePath, Engine::Core::ETextureType::Color, sceneName, gpuTexturesByPath);
            }

            if (textures.Diffuse == nullptr)
            {
                textures.Diffuse = CreateDiffuseColorTexture(renderModule, sceneName, materials[materialIndex]);
            }
            
            textures.Normal = defaultTextures.FlatNormal;
            textures.Specular = defaultTextures.White;
            textures.Roughness = defaultTextures.White;
            textures.Emissive = defaultTextures.Black;
            textures.Opacity = defaultTextures.White;

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

            GDX12Texture* opacityTexture = LoadSceneGpuTexture(renderModule, assetManager, materials[materialIndex].OpacityTexturePath, Engine::Core::ETextureType::Data, sceneName, gpuTexturesByPath);
            if (opacityTexture != nullptr)
            {
                textures.Opacity = opacityTexture;
                textures.HasOpacityMap = true;
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
            return false;
        }
        
        CreateCameraForBounds(world, sceneBounds);
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
    
    if (std::filesystem::is_directory(path))
    {
        return LoadObjSceneFiles(world, *renderModule, assetManager, path, CollectSceneObjPaths(path));
    }
    
    if (IsObjPath(path))
    {
        DirectX::BoundingBox sceneBounds = {};
        std::unordered_map<std::wstring, GDX12Texture*> gpuTexturesByPath;
        if (!LoadObjSceneEntity(
            world,
            *renderModule,
            assetManager,
            path,
            BuildScenePartName(path.parent_path(), path),
            gpuTexturesByPath,
            sceneBounds))
        {
            return false;
        }

        //CreateCameraForBounds(world, sceneBounds);
        //return true;
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
    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
