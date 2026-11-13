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
struct Resource { std::string Name; State Initial, Final; bool Unordered = false; };
struct Access { Id Resource; Mode Usage; State Required; };
// Each operation is one packet, containing one or more independently recorded lists.
// Accesses describe the boundary of the entire packet. Internal changes must be
// explicit operations; callbacks may not perform undeclared transitions.
struct Operation {
    std::string Name;
    std::vector<Access> Accesses;
    std::vector<Id> Dependencies;
    bool SideEffect = false;
    uint32_t Lists = 1;
};
struct Graph { std::vector<Resource> Resources; std::vector<Operation> Operations; };
struct Barrier { Id Resource; State Before, After; bool Uav = false; };
struct Packet {
    Id Operation;
    std::vector<Id> Dependencies; // packet indices (completion tokens)
    std::vector<Access> Accesses;
    std::vector<Barrier> Before, After;
    uint32_t Lists;
};
struct Schedule {
    std::vector<Packet> Packets;
    std::vector<State> FinalStates;
    void Write(std::ostream&) const;
};
Schedule Compile(const Graph&);
}
