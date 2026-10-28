#include "Engine.RendererDX12/FrameGraph/FrameGraphCompiler.h"

#include <functional>
#include <limits>
#include <memory>
#include <algorithm>
#include <set>
#include <sstream>

namespace
{
    bool IsInput(const FrameGraphDefinition& definition, const PinAddress& address)
    { return definition.Nodes.at(address.Node).Description.Inputs.count(address.Pin) != 0; }

    std::string Label(const PinAddress& address) { return address.Node + "." + address.Pin; }

    const FrameGraphPortData& GetPortCompileData(const FrameGraphCompileData& data, const PinAddress& address)
    {
        static const FrameGraphPortData defaults;
        auto port = data.Ports.find(address);
        return port == data.Ports.end() ? defaults : port->second;
    }
}

const PortDescription& FrameGraphDefinition::GetPortDescription(const PinAddress& address) const
{
    auto node = Nodes.find(address.Node);
    if (node == Nodes.end())
    { throw std::invalid_argument("ERROR: NodeId is not registered: " + address.Node + ". Add this node before referencing it in a connection or dependency."); }
    return node->second.Description.GetPortDescription(address.Pin);
}

FrameGraphPlan CompileFrameGraph(const FrameGraphDefinition& definition, const FrameGraphCompileData& data)
{
    if (!data.FrameSlotCount) { throw std::invalid_argument("ERROR: Given frame slot count is zero. Pass a frame slot count greater than zero."); }
    auto connections = definition.Connections;
    std::sort(connections.begin(), connections.end(), [](const Connection& a, const Connection& b)
    {
        return a.Source < b.Source || (a.Source == b.Source && a.Target < b.Target);
    });
    FrameGraphPlan result;
    result.FrameSlotCount = data.FrameSlotCount;
    result.UseCopyQueue = data.QueuePolicy == TransportQueuePolicy::ForceCopy
        || (data.QueuePolicy == TransportQueuePolicy::Auto && data.FrameSlotCount > 1);
    std::map<PinAddress, PinAddress> sources;
    std::map<NodeId, std::set<NodeId>> dependencies;
    for (const auto& entry : definition.Nodes)
    {
        const auto& node = entry.second;
        if (entry.first.empty() || entry.first != node.Id) { throw std::invalid_argument("ERROR: NodeId is empty or does not match its definition key. Use a non-empty NodeId that matches its key in "
            "FrameGraphDefinition::Nodes."); }
        const auto& description = node.Description;
        if (node.DeviceRole != DEVICE_ROLE_PRIMARY && node.DeviceRole != DEVICE_ROLE_SECONDARY) { throw std::invalid_argument("ERROR: " + node.Id + ": device role is not supported. Use DEVICE_ROLE_PRIMARY or DEVICE_ROLE_SECONDARY."); }
        if (description.PrimaryOnly && node.DeviceRole != DEVICE_ROLE_PRIMARY) { throw std::invalid_argument("ERROR: " + node.Id + ": pass was assigned to the secondary device but requires the primary device. Add this pass with "
            "AddPrimaryPass()."); }
        for (const auto& port : description.Outputs)
        {
            if (HasUsage(port.second.Usages, ResourceUsage::Present) && node.DeviceRole != DEVICE_ROLE_PRIMARY)
            { throw std::invalid_argument("ERROR: " + node.Id + ": presentation pass was assigned to the secondary device. Add GDX12OutputToScreenPass on the primary device."); }
        }
        dependencies[node.Id];
    }
    for (const auto& edge : connections)
    {
        const auto& output = definition.GetPortDescription(edge.Source);
        const auto& input = definition.GetPortDescription(edge.Target);
        const auto& producer = definition.Nodes.at(edge.Source.Node);
        const auto& consumer = definition.Nodes.at(edge.Target.Node);
        if (IsInput(definition, edge.Source) || !IsInput(definition, edge.Target)) { throw std::invalid_argument("ERROR: Connection source is not an output or target is not an input: " + Label(edge.Target) + ". Pass GetOutput() as the source and GetInput() as the target of ConnectNodes()."); }
        if (!sources.emplace(edge.Target, edge.Source).second) { throw std::invalid_argument("ERROR: " + Label(edge.Target) + ": input has multiple connected sources. Connect exactly one output to this input and remove duplicate "
            "connections."); }
        if (output.Type != input.Type)
        { throw std::invalid_argument("ERROR: " + Label(edge.Target) + ": expected " + GetResourceTypeName(input.Type)
            + ", but " + Label(edge.Source) + " provides " + GetResourceTypeName(output.Type)
            + ". Connect ports with matching resource types."); }
        if (!output.Layout.empty() && !input.Layout.empty() && (output.Layout != input.Layout || output.Stride != input.Stride))
        { throw std::invalid_argument("ERROR: " + Label(edge.Target) + ": expected layout '" + input.Layout
            + "' with stride " + std::to_string(input.Stride) + ", but " + Label(edge.Source) + " provides layout '"
            + output.Layout + "' with stride " + std::to_string(output.Stride) + ". Match the connected buffer layouts and strides."); }
        dependencies[consumer.Id].insert(producer.Id);
    }
    for (const auto& edge : definition.OrderDependencies)
    {
        if (!definition.Nodes.count(edge.first) || !definition.Nodes.count(edge.second)) { throw std::invalid_argument("ERROR: Given execution dependency references a node that has not been added. Add both nodes before declaring "
            "their execution dependency."); }
        dependencies[edge.second].insert(edge.first);
    }
    // Destructive writes wait for every reader of the previous logical version.
    std::map<PinAddress, NodeId> writers;
    for (const auto& edge : connections)
    {
        const auto& input = definition.GetPortDescription(edge.Target);
        if (input.Access == DataAccess::ReadOnly) { continue; }
        const auto& schema = definition.Nodes.at(edge.Target.Node).Description;
        bool published = false;
        for (const auto& pin : schema.Outputs) { published |= pin.second.AliasInput == edge.Target.Pin; }
        if (!published) { throw std::invalid_argument("ERROR: " + Label(edge.Target) + ": writable input has no published output version. Publish one output with AliasInput naming this mutable "
            "input."); }
        if (!writers.emplace(edge.Source, edge.Target.Node).second) { throw std::invalid_argument("ERROR: " + Label(edge.Source) + ": multiple passes write the same resource version. Chain writers through successive output versions instead "
            "of writing the same input version twice."); }
        for (const auto& reader : connections)
        {
            if (reader.Source == edge.Source && reader.Target.Node != edge.Target.Node)
            { dependencies[edge.Target.Node].insert(reader.Target.Node); }
        }
    }
    for (const auto& node : dependencies)
    { result.Dependencies[node.first] = std::vector<NodeId>(node.second.begin(), node.second.end()); }
    auto pending = dependencies;
    while (!pending.empty())
    {
        auto ready = std::find_if(pending.begin(), pending.end(), [](const auto& item) { return item.second.empty(); });
        if (ready == pending.end())
        {
            std::string cycle;
            for (const auto& node : pending) { cycle += " " + node.first; }
            throw std::invalid_argument("ERROR: FrameGraph connections, execution dependencies, or alias versions form a cycle:" + cycle + ". Remove the cyclic connections or execution dependencies and chain alias writes through successive versions.");
        }
        const auto id = ready->first;
        result.Order.push_back(id);
        pending.erase(ready);
        for (auto& node : pending) { node.second.erase(id); }
    }
    for (const auto& id : result.Order)
    {
        const auto& node = definition.Nodes.at(id);
        for (const auto& pin : node.Description.Inputs)
        {
            if (pin.second.Required && !sources.count({id, pin.first}))
            { throw std::invalid_argument("ERROR: " + id + "." + pin.first + " [" + GetDeviceRoleName(node.DeviceRole) + "]: required input has no connected source. Connect a compatible output to this required input before "
                "Compile()."); }
        }
    }
    std::map<PinAddress, ResourceDescription> descriptions;
    std::set<PinAddress> resolving;
    std::function<ResourceDescription(PinAddress)> resolve;
    std::function<UInt2(const NodeDefinition&, const Extent&)> extent = [&](const NodeDefinition& node, const Extent& expression) -> UInt2
    {
        switch (expression.Type)
        {
        case Extent::Source::Render: return data.RenderSize;
        case Extent::Source::Display: return data.DisplaySize;
        case Extent::Source::Fixed: return expression.Value;
        case Extent::Source::Input: return resolve({node.Id, expression.Input}).GetSize();
        default: throw std::invalid_argument("ERROR: " + node.Id + ": texture output has no Size declaration. Assign Extent::Render(), Extent::Display(), Extent::Fixed(), or "
            "Extent::SameAs() to the output Size.");
        }
    };
    resolve = [&](PinAddress address) -> ResourceDescription
    {
        if (descriptions.count(address)) { return descriptions.at(address); }
        if (!resolving.insert(address).second) { throw std::invalid_argument("ERROR: " + Label(address) + ": texture-size expressions form a cycle. Remove cyclic Extent::SameAs() references and resource connections."); }
        const auto& p = definition.GetPortDescription(address);
        const auto& node = definition.Nodes.at(address.Node);
        const auto& portData = GetPortCompileData(data, address);
        const auto size = portData.RequiredTextureSize ? Extent::Fixed(*portData.RequiredTextureSize) : p.Size;
        ResourceDescription desc;
        if (IsInput(definition, address))
        {
            auto source = sources.find(address);
            if (source == sources.end()) { throw std::invalid_argument("ERROR: " + Label(address) + ": texture Size depends on an unconnected optional input. Connect the optional input or choose an extent that "
                "does not depend on it."); }
            desc = resolve(source->second);
            if (!p.Formats.empty() && std::find(p.Formats.begin(), p.Formats.end(), desc.Format) == p.Formats.end())
            {
                std::string expectedFormats;
                for (auto format : p.Formats)
                { expectedFormats += (expectedFormats.empty() ? "" : ", ") + GetTextureFormatName(format); }
                throw std::invalid_argument("ERROR: " + Label(address) + ": expected format [" + expectedFormats
                    + "], but " + Label(source->second) + " provides " + GetTextureFormatName(desc.Format)
                    + ". Declare a consumer view format compatible with the producer output format.");
            }
            if (p.IsDepth() != desc.IsDepth()) { throw std::invalid_argument("ERROR: " + Label(address) + ": depth and color resource families do not match. Connect depth ports to depth resources and color ports to "
                "color resources."); }
            if (size.Type != Extent::Source::None)
            {
                const auto requiredSize = extent(node, size);
                if (desc.GetSize() != requiredSize)
                { throw std::invalid_argument("ERROR: " + Label(address) + ": expected texture size " + FormatTextureSize(requiredSize)
                    + ", but " + Label(source->second) + " provides " + FormatTextureSize(desc.GetSize())
                    + ". Make the producer texture size match the consumer requirement."); }
            }
        }
        else if (portData.Imported)
        {
            desc = *portData.Imported;
            if (desc.Type != p.Type || desc.IsDepth() != p.IsDepth()
                || (!p.Formats.empty() && std::find(p.Formats.begin(), p.Formats.end(), desc.Format) == p.Formats.end()))
            { throw std::invalid_argument("ERROR: " + Label(address) + ": imported texture type, depth usage, or format conflicts with its port description. Match the imported "
                "texture type, depth usage, and format to its declared port."); }
        }
        else if (!p.AliasInput.empty())
        {
            desc = resolve({node.Id, p.AliasInput});
            if ((!p.Formats.empty() && (p.Formats.size() != 1 || p.Formats[0] != desc.Format))
                || (size.Type != Extent::Source::None && extent(node, size) != desc.GetSize()))
            { throw std::invalid_argument("ERROR: " + Label(address) + ": AliasInput overrides its source resource description. Keep the alias output description identical to its "
                "source allocation."); }
        }
        else
        {
            desc.Type = p.Type;
            desc.Stride = p.Stride;
            desc.Layout = p.Layout;
            if (desc.IsBuffer())
            {
                desc.Capacity = p.Capacity;
                if (!desc.Capacity || (desc.Type != ResourceType::RawBuffer && !desc.Stride)
                    || desc.Capacity > (std::numeric_limits<uint32_t>::max)()
                    || desc.Capacity > (std::numeric_limits<uint64_t>::max)() / (std::max)(1u, desc.Stride))
                { throw std::invalid_argument("ERROR: " + Label(address) + ": buffer capacity or stride is zero, too large, or otherwise invalid. Use Capacity in [1, UINT32_MAX], a "
                    "valid structured Stride, and four-byte alignment for raw-buffer capacity."); }
            }
            else
            {
                if (p.Formats.size() != 1) { throw std::invalid_argument("ERROR: " + Label(address) + ": texture output does not declare exactly one concrete format. Declare exactly one concrete format for each "
                    "texture output."); }
                desc.Format = p.Formats[0];
                auto dimensions = extent(node, size);
                desc.Width = dimensions.x;
                desc.Height = dimensions.y;
                if (p.Layers > 2048) { throw std::invalid_argument("ERROR: " + Label(address) + ": texture array layer count exceeds 2048. Use no more than 2048 layers for a Texture2DArray."); }
                desc.ArraySize = p.Layers;
            }
        }
        desc.Usages |= p.Usages;
        if (!desc.IsBuffer() && (!desc.Width || !desc.Height || desc.Width > 16384 || desc.Height > 16384
            || !desc.ArraySize
            || (desc.Type == ResourceType::Texture2D && desc.ArraySize != 1)
            || (desc.Type == ResourceType::TextureCube && (desc.ArraySize != 6 || desc.Width != desc.Height))))
        { throw std::invalid_argument("ERROR: " + Label(address) + ": texture dimensions, layer count, or shape are invalid. Use non-zero supported dimensions and a valid "
            "Texture2D, Texture2DArray, or six-face square TextureCube description."); }
        resolving.erase(address);
        descriptions.emplace(address, desc);
        return desc;
    };
    for (const auto& id : result.Order)
    {
        const auto& schema = definition.Nodes.at(id).Description;
        for (const auto& entry : schema.Inputs)
        { if (sources.count({id, entry.first})) { resolve({id, entry.first}); } }
        for (const auto& entry : schema.Outputs) { resolve({id, entry.first}); }
        for (const auto& group : schema.SameTextureSizeGroups)
        {
            auto size = resolve({id, group.front()}).GetSize();
            for (const auto& pin : group)
            {
                const auto actualSize = resolve({id, pin}).GetSize();
                if (actualSize != size)
                { throw std::invalid_argument("ERROR: " + id + "." + pin + ": texture size " + FormatTextureSize(actualSize)
                    + " differs from " + id + "." + group.front() + " (" + FormatTextureSize(size)
                    + "). Make the textures in this RequireSameTextureSize() group use the same dimensions."); }
            }
        }
    }
    auto capabilities = [&](const PinAddress& address)
    {
        const auto& p = definition.GetPortDescription(address);
        auto caps = GetPortCompileData(data, address).Capabilities;
        caps.Local &= p.AllowLocal;
        caps.Shared &= p.AllowShared;
        auto desc = descriptions.at(address);
        if (!desc.IsBuffer() && (desc.IsDepth() || desc.Type != ResourceType::Texture2D)) { caps.Shared = false; }
        return caps;
    };
    struct Choice
    {
        PinAddress Source;
        ResourcePlan Plan;
        uint64_t TotalCopies = 0;
        std::vector<const Choice*> Aliases;
    };
    // Only export plus one replica per device can affect fan-out cost. Eight states
    // suffice, irrespective of the number of consumers; alias versions are solved
    // recursively with the actual placement of their mutable input fixed.
    struct BindingChoice
    {
        std::map<PinAddress, MemoryType> Consumers;
        std::vector<const Choice*> Aliases;
        uint64_t AliasCopies = 0;
    };
    auto stageCount = [](uint32_t mask) { return uint32_t(bool(mask & 1)) + uint32_t(bool(mask & 2)) + uint32_t(bool(mask & 4)); };
    std::map<std::pair<PinAddress, MemoryType>, std::unique_ptr<Choice>> memo;
    std::function<bool(const PinAddress&)> isImport = [&](const PinAddress& address)
    {
        if (GetPortCompileData(data, address).Imported.has_value()) { return true; }
        const auto& p = definition.GetPortDescription(address);
        return !p.AliasInput.empty() && isImport(sources.at({address.Node, p.AliasInput}));
    };
    std::function<const Choice*(const PinAddress&, MemoryType)> solve;
    solve = [&](const PinAddress& address, MemoryType placement) -> const Choice*
    {
        const auto key = std::make_pair(address, placement);
        if (memo.count(key)) { return memo.at(key).get(); }
        auto& saved = memo[key];
        const auto& node = definition.Nodes.at(address.Node);
        const auto& p = definition.GetPortDescription(address);
        const auto requested = p.RequestedMemory;
        const auto caps = capabilities(address);
        if ((requested != MemoryType::Auto && requested != placement)
            || !(placement == MemoryType::Local ? caps.Local : caps.Shared)
            || (GetPortCompileData(data, address).Imported && placement != MemoryType::Local)) { return nullptr; }
        std::map<uint32_t, BindingChoice> states;
        states[0];
        auto description = descriptions.at(address);
        for (const auto& edge : connections)
        {
            if (!(edge.Source == address)) { continue; }
            const auto& consumer = definition.Nodes.at(edge.Target.Node);
            const auto& schema = consumer.Description;
            const auto inputCaps = capabilities(edge.Target);
            const auto access = schema.Inputs.at(edge.Target.Pin).RequestedMemory;
            std::map<uint32_t, BindingChoice> next;
            for (const auto& state : states)
            {
                for (auto binding : {MemoryType::Local, MemoryType::Shared})
                {
                    if ((access != MemoryType::Auto && access != binding) || !(binding == MemoryType::Local ? inputCaps.Local : inputCaps.Shared)) { continue; }
                    if (isImport(address) && definition.GetPortDescription(edge.Target).Access != DataAccess::ReadOnly
                        && (consumer.DeviceRole != node.DeviceRole || placement != (binding == MemoryType::Local ? MemoryType::Local : MemoryType::Shared))) { continue; }
                    BindingChoice candidate = state.second;
                    bool valid = true;
                    for (const auto& output : schema.Outputs)
                    {
                        if (output.second.AliasInput != edge.Target.Pin) { continue; }
                        auto alias = solve({consumer.Id, output.first}, binding == MemoryType::Local ? MemoryType::Local : MemoryType::Shared);
                        if (!alias) { valid = false; break; }
                        candidate.Aliases.push_back(alias);
                        candidate.AliasCopies += alias->TotalCopies;
                    }
                    if (!valid) { continue; }
                    const bool remote = consumer.DeviceRole != node.DeviceRole;
                    const bool replica = NeedsFrameGraphLocalCopy(placement, binding, remote);
                    uint32_t mask = state.first;
                    if (placement == MemoryType::Local && (remote || binding == MemoryType::Shared)) { mask |= 1; }
                    if (replica) { mask |= consumer.DeviceRole == DEVICE_ROLE_PRIMARY ? 2 : 4; }
                    candidate.Consumers.emplace(edge.Target, binding);
                    const auto old = next.find(mask);
                    if (old == next.end() || candidate.AliasCopies < old->second.AliasCopies) { next[mask] = std::move(candidate); }
                }
            }
            states = std::move(next);
            if (states.empty()) { return nullptr; }
            description.Usages |= definition.GetPortDescription(edge.Target).Usages;
        }
        uint64_t bestCost = (std::numeric_limits<uint64_t>::max)();
        const uint32_t components = description.Type == ResourceType::DrawStream ? 2 : 1;
        for (const auto& state : states)
        {
            const auto copies = stageCount(state.first) * components;
            const auto total = uint64_t(copies) + state.second.AliasCopies;
            if (total >= bestCost) { continue; }
            bestCost = total;
            saved = std::make_unique<Choice>();
            saved->Source = address;
            auto& plan = saved->Plan;
            plan.Description = description;
            plan.Storage = placement;
            if (!p.AliasInput.empty()) { plan.Alias = {node.Id, p.AliasInput}; }
            plan.Export = (state.first & 1) != 0;
            plan.BufferPayload = !description.IsBuffer() && (description.IsDepth() || description.Type != ResourceType::Texture2D
                || (placement == MemoryType::Local && std::all_of(state.second.Consumers.begin(), state.second.Consumers.end(),
                    [](const auto& consumer) { return consumer.second == MemoryType::Local; })));
            plan.CopyCount = copies;
            plan.Consumers = state.second.Consumers;
            saved->TotalCopies = total;
            saved->Aliases = state.second.Aliases;
        }
        return saved.get();
    };
    std::function<void(const Choice*)> publish = [&](const Choice* choice)
    {
        if (!result.Resources.emplace(choice->Source, choice->Plan).second) { return; }
        if (choice->Plan.CopyCount > (std::numeric_limits<uint32_t>::max)() - result.CopyCount)
        { throw std::invalid_argument("ERROR: FrameGraph transport copy count exceeds UINT32_MAX. Reduce the number of transported resource "
            "components or cross-device connections."); }
        result.CopyCount += choice->Plan.CopyCount;
        for (const auto& alias : choice->Aliases) { publish(alias); }
    };
    for (const auto& id : result.Order)
    {
        const auto& node = definition.Nodes.at(id);
        for (const auto& entry : node.Description.Outputs)
        {
            if (!entry.second.AliasInput.empty()) { continue; }
            const PinAddress address{id, entry.first};
            auto local = solve(address, MemoryType::Local);
            auto shared = solve(address, MemoryType::Shared);
            // Iterating Local first gives the required stable producer tie-break.
            auto best = local && (!shared || local->TotalCopies <= shared->TotalCopies) ? local : shared;
            if (!best)
            {
                std::string policies = " requested storage=" + std::string(GetMemoryTypeName(entry.second.RequestedMemory));
                for (const auto& edge : connections)
                {
                    if (!(edge.Source == address)) { continue; }
                    const auto& consumer = definition.Nodes.at(edge.Target.Node);
                    auto access = consumer.Description.Inputs.at(edge.Target.Pin).RequestedMemory;
                    policies += " " + Label(edge.Target) + " requested access=" + GetMemoryTypeName(access);
                }
                throw std::invalid_argument("ERROR: " + Label(address) + " [" + GetDeviceRoleName(node.DeviceRole) + "]: no supported resource placement or input access satisfies the requested policies;" + policies
                    + "; schema, SDK, hardware or alias constraint excludes requested access. Choose Local storage/access where "
                    "supported, remove conflicting alias requirements, or use adapters that support the requested shared route.");
            }
            publish(best);
        }
    }
    return result;
}

std::string FrameGraphPlan::GetDebugReport(const FrameGraphDefinition& definition) const
{
    std::ostringstream out;
    out << "FrameGraph slots=" << FrameSlotCount << " CopyCount=" << CopyCount << '\n';
    for (const auto& id : Order)
    {
        out << "DIRECT batch " << id;
        auto dependencies = Dependencies.find(id);
        if (dependencies != Dependencies.end())
        { for (const auto& dependency : dependencies->second) { out << " wait=" << dependency; } }
        out << '\n';
    }
    for (const auto& entry : Resources)
    {
        const auto& p = entry.second;
        out << Label(entry.first) << " storage=" << GetMemoryTypeName(p.Storage)
            << " type=" << GetResourceTypeName(p.Description.Type) << " format=" << GetTextureFormatName(p.Description.Format)
            << " extent=" << p.Description.Width << 'x' << p.Description.Height << " layers=" << p.Description.ArraySize
            << " capacity=" << p.Description.Capacity << " usages=" << uint32_t(p.Description.Usages)
            << " export=" << p.Export << " payload=" << (p.BufferPayload ? "footprint buffer" : "native")
            << " copies=" << p.CopyCount << " transport=" << (UseCopyQueue ? "COPY" : "DIRECT")
            << " reason=" << (p.CopyCount ? "minimum materializations; " : "compatible original allocation; ")
            << (UseCopyQueue ? "COPY allows independent frame slots" : "DIRECT transport policy") << '\n';
        if (p.Consumers.empty()) { out << "  unused output (producer retained)\n"; }
        for (const auto& [target, access] : p.Consumers)
        {
            const bool remote = definition.Nodes.at(entry.first.Node).DeviceRole != definition.Nodes.at(target.Node).DeviceRole;
            out << "  -> " << Label(target) << " access=" << (access == MemoryType::Local ? "Local" : "Shared")
                << " replica=" << NeedsFrameGraphLocalCopy(p.Storage, access, remote) << '\n';
        }
    }
    return out.str();
}
