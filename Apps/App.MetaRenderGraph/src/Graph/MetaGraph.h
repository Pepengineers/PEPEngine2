#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <ostream>
namespace Meta {
using Id = uint32_t;
using State = uint32_t; // D3D12 legacy resource-state bits; CPU tests need no device.
constexpr State UnorderedAccess = 0x8;
constexpr Id Invalid = ~Id(0);
enum class Mode { Read, Write, ReadWrite };
struct Resource {
    std::string Name; State Initial, Final; bool Unordered = false;
    // DEFAULT-heap buffers decay to COMMON after each ExecuteCommandLists.
    // Upload/readback heaps have fixed states and must leave this false.
    bool Decays = false;
};
struct Access { Id Resource; Mode Usage; State Required; };
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
};
struct Graph { std::vector<Resource> Resources; std::vector<Operation> Operations; };
struct Barrier { Id Resource; State Before, After; bool Uav = false; };
struct Packet {
    Id Operation;
    std::vector<Id> Dependencies; // packet indices (completion tokens)
    std::vector<Access> Accesses;
    std::vector<Barrier> Before, After;
    uint32_t Lists;
    bool OwnerRecording = false;
};
struct Schedule {
    std::vector<Packet> Packets;
    std::vector<State> FinalStates;
    std::vector<bool> Decays;
    void Write(std::ostream&) const;
};
Schedule Compile(const Graph&);
}
