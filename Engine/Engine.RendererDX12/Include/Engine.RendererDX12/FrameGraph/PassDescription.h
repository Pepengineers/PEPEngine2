#pragma once

#include <dxgiformat.h>
#include <any>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using NodeId = std::string;
using PinId = std::string;

struct UInt2
{
    uint32_t x = 0;
    uint32_t y = 0;
    bool operator==(const UInt2& rhs) const { return x == rhs.x && y == rhs.y; }
    bool operator!=(const UInt2& rhs) const { return !(*this == rhs); }
};

enum class MemoryType { Auto, Local, Shared };
const char* GetMemoryTypeName(MemoryType type);
enum class ResourceType { Texture2D, Texture2DArray, TextureCube, RawBuffer, StructuredBuffer, DrawStream };
const char* GetResourceTypeName(ResourceType type);
std::string GetTextureFormatName(DXGI_FORMAT format);
std::string FormatTextureSize(UInt2 size);
inline bool IsTexture(ResourceType type)
{
    return type == ResourceType::Texture2D || type == ResourceType::Texture2DArray || type == ResourceType::TextureCube;
}
enum class DataAccess { ReadOnly, WriteOnly, ReadWrite };

enum class ResourceUsage : uint32_t
{
    None = 0, PixelShaderRead = 1, NonPixelShaderRead = 2, ComputeShaderRead = 2, GraphicsShaderRead = 3,
    RenderTarget = 4, DepthWrite = 8, DepthRead = 16, UnorderedAccess = 32,
    CopySource = 64, CopyDest = 128, Present = 256, IndirectArguments = 512,
    VertexIndex = 2048, ConstantBuffer = 4096
};
inline ResourceUsage operator|(ResourceUsage a, ResourceUsage b) { return ResourceUsage(uint32_t(a) | uint32_t(b)); }
inline ResourceUsage& operator|=(ResourceUsage& a, ResourceUsage b) { return a = a | b; }
inline bool HasUsage(ResourceUsage usages, ResourceUsage requested) { return (uint32_t(usages) & uint32_t(requested)) != 0; }
inline bool IsDepthFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D16_UNORM
        || format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

struct Extent
{
    enum class Source { None, Render, Display, Fixed, Input };
    Source Type = Source::None;
    PinId Input;
    UInt2 Value;
    static Extent Render() { return {Source::Render, {}, {}}; }
    static Extent Display() { return {Source::Display, {}, {}}; }
    static Extent Fixed(UInt2 value) { return {Source::Fixed, {}, value}; }
    static Extent SameAs(std::string input) { return {Source::Input, std::move(input), {}}; }
};

inline constexpr uint32_t DrawStreamStride = 6 * sizeof(uint32_t);

struct PortDescription
{
    ResourceType Type = ResourceType::Texture2D;
    std::vector<DXGI_FORMAT> Formats;
    ResourceUsage Usages = ResourceUsage::None;
    DataAccess Access = DataAccess::ReadOnly;
    MemoryType RequestedMemory = MemoryType::Auto;
    bool Required = true;
    bool AllowLocal = true;
    bool AllowShared = false;
    Extent Size;
    uint32_t Layers = 1;
    uint64_t Capacity = 1;
    uint32_t Stride = 0;
    std::string Layout;
    PinId AliasInput;
    bool IsDepth() const { return !Formats.empty() && IsDepthFormat(Formats.front()); }
};

struct PassDescription
{
    bool PrimaryOnly = false;
    std::map<PinId, PortDescription> Inputs;
    std::map<PinId, PortDescription> Outputs;
    std::vector<std::vector<PinId>> SameTextureSizeGroups;

    PortDescription& AddInputTexture(PinId id, ResourceType type = ResourceType::Texture2D);
    PortDescription& AddOutputTexture(PinId id, ResourceType type = ResourceType::Texture2D);
    PortDescription& AddInputBuffer(PinId id, ResourceType type = ResourceType::StructuredBuffer);
    PortDescription& AddOutputBuffer(PinId id, ResourceType type = ResourceType::StructuredBuffer);
    PortDescription& AddInputDrawStream(PinId id);
    PortDescription& AddOutputDrawStream(PinId id);
    template<class T> void AddParameter(std::string name, T defaultValue)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "Render pass parameters must be numeric, bool or enum.");
        if (name.empty() || !_parameters.try_emplace(name, defaultValue).second)
        { throw std::invalid_argument("ERROR: Render pass parameter '" + name + "' is empty or already declared."); }
    }
    template<class T> T GetParameter(const std::string& name) const
    {
        const auto parameter = _parameters.find(name);
        if (parameter == _parameters.end())
        { throw std::invalid_argument("ERROR: Render pass parameter '" + name + "' is not declared."); }
        const auto* value = std::any_cast<T>(&parameter->second);
        if (!value)
        { throw std::invalid_argument("ERROR: Incorrect type for render pass parameter '" + name + "'."); }
        return *value;
    }
    void RequireSameTextureSize(std::initializer_list<PinId> ids) { SameTextureSizeGroups.emplace_back(ids); }
    const PortDescription& GetPortDescription(const PinId& id) const;
    const PortDescription& GetInputDescription(const PinId& id) const;
    const PortDescription& GetOutputDescription(const PinId& id) const;
    void ValidatePorts(const NodeId& node) const;
private:
    friend class NodeHandle;
    std::map<std::string, std::any> _parameters;
    PortDescription& AddPort(PinId id, bool input, ResourceType type);
};

struct ResourceDescription
{
    ResourceType Type = ResourceType::Texture2D;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t ArraySize = 1;
    uint64_t Capacity = 1;
    uint32_t Stride = 0;
    std::string Layout;
    ResourceUsage Usages = ResourceUsage::None;
    bool IsBuffer() const { return !IsTexture(Type); }
    bool IsDepth() const { return IsDepthFormat(Format); }
    UInt2 GetSize() const { return {Width, Height}; }
};
