#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <ostream>
#include <unordered_map>
struct ID3D12Resource;
namespace BistroRender {
using Id = uint32_t;
using State = uint32_t; // D3D12 legacy resource-state bits; CPU tests need no device.
constexpr State UnorderedAccess = 0x8;
constexpr Id Invalid = ~Id(0);
enum class Mode { Read, Write, ReadWrite };
enum class Queue { Direct, Compute, Copy };
enum class Heap { Default, Upload, Readback };
struct Resource {
    std::string Name; State Initial, Final; bool Unordered = false;
    // DEFAULT-heap buffers decay to COMMON after each ExecuteCommandLists.
    // Upload/readback heaps have fixed states and must leave this false.
    bool Decays = false;
    Heap Memory = Heap::Default;
    bool operator==(const Resource& rhs) const {
        return Initial == rhs.Initial && Final == rhs.Final && Unordered == rhs.Unordered &&
               Decays == rhs.Decays && Memory == rhs.Memory && Name == rhs.Name;
    }
};
struct Access {
    Id Resource; Mode Usage; State Required;
    bool operator==(const Access& rhs) const {
        return Resource == rhs.Resource && Usage == rhs.Usage && Required == rhs.Required;
    }
};
// Each operation is one packet, containing one or more independently recorded lists.
// Accesses describe the boundary of the entire packet. Internal changes must be
// explicit operations or a strict recording-local scope restored to the packet
// boundary before closing; callbacks may not perform undeclared transitions.
struct Operation {
    std::string Name;
    std::vector<Access> Accesses;
    std::vector<Id> Dependencies;
    bool SideEffect = false;
    uint32_t Lists = 1;
    // A concrete owner-only cold recorder, e.g. the SDK's first lazy constant
    // ring allocation. Resources and packet boundaries are still plan-owned.
    bool OwnerRecording = false;
    Queue Target = Queue::Direct;
    uint64_t TransferBytes = 0;
    bool operator==(const Operation& rhs) const {
        return Target == rhs.Target && Lists == rhs.Lists && SideEffect == rhs.SideEffect &&
               OwnerRecording == rhs.OwnerRecording && Name == rhs.Name && TransferBytes == rhs.TransferBytes &&
               Dependencies == rhs.Dependencies && Accesses == rhs.Accesses;
    }
};
// Resource IDs are local to Device; shared allocations bind separate local handles.
struct Graph {
    std::vector<Resource> Resources; std::vector<Operation> Operations;
    Id Device = 0;
    bool FrameBoundary = true;
    Queue TerminalQueue = Queue::Direct;
    // Physical bindings are append-only while this graph is being built.
    std::unordered_map<ID3D12Resource*, Id> BindingIds;
    size_t IndexedBindings = 0;
    Id FindBinding(const std::vector<ID3D12Resource*>& bindings, ID3D12Resource* resource) {
        while (IndexedBindings < bindings.size()) {
            BindingIds.try_emplace(bindings[IndexedBindings], Id(IndexedBindings));
            ++IndexedBindings;
        }
        const auto found = BindingIds.find(resource);
        return found == BindingIds.end() ? Invalid : found->second;
    }
};
struct Barrier { Id Resource; State Before, After; bool Uav = false; };
struct Packet {
    Id Operation;
    std::vector<Id> Dependencies; // packet indices (completion tokens)
    std::vector<Access> Accesses;
    std::vector<Barrier> Before, After;
    uint32_t Lists;
    bool OwnerRecording = false;
    Queue Target = Queue::Direct;
    std::string Name;
    uint64_t TransferBytes = 0;
};
struct Schedule {
    std::vector<Packet> Packets;
    std::vector<State> FinalStates;
    std::vector<bool> Decays;
    std::vector<Heap> Memory;
    Id Device = 0;
    bool FrameBoundary = true;
    Queue TerminalQueue = Queue::Direct;
    void Write(std::ostream&) const;
};
Schedule Compile(const Graph&, bool singleDirect = false);
}
