#pragma once
#include "QueueSubmission.h"
#include <stdexcept>

namespace Meta {
// Water and particles share one secondary device and one queue/fence owner.
struct SimulationDomain {
    GDX12Device Secondary;
    std::shared_ptr<QueueSubmission> Submissions;
    SimulationDomain(GDX12Device& primary, int adapter) {
        if (adapter < 0) throw std::invalid_argument("Meta simulations require an explicit secondary adapter");
        ComPtr<IDXGIFactory6> factory; ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> selected;
        ThrowIfFailed(factory->EnumAdapters1(UINT(adapter), &selected));
        DXGI_ADAPTER_DESC1 desc{}; ThrowIfFailed(selected->GetDesc1(&desc));
        const auto luid = primary.GetAdapterLuid();
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart))
            throw std::invalid_argument("Meta simulations require two distinct hardware adapters");
        ComPtr<IDXGIAdapter4> hardware; ThrowIfFailed(selected.As(&hardware));
        ThrowIfFailed(Secondary.Initialize(hardware));
        Submissions = std::make_shared<QueueSubmission>(primary, &Secondary);
    }
};
}
