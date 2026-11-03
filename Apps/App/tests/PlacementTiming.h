#pragma once
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include <chrono>

static bool PlacementCaptured = false;

static void LoadPlacementCapturer()
{
#if defined(PROFILE_BUILD)
    if (GetEnvironmentVariableW(L"PEP_PLACEMENT_CAPTURE", nullptr, 0))
        Require(PIXLoadLatestWinPixGpuCapturerLibrary() != nullptr, "PIX GPU capturer is unavailable");
#endif
}

template<class Record>
static void MeasurePlacement(GDX12Device& device, const char* name, UINT slots, Record record)
{
    if (PlacementCaptured) return;
    auto* queue = device.GetCommandQueue();
    GDX12GPUTimer timer(&device);
    using Clock = std::chrono::steady_clock;
#if defined(PROFILE_BUILD)
    wchar_t capturePath[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"PEP_PLACEMENT_CAPTURE", capturePath, MAX_PATH);
    if (length)
    {
        Require(length < MAX_PATH, "PIX capture path is too long");
        PIXCaptureParameters parameters = {};
        parameters.GpuCaptureParameters.FileName = capturePath;
        ThrowIfFailed(PIXBeginCapture(PIX_CAPTURE_GPU, &parameters));
        auto* commands = queue->GetCommandList();
        record(commands, 0, 0);
        queue->ExecuteCommandList(commands); queue->Flush();
        ThrowIfFailed(PIXEndCapture(FALSE));
        PlacementCaptured = true;
        std::wcout << L"PLACEMENT_CAPTURE " << capturePath << L'\n';
        return;
    }
#endif
    for (UINT run = 0; run < 5; ++run)
    {
        std::vector<double> gpu, cpu, submit, wait;
        const auto warmed = Clock::now() + std::chrono::seconds(2);
        for (UINT i = 0; gpu.size() < 768; ++i)
        {
            auto* commands = queue->GetCommandList();
            timer.Start(commands);
            const auto start = Clock::now();
            {
                GDX12_SCOPED_EVENT(commands, name);
                record(commands, i % slots, i);
            }
            const auto prepared = Clock::now();
            timer.Stop(commands);
            queue->ExecuteCommandList(commands);
            timer.OnCommandListExecuted(commands->FenceValue);
            const auto submitted = Clock::now();
            queue->Flush();
            const auto completed = Clock::now();
            if (i < 128 || completed < warmed) continue;
            gpu.push_back(timer.GetTimeMS());
            cpu.push_back(std::chrono::duration<double, std::milli>(prepared - start).count());
            submit.push_back(std::chrono::duration<double, std::milli>(submitted - prepared).count());
            wait.push_back(std::chrono::duration<double, std::milli>(completed - submitted).count());
        }
        const auto median = [](auto& values) { std::sort(values.begin(), values.end()); return values[values.size() / 2]; };
        std::cout << "PLACEMENT " << name << " run=" << run << " samples=" << gpu.size()
            << " gpu_ms=" << median(gpu) << " cpu_record_ms=" << median(cpu)
            << " cpu_submit_ms=" << median(submit) << " wait_ms=" << median(wait) << '\n';
        D3D12MA::TotalStatistics memory = {};
        device.GetMemoryAllocator()->CalculateStatistics(&memory);
        for (UINT heap = 0; heap < 2; ++heap)
            std::cout << "PLACEMENT_MEMORY " << name << " heap=" << (heap ? "UPLOAD" : "DEFAULT")
                << " allocations=" << memory.HeapType[heap].Stats.AllocationCount
                << " allocation_bytes=" << memory.HeapType[heap].Stats.AllocationBytes << '\n';
    }
}
