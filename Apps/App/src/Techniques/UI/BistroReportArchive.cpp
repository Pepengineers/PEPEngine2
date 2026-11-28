#include "BistroReportArchive.h"
#include "BistroReportCsvReader.h"
#include "../../../Resources/Resource.h"
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include <atomic>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace BistroReport
{
namespace fs = std::filesystem;

std::string Escape(std::string_view text)
{
    std::string escaped;
    for (char c : text)
    {
        switch (c)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += c;
        }
    }
    return escaped;
}

namespace
{
std::string Link(const fs::path& relative, std::string_view label)
{
    const auto name = relative.generic_u8string();
    std::string url = "./";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '.' || c == '-' || c == '_') url += char(c);
        else { url += '%'; url += hex[c >> 4]; url += hex[c & 15]; }
    }
    return "<a href=\"" + url + "\" download>" + Escape(label) + "</a>";
}

struct Sources
{
    const fs::path Root;
    std::map<fs::path, fs::file_time_type> Files;
    std::set<fs::path> Missing, Scanned;
    std::vector<fs::path> Pending;

    void Add(const fs::path& source, bool scan)
    {
        const auto path = source.lexically_normal();
        const auto rel = path.lexically_relative(Root);
        if (rel.empty() || *rel.begin() == ".." || rel.has_root_path() || fs::weakly_canonical(path) != path)
            throw std::runtime_error("Source file is outside the benchmark archive layout.");
        const auto stem = path.stem().u8string();
        const bool report = stem.rfind("bistro-", 0) == 0;
        const bool detail = path.parent_path().filename().u8string().rfind("bistro-", 0) == 0 &&
            (stem.rfind("pass-", 0) == 0 || stem.rfind("trial-", 0) == 0);
        if (!(path.extension() == ".csv" && (report || detail)) && !(path.extension() == ".ini" && report))
            throw std::runtime_error("Unexpected source file in benchmark report: " + rel.generic_u8string());
        if (!Files.count(rel))
        {
            if (!fs::is_regular_file(path)) { Missing.insert(rel); return; }
            Files.emplace(rel, fs::last_write_time(path));
        }
        if (scan && path.extension() == ".csv" && Scanned.insert(path).second) Pending.push_back(path);
    }

    void Collect(const fs::path& run, const std::vector<fs::path>& measuredPasses)
    {
        Add(run, true);
        if (!Missing.empty()) throw std::runtime_error("The selected run CSV is no longer available.");
        for (const auto& path : measuredPasses) Add(path, true);
        while (!Pending.empty())
        {
            const auto path = Pending.back();
            Pending.pop_back();
            CsvReader csv(path);
            while (csv.Next(true))
            {
                const auto& key = csv.Get("name");
                const auto leaf = key.substr(key.rfind('.') + 1);
                const auto& raw = csv.Get("value");
                if (raw.empty() || (leaf != "PassCSV" && leaf != "TrialCSV" && leaf != "LastPassCSV" &&
                    leaf != "ReusedPassCSV" && leaf != "RunCSV" && leaf != "CSV" && leaf != "PresetFile")) continue;
                const auto ref = fs::u8path(raw);
                if (ref.has_root_path()) throw std::runtime_error("Expected a relative benchmark source path.");
                // Include a reused pass's parent summary without expanding that entire run.
                Add(path.parent_path() / ref, leaf != "RunCSV");
            }
        }
    }
};

struct StagingDirectory
{
    fs::path Path;
    explicit StagingDirectory(const fs::path& directory)
    {
        static std::atomic<unsigned> sequence{0};
        do
        {
            Path = directory / (L".export-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence));
        } while (!fs::create_directory(Path));
    }
    ~StagingDirectory()
    {
        std::error_code ignored;
        fs::remove_all(Path, ignored);
    }
};

void Compress(const fs::path& staging)
{
    wchar_t system[MAX_PATH];
    const auto length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) throw std::runtime_error("Cannot locate Windows tar.exe.");
    const auto tar = fs::path(system) / "tar.exe";
    std::wstring args = L"tar.exe -cf report.zip --format zip --no-recursion -T files.list";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, staging.c_str(), &startup, &process))
        throw std::system_error(GetLastError(), std::system_category(), "Cannot start Windows tar.exe");
    CloseHandle(process.hThread);
    DWORD code = 1;
    const bool ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &code) && code == 0;
    CloseHandle(process.hProcess);
    if (!ok) throw std::runtime_error("Could not compress the benchmark source files.");
}

std::string_view ResourceBytes(int id, LPCWSTR type)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), type);
    const char* data = resource ? static_cast<const char*>(LockResource(LoadResource(module, resource))) : nullptr;
    if (!data) throw std::runtime_error("Cannot load a benchmark report resource.");
    return {data, SizeofResource(module, resource)};
}

std::string IconDataUrl()
{
    const auto group = ResourceBytes(IDI_PEPENGINE2, RT_GROUP_ICON);
    const auto id = LookupIconIdFromDirectoryEx(reinterpret_cast<PBYTE>(const_cast<char*>(group.data())), TRUE, 64, 64, LR_DEFAULTCOLOR);
    const auto png = ResourceBytes(id, RT_ICON);
    const auto data = reinterpret_cast<const BYTE*>(png.data());
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD length = 0;
    if (!CryptBinaryToStringA(data, DWORD(png.size()), flags, nullptr, &length))
        throw std::system_error(GetLastError(), std::system_category(), "Cannot size the report icon");
    std::string encoded(length, '\0');
    if (!CryptBinaryToStringA(data, DWORD(png.size()), flags, encoded.data(), &length))
        throw std::system_error(GetLastError(), std::system_category(), "Cannot encode the report icon");
    encoded.resize(length);
    return "data:image/png;base64," + encoded;
}

void WriteFileBytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), bytes.size());
    file.close();
}
}

std::string PassSourceMarker(size_t index)
{
    return "<!--BENCHMARK_PASS_SOURCE_" + std::to_string(index) + "-->";
}

fs::path WriteArchive(const fs::path& root, const fs::path& run, fs::file_time_type indexedAt,
    fs::path archiveStem, std::string body, std::vector<fs::path> measuredPasses, std::vector<fs::path> completedPasses)
{
    Sources sources{fs::canonical(root)};
    const auto selected = sources.Root / run.lexically_relative(root);
    for (auto& path : measuredPasses) path = sources.Root / path.lexically_relative(root);
    for (auto& path : completedPasses)
    {
        path = path.lexically_normal().lexically_relative(root);
        measuredPasses.push_back(sources.Root / path);
    }
    sources.Collect(selected, measuredPasses);
    if (sources.Files.at(selected.lexically_relative(sources.Root)) != indexedAt)
        throw std::runtime_error("This result has changed. Refresh the run history and export it again.");
    const auto directory = sources.Root / L"Exports";
    fs::create_directories(directory);
    StagingDirectory staging(directory);
    std::ofstream list(staging.Path / "files.list", std::ios::binary);
    list.exceptions(std::ios::failbit | std::ios::badbit);
    // bsdtar applies a directory change inside a file list to the following entries.
    list << "index.html\n-C\n" << sources.Root.lexically_relative(staging.Path).generic_u8string() << '\n';
    size_t markerOffset = 0;
    for (size_t i = 0; i < completedPasses.size(); ++i)
    {
        const auto marker = PassSourceMarker(i);
        const auto pos = body.find(marker, markerOffset);
        if (pos == std::string::npos) throw std::runtime_error("Cannot locate a completed pass source in the report.");
        const auto& path = completedPasses[i];
        const auto link = sources.Files.count(path) ? Link(path, "CSV") : "<span class=\"muted\">Unavailable</span>";
        body.replace(pos, marker.size(), link);
        markerOffset = pos + link.size();
    }
    body += "<details id=\"sources\"><summary>Source files <small>" + std::to_string(sources.Files.size());
    if (!sources.Missing.empty()) body += " saved, " + std::to_string(sources.Missing.size()) + " unavailable";
    body += "</small></summary>";
    std::map<fs::path, std::vector<fs::path>> groups;
    for (const auto& [path, time] : sources.Files)
    {
        groups[path.parent_path()].push_back(path);
        list << path.generic_u8string() << '\n';
    }
    list.close();
    for (const auto& [folder, paths] : groups)
    {
        body += "<details class=\"source-group\"><summary>" + Escape(folder.empty() ? "Report directory" : folder.generic_u8string()) +
            " <small>" + std::to_string(paths.size()) + "</small></summary><ul class=\"files\">";
        for (const auto& path : paths) body += "<li>" + Link(path, path.filename().u8string()) + "</li>";
        body += "</ul></details>";
    }
    if (!sources.Missing.empty())
    {
        body += "<h3>Unavailable source files</h3><ul>";
        for (const auto& path : sources.Missing) body += "<li>" + Escape(path.generic_u8string()) + "</li>";
        body += "</ul>";
    }
    body += "</details>";
    std::string page(ResourceBytes(IDR_BISTRO_REPORT, RT_RCDATA));
    constexpr std::string_view icon = "@@ENGINE_ICON@@";
    page.replace(page.find(icon), icon.size(), IconDataUrl());
    constexpr std::string_view marker = "<!--BENCHMARK-->";
    page.replace(page.find(marker), marker.size(), body);
    WriteFileBytes(staging.Path / "index.html", page);
    Compress(staging.Path);
    for (const auto& [path, time] : sources.Files)
        if (fs::last_write_time(sources.Root / path) != time)
            throw std::runtime_error("A source file changed during export. Refresh the run history and retry.");
    const auto base = archiveStem.wstring();
    for (unsigned version = 1; ; ++version)
    {
        const auto output = directory / (base + (version == 1 ? L"" : L"-" + std::to_wstring(version)) + L".zip");
        if (MoveFileW((staging.Path / "report.zip").c_str(), output.c_str())) return output;
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            throw std::system_error(error, std::system_category(), "Cannot save the ZIP archive");
    }
}
}
