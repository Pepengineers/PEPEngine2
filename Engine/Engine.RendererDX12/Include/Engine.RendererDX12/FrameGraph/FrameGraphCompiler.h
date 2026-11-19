#pragma once

#include "Engine.RendererDX12/FrameGraph/PassDescription.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include <array>
#include <optional>
#include <set>

enum class TransportQueuePolicy { Auto, ForceDirect, ForceCopy };

inline const char* GetDeviceRoleName(EDeviceRole role)
{ return role == DEVICE_ROLE_PRIMARY ? "primary" : "secondary"; }

struct PinAddress
{
    NodeId Node;
    PinId Pin;
    bool operator==(const PinAddress& rhs) const { return Node == rhs.Node && Pin == rhs.Pin; }
    bool operator<(const PinAddress& rhs) const { return Node < rhs.Node || (Node == rhs.Node && Pin < rhs.Pin); }
};

struct NodeDefinition
{
    NodeId Id;
    EDeviceRole DeviceRole = DEVICE_ROLE_PRIMARY;
    PassDescription Description;
};
struct Connection { PinAddress Source; PinAddress Target; };
struct FrameGraphDefinition
{
    bool IsCompiled = false;
    std::map<NodeId, NodeDefinition> Nodes;
    std::vector<Connection> Connections;
    std::vector<std::pair<NodeId, NodeId>> OrderDependencies;
    const PortDescription& GetPortDescription(const PinAddress& address) const;
};

struct PortCapabilities { bool Local = true; bool Shared = false; };
// The CPU planner receives resolved device and SDK data.
struct FrameGraphPortData
{
    PortCapabilities Capabilities;
    std::optional<UInt2> RequiredTextureSize;
    std::optional<ResourceDescription> Imported;
};

struct FrameGraphCompileData
{
    UInt2 DisplaySize;
    UInt2 RenderSize;
    std::map<PinAddress, FrameGraphPortData> Ports;
    std::array<std::set<DXGI_FORMAT>, 2> ComputeCopyFormats;
    std::set<PinAddress> RejectedComputeExports;
    std::set<PinAddress> RejectedComputeImports;
    TransportQueuePolicy QueuePolicy = TransportQueuePolicy::Auto;
    uint32_t FrameSlotCount = 3;
};

inline bool NeedsFrameGraphLocalCopy(MemoryType storage, MemoryType access, bool remote)
{ return access == MemoryType::Local && (remote || storage == MemoryType::Shared); }

struct ResourcePlan
{
    ResourceDescription Description;
    MemoryType Storage = MemoryType::Local;
    bool Export = false;
    bool BufferPayload = false;
    bool ComputeExport = false;
    bool ComputeImport = false;
    std::array<ResourceUsage, 2> SharedTextureUsages{};
    std::array<ResourceUsage, 2> LocalTextureUsages{};
    PinAddress Alias;
    std::map<PinAddress, MemoryType> Consumers;
    uint32_t CopyCount = 0;
};
struct FrameGraphPlan
{
    std::vector<NodeId> Order;
    std::map<NodeId, std::vector<NodeId>> Dependencies;
    std::map<PinAddress, ResourcePlan> Resources;
    uint32_t FrameSlotCount = 0;
    uint32_t CopyCount = 0;
    bool UseCopyQueue = false;
    std::string GetDebugReport(const FrameGraphDefinition& definition, const FrameGraphCompileData& compileData) const;
};

// Validate topology, resolve descriptions and rank the available transport methods.
// Return a CPU plan; the runtime allocates GPU resources and initializes passes separately.
FrameGraphPlan CompileFrameGraph(const FrameGraphDefinition& graphDefinition, const FrameGraphCompileData& compileData);
