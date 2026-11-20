#include "FrameGraphDiagnostics.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
    bool IsImportedPort(const FrameGraphCompileData& data, const PinAddress& address)
    {
        const auto port = data.Ports.find(address);
        return port != data.Ports.end() && port->second.Imported.has_value();
    }

    struct GPUTimeSpan
    {
        double Begin = (std::numeric_limits<double>::max)();
        double End = (std::numeric_limits<double>::lowest)();
        bool Used = false;
        bool Complete = true;
        double ExcludedGap = 0.0;

        double DurationMS() const
        {
            if (!Used || !Complete) { return -1.0; }
            return End - Begin - ExcludedGap;
        }
    };

    std::array<GPUTimeSpan, 2> CalculateGPUFrameSpans(const FrameGraphDiagnostics::CompletedFrameTiming& frame)
    {
        LARGE_INTEGER qpcFrequency{}, qpcOrigin{};
        QueryPerformanceFrequency(&qpcFrequency);
        QueryPerformanceCounter(&qpcOrigin);
        std::array<GPUTimeSpan, 2> spans;
        for (const auto& sample : frame.QueueSamples)
        {
            if (!sample.Queue) { continue; }
            const auto role = sample.Queue->GetCompletionPoint().Device->Role;
            auto& span = spans[role == DEVICE_ROLE_PRIMARY ? 0 : 1];
            span.Used = true;
            UINT64 gpuNow = 0, cpuNow = 0;
            if (!sample.Frequency || sample.Timestamps.End < sample.Timestamps.Begin || qpcFrequency.QuadPart <= 0 ||
                FAILED(sample.Queue->GetCommandQueue()->GetClockCalibration(&gpuNow, &cpuNow)))
            {
                span.Complete = false;
                continue;
            }

            // Compare timestamps from different queues on the common QPC timeline.
            const double calibrationMS = (static_cast<double>(cpuNow) - qpcOrigin.QuadPart) * 1000.0 / qpcFrequency.QuadPart;
            const auto toMS = [&](UINT64 ticks)
            {
                const double delta = ticks >= gpuNow ? static_cast<double>(ticks - gpuNow) : -static_cast<double>(gpuNow - ticks);
                return calibrationMS + delta * 1000.0 / sample.Frequency;
            };
            span.Begin = (std::min)(span.Begin, toMS(sample.Timestamps.Begin));
            span.End = (std::max)(span.End, toMS(sample.Timestamps.End));
        }

        return spans;
    }

    void WriteGPUFrameTiming(std::ostream& out, const std::optional<FrameGraphDiagnostics::CompletedFrameTiming>& timing, bool usesSecondary)
    {
        if (!timing) { out << "\n[GPU frame timings]\n    Pending\n"; return; }

        const auto& frame = *timing;
        const auto [primary, secondary] = CalculateGPUFrameSpans(frame);
        const auto writeSpan = [&](const char* name, const GPUTimeSpan& span)
        {
            out << "    " << name << ": ";
            if (!span.Complete) { out << "unavailable (incomplete queue timestamps)"; }
            else if (span.Used) { out << span.DurationMS() << " ms"; }
            else { out << "idle"; }
            out << '\n';
        };

        out << "\n[GPU frame " << frame.FrameNumber << " timings]\n";
        if (!usesSecondary)
        {
            writeSpan("total-frame", primary);
            return;
        }

        writeSpan("primary", primary);
        writeSpan("secondary", secondary);
        GPUTimeSpan total{
            (std::min)(primary.Begin, secondary.Begin), (std::max)(primary.End, secondary.End),
            primary.Used || secondary.Used, primary.Complete && secondary.Complete};
        if (total.Complete && primary.Used && secondary.Used)
        {
            // Count overlapping GPU spans once and exclude any gap between them.
            total.ExcludedGap = (std::max)(0.0,
                (std::max)(primary.Begin, secondary.Begin) - (std::min)(primary.End, secondary.End));
        }
        writeSpan("total-frame", total);
    }

    size_t CountCrossGPUTransferStages(const FrameGraphPlan& plan, const FrameGraphDefinition& definition)
    {
        size_t count = 0;
        for (const auto& [source, resource] : plan.Resources)
        {
            if (definition.Nodes.at(source.Node).Description.RunOnceDuringSetup) { continue; }
            bool needsExport = false;
            std::array<bool, 2> needsReplica{};
            for (const auto& [target, access] : resource.Consumers)
            {
                const auto& reader = definition.Nodes.at(target.Node);
                if (reader.Description.RunOnceDuringSetup) { continue; }
                const bool replica = access == MemoryType::Local && resource.Imports[reader.DeviceRole];
                needsReplica[reader.DeviceRole] |= replica;
                needsExport |= replica || access == MemoryType::Shared;
            }
            if (needsExport && resource.Export && resource.Export.Scope != CopyScope::Local) { ++count; }
            for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
            { if (needsReplica[role] && resource.Imports[role].Scope != CopyScope::Local) { ++count; } }
        }
        return count;
    }
}

void FrameGraphDiagnostics::CopyTiming::Add(double ms)
{
    ++Copies;
    if (ms >= 0.0) { TimeMS += ms; ++Measured; }
}

GDX12GPUTimer& FrameGraphDiagnostics::GetTransferTimer(GDX12CommandQueue* queue, const PinAddress& source, bool exporting, uint32_t slots)
{
    auto [it, inserted] = _transferTimers[queue].try_emplace(std::make_pair(source, exporting));
    if (inserted) { it->second.Initialize(queue->GetCompletionPoint().Device, queue, slots); }
    return it->second;
}

void FrameGraphDiagnostics::CompleteFrame(uint64_t frame, uint32_t slot, uint32_t passTimingSlot,
    const std::vector<CopySample>& copies, const std::vector<GDX12CommandQueue*>& queues)
{
    CompletedFrameTiming timing{frame, passTimingSlot};
    CopyTiming exports, imports;
    for (const auto& sample : copies)
    {
        const double ms = sample.Timer->GetTimeMS(sample.FenceValue);
        timing.CopyTimes[sample.Device == DEVICE_ROLE_PRIMARY ? 0 : 1].Add(ms);
        (sample.Exporting ? exports : imports).Add(ms);
        if (sample.ResourceBytes)
        {
            ++timing.CrossGPUCopies.ResourceCount;
            timing.CrossGPUCopies.Bytes += sample.ResourceBytes;
        }
    }
    timing.CrossGPUCopies.ExportMS = exports.GetCompleteTimeMS();
    timing.CrossGPUCopies.ImportMS = imports.GetCompleteTimeMS();
    for (size_t i = 0; i < queues.size(); ++i)
    {
        auto* queue = queues[i];
        const auto& timer = QueueTimers.at(queue);
        timing.QueueSamples[i].Queue = queue;
        if (timer.IsAvailable())
        {
            timing.QueueSamples[i].Frequency = timer.GetTimestampFrequency();
            timing.QueueSamples[i].Timestamps = timer.ReadResolvedTimestamps(slot);
        }
    }
    LastCompletedFrame = timing;
}

FrameGraphPerFrameMetrics FrameGraphDiagnostics::GetLatestFrameMetrics(const FrameGraphPlan& plan, const FrameGraphDefinition& definition) const
{
    const auto& frame = LastCompletedFrame;
    if (!frame) { return {}; }
    const auto [primary, secondary] = CalculateGPUFrameSpans(*frame);
    FrameGraphPerFrameMetrics metrics{primary.DurationMS(), secondary.DurationMS(), frame->CrossGPUCopies};
    metrics.PassTimings.reserve(PassTimers.size());
    for (const auto& id : plan.Order)
    {
        const auto timer = PassTimers.find(id);
        if (timer == PassTimers.end()) { continue; }
        const auto role = definition.Nodes.at(id).DeviceRole;
        metrics.PassTimings.push_back({id, role, timer->second->ReadResolvedTimeMS(frame->PassTimingSlot)});
    }
    return metrics;
}

std::string FrameGraphDiagnostics::GetDebugReport(const FrameGraphPlan& plan, const FrameGraphDefinition& definition,
    const FrameGraphCompileData& compileData, bool usesSecondary, const std::vector<std::string>& fallbacks,
    const std::map<PinAddress, FrameGraphMaterialization>* allocations)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "\nFRAME GRAPH INFO:";
    WriteGPUFrameTiming(out, LastCompletedFrame, usesSecondary);
    if (usesSecondary)
    {
        out << "\n[Cross-GPU copies]\n"
            << "  Planned per frame: " << CountCrossGPUTransferStages(plan, definition) << " transfer stages"
            << " | Default copy queue: " << (plan.UseCopyQueue ? "COPY" : "DIRECT") << '\n'
            << "  Submitted so far: " << SubmittedCopyCommands << " commands, " << SubmittedCopyBytes << " command bytes (export + import)\n"
            << "  Transfer timings cover copy commands and compute dispatches, excluding queue waits and state transitions.\n";
        if (const auto& timing = LastCompletedFrame)
        {
            out << "  Frame " << timing->FrameNumber << " totals:\n";
            for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
            {
                const auto& device = timing->CopyTimes[role == DEVICE_ROLE_PRIMARY ? 0 : 1];
                out << "    " << GetDeviceRoleName(role) << ": ";
                if (device.Copies && !device.Measured) { out << "unavailable"; }
                else { out << device.TimeMS << " ms"; }
                if (device.Measured < device.Copies)
                {
                    out << " | WARNING: " << (device.Copies - device.Measured) << '/' << device.Copies
                        << " timers were unable to return a result. Total timings are incomplete.";
                }
                out << '\n';
            }
        }
        else { out << "  Frame totals: pending\n"; }

        if (!_transferTimers.empty()) { out << "  Latest individual transfers:\n"; }
        for (auto& [queue, timers] : _transferTimers)
        {
            const auto role = queue->GetCompletionPoint().Device->Role;
            const char* queueName = queue->GetType() == D3D12_COMMAND_LIST_TYPE_COPY ? "COPY" : "DIRECT";
            for (auto& [key, timer] : timers)
            {
                out << "    " << (key.second ? "Export " : "Import ") << key.first.Node << '.' << key.first.Pin
                    << " | " << GetDeviceRoleName(role) << '/' << queueName << " | ";
                const double ms = timer.GetTimeMS();
                if (!timer.IsAvailable()) { out << "unsupported"; }
                else if (ms < 0.0) { out << "pending"; }
                else { out << ms << " ms"; }
                out << '\n';
            }
        }
    }
    out << "\n[GPU pass timings]\n";
    for (const auto& id : plan.Order)
    {
        auto timer = PassTimers.find(id);
        if (timer == PassTimers.end()) { continue; }
        out << "  " << id << " | " << GetDeviceRoleName(definition.Nodes.at(id).DeviceRole) << "/DIRECT | ";
        const double ms = timer->second->GetTimeMS();
        if (ms < 0.0) { out << "pending"; }
        else { out << ms << " ms"; }
        out << '\n';
    }

    out << '\n' << plan.GetDebugReport(definition, compileData);

    bool hasPortOverrides = false;
    for (const auto& node : definition.Nodes)
    {
        for (const auto& pin : node.second.Description.Outputs)
        {
            const auto storage = pin.second.RequestedMemory;
            if (storage == MemoryType::Auto || (storage == MemoryType::Local && IsImportedPort(compileData, {node.first, pin.first}))) { continue; }
            if (!hasPortOverrides) { out << "\n[Port overrides]\n"; hasPortOverrides = true; }
            out << "  " << node.first << '.' << pin.first << " storage=" << GetMemoryTypeName(storage) << '\n';
        }
        for (const auto& pin : node.second.Description.Inputs)
        {
            if (pin.second.RequestedMemory == MemoryType::Auto) { continue; }
            if (!hasPortOverrides) { out << "\n[Port overrides]\n"; hasPortOverrides = true; }
            out << "  " << node.first << '.' << pin.first << " access=" << GetMemoryTypeName(pin.second.RequestedMemory) << '\n';
        }
    }
    if (!fallbacks.empty()) { out << "\n[Placement fallbacks]\n"; }
    for (const auto& fallback : fallbacks) { out << "  " << fallback << '\n'; }

    if (allocations)
    {
        out << "\n[Allocations per slot]\n";
        for (const auto& resource : *allocations)
        {
            const auto& material = resource.second;
            if (!material.LocalResource && material.Shared.Allocations.empty() && material.LocalCopies.empty()) { continue; }
            out << "  " << resource.first.Node << '.' << resource.first.Pin;
            if (material.LocalResource) { out << " native bytes=" << material.LocalResource->AllocationBytes; }
            for (const auto& shared : material.Shared.Allocations) { out << " shared bytes=" << shared->GetHeapSize(); }
            for (const auto& replica : material.LocalCopies) { out << " replica=" << GetDeviceRoleName(replica.first) << " bytes=" << replica.second->AllocationBytes; }
            out << '\n';
            for (const auto* mapping : {material.Shared.Producer.get(), material.Shared.Consumer.get()})
            {
                if (!mapping) { continue; }
                for (size_t i = 0; i < mapping->Footprints.size(); ++i)
                {
                    const auto& footprint = mapping->Footprints[i];
                    out << "    subresource=" << i << " offset=" << footprint.Offset << " pitch=" << footprint.Footprint.RowPitch
                        << " extent=" << footprint.Footprint.Width << 'x' << footprint.Footprint.Height << '\n';
                }
            }
        }
    }
    return out.str();
}

std::string FrameGraphPlan::GetDebugReport(const FrameGraphDefinition& definition, const FrameGraphCompileData& compileData) const
{
    const auto isImported = [&](const NodeId& id)
    {
        const auto& outputs = definition.Nodes.at(id).Description.Outputs;
        return std::any_of(outputs.begin(), outputs.end(), [&](const auto& output)
        { return IsImportedPort(compileData, {id, output.first}); });
    };

    std::ostringstream out;
    out << "[Execution plan | " << FrameSlotCount << " slots]\n";
    for (const auto& id : Order)
    {
        if (isImported(id)) { continue; }
        out << "  " << id << " | " << GetDeviceRoleName(definition.Nodes.at(id).DeviceRole);
        if (definition.Nodes.at(id).Description.RunOnceDuringSetup) { out << " | once"; }
        auto dependencies = Dependencies.find(id);
        if (dependencies != Dependencies.end())
        {
            const char* separator = " | after: ";
            for (const auto& dependency : dependencies->second)
            {
                if (isImported(dependency)) { continue; }
                out << separator << dependency;
                separator = ", ";
            }
        }
        out << '\n';
    }
    out << "\n[Resource placement]\n";
    for (const auto& entry : Resources)
    {
        const auto& p = entry.second;
        out << "  " << entry.first.Node << '.' << entry.first.Pin << " | " << GetMemoryTypeName(p.Storage)
            << " | " << GetResourceTypeName(p.Description.Type);
        if (p.Description.IsBuffer()) { out << " capacity=" << p.Description.Capacity; }
        else
        {
            out << ' ' << GetTextureFormatName(p.Description.Format) << ' ' << p.Description.Width << 'x' << p.Description.Height;
            if (p.Description.ArraySize > 1) { out << " layers=" << p.Description.ArraySize; }
            if (p.Description.MipLevels > 1) { out << " mips=" << p.Description.MipLevels; }
        }
        out << " | usages=" << uint32_t(p.Description.Usages);
        if (p.CopyCount) { out << " | copies=" << p.CopyCount; }
        if (p.Export) { out << (p.Export.Method == TransferMethod::ComputeCopy ? " export via ComputeCopy (DIRECT)" : " export"); }
        if (p.BufferPayload && (p.Export || p.Storage == MemoryType::Shared)) { out << " via footprint buffer"; }
        if (p.Consumers.empty()) { out << " | unused"; }
        out << '\n';
        for (const auto& [target, access] : p.Consumers)
        {
            const auto& replica = p.Imports[definition.Nodes.at(target.Node).DeviceRole];
            out << "    -> " << target.Node << '.' << target.Pin << " | " << GetMemoryTypeName(access);
            if (access == MemoryType::Local && replica)
            { out << (replica.Method == TransferMethod::ComputeCopy ? " replica via ComputeCopy (DIRECT)" : " replica"); }
            out << '\n';
        }
    }
    return out.str();
}
