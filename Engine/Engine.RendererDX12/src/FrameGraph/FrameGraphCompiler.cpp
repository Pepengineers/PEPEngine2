#include "Engine.RendererDX12/FrameGraph/FrameGraphCompiler.h"

#include <functional>
#include <limits>
#include <tuple>
#include <algorithm>
#include <set>

namespace
{
    bool IsInputPort(const FrameGraphDefinition& definition, const PinAddress& address)
    { return definition.Nodes.at(address.Node).Description.Inputs.count(address.Pin) != 0; }

    std::string FormatPinAddress(const PinAddress& address) { return address.Node + "." + address.Pin; }

    const FrameGraphPortData& GetPortCompileData(const FrameGraphCompileData& data, const PinAddress& address)
    {
        static const FrameGraphPortData defaults;
        auto port = data.Ports.find(address);
        return port == data.Ports.end() ? defaults : port->second;
    }

    using InputSources = std::map<PinAddress, PinAddress>;
    using NodeDependencies = std::map<NodeId, std::set<NodeId>>;
    using ResourceDescriptions = std::map<PinAddress, ResourceDescription>;

    // Use a stable connection order for placement tie-breaks and diagnostics.
    std::vector<Connection> GetSortedConnections(const FrameGraphDefinition& graphDefinition)
    {
        auto graphConnections = graphDefinition.Connections;
        std::sort(graphConnections.begin(), graphConnections.end(), [](const Connection& firstConnection, const Connection& secondConnection)
        {
            return firstConnection.Source < secondConnection.Source || (firstConnection.Source == secondConnection.Source && firstConnection.Target < secondConnection.Target);
        });
        return graphConnections;
    }

    // Validate node identifiers and device roles, and retain nodes without incoming edges.
    void ValidateGraphNodes(const FrameGraphDefinition& graphDefinition, NodeDependencies& nodeDependencies)
    {
        for (const auto& nodeEntry : graphDefinition.Nodes)
        {
            const auto& nodeDefinition = nodeEntry.second;
            if (nodeEntry.first.empty() || nodeEntry.first != nodeDefinition.Id) { throw std::invalid_argument("ERROR: NodeId is empty or does not match its definition key. Use a non-empty NodeId that matches its key in "
                "FrameGraphDefinition::Nodes."); }
            const auto& passDescription = nodeDefinition.Description;
            if (nodeDefinition.DeviceRole != DEVICE_ROLE_PRIMARY && nodeDefinition.DeviceRole != DEVICE_ROLE_SECONDARY) { throw std::invalid_argument("ERROR: " + nodeDefinition.Id + ": device role is not supported. Use DEVICE_ROLE_PRIMARY or DEVICE_ROLE_SECONDARY."); }
            if (passDescription.PrimaryOnly && nodeDefinition.DeviceRole != DEVICE_ROLE_PRIMARY) { throw std::invalid_argument("ERROR: " + nodeDefinition.Id + ": pass was assigned to the secondary device but requires the primary device. Add this pass with "
                "AddPrimaryGPUPass()."); }
            nodeDependencies[nodeDefinition.Id];
        }
    }

    // Validate connected ports and record both input sources and producer dependencies.
    void BuildConnectionDependencies(const FrameGraphDefinition& graphDefinition, const std::vector<Connection>& graphConnections,
        InputSources& inputSources, NodeDependencies& nodeDependencies)
    {
        for (const auto& graphConnection : graphConnections)
        {
            const auto& outputDescription = graphDefinition.GetPortDescription(graphConnection.Source);
            const auto& inputDescription = graphDefinition.GetPortDescription(graphConnection.Target);
            const auto& producerNode = graphDefinition.Nodes.at(graphConnection.Source.Node);
            const auto& consumerNode = graphDefinition.Nodes.at(graphConnection.Target.Node);
            if (IsInputPort(graphDefinition, graphConnection.Source) || !IsInputPort(graphDefinition, graphConnection.Target)) { throw std::invalid_argument("ERROR: Connection source is not an output or target is not an input: " + FormatPinAddress(graphConnection.Target) + ". Pass GetOutput() as the source and GetInput() as the target of ConnectNodes()."); }
            if ((producerNode.Description.RunOnceDuringSetup || consumerNode.Description.RunOnceDuringSetup) && inputDescription.Access != DataAccess::ReadOnly)
            { throw std::invalid_argument("Setup passes read their inputs once and publish immutable outputs: " + FormatPinAddress(graphConnection.Target)); }
            if (!inputSources.emplace(graphConnection.Target, graphConnection.Source).second) { throw std::invalid_argument("ERROR: " + FormatPinAddress(graphConnection.Target) + ": input has multiple connected sources. Connect exactly one output to this input and remove duplicate "
                "connections."); }
            if (outputDescription.Type != inputDescription.Type)
            { throw std::invalid_argument("ERROR: " + FormatPinAddress(graphConnection.Target) + ": expected " + GetResourceTypeName(inputDescription.Type)
                + ", but " + FormatPinAddress(graphConnection.Source) + " provides " + GetResourceTypeName(outputDescription.Type)
                + ". Connect ports with matching resource types."); }
            if (!outputDescription.Layout.empty() && !inputDescription.Layout.empty()
                && (outputDescription.Layout != inputDescription.Layout || outputDescription.Stride != inputDescription.Stride))
            { throw std::invalid_argument("ERROR: " + FormatPinAddress(graphConnection.Target) + ": expected layout '" + inputDescription.Layout
                + "' with stride " + std::to_string(inputDescription.Stride) + ", but " + FormatPinAddress(graphConnection.Source) + " provides layout '"
                + outputDescription.Layout + "' with stride " + std::to_string(outputDescription.Stride) + ". Match the connected buffer layouts and strides."); }
            nodeDependencies[consumerNode.Id].insert(producerNode.Id);
        }
    }

    // Include ordering constraints that do not originate from resource connections.
    void AddExplicitDependencies(const FrameGraphDefinition& graphDefinition, NodeDependencies& nodeDependencies)
    {
        for (const auto& orderDependency : graphDefinition.OrderDependencies)
        {
            if (!graphDefinition.Nodes.count(orderDependency.first) || !graphDefinition.Nodes.count(orderDependency.second)) { throw std::invalid_argument("ERROR: Given execution dependency references a node that has not been added. Add both nodes before declaring "
                "their execution dependency."); }
            nodeDependencies[orderDependency.second].insert(orderDependency.first);
        }
    }

    // Order destructive writes after every reader of the previous logical resource version.
    void AddAliasWriteDependencies(const FrameGraphDefinition& graphDefinition, const std::vector<Connection>& graphConnections,
        NodeDependencies& nodeDependencies)
    {
        // Destructive writes wait for every reader of the previous logical version.
        std::map<PinAddress, NodeId> resourceWriters;
        for (const auto& graphConnection : graphConnections)
        {
            const auto& inputDescription = graphDefinition.GetPortDescription(graphConnection.Target);
            if (inputDescription.Access == DataAccess::ReadOnly) { continue; }
            const auto& passDescription = graphDefinition.Nodes.at(graphConnection.Target.Node).Description;
            bool hasPublishedOutput = false;
            for (const auto& outputEntry : passDescription.Outputs) { hasPublishedOutput |= outputEntry.second.AliasInput == graphConnection.Target.Pin; }
            if (!hasPublishedOutput) { throw std::invalid_argument("ERROR: " + FormatPinAddress(graphConnection.Target) + ": writable input has no published output version. Publish one output with AliasInput naming this mutable "
                "input."); }
            if (!resourceWriters.emplace(graphConnection.Source, graphConnection.Target.Node).second) { throw std::invalid_argument("ERROR: " + FormatPinAddress(graphConnection.Source) + ": multiple passes write the same resource version. Chain writers through successive output versions instead "
                "of writing the same input version twice."); }
            for (const auto& readerConnection : graphConnections)
            {
                if (readerConnection.Source == graphConnection.Source && readerConnection.Target.Node != graphConnection.Target.Node)
                { nodeDependencies[graphConnection.Target.Node].insert(readerConnection.Target.Node); }
            }
        }
    }

    // Publish dependencies and topologically order nodes, rejecting cycles before resource resolution.
    void BuildExecutionOrder(const NodeDependencies& nodeDependencies, FrameGraphPlan& executionPlan)
    {
        for (const auto& dependencyEntry : nodeDependencies)
        { executionPlan.Dependencies[dependencyEntry.first] = std::vector<NodeId>(dependencyEntry.second.begin(), dependencyEntry.second.end()); }
        auto pendingDependencies = nodeDependencies;
        while (!pendingDependencies.empty())
        {
            auto readyNode = std::find_if(pendingDependencies.begin(), pendingDependencies.end(), [](const auto& dependencyEntry) { return dependencyEntry.second.empty(); });
            if (readyNode == pendingDependencies.end())
            {
                std::string cycleNodes;
                for (const auto& dependencyEntry : pendingDependencies) { cycleNodes += " " + dependencyEntry.first; }
                throw std::invalid_argument("ERROR: FrameGraph connections, execution dependencies, or alias versions form a cycle:" + cycleNodes + ". Remove the cyclic connections or execution dependencies and chain alias writes through successive versions.");
            }
            const auto nodeId = readyNode->first;
            executionPlan.Order.push_back(nodeId);
            pendingDependencies.erase(readyNode);
            for (auto& dependencyEntry : pendingDependencies) { dependencyEntry.second.erase(nodeId); }
        }
    }

    // Reject required inputs without sources before evaluating their resource descriptions.
    void ValidateRequiredInputs(const FrameGraphDefinition& graphDefinition, const std::vector<NodeId>& nodeOrder,
        const InputSources& inputSources)
    {
        for (const auto& nodeId : nodeOrder)
        {
            const auto& nodeDefinition = graphDefinition.Nodes.at(nodeId);
            for (const auto& inputEntry : nodeDefinition.Description.Inputs)
            {
                if (!inputEntry.second.IsOptionalInput && !inputSources.count({nodeId, inputEntry.first}))
                { throw std::invalid_argument("ERROR: " + nodeId + "." + inputEntry.first + " [" + GetDeviceRoleName(nodeDefinition.DeviceRole) + "]: required input has no connected source. Connect a compatible output to this required input before "
                    "Compile()."); }
            }
        }
    }

    // Resolve Render, Display, Fixed and SameAs extents through the current description resolver.
    UInt2 ResolveTextureExtent(const NodeDefinition& nodeDefinition, const Extent& extentExpression,
        const FrameGraphCompileData& compileData, const std::function<ResourceDescription(PinAddress)>& resolveResourceDescription)
    {
        switch (extentExpression.Type)
        {
        case Extent::Source::Render: return compileData.RenderSize;
        case Extent::Source::Display: return compileData.DisplaySize;
        case Extent::Source::Fixed: return extentExpression.Value;
        case Extent::Source::Input: return resolveResourceDescription({nodeDefinition.Id, extentExpression.Input}).GetSize();
        default: throw std::invalid_argument("ERROR: " + nodeDefinition.Id + ": texture output has no Size declaration. Assign Extent::Render(), Extent::Display(), Extent::Fixed(), or "
            "Extent::SameAs() to the output Size.");
        }
    }

    // Check consumer formats and depth/color compatibility against the resolved producer description.
    void ValidateInputResourceDescription(const PinAddress& inputAddress, const PinAddress& sourceAddress,
        const PortDescription& portDescription, const ResourceDescription& resourceDescription)
    {
        if (!portDescription.Formats.empty() && std::find(portDescription.Formats.begin(), portDescription.Formats.end(), resourceDescription.Format) == portDescription.Formats.end())
        {
            std::string expectedFormats;
            for (auto textureFormat : portDescription.Formats)
            { expectedFormats += (expectedFormats.empty() ? "" : ", ") + GetTextureFormatName(textureFormat); }
            throw std::invalid_argument("ERROR: " + FormatPinAddress(inputAddress) + ": expected format [" + expectedFormats
                + "], but " + FormatPinAddress(sourceAddress) + " provides " + GetTextureFormatName(resourceDescription.Format)
                + ". Declare a consumer view format compatible with the producer output format.");
        }
        if (portDescription.IsDepth() != resourceDescription.IsDepth()) { throw std::invalid_argument("ERROR: " + FormatPinAddress(inputAddress) + ": depth and color resource families do not match. Connect depth ports to depth resources and color ports to "
            "color resources."); }
    }

    // Build a concrete buffer or texture description from an output declaration.
    ResourceDescription CreateOutputResourceDescription(const PinAddress& portAddress, const PortDescription& portDescription,
        const Extent& extentExpression, const NodeDefinition& nodeDefinition, const FrameGraphCompileData& compileData,
        const std::function<ResourceDescription(PinAddress)>& resolveResourceDescription)
    {
        ResourceDescription resourceDescription;
        resourceDescription.Type = portDescription.Type;
        resourceDescription.Stride = portDescription.Stride;
        resourceDescription.Layout = portDescription.Layout;
        if (resourceDescription.IsBuffer())
        {
            resourceDescription.Capacity = portDescription.Capacity;
            if (!resourceDescription.Capacity || (resourceDescription.Type != ResourceType::RawBuffer && !resourceDescription.Stride)
                || resourceDescription.Capacity > (std::numeric_limits<uint32_t>::max)()
                || resourceDescription.Capacity > (std::numeric_limits<uint64_t>::max)() / (std::max)(1u, resourceDescription.Stride))
            { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": buffer capacity or stride is zero, too large, or otherwise invalid. Use Capacity in [1, UINT32_MAX], a "
                "valid structured Stride, and four-byte alignment for raw-buffer capacity."); }
        }
        else
        {
            if (portDescription.Formats.size() != 1) { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": texture output does not declare exactly one concrete format. Declare exactly one concrete format for each "
                "texture output."); }
            resourceDescription.Format = portDescription.Formats[0];
            auto textureDimensions = ResolveTextureExtent(nodeDefinition, extentExpression, compileData, resolveResourceDescription);
            resourceDescription.Width = textureDimensions.x;
            resourceDescription.Height = textureDimensions.y;
            if (portDescription.ArraySize > 2048) { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": texture array size exceeds 2048. Use an ArraySize from 1 to 2048 for a Texture2DArray."); }
            resourceDescription.ArraySize = portDescription.ArraySize;
            resourceDescription.MipLevels = portDescription.MipLevels;
            const auto extent = (std::max)(textureDimensions.x, textureDimensions.y);
            if (!portDescription.MipLevels || portDescription.MipLevels > D3D12_REQ_MIP_LEVELS || (1u << (portDescription.MipLevels - 1)) > extent)
            { throw std::invalid_argument("Invalid texture mip count: " + FormatPinAddress(portAddress)); }
            resourceDescription.ClearColor = portDescription.ClearColor;
        }
        return resourceDescription;
    }

    // Validate dimensions and array/cube shape after imports, aliases and extents have been resolved.
    void ValidateResourceDimensions(const PinAddress& portAddress, const ResourceDescription& resourceDescription)
    {
        if (!resourceDescription.IsBuffer() && (!resourceDescription.Width || !resourceDescription.Height || resourceDescription.Width > 16384 || resourceDescription.Height > 16384
            || !resourceDescription.ArraySize
            || (resourceDescription.Type == ResourceType::Texture2D && resourceDescription.ArraySize != 1)
            || (resourceDescription.Type == ResourceType::TextureCube && (resourceDescription.ArraySize != 6 || resourceDescription.Width != resourceDescription.Height))))
        { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": texture dimensions, layer count, or shape are invalid. Use non-zero supported dimensions and a valid "
            "Texture2D, Texture2DArray, or six-face square TextureCube description."); }
    }

    // Enforce pass-declared size groups after resolving connected inputs and outputs.
    void ValidateMatchingTextureSizes(const NodeDefinition& nodeDefinition,
        const InputSources& inputSources,
        const std::function<ResourceDescription(PinAddress)>& resolveResourceDescription)
    {
        for (const auto& textureGroup : nodeDefinition.Description.SameTextureSizeGroups)
        {
            UInt2 expectedSize{};
            PinId expectedPin;
            for (const auto& pinId : textureGroup)
            {
                const auto input = nodeDefinition.Description.Inputs.find(pinId);
                if (input != nodeDefinition.Description.Inputs.end() && input->second.IsOptionalInput
                    && !inputSources.count({nodeDefinition.Id, pinId})) { continue; }
                const auto actualSize = resolveResourceDescription({nodeDefinition.Id, pinId}).GetSize();
                if (expectedPin.empty()) { expectedSize = actualSize; expectedPin = pinId; }
                if (actualSize != expectedSize)
                { throw std::invalid_argument("ERROR: " + nodeDefinition.Id + "." + pinId + ": texture size " + FormatTextureSize(actualSize)
                    + " differs from " + nodeDefinition.Id + "." + expectedPin + " (" + FormatTextureSize(expectedSize)
                    + "). Make the textures in this RequireSameTextureSize() group use the same dimensions."); }
            }
        }
    }

    // Resolve connected, imported and alias resources with memoization and cycle detection.
    ResourceDescriptions ResolveResourceDescriptions(const FrameGraphDefinition& graphDefinition,
        const FrameGraphCompileData& compileData, const InputSources& inputSources, const std::vector<NodeId>& nodeOrder)
    {
        ResourceDescriptions resourceDescriptions;
        std::set<PinAddress> resolvingPorts;
        std::function<ResourceDescription(PinAddress)> resolveResourceDescription;
        resolveResourceDescription = [&](PinAddress portAddress) -> ResourceDescription
        {
            if (resourceDescriptions.count(portAddress)) { return resourceDescriptions.at(portAddress); }
            if (!resolvingPorts.insert(portAddress).second) { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": texture-size expressions form a cycle. Remove cyclic Extent::SameAs() references and resource connections."); }
            const auto& portDescription = graphDefinition.GetPortDescription(portAddress);
            const auto& nodeDefinition = graphDefinition.Nodes.at(portAddress.Node);
            const auto& portRequirements = GetPortCompileData(compileData, portAddress);
            const auto extentExpression = portRequirements.RequiredTextureSize ? Extent::Fixed(*portRequirements.RequiredTextureSize) : portDescription.Size;
            ResourceDescription resourceDescription;
            if (IsInputPort(graphDefinition, portAddress))
            {
                auto sourceEntry = inputSources.find(portAddress);
                if (sourceEntry == inputSources.end()) { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": texture Size depends on an unconnected optional input. Connect the optional input or choose an extent that "
                    "does not depend on it."); }
                resourceDescription = resolveResourceDescription(sourceEntry->second);
                ValidateInputResourceDescription(portAddress, sourceEntry->second, portDescription, resourceDescription);
                if (extentExpression.Type != Extent::Source::None)
                {
                    const auto requiredSize = ResolveTextureExtent(nodeDefinition, extentExpression, compileData, resolveResourceDescription);
                    if (resourceDescription.GetSize() != requiredSize)
                    { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": expected texture size " + FormatTextureSize(requiredSize)
                        + ", but " + FormatPinAddress(sourceEntry->second) + " provides " + FormatTextureSize(resourceDescription.GetSize())
                        + ". Make the producer texture size match the consumer requirement."); }
                }
            }
            else if (portRequirements.Imported)
            {
                resourceDescription = *portRequirements.Imported;
                if (resourceDescription.Type != portDescription.Type || resourceDescription.IsDepth() != portDescription.IsDepth()
                    || (!portDescription.Formats.empty() && std::find(portDescription.Formats.begin(), portDescription.Formats.end(),
                        resourceDescription.Format) == portDescription.Formats.end()))
                { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": imported texture type, depth usage, or format conflicts with its port description. Match the imported "
                    "texture type, depth usage, and format to its declared port."); }
            }
            else if (!portDescription.AliasInput.empty())
            {
                resourceDescription = resolveResourceDescription({nodeDefinition.Id, portDescription.AliasInput});
                if ((!portDescription.Formats.empty() && (portDescription.Formats.size() != 1 || portDescription.Formats[0] != resourceDescription.Format))
                    || (extentExpression.Type != Extent::Source::None
                        && ResolveTextureExtent(nodeDefinition, extentExpression, compileData, resolveResourceDescription) != resourceDescription.GetSize()))
                { throw std::invalid_argument("ERROR: " + FormatPinAddress(portAddress) + ": AliasInput overrides its source resource description. Keep the alias output description identical to its "
                    "source allocation."); }
            }
            else
            {
                resourceDescription = CreateOutputResourceDescription(portAddress, portDescription, extentExpression,
                    nodeDefinition, compileData, resolveResourceDescription);
            }
            resourceDescription.Usages |= portDescription.Usages;
            ValidateResourceDimensions(portAddress, resourceDescription);
            resolvingPorts.erase(portAddress);
            resourceDescriptions.emplace(portAddress, resourceDescription);
            return resourceDescription;
        };
        for (const auto& nodeId : nodeOrder)
        {
            const auto& passDescription = graphDefinition.Nodes.at(nodeId).Description;
            for (const auto& portEntry : passDescription.Inputs)
            { if (inputSources.count({nodeId, portEntry.first})) { resolveResourceDescription({nodeId, portEntry.first}); } }
            for (const auto& portEntry : passDescription.Outputs) { resolveResourceDescription({nodeId, portEntry.first}); }
            ValidateMatchingTextureSizes(graphDefinition.Nodes.at(nodeId), inputSources, resolveResourceDescription);
        }
        return resourceDescriptions;
    }

    // Intersect hardware capabilities with declared access modes and native texture restrictions.
    PortCapabilities GetResourceMemoryCapabilities(const FrameGraphDefinition& graphDefinition,
        const FrameGraphCompileData& compileData, const ResourceDescriptions& resourceDescriptions, const PinAddress& portAddress)
    {
        const auto& portDescription = graphDefinition.GetPortDescription(portAddress);
        auto memoryCapabilities = GetPortCompileData(compileData, portAddress).Capabilities;
        memoryCapabilities.Local &= portDescription.AllowLocal;
        memoryCapabilities.Shared &= portDescription.AllowShared;
        auto resourceDescription = resourceDescriptions.at(portAddress);
        if (!resourceDescription.IsBuffer() && (resourceDescription.IsDepth() || resourceDescription.Type != ResourceType::Texture2D || resourceDescription.MipLevels != 1)) { memoryCapabilities.Shared = false; }
        return memoryCapabilities;
    }

    // Compare complete placements lexicographically, including downstream aliases.
    struct PlacementCost
    {
        // The first two preferences apply to each remote input. Multiple inputs may share a replica.
        uint64_t InputsWithoutComputeImport = 0;
        uint64_t InputsWithNativeImport = 0;
        // Penalize a native export for texture routes where the queue policy permits ComputeCopy.
        uint64_t NativeExportStages = 0;
        // Count physical copies once per resource component, including downstream aliases.
        uint64_t ComponentCopies = 0;

        auto Rank() const { return std::tie(InputsWithoutComputeImport, InputsWithNativeImport, NativeExportStages, ComponentCopies); }
        void Add(const PlacementCost& cost)
        {
            InputsWithoutComputeImport += cost.InputsWithoutComputeImport;
            InputsWithNativeImport += cost.InputsWithNativeImport;
            NativeExportStages += cost.NativeExportStages;
            ComponentCopies += cost.ComponentCopies;
        }
    };

    struct PlacementCandidate
    {
        ResourcePlan Plan;
        PlacementCost Cost;
    };
    using PlacementCandidates = std::map<std::pair<PinAddress, MemoryType>, PlacementCandidate>;

    constexpr uint32_t ExportStage = 1;
    constexpr uint32_t ReplicaStage(EDeviceRole role) { return 2u << role; }

    // Treat alias versions of an import as imported allocations for writable-access constraints.
    const ResourceDescription* FindImportedResource(const FrameGraphDefinition& graphDefinition, const FrameGraphCompileData& compileData,
        const InputSources& inputSources, const PinAddress& portAddress)
    {
        const auto& imported = GetPortCompileData(compileData, portAddress).Imported;
        if (imported) { return &*imported; }
        const auto& portDescription = graphDefinition.GetPortDescription(portAddress);
        if (portDescription.AliasInput.empty()) { return nullptr; }
        return FindImportedResource(graphDefinition, compileData, inputSources, inputSources.at({portAddress.Node, portDescription.AliasInput}));
    }

    // Rank consumer bindings for one placement, including mutable alias descendants.
    void EvaluateResourcePlacement(const FrameGraphDefinition& graphDefinition,
        const FrameGraphCompileData& compileData, const std::vector<Connection>& graphConnections, const InputSources& inputSources,
        const ResourceDescriptions& resourceDescriptions, PlacementCandidates& candidates, const PinAddress& sourceAddress,
        MemoryType storageType, TransferMethod nativeCopy)
    {
        const auto& nodeDefinition = graphDefinition.Nodes.at(sourceAddress.Node);
        const auto& portDescription = graphDefinition.GetPortDescription(sourceAddress);
        const auto requestedMemory = portDescription.RequestedMemory;
        const auto memoryCapabilities = GetResourceMemoryCapabilities(graphDefinition, compileData, resourceDescriptions, sourceAddress);
        if ((requestedMemory != MemoryType::Auto && requestedMemory != storageType)
            || !(storageType == MemoryType::Local ? memoryCapabilities.Local : memoryCapabilities.Shared)
            || (GetPortCompileData(compileData, sourceAddress).Imported && storageType != MemoryType::Local)) { return; }
        // One best candidate per combination of export and per-device replicas.
        std::map<uint32_t, PlacementCandidate> consumerChoices;
        consumerChoices[0];
        auto resourceDescription = resourceDescriptions.at(sourceAddress);
        const auto receiver = nodeDefinition.DeviceRole == DEVICE_ROLE_PRIMARY ? DEVICE_ROLE_SECONDARY : DEVICE_ROLE_PRIMARY;
        const auto* imported = FindImportedResource(graphDefinition, compileData, inputSources, sourceAddress);
        const bool allowCompute = resourceDescription.Type == ResourceType::Texture2D && resourceDescription.MipLevels == 1 && !resourceDescription.IsDepth()
            && compileData.QueuePolicy != TransportQueuePolicy::ForceCopy;
        const bool canComputeExport = allowCompute && compileData.ComputeCopyFormats[nodeDefinition.DeviceRole].count(resourceDescription.Format)
            && !compileData.RejectedComputeExports.count(sourceAddress)
            && (!imported || HasUsage(imported->Usages, ResourceUsage::GraphicsShaderRead));
        const bool canComputeImport = allowCompute
            && compileData.ComputeCopyFormats[receiver].count(resourceDescription.Format)
            && !compileData.RejectedComputeImports.count(sourceAddress);
        bool hasRemoteConsumer = false;
        for (const auto& graphConnection : graphConnections)
        {
            if (!(graphConnection.Source == sourceAddress)) { continue; }
            const auto& consumerDefinition = graphDefinition.Nodes.at(graphConnection.Target.Node);
            const bool isRemote = consumerDefinition.DeviceRole != nodeDefinition.DeviceRole;
            hasRemoteConsumer |= isRemote;
            const auto& passDescription = consumerDefinition.Description;
            const auto consumerCapabilities = GetResourceMemoryCapabilities(graphDefinition, compileData, resourceDescriptions, graphConnection.Target);
            const auto requestedAccess = passDescription.Inputs.at(graphConnection.Target.Pin).RequestedMemory;
            std::map<uint32_t, PlacementCandidate> nextChoices;
            for (const auto& choiceEntry : consumerChoices)
            {
                for (auto accessType : {MemoryType::Local, MemoryType::Shared})
                {
                    if ((requestedAccess != MemoryType::Auto && requestedAccess != accessType)
                        || !(accessType == MemoryType::Local ? consumerCapabilities.Local : consumerCapabilities.Shared)) { continue; }
                    if (imported
                        && graphDefinition.GetPortDescription(graphConnection.Target).Access != DataAccess::ReadOnly
                        && (consumerDefinition.DeviceRole != nodeDefinition.DeviceRole
                            || storageType != accessType)) { continue; }
                    auto consumerChoice = choiceEntry.second;
                    bool validChoice = true;
                    for (const auto& outputEntry : passDescription.Outputs)
                    {
                        if (outputEntry.second.AliasInput != graphConnection.Target.Pin) { continue; }
                        const auto alias = candidates.find({{consumerDefinition.Id, outputEntry.first}, accessType});
                        if (alias == candidates.end()) { validChoice = false; break; }
                        consumerChoice.Cost.Add(alias->second.Cost);
                    }
                    if (!validChoice) { continue; }
                    const bool needsReplica = accessType == MemoryType::Local && (isRemote || storageType == MemoryType::Shared);
                    if (isRemote && !(canComputeImport && needsReplica))
                    {
                        ++consumerChoice.Cost.InputsWithoutComputeImport;
                        if (needsReplica) { ++consumerChoice.Cost.InputsWithNativeImport; }
                    }
                    uint32_t transferMask = choiceEntry.first;
                    if (storageType == MemoryType::Local && (isRemote || accessType == MemoryType::Shared)) { transferMask |= ExportStage; }
                    if (needsReplica) { transferMask |= ReplicaStage(consumerDefinition.DeviceRole); }
                    consumerChoice.Plan.Consumers.emplace(graphConnection.Target, accessType);
                    const auto previousChoice = nextChoices.find(transferMask);
                    if (previousChoice == nextChoices.end() || consumerChoice.Cost.Rank() < previousChoice->second.Cost.Rank())
                    { nextChoices[transferMask] = std::move(consumerChoice); }
                }
            }
            consumerChoices = std::move(nextChoices);
            if (consumerChoices.empty()) { return; }
            resourceDescription.Usages |= graphDefinition.GetPortDescription(graphConnection.Target).Usages;
        }
        const uint32_t resourceComponents = resourceDescription.Type == ResourceType::DrawStream ? 2 : 1;
        auto best = consumerChoices.begin();
        for (auto it = consumerChoices.begin(); it != consumerChoices.end(); ++it)
        {
            auto& [mask, candidate] = *it;
            auto& plan = candidate.Plan;
            const bool computeImport = (mask & ReplicaStage(receiver)) && canComputeImport;
            if (mask & ExportStage)
            {
                plan.Export.Method = nativeCopy;
                if (hasRemoteConsumer && canComputeExport) { plan.Export.Method = TransferMethod::ComputeCopy; }
                // A native export paired with a shader import runs on DIRECT.
                else if (computeImport) { plan.Export.Method = TransferMethod::DirectQueue; }
                plan.Export.Scope = hasRemoteConsumer ? CopyScope::CrossGPU : CopyScope::Local;
                plan.CopyCount += resourceComponents;
                if (allowCompute && hasRemoteConsumer && !canComputeExport) { ++candidate.Cost.NativeExportStages; }
            }
            for (auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
            {
                if (!(mask & ReplicaStage(role))) { continue; }
                auto& transfer = plan.Imports[role];
                transfer.Method = role == receiver && computeImport ? TransferMethod::ComputeCopy : nativeCopy;
                if (role == receiver) { transfer.Scope = plan.Export ? CopyScope::CrossGPUAfterExport : CopyScope::CrossGPU; }
                plan.CopyCount += resourceComponents;
            }
            candidate.Cost.ComponentCopies += plan.CopyCount;
            if (candidate.Cost.Rank() < best->second.Cost.Rank()) { best = it; }
        }
        auto& plan = best->second.Plan;
        plan.Description = resourceDescription;
        plan.Storage = storageType;
        if (!portDescription.AliasInput.empty()) { plan.Alias = {nodeDefinition.Id, portDescription.AliasInput}; }
        if (!resourceDescription.IsBuffer())
        {
            const bool localBindingsOnly = storageType == MemoryType::Local &&
                std::all_of(plan.Consumers.begin(), plan.Consumers.end(), [](const auto& consumer) { return consumer.second == MemoryType::Local; });
            const bool computeExport = plan.Export.Method == TransferMethod::ComputeCopy;
            const bool computeImport = plan.Imports[receiver].Method == TransferMethod::ComputeCopy;
            plan.BufferPayload = !computeExport && !computeImport &&
                (resourceDescription.IsDepth() || resourceDescription.Type != ResourceType::Texture2D || localBindingsOnly);
            auto& producerUsages = storageType == MemoryType::Shared ? plan.SharedTextureUsages : plan.LocalTextureUsages;
            producerUsages[nodeDefinition.DeviceRole] |= portDescription.Usages;
            if (computeExport)
            {
                plan.LocalTextureUsages[nodeDefinition.DeviceRole] |= ResourceUsage::NonPixelShaderRead;
                plan.SharedTextureUsages[nodeDefinition.DeviceRole] |= ResourceUsage::UnorderedAccess;
            }
            if (computeImport)
            {
                plan.SharedTextureUsages[receiver] |= ResourceUsage::NonPixelShaderRead;
                plan.LocalTextureUsages[receiver] |= ResourceUsage::UnorderedAccess;
            }
            for (const auto& [target, access] : plan.Consumers)
            {
                const auto role = graphDefinition.Nodes.at(target.Node).DeviceRole;
                auto& usages = access == MemoryType::Shared ? plan.SharedTextureUsages : plan.LocalTextureUsages;
                usages[role] |= graphDefinition.GetPortDescription(target).Usages;
            }
        }
        candidates.emplace(std::make_pair(sourceAddress, storageType), std::move(best->second));
    }

    // Compare Local and Shared candidates for each root output; preserve the Local tie-break.
    void BuildResourcePlacementPlans(const FrameGraphDefinition& graphDefinition, const FrameGraphCompileData& compileData,
        const std::vector<Connection>& graphConnections, const InputSources& inputSources,
        const ResourceDescriptions& resourceDescriptions, FrameGraphPlan& executionPlan)
    {
        // The cost of a later version must be known before ranking its source.
        PlacementCandidates candidates;
        const auto nativeCopy = executionPlan.UseCopyQueue ? TransferMethod::CopyQueue : TransferMethod::DirectQueue;
        for (auto it = executionPlan.Order.rbegin(); it != executionPlan.Order.rend(); ++it)
        {
            for (const auto& [pin, output] : graphDefinition.Nodes.at(*it).Description.Outputs)
            {
                for (auto storage : {MemoryType::Local, MemoryType::Shared})
                {
                    EvaluateResourcePlacement(graphDefinition, compileData, graphConnections, inputSources,
                        resourceDescriptions, candidates, {*it, pin}, storage, nativeCopy);
                }
            }
        }
        // In execution order an alias can inherit its input's already selected binding.
        for (const auto& nodeId : executionPlan.Order)
        {
            const auto& nodeDefinition = graphDefinition.Nodes.at(nodeId);
            for (const auto& outputEntry : nodeDefinition.Description.Outputs)
            {
                const PinAddress sourceAddress{nodeId, outputEntry.first};
                auto selected = candidates.end();
                if (!outputEntry.second.AliasInput.empty())
                {
                    const PinAddress input{nodeId, outputEntry.second.AliasInput};
                    const auto& sourcePlan = executionPlan.Resources.at(inputSources.at(input));
                    selected = candidates.find({sourceAddress, sourcePlan.Consumers.at(input)});
                }
                else
                {
                    selected = candidates.find({sourceAddress, MemoryType::Local});
                    const auto shared = candidates.find({sourceAddress, MemoryType::Shared});
                    if (shared != candidates.end() && (selected == candidates.end() || shared->second.Cost.Rank() < selected->second.Cost.Rank()))
                    { selected = shared; }
                }
                if (selected == candidates.end())
                {
                    std::string requestedPolicies = " requested storage=" + std::string(GetMemoryTypeName(outputEntry.second.RequestedMemory));
                    for (const auto& graphConnection : graphConnections)
                    {
                        if (!(graphConnection.Source == sourceAddress)) { continue; }
                        const auto& consumerDefinition = graphDefinition.Nodes.at(graphConnection.Target.Node);
                        auto requestedAccess = consumerDefinition.Description.Inputs.at(graphConnection.Target.Pin).RequestedMemory;
                        requestedPolicies += " " + FormatPinAddress(graphConnection.Target) + " requested access=" + GetMemoryTypeName(requestedAccess);
                    }
                    throw std::invalid_argument("ERROR: " + FormatPinAddress(sourceAddress) + " [" + GetDeviceRoleName(nodeDefinition.DeviceRole) + "]: no supported resource placement or input access satisfies the requested policies;" + requestedPolicies
                        + "; schema, SDK, hardware or alias constraint excludes requested access. Choose Local storage/access where "
                        "supported, remove conflicting alias requirements, or use adapters that support the requested shared route.");
                }
                auto& plan = selected->second.Plan;
                if (plan.CopyCount > (std::numeric_limits<uint32_t>::max)() - executionPlan.CopyCount)
                { throw std::invalid_argument("ERROR: FrameGraph transport copy count exceeds UINT32_MAX. Reduce the number of transported resource "
                    "components or cross-device connections."); }
                executionPlan.CopyCount += plan.CopyCount;
                executionPlan.Resources.emplace(sourceAddress, std::move(plan));
            }
        }
    }
    // Select one lifetime for the allocation and every alias, after placement is known.
    void SelectResourceReuse(const FrameGraphDefinition& definition, const FrameGraphCompileData& compileData,
        const InputSources& inputSources, FrameGraphPlan& plan)
    {
        std::map<PinAddress, PinAddress> roots;
        for (const auto& node : plan.Order)
        {
            for (const auto& [pin, port] : definition.Nodes.at(node).Description.Outputs)
            {
                const PinAddress address{node, pin};
                const auto& resource = plan.Resources.at(address);
                const auto root = resource.Alias.Node.empty() ? address : roots.at(inputSources.at(resource.Alias));
                roots.emplace(address, root);
                auto& allocation = plan.Resources.at(root);
                const auto& producer = definition.Nodes.at(root.Node);
                if (root == address)
                {
                    // Rotating imports retain their provider/slot bindings. Setup outputs
                    // already have their own immutable storage outside frame slots.
                    allocation.ReuseAcrossFrames = !GetPortCompileData(compileData, root).Imported.has_value()
                        && !producer.Description.RunOnceDuringSetup;
                }
                if (definition.GetPortDescription(root).ReuseAcrossFrames) { continue; }
                const bool local = resource.Storage == MemoryType::Local && !resource.Export
                    && !resource.Imports[0] && !resource.Imports[1]
                    && definition.Nodes.at(node).DeviceRole == producer.DeviceRole
                    && std::all_of(resource.Consumers.begin(), resource.Consumers.end(), [&](const auto& consumer)
                    {
                        return consumer.second == MemoryType::Local
                            && definition.Nodes.at(consumer.first.Node).DeviceRole == producer.DeviceRole;
                    });
                allocation.ReuseAcrossFrames &= local;
            }
        }
        for (const auto& [address, root] : roots)
        { plan.Resources.at(address).ReuseAcrossFrames = plan.Resources.at(root).ReuseAcrossFrames; }
    }
}

const PortDescription& FrameGraphDefinition::GetPortDescription(const PinAddress& address) const
{
    auto node = Nodes.find(address.Node);
    if (node == Nodes.end())
    { throw std::invalid_argument("ERROR: NodeId is not registered: " + address.Node + ". Add this node before referencing it in a connection or dependency."); }
    return node->second.Description.GetPortDescription(address.Pin);
}

FrameGraphPlan CompileFrameGraph(const FrameGraphDefinition& graphDefinition, const FrameGraphCompileData& compileData)
{
    if (!compileData.FrameSlotCount) { throw std::invalid_argument("ERROR: Given frame slot count is zero. Pass a frame slot count greater than zero."); }
    const auto graphConnections = GetSortedConnections(graphDefinition);
    FrameGraphPlan executionPlan;
    executionPlan.FrameSlotCount = compileData.FrameSlotCount;
    executionPlan.UseCopyQueue = compileData.QueuePolicy == TransportQueuePolicy::ForceCopy
        || (compileData.QueuePolicy == TransportQueuePolicy::Auto && compileData.FrameSlotCount > 1);

    InputSources inputSources;
    NodeDependencies nodeDependencies;
    ValidateGraphNodes(graphDefinition, nodeDependencies);
    BuildConnectionDependencies(graphDefinition, graphConnections, inputSources, nodeDependencies);
    AddExplicitDependencies(graphDefinition, nodeDependencies);
    AddAliasWriteDependencies(graphDefinition, graphConnections, nodeDependencies);
    for (const auto& [id, dependencies] : nodeDependencies)
    {
        if (!graphDefinition.Nodes.at(id).Description.RunOnceDuringSetup) { continue; }
        for (const auto& dependency : dependencies)
        {
            const auto& producer = graphDefinition.Nodes.at(dependency);
            const bool imported = std::any_of(producer.Description.Outputs.begin(), producer.Description.Outputs.end(),
                [&](const auto& output) { return GetPortCompileData(compileData, {dependency, output.first}).Imported.has_value(); });
            if (!producer.Description.RunOnceDuringSetup && !imported)
            { throw std::invalid_argument("One-time pass " + id + " depends on per-frame pass " + dependency); }
        }
    }
    BuildExecutionOrder(nodeDependencies, executionPlan);
    ValidateRequiredInputs(graphDefinition, executionPlan.Order, inputSources);

    const auto resourceDescriptions = ResolveResourceDescriptions(graphDefinition, compileData, inputSources, executionPlan.Order);
    BuildResourcePlacementPlans(graphDefinition, compileData, graphConnections, inputSources, resourceDescriptions, executionPlan);
    SelectResourceReuse(graphDefinition, compileData, inputSources, executionPlan);
    return executionPlan;
}
