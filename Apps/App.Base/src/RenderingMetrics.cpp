#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "App.Base/RenderingMetrics.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"

#include <cfgmgr32.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <initguid.h>
#include <devpkey.h>
#include <pciprop.h>

#pragma comment(lib, "Cfgmgr32.lib")

namespace
{
    // Hardware queries return an empty string or zero when the value is unavailable.
    std::string ReadSmbiosString(const uint8_t* first, const uint8_t* last, unsigned index)
    {
        if (!index) { return {}; }
        while (first < last)
        {
            const auto* end = std::find(first, last, uint8_t{0});
            if (--index == 0)
            { return std::string(reinterpret_cast<const char*>(first), static_cast<size_t>(end - first)); }
            if (end == last) { break; }
            first = end + 1;
        }
        return {};
    }

    std::string GetCPUName()
    {
#if defined(_M_IX86) || defined(_M_X64)
        constexpr unsigned firstBrandLeaf = 0x80000002u;
        constexpr unsigned lastBrandLeaf = 0x80000004u;
        int registers[4]{};
        __cpuid(registers, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(registers[0]) < lastBrandLeaf) { return {}; }

        std::array<char, sizeof(registers) * 3 + 1> brand{};
        for (unsigned leaf = firstBrandLeaf; leaf <= lastBrandLeaf; ++leaf)
        {
            __cpuid(registers, static_cast<int>(leaf));
            std::memcpy(brand.data() + (leaf - firstBrandLeaf) * sizeof(registers), registers, sizeof(registers));
        }
        const std::string name(brand.data());
        const auto begin = name.find_first_not_of(' ');
        if (begin != std::string::npos) { return name.substr(begin, name.find_last_not_of(' ') - begin + 1); }
#endif
        return {};
    }

    std::uint64_t GetInstalledRAMBytes()
    {
        ULONGLONG installedKilobytes = 0;
        if (!GetPhysicallyInstalledSystemMemory(&installedKilobytes)) { return 0; }
        return installedKilobytes * 1024;
    }

    std::string GetMotherboardName()
    {
        constexpr DWORD rawSmbiosProvider = 'RSMB';
        constexpr size_t rawHeaderSize = 8;
        constexpr size_t tableLengthOffset = 4;
        constexpr uint8_t baseboardType = 2;
        constexpr uint8_t endOfTableType = 127;
        constexpr size_t manufacturerIndexOffset = 4;
        constexpr size_t productIndexOffset = 5;

        const UINT bufferSize = GetSystemFirmwareTable(rawSmbiosProvider, 0, nullptr, 0);
        if (bufferSize < rawHeaderSize) { return {}; }
        std::vector<uint8_t> buffer(bufferSize);
        if (GetSystemFirmwareTable(rawSmbiosProvider, 0, buffer.data(), bufferSize) != bufferSize) { return {}; }

        uint32_t tableSize = 0;
        std::memcpy(&tableSize, buffer.data() + tableLengthOffset, sizeof(tableSize));
        if (tableSize > bufferSize - rawHeaderSize) { return {}; }

        const auto* record = buffer.data() + rawHeaderSize;
        const auto* tableEnd = record + tableSize;
        while (tableEnd - record >= 4 && record[0] != endOfTableType)
        {
            const size_t formattedSize = record[1];
            if (formattedSize < 4 || formattedSize > static_cast<size_t>(tableEnd - record)) { break; }
            const auto* strings = record + formattedSize;
            const auto* stringsEnd = std::adjacent_find(strings, tableEnd, [](uint8_t first, uint8_t second)
            { return first == 0 && second == 0; });
            if (stringsEnd == tableEnd) { break; }

            if (record[0] == baseboardType && formattedSize > productIndexOffset)
            {
                const auto product = ReadSmbiosString(strings, stringsEnd, record[productIndexOffset]);
                if (!product.empty())
                {
                    const auto manufacturer = ReadSmbiosString(strings, stringsEnd, record[manufacturerIndexOffset]);
                    auto name = manufacturer.empty() ? product : manufacturer + ' ' + product;
                    // SMBIOS strings should be UTF-8. Omit malformed firmware strings from JSON.
                    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0))
                    { return name; }
                }
            }
            record = stringsEnd + 2;
        }
        return {};
    }

    std::string GetAdapterName(LUID luid)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(GDX12DeviceFactory::GetFactory()->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) { return {}; }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) { return {}; }

        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) { return {}; }
        std::string name(size, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, desc.Description, -1, name.data(), size, nullptr, nullptr))
        { return {}; }
        name.pop_back();
        return name;
    }

    unsigned GetDevicePCIeGeneration(GDX12Device& gpu)
    {
        if (gpu.GetDeviceFeatures().Features.UMA()) { return 0; }

        DISPLAYCONFIG_ADAPTER_NAME adapter{};
        adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        adapter.header.size = sizeof(adapter);
        adapter.header.adapterId = gpu.GetAdapterLuid();
        if (DisplayConfigGetDeviceInfo(&adapter.header) != ERROR_SUCCESS) { return 0; }

        WCHAR instanceId[MAX_DEVICE_ID_LEN]{};
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        ULONG size = sizeof(instanceId);
        if (CM_Get_Device_Interface_PropertyW(adapter.adapterDevicePath, &DEVPKEY_Device_InstanceId, &type,
                reinterpret_cast<PBYTE>(instanceId), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        { return 0; }

        DEVINST device = 0;
        if (CM_Locate_DevNodeW(&device, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) { return 0; }

        ULONG generation = 0;
        size = sizeof(generation);
        if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_PciDevice_CurrentLinkSpeed, &type,
                reinterpret_cast<PBYTE>(&generation), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_UINT32 || size != sizeof(generation) || generation > 7)
        { return 0; }
        return generation;
    }

    void WriteHardwareName(ryml::NodeRef node, const std::string& name)
    {
        if (name.empty()) { node << nullptr; return; }
        node << name;
        node |= ryml::VAL_DQUO;
    }

    void WriteGPUTime(ryml::NodeRef node, double milliseconds)
    {
        if (milliseconds >= 0.0) { node << milliseconds; }
        else { node << nullptr; }
    }
}

void RenderingMetrics::Initialize(GDX12Device* primary, GDX12Device* secondary)
{
    _initialized = false;
    ResetInterval();
    _history.clear();

    _cpuName = GetCPUName();
    _ramBytes = GetInstalledRAMBytes();
    _motherboardName = GetMotherboardName();
    _adapters.clear();
    const auto addAdapter = [&](EDeviceRole role, GDX12Device* device)
    {
        if (!device) { return; }
        const auto luid = device->GetAdapterLuid();
        _adapters.push_back({role, GetAdapterName(luid), GetDevicePCIeGeneration(*device)});
    };
    addAdapter(DEVICE_ROLE_PRIMARY, primary);
    addAdapter(DEVICE_ROLE_SECONDARY, secondary);
    _initialized = true;
}

void RenderingMetrics::RecordPresentedFrame(const CompiledFrameGraph& frameGraph)
{
    if (!_initialized) { return; }

    const auto now = Clock::now();
    if (!_intervalStart)
    {
        _intervalStart = now;
        return;
    }

    ++_presentCount;
    const std::chrono::duration<double> interval = now - *_intervalStart;
    if (interval < std::chrono::seconds(1)) { return; }

    auto frame = frameGraph.GetLatestFrameMetrics();
    _history.push_back({_presentCount / interval.count(), frame.PrimaryMS, frame.SecondaryMS,
        frame.CrossGPUCopies, std::move(frame.PassTimings)});
    _intervalStart = now;
    _presentCount = 0;
}

void RenderingMetrics::ResetInterval()
{
    _intervalStart.reset();
    _presentCount = 0;
}

std::string RenderingMetrics::ToJson() const
{
    ryml::Tree report;
    auto root = report.rootref();
    root |= ryml::MAP;
    auto hardware = root["hardware"];
    hardware |= ryml::MAP;
    WriteHardwareName(hardware["cpu_name"], _cpuName);
    if (_ramBytes) { hardware["ram_bytes"] << _ramBytes; }
    else { hardware["ram_bytes"] << nullptr; }
    WriteHardwareName(hardware["motherboard"], _motherboardName);

    auto adapters = hardware["adapters"];
    adapters |= ryml::SEQ;
    for (const auto& adapter : _adapters)
    {
        auto node = adapters.append_child();
        node |= ryml::MAP;
        node["role"] = GetDeviceRoleName(adapter.Role);
        node["name"] << adapter.Name;
        node["name"] |= ryml::VAL_DQUO;
        if (adapter.PCIeGeneration) { node["pcie_generation"] << adapter.PCIeGeneration; }
        else { node["pcie_generation"] << nullptr; }
    }

    const bool usesTwoGPUs = _adapters.size() > 1;
    auto samples = root["samples"];
    samples |= ryml::SEQ;
    for (const auto& sample : _history)
    {
        auto node = samples.append_child();
        node |= ryml::MAP;
        node["fps"] << sample.FPS;
        WriteGPUTime(node["gpu_primary_frame_ms"], sample.GPUPrimaryFrameMS);
        WriteGPUTime(node["gpu_secondary_frame_ms"], sample.GPUSecondaryFrameMS);
        auto passTimings = node["render_pass_timings"];
        passTimings |= ryml::MAP;
        for (const auto& adapter : _adapters)
        {
            auto passes = passTimings[GetDeviceRoleName(adapter.Role)];
            passes |= ryml::SEQ;
            for (const auto& timing : sample.PassTimings)
            {
                if (timing.Device != adapter.Role) { continue; }
                auto pass = passes.append_child();
                pass |= ryml::MAP;
                pass["name"] << timing.Name;
                pass["name"] |= ryml::VAL_DQUO;
                WriteGPUTime(pass["time_ms"], timing.TimeMS);
            }
        }
        if (usesTwoGPUs)
        {
            auto copies = node["cross_gpu_copies"];
            copies |= ryml::MAP;
            copies["resource_count"] << sample.CrossGPUCopies.ResourceCount;
            copies["bytes"] << sample.CrossGPUCopies.Bytes;
            WriteGPUTime(copies["export_ms"], sample.CrossGPUCopies.ExportMS);
            WriteGPUTime(copies["import_ms"], sample.CrossGPUCopies.ImportMS);
        }
    }
    return ryml::emitrs_json<std::string>(report);
}

void RenderingMetrics::SaveReport() const
{
    if (!_initialized) { return; }

    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
    gmtime_s(&utc, &timestamp);

    std::wostringstream fileName;
    fileName << L"render-metrics-" << std::put_time(&utc, L"%Y%m%d-%H%M%S")
        << L'-' << std::setfill(L'0') << std::setw(3) << milliseconds << L"Z-" << GetCurrentProcessId() << L".json";
    const auto directory = std::filesystem::path(SOLUTION_DIR) / L"Reports";
    const auto path = directory / fileName.str();
    auto temporaryPath = path;
    temporaryPath += L".tmp";

    try
    {
        std::filesystem::create_directories(directory);
        std::ofstream report;
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report.open(temporaryPath, std::ios::binary);
        report << ToJson() << '\n';
        report.close();

        if (!MoveFileW(temporaryPath.c_str(), path.c_str()))
        { throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "publish render metrics report"); }
        OutputDebugStringW((L"Render metrics saved to " + path.wstring() + L"\n").c_str());
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        OutputDebugStringA((std::string("ERROR: Unable to save render metrics report. ") + error.what() + '\n').c_str());
    }
}
