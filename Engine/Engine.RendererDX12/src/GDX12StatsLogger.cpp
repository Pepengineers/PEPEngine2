#include "Engine.RendererDX12/GDX12StatsLogger.h"

#include <sstream>
#include <iomanip>
#include <filesystem>

#include "Engine.RendererDX12/GDX12Device.h"

GDX12StatsLogger* GDX12StatsLogger::_instance = nullptr;

GDX12StatsLogger* GDX12StatsLogger::GetInstance()
{
    if (_instance == nullptr)
        _instance = new GDX12StatsLogger();
    return _instance;
}

void GDX12StatsLogger::RecordFrameStats(float mspf, double primaryGPUTimeMS, double secondaryGPUTimeMS)
{
    _mspfLogs.push_back(mspf);
    _primaryGPUTimeLogs.push_back(primaryGPUTimeMS);
    _secondaryGPUTimeLogs.push_back(secondaryGPUTimeMS);

    if (_mspfLogs.size() == 200) { PostQuitMessage(0); }
}

void GDX12StatsLogger::GenerateReport(GDX12Device* primaryDevice, GDX12Device* secondaryDevice, int upscaletype)
{

    std::string fileName = "Report_SingleGPU";
    if (secondaryDevice) { fileName = "Report_DualGPU"; }
    
    fileName += "_" + upscaleTypeToString[upscaletype] + ".txt";

    std::ofstream reportFile(GetProjectRootPath() / fileName);

    reportFile << "CPU: " + GetCPUName() << "\n";
    reportFile << "Total RAM: " + std::to_string(GetTotalRAMMB()) << " MB" << "\n";
    reportFile << "Primary GPU: " + primaryDevice->GetDeviceFeatures().Name << "\n";
    reportFile << "Secondary GPU: " << (secondaryDevice ? secondaryDevice->GetDeviceFeatures().Name : "NONE");
    reportFile << "\n";
    reportFile << "mspfLogs: " << _mspfLogs.size() << "\n";
    for (float mspf : _mspfLogs)
        reportFile << std::fixed << std::setprecision(6) << mspf << "\n";
    reportFile << "primaryGpuTimeLogs: " << _primaryGPUTimeLogs.size() << "\n";
    for (double primaryGPUTimeMS : _primaryGPUTimeLogs)
        reportFile << std::fixed << std::setprecision(6) << primaryGPUTimeMS << "\n";
    reportFile << "secondaryGpuTimeLogs: " << _secondaryGPUTimeLogs.size() << "\n";
    for (double secondaryGPUTimeMS : _secondaryGPUTimeLogs)
        reportFile << std::fixed << std::setprecision(6) << secondaryGPUTimeMS << "\n";

    reportFile.close();
}

void GDX12StatsLogger::Shutdown()
{
    if (_instance)
    {
        delete _instance;
        _instance = nullptr;
    }
}

int GDX12StatsLogger::GetNumLogs() const
{
    return static_cast<int>(_mspfLogs.size());
}

std::string GDX12StatsLogger::GetCPUName() const
{
    std::string cpuName = "Unknown";

    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 0x80000000);

    if ((unsigned int)cpuInfo[0] >= 0x80000004)
    {
        char brand[49] = { 0 };
        __cpuid(cpuInfo, 0x80000002);
        memcpy(brand, cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0x80000003);
        memcpy(brand + 16, cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0x80000004);
        memcpy(brand + 32, cpuInfo, sizeof(cpuInfo));

        cpuName = std::string(brand);

        size_t end = cpuName.find_last_not_of(" \t\n\r\f\v");
        if (end != std::string::npos)
            cpuName = cpuName.substr(0, end + 1);
    }

    return cpuName;
}

uint64_t GDX12StatsLogger::GetTotalRAMMB() const
{
    MEMORYSTATUSEX memoryStatus;
    memoryStatus.dwLength = sizeof(memoryStatus);
    GlobalMemoryStatusEx(&memoryStatus);
    return memoryStatus.ullTotalPhys / (1024 * 1024);
}

std::filesystem::path GDX12StatsLogger::GetProjectRootPath() const
{
    return std::filesystem::path(SOLUTION_DIR);
}

int GDX12StatsLogger::ReadConfigValue(const std::string& line) const
{
    return std::stoi(line.substr(line.find(':') + 1));
}

void GDX12StatsLogger::ReadInitConfig(int& primaryGPUIndex, int& secondaryGPUIndex, int& ssaaMultiplier, int& upscaleType, bool& singleGPUMode)
{
    std::ifstream configFile(GetProjectRootPath() / "SUPER_INIT_CONFIG.txt");
    std::string line;

    std::getline(configFile, line);
    primaryGPUIndex = ReadConfigValue(line);

    std::getline(configFile, line);
    secondaryGPUIndex = ReadConfigValue(line);
    singleGPUMode = secondaryGPUIndex == -1;

    std::getline(configFile, line);
    ssaaMultiplier = ReadConfigValue(line);

    std::getline(configFile, line);
    upscaleType = ReadConfigValue(line);
}
