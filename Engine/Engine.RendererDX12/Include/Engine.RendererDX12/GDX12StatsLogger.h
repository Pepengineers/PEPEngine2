#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <windows.h>
#include <intrin.h>
#include <sysinfoapi.h>
#include <unordered_map>

#pragma comment(lib, "psapi.lib")
class GDX12Device;

class GDX12StatsLogger
{
public:
    static GDX12StatsLogger* GetInstance();

    void RecordFrameStats(float mspf, double primaryGPUTimeMS, double secondaryGPUTimeMS);
    // 0 - FSR
    // 1 - DLSS
    // 2 - XeSS
    void GenerateReport(GDX12Device* primaryDevice, GDX12Device* secondaryDevice, int upscaletype);
    void Shutdown();
    int GetNumLogs() const;
    void ReadInitConfig(int& primaryGPUIndex, int& secondaryGPUIndex, int& ssaaMultiplier, int& upscaleType, bool& singleGPUMode);

private:
    GDX12StatsLogger() = default;
    ~GDX12StatsLogger() = default;
    GDX12StatsLogger(const GDX12StatsLogger&) = delete;
    GDX12StatsLogger& operator=(const GDX12StatsLogger&) = delete;

    std::string GetCPUName() const;
    uint64_t GetTotalRAMMB() const;
    std::filesystem::path GetProjectRootPath() const;
    int ReadConfigValue(const std::string& line) const;

    static GDX12StatsLogger* _instance;
    std::vector<float> _mspfLogs;
    std::vector<double> _primaryGPUTimeLogs;
    std::vector<double> _secondaryGPUTimeLogs;

    std::unordered_map<int, std::string> upscaleTypeToString = 
    {   {0, "FSR"},
        {1, "DLSS"},
        {2, "XeSS"} };
};
