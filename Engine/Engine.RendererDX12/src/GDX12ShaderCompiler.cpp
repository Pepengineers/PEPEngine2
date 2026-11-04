#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12Device.h"

#include <filesystem>
#include <map>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

namespace
{
    class ShaderIncludes final : public ID3DInclude
    {
    public:
        explicit ShaderIncludes(const std::filesystem::path& source) : _sourceDirectory(source.parent_path()) {}

        HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* size) override
        {
            try
            {
                const auto owner = _files.find(parent);
                const auto directory = owner == _files.end() ? _sourceDirectory : owner->second.Path.parent_path();
                auto path = directory / name;
                if (!std::filesystem::is_regular_file(path)) path = std::filesystem::path(SHADERS_FOLDER) / name;
                std::ifstream input(path, std::ios::binary);
                if (!input) return E_FAIL;
                File file{ path, std::vector<char>(std::istreambuf_iterator<char>(input), {}) };
                if (input.bad() || file.Bytes.size() > UINT_MAX) return E_FAIL;
                const auto byteCount = static_cast<UINT>(file.Bytes.size());
                file.Bytes.push_back('\0');
                const auto contents = file.Bytes.data();
                const auto inserted = _files.emplace(contents, std::move(file));
                *data = inserted.first->second.Bytes.data();
                *size = byteCount;
                return S_OK;
            }
            catch (...) { return E_FAIL; }
        }
        HRESULT __stdcall Close(LPCVOID data) override { _files.erase(data); return S_OK; }
    private:
        struct File { std::filesystem::path Path; std::vector<char> Bytes; };
        std::filesystem::path _sourceDirectory;
        std::map<LPCVOID, File> _files;
    };
}

GDX12ShaderCompiler* GDX12ShaderCompiler::_instance = nullptr;

GDX12ShaderCompiler& GDX12ShaderCompiler::GetInstance()
{
    if (!_instance)
    {
        _instance = new GDX12ShaderCompiler();
    }
    return *_instance;
}

GDX12ShaderCompiler::~GDX12ShaderCompiler()
{
    _dxcCompiler.Reset();
    _dxcUtils.Reset();
    _dxcIncludeHandler.Reset();
}

void GDX12ShaderCompiler::Initialize()
{
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_dxcCompiler)));
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&_dxcUtils)));
    ThrowIfFailed(_dxcUtils->CreateDefaultIncludeHandler(&_dxcIncludeHandler));
}

ComPtr<ID3DBlob> GDX12ShaderCompiler::CompileShader(GDX12Device* device, const std::wstring& filename, const D3D_SHADER_MACRO* defines, const std::string& entrypoint, const std::string& shaderType)
{
    std::string target = GetShaderTargetForModel(device, shaderType);

    if (device->GetDeviceFeatures().Features.HighestShaderModel() == D3D_SHADER_MODEL_5_1)
    {
        return CompileShaderFXC(filename, defines, entrypoint, target);
    }
    else return CompileShaderDXC(filename, defines, entrypoint, target);
}

GDX12ShaderCompiler::GDX12ShaderCompiler()
{
    Initialize();
}

void GDX12ShaderCompiler::Shutdown()
{
    if (_instance == nullptr) return;
    delete _instance;
    _instance = nullptr;
}

std::string GDX12ShaderCompiler::GetShaderTargetForModel(GDX12Device* device, const std::string& shaderType)
{
    D3D_SHADER_MODEL targetModel = device->GetDeviceFeatures().Features.HighestShaderModel();

    UINT modelMajor = (targetModel >> 4) & 0xF;
    UINT modelMinor = targetModel & 0xF;

    std::string targetVersion;

    switch (modelMajor) {
    case 6:
        if (modelMinor >= 10 && targetModel >= D3D_SHADER_MODEL_6_10) { targetVersion = "6_10"; }
        else if (modelMinor >= 9 && targetModel >= D3D_SHADER_MODEL_6_9) { targetVersion = "6_9"; }
        else if (modelMinor >= 8 && targetModel >= D3D_SHADER_MODEL_6_8) { targetVersion = "6_8"; }
        else if (modelMinor >= 7 && targetModel >= D3D_SHADER_MODEL_6_7) { targetVersion = "6_7"; }
        else if (modelMinor >= 6 && targetModel >= D3D_SHADER_MODEL_6_6) { targetVersion = "6_6"; }
        else if (modelMinor >= 5 && targetModel >= D3D_SHADER_MODEL_6_5) { targetVersion = "6_5"; }
        else if (modelMinor >= 4 && targetModel >= D3D_SHADER_MODEL_6_4) { targetVersion = "6_4"; }
        else if (modelMinor >= 3 && targetModel >= D3D_SHADER_MODEL_6_3) { targetVersion = "6_3"; }
        else if (modelMinor >= 2 && targetModel >= D3D_SHADER_MODEL_6_2) { targetVersion = "6_2"; }
        else if (modelMinor >= 1 && targetModel >= D3D_SHADER_MODEL_6_1) { targetVersion = "6_1"; }
        else { targetVersion = "6_0"; }
        break;
    case 5:
        targetVersion = "5_1";
        break;
    default:
        targetVersion = "5_1";
        break;
    }

    return shaderType + "_" + targetVersion;
}

ComPtr<ID3DBlob> GDX12ShaderCompiler::CompileShaderDXC(const std::wstring& filename, const D3D_SHADER_MACRO* defines, const std::string& entrypoint, const std::string& shaderType)
{
    Log::Info("Shader", "Compiling ", filename, " [", entrypoint, ", ", shaderType, ']');
    ComPtr<IDxcBlobEncoding> sourceBlob;
    ThrowIfFailed(_dxcUtils->LoadFile(filename.c_str(), nullptr, &sourceBlob));

    std::vector<LPCWSTR> arguments;
    std::vector<std::wstring> storage;

    arguments.push_back(L"-E");
    std::wstring entrypointW(entrypoint.begin(), entrypoint.end());
    arguments.push_back(entrypointW.c_str());

    arguments.push_back(L"-T");
    std::wstring targetW(shaderType.begin(), shaderType.end());
    arguments.push_back(targetW.c_str());

    const auto sourceDirectory = std::filesystem::path(filename).parent_path().wstring();
    arguments.push_back(L"-I");
    arguments.push_back(sourceDirectory.c_str());

    arguments.push_back(L"-I");
    arguments.push_back(SHADERS_FOLDER);

    arguments.push_back(L"-I");
    arguments.push_back(L"./");

    arguments.push_back(L"-I");
    arguments.push_back(L"../");

    arguments.push_back(L"-Zi");    // Generate debug information
    arguments.push_back(L"-O3"); // max optimization

    if (defines)
    {
        const D3D_SHADER_MACRO* define = defines;
        while (define->Name && define->Definition)
        {
            std::string defineStr = std::string(define->Name) + "=" + define->Definition;
            arguments.push_back(L"-D");
            std::wstring defineWide(defineStr.begin(), defineStr.end());
            storage.push_back(defineWide);
            arguments.push_back(storage.back().c_str());

            define++;
        }
    }

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> results;
    HRESULT hr = _dxcCompiler->Compile(&sourceBuffer, arguments.data(), (UINT32)arguments.size(), 
        _dxcIncludeHandler.Get(), IID_PPV_ARGS(&results));

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(hr)) results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (errors != nullptr && errors->GetStringLength() > 0)
    {
        Log::Warning("Shader", "Compiler diagnostics for ", filename, ": ",
            std::string_view(errors->GetStringPointer(), errors->GetStringLength()));
        //print a bunch of info if shader doesnt want to compile
        OutputDebugStringA("Shader compilation warnings/errors:\n");
        OutputDebugStringA(errors->GetStringPointer());

        if (errors->GetStringLength() > 0) {
            OutputDebugStringA("\n=== Shader Compilation Details ===\n");
            OutputDebugStringW((L"Shader: " + filename + L"\n").c_str());
            OutputDebugStringA(("Entry point: " + entrypoint + "\n").c_str());
            OutputDebugStringA(("Target: " + shaderType + "\n").c_str());
        }
    }

    ComPtr<IDxcBlobUtf16> outputName;
    HRESULT compileStatus;
    if (SUCCEEDED(results->GetStatus(&compileStatus)) && FAILED(compileStatus))
    {
        Log::Error("Shader", "Compilation failed: ", filename, " [", entrypoint, ", ", shaderType, ']');
        if (errors != nullptr && errors->GetStringLength() > 0)
        {
            OutputDebugStringA("Shader compilation failed:\n");
            OutputDebugStringA(errors->GetStringPointer());
        }
        ThrowIfFailed(compileStatus);
    }

    //save .pdb file for debugging
    ComPtr<IDxcBlob> pdbBlob;
    ComPtr<IDxcBlobWide> pdbName;
    results->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdbBlob), &pdbName);
    const wchar_t* pdbNameStr = pdbName->GetStringPointer();
    std::wstring pdbFilePath = std::wstring(SHADERS_FOLDER) + L"/PDBs/" + pdbNameStr;
    std::ofstream pdbFile(pdbFilePath, std::ios::binary);
    if (pdbFile.is_open())
    {
        pdbFile.write(static_cast<const char*>(pdbBlob->GetBufferPointer()),
            pdbBlob->GetBufferSize());
        pdbFile.close();
    }

    //IDxcBlob to ID3DBlob conversion
    ComPtr<IDxcBlob> dxcBlob;
    ThrowIfFailed(results->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxcBlob), &outputName));

    ComPtr<ID3DBlob> d3dBlob;
    D3DCreateBlob(dxcBlob->GetBufferSize(), &d3dBlob);
    memcpy(d3dBlob->GetBufferPointer(), dxcBlob->GetBufferPointer(), dxcBlob->GetBufferSize());

    Log::Info("Shader", "Compiled ", filename, " [", entrypoint, ", ", shaderType, ']');
    return d3dBlob;
}

ComPtr<ID3DBlob> GDX12ShaderCompiler::CompileShaderFXC(const std::wstring& filename, const D3D_SHADER_MACRO* defines, const std::string& entrypoint, const std::string& shaderType)
{
    Log::Info("Shader", "Compiling ", filename, " [", entrypoint, ", ", shaderType, ']');
    UINT compileFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)  
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = S_OK;

    std::wstring prettyPathToShader = std::filesystem::weakly_canonical(filename).wstring();
    ComPtr<ID3DBlob> byteCode = nullptr;
    ComPtr<ID3DBlob> errors;
    ShaderIncludes includes(prettyPathToShader);
    hr = D3DCompileFromFile(prettyPathToShader.c_str(), defines, &includes, entrypoint.c_str(), shaderType.c_str(), compileFlags, 0, &byteCode, &errors);

    std::wstring compilerLog;
    if (errors) 
    {
        Log::Warning("Shader", "Compiler diagnostics for ", filename, ": ",
            std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        const char* ptr = (const char*)errors->GetBufferPointer();
        size_t len = errors->GetBufferSize();
        int need = MultiByteToWideChar(CP_ACP, 0, ptr, (int)len, nullptr, 0);
        compilerLog.resize(need);
        MultiByteToWideChar(CP_ACP, 0, ptr, (int)len, compilerLog.data(), need);
    }

    if (FAILED(hr)) 
    {
        Log::Error("Shader", "Compilation failed: ", filename, " [", entrypoint, ", ", shaderType, ']');
        std::wstringstream det;

        std::wstring strCopy = prettyPathToShader;
        for (auto& ch : strCopy) if (ch == L' ') ch = L'\u00A0';
        prettyPathToShader = strCopy;

        det << L"Path to the shader: " << prettyPathToShader << L"\n";
        if (!compilerLog.empty()) det << L"\n" << compilerLog;

        ThrowIfFailed(hr);
    }

    Log::Info("Shader", "Compiled ", filename, " [", entrypoint, ", ", shaderType, ']');
    return byteCode;
}
