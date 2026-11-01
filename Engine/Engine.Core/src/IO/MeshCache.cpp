#include "Engine.Core/IO/MeshCache.h"
#include "Engine.Core/IO/AssetCache.h"
#include "Engine.Core/Logging/Logger.h"
#include <bcrypt.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>
#pragma comment(lib, "bcrypt.lib")

namespace
{
    using namespace Engine::Core;
    constexpr uint64_t Magic = 0x004853454d504550ull; // PEPMESH, Windows little-endian.
    constexpr size_t DigestSize = 32;
    static_assert(sizeof(Vertex) == 56 && sizeof(DirectX::BoundingBox) == 24 &&
        sizeof(DirectX::SimpleMath::Vector3) == 12 && sizeof(DirectX::SimpleMath::Matrix) == 64 &&
        sizeof(SkinVertexRange) == 8 && sizeof(SkinInfluence) == 8, "Update the native mesh cache format for this layout");

    class Digest
    {
    public:
        Digest()
        {
            if (BCryptOpenAlgorithmProvider(&_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                throw std::runtime_error("Cannot open cache checksum provider");
            if (BCryptCreateHash(_algorithm, &_hash, nullptr, 0, nullptr, 0, 0) < 0)
            {
                BCryptCloseAlgorithmProvider(_algorithm, 0);
                throw std::runtime_error("Cannot create cache checksum");
            }
        }
        ~Digest() { BCryptDestroyHash(_hash); BCryptCloseAlgorithmProvider(_algorithm, 0); }
        void Add(const void* data, ULONG size)
        {
            if (BCryptHashData(_hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), size, 0) < 0)
                throw std::runtime_error("Cannot checksum cache data");
        }
        std::array<unsigned char, DigestSize> Finish()
        {
            std::array<unsigned char, DigestSize> result{};
            if (BCryptFinishHash(_hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
                throw std::runtime_error("Cannot finish cache checksum");
            return result;
        }
    private:
        BCRYPT_ALG_HANDLE _algorithm = nullptr;
        BCRYPT_HASH_HANDLE _hash = nullptr;
    };

    class Reader
    {
    public:
        explicit Reader(const std::filesystem::path& path) : Stream(path, std::ios::binary | std::ios::ate)
        {
            const auto size = Stream.tellg();
            if (size < static_cast<std::streamoff>(DigestSize)) throw std::runtime_error("Truncated mesh cache");
            Remaining = static_cast<uint64_t>(size) - DigestSize;
            Stream.seekg(0);
        }
        void Bytes(void* data, size_t size)
        {
            if (size > Remaining) throw std::runtime_error("Truncated mesh cache record");
            Remaining -= size;
            auto* bytes = static_cast<char*>(data);
            while (size)
            {
                const auto chunk = static_cast<ULONG>((std::min)(size, size_t(1024 * 1024)));
                if (!Stream.read(bytes, chunk)) throw std::runtime_error("Cannot read mesh cache");
                Hash.Add(bytes, chunk);
                bytes += chunk;
                size -= chunk;
            }
        }
        template<class T> T Pod()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T value{}; Bytes(&value, sizeof(value)); return value;
        }
        uint32_t Count(size_t minimumBytes)
        {
            const auto count = Pod<uint32_t>();
            if (count > Remaining / minimumBytes) throw std::runtime_error("Invalid mesh cache count");
            return count;
        }
        template<class T> std::vector<T> Array()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            std::vector<T> values(Count(sizeof(T)));
            Bytes(values.data(), values.size() * sizeof(T));
            return values;
        }
        std::string String()
        {
            std::string value(Count(1), '\0'); Bytes(value.data(), value.size()); return value;
        }
        void Finish()
        {
            if (Remaining) throw std::runtime_error("Unexpected mesh cache payload");
            std::array<unsigned char, DigestSize> saved{};
            if (!Stream.read(reinterpret_cast<char*>(saved.data()), saved.size()) || saved != Hash.Finish())
                throw std::runtime_error("Mesh cache checksum mismatch");
        }
    private:
        std::ifstream Stream;
        Digest Hash;
        uint64_t Remaining = 0;
    };

    class Writer
    {
    public:
        explicit Writer(const std::filesystem::path& path) : Stream(path, std::ios::binary | std::ios::trunc) {}
        void Bytes(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const char*>(data);
            while (size)
            {
                const auto chunk = static_cast<ULONG>((std::min)(size, size_t(1024 * 1024)));
                if (!Stream.write(bytes, chunk)) throw std::runtime_error("Cannot write mesh cache");
                Hash.Add(bytes, chunk);
                bytes += chunk;
                size -= chunk;
            }
        }
        template<class T> void Pod(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            Bytes(&value, sizeof(value));
        }
        void Count(size_t size)
        {
            if (size > UINT32_MAX) throw std::runtime_error("Mesh cache count exceeds 32 bits");
            Pod(static_cast<uint32_t>(size));
        }
        template<class T> void Array(const std::vector<T>& values)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            Count(values.size()); Bytes(values.data(), values.size() * sizeof(T));
        }
        void String(const std::string& value) { Count(value.size()); Bytes(value.data(), value.size()); }
        void Finish()
        {
            const auto hash = Hash.Finish();
            Stream.write(reinterpret_cast<const char*>(hash.data()), hash.size());
            Stream.close();
            if (!Stream) throw std::runtime_error("Cannot finish mesh cache file");
        }
    private:
        std::ofstream Stream;
        Digest Hash;
    };

    void CheckCancellation(const std::function<bool()>& keepLoading)
    {
        if (keepLoading && !keepLoading()) throw std::runtime_error("Mesh cache operation cancelled");
    }
}

namespace Engine::Core
{
    std::unique_ptr<Mesh> LoadMeshCache(const std::filesystem::path& path, const std::function<bool()>& keepLoading)
    {
        std::error_code error;
        if (path.empty() || !std::filesystem::is_regular_file(path, error)) return nullptr;
        try
        {
            Reader input(path);
            if (input.Pod<uint64_t>() != Magic || input.Pod<uint32_t>() != MeshCacheVersion ||
                input.Pod<uint32_t>() != sizeof(Vertex)) throw std::runtime_error("Unsupported mesh cache format");
            const auto bounds = input.Pod<DirectX::BoundingBox>();
            const auto materialCount = input.Count(4 + 6 * 4 + 3 * 12 + 3 * 4 + 2);
            std::vector<MeshMaterial> materials;
            materials.reserve(materialCount);
            for (uint32_t i = 0; i < materialCount; ++i)
            {
                CheckCancellation(keepLoading);
                MeshMaterial material;
                material.Name = input.String();
                for (auto* texture : { &material.DiffuseTexturePath, &material.NormalTexturePath, &material.SpecularTexturePath,
                    &material.RoughnessTexturePath, &material.EmissiveTexturePath, &material.OpacityTexturePath })
                    *texture = std::filesystem::u8path(input.String());
                material.DiffuseColor = input.Pod<DirectX::SimpleMath::Vector3>();
                material.SpecularColor = input.Pod<DirectX::SimpleMath::Vector3>();
                material.EmissiveColor = input.Pod<DirectX::SimpleMath::Vector3>();
                material.Roughness = input.Pod<float>(); material.Metallic = input.Pod<float>(); material.Opacity = input.Pod<float>();
                const auto type = input.Pod<uint8_t>();
                const auto baked = input.Pod<uint8_t>();
                if (type > static_cast<uint8_t>(EMaterialType::Transparent) || baked > 1)
                    throw std::runtime_error("Invalid cached material");
                material.Type = static_cast<EMaterialType>(type); material.UseBakedLighting = baked != 0;
                materials.push_back(std::move(material));
            }
            const auto partCount = input.Count(4 + 24 + 4 + 5 * 4);
            std::vector<SubMesh> parts;
            parts.reserve(partCount);
            for (uint32_t i = 0; i < partCount; ++i)
            {
                CheckCancellation(keepLoading);
                SubMesh part;
                part.MaterialIndex = input.Pod<uint32_t>();
                if (!materials.empty() && part.MaterialIndex >= materials.size()) throw std::runtime_error("Invalid cached material index");
                part.Bounds = input.Pod<DirectX::BoundingBox>();
                part.SourceNodeName = input.String();
                part.Vertices = input.Array<Vertex>();
                part.Indices = input.Array<uint32_t>();
                for (const auto index : part.Indices)
                    if (index >= part.Vertices.size()) throw std::runtime_error("Invalid cached vertex index");
                const auto boneCount = input.Count(4 + 64);
                part.Bones.reserve(boneCount);
                for (uint32_t bone = 0; bone < boneCount; ++bone)
                    part.Bones.push_back({ input.String(), input.Pod<DirectX::SimpleMath::Matrix>() });
                part.SkinVertices = input.Array<SkinVertexRange>();
                part.Influences = input.Array<SkinInfluence>();
                for (const auto& range : part.SkinVertices)
                    if (range.Offset > part.Influences.size() || range.Count > part.Influences.size() - range.Offset)
                        throw std::runtime_error("Invalid cached skin range");
                for (const auto& influence : part.Influences)
                    if (influence.BoneIndex >= part.Bones.size()) throw std::runtime_error("Invalid cached bone index");
                if (!part.SkinVertices.empty() && part.SkinVertices.size() != part.Vertices.size())
                    throw std::runtime_error("Invalid cached skin vertex count");
                parts.push_back(std::move(part));
            }
            input.Finish();
            CheckCancellation(keepLoading);
            if (parts.empty()) throw std::runtime_error("Empty mesh cache");
            return std::make_unique<Mesh>(std::move(parts), bounds, std::move(materials));
        }
        catch (const std::exception& exception)
        {
            Log::Warning("Mesh", "Cannot use engine mesh cache ", path, ": ", exception.what());
            return nullptr;
        }
    }

    bool SaveMeshCache(const std::filesystem::path& path, const Mesh& mesh, const std::function<bool()>& keepLoading)
    {
        if (path.empty()) return false;
        const auto temporary = CacheTemporaryPath(path);
        try
        {
            Writer output(temporary);
            output.Pod(Magic); output.Pod(static_cast<uint32_t>(MeshCacheVersion)); output.Pod(static_cast<uint32_t>(sizeof(Vertex)));
            output.Pod(mesh.GetBounds());
            output.Count(mesh.GetMaterials().size());
            for (const auto& material : mesh.GetMaterials())
            {
                CheckCancellation(keepLoading);
                output.String(material.Name);
                for (const auto* texture : { &material.DiffuseTexturePath, &material.NormalTexturePath, &material.SpecularTexturePath,
                    &material.RoughnessTexturePath, &material.EmissiveTexturePath, &material.OpacityTexturePath }) output.String(texture->u8string());
                output.Pod(material.DiffuseColor); output.Pod(material.SpecularColor); output.Pod(material.EmissiveColor);
                output.Pod(material.Roughness); output.Pod(material.Metallic); output.Pod(material.Opacity);
                output.Pod(static_cast<uint8_t>(material.Type)); output.Pod(static_cast<uint8_t>(material.UseBakedLighting));
            }
            output.Count(mesh.GetSubMeshCount());
            for (const auto& part : mesh.GetSubMeshes())
            {
                CheckCancellation(keepLoading);
                output.Pod(part.MaterialIndex); output.Pod(part.Bounds); output.String(part.SourceNodeName);
                output.Array(part.Vertices); output.Array(part.Indices);
                output.Count(part.Bones.size());
                for (const auto& bone : part.Bones) { output.String(bone.NodeName); output.Pod(bone.InverseBind); }
                output.Array(part.SkinVertices); output.Array(part.Influences);
            }
            output.Finish();
            CheckCancellation(keepLoading);
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::runtime_error("Cannot publish mesh cache");
            return true;
        }
        catch (const std::exception& exception)
        {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            Log::Warning("Mesh", "Cannot save engine mesh cache ", path, ": ", exception.what());
            return false;
        }
    }
}
