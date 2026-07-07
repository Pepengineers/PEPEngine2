#include "Engine.RendererDX12/GDX12StatsLogger.h"

#include <sstream>
#include <iomanip>

#include "Engine.RendererDX12/GDX12Device.h"

GDX12StatsLogger* GDX12StatsLogger::_instance = nullptr;

GDX12StatsLogger* GDX12StatsLogger::GetInstance()
{
    if (_instance == nullptr)
        _instance = new GDX12StatsLogger();
    return _instance;
}

void GDX12StatsLogger::RecordMspf(float mspf)
{
    _frameTimes.push_back(mspf);
    if (_frameTimes.size() == 200) { PostQuitMessage(0); }
}

void GDX12StatsLogger::GenerateReport(GDX12Device* primaryDevice, GDX12Device* secondaryDevice, int upscaletype)
{

    std::string fileName = "Report_SingleGPU";
    if (secondaryDevice) { fileName = "Report_DualGPU"; }
    
    fileName += "_" + upscaleTypeToString[upscaletype] + ".txt";

    std::ofstream reportFile(fileName);

    reportFile << "CPU: " + GetCPUName() << "\n";
    reportFile << "Total RAM: " + std::to_string(GetTotalRAMMB()) << " MB" << "\n";
    reportFile << "Primary GPU: " + primaryDevice->GetDeviceFeatures().Name << "\n";
    reportFile << "Secondary GPU: " << (secondaryDevice ? secondaryDevice->GetDeviceFeatures().Name : "NONE");
    reportFile << "\n";
    reportFile << _frameTimes.size() << "\n";
    for (float mspf : _frameTimes)
        reportFile << std::fixed << std::setprecision(6) << mspf << "\n";

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
    return static_cast<int>(_frameTimes.size());
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

void GDX12StatsLogger::ReadInitConfig(int& primaryGPUIndex, int& secondaryGPUIndex, int& ssaaMultiplier)
{
    std::ifstream configFile("SUPER_INIT_CONFIG.txt");
    std::string line;

    while (std::getline(configFile, line))
    {
        // Remove whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') { continue; }

        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) { continue; }

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim whitespace from key and value
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        if (key == "PrimaryGPUIndex") { primaryGPUIndex = std::stoi(value); }
        else if (key == "SecondaryGPUIndex") { secondaryGPUIndex = std::stoi(value); }
        else if (key == "SSAA multiplier") { ssaaMultiplier = std::stoi(value); }
    }
}