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

enum class TransferMethod
{
    None,
    DirectQueue,
    CopyQueue,
    // Requires shader access to cross-adapter ROW_MAJOR textures on the GPU executing the dispatch.
    ComputeCopy
};

// Controls how a copy contributes to per-frame cross-GPU transfer metrics.
enum class CopyScope
{
    // Omit this same-device copy from cross-GPU metrics.
    Local,
    // Record the copy time and count the transferred resource and its bytes.
    CrossGPU,
    // Record only the import time. The export already counted the resource and its bytes.
    CrossGPUAfterExport
};

struct ResourceTransfer
{
    TransferMethod Method = TransferMethod::None;
    CopyScope Scope = CopyScope::Local;
    explicit operator bool() const { return Method != TransferMethod::None; }
};

struct ResourcePlan
{
    ResourceDescription Description;
    MemoryType Storage = MemoryType::Local;
    // Resolved for the entire allocation, including all mutable alias versions.
    bool ReuseAcrossFrames = false;
    ResourceTransfer Export;
    // Each device imports at most once for all consumers of this version of the resource.
    std::array<ResourceTransfer, 2> Imports{};
    bool BufferPayload = false;
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
