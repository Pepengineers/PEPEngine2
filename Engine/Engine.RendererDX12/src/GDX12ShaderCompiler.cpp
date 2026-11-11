#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12Device.h"

#include <filesystem>
#include <stdexcept>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

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
    return CompileShaderDXC(filename, defines, entrypoint, GetShaderTargetForModel(device, shaderType));
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
    const auto model = device->GetDeviceFeatures().Features.HighestShaderModel();
    if (model < D3D_SHADER_MODEL_6_0)
        throw std::invalid_argument("Shader compilation requires Shader Model 6.0 or newer (DXC)");
    // shortcut: bundled DXC 1.8 signs retail shaders through SM 6.8; raise with the compiler upgrade.
    const auto targetModel = (std::min)(model, D3D_SHADER_MODEL_6_8);
    return shaderType + "_6_" + std::to_string(targetModel & 0xF);
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
            std::wstring defineWide(defineStr.begin(), defineStr.end());
            storage.push_back(defineWide);

            define++;
        }
    }
    // Take pointers only after the strings stop moving (including short strings).
    for (const auto& define : storage)
    {
        arguments.push_back(L"-D");
        arguments.push_back(define.c_str());
    }

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> results;
    ThrowIfFailed(_dxcCompiler->Compile(&sourceBuffer, arguments.data(), (UINT32)arguments.size(),
        _dxcIncludeHandler.Get(), IID_PPV_ARGS(&results)));

    ComPtr<IDxcBlobUtf8> errors;
    ThrowIfFailed(results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr));

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
    ThrowIfFailed(results->GetStatus(&compileStatus));
    if (FAILED(compileStatus))
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
    if (SUCCEEDED(results->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdbBlob), &pdbName)) && pdbBlob && pdbName)
    {
        std::wstring pdbFilePath = std::wstring(SHADERS_FOLDER) + L"/PDBs/" + pdbName->GetStringPointer();
        std::ofstream pdbFile(pdbFilePath, std::ios::binary);
        if (pdbFile.is_open())
            pdbFile.write(static_cast<const char*>(pdbBlob->GetBufferPointer()), pdbBlob->GetBufferSize());
    }

    //IDxcBlob to ID3DBlob conversion
    ComPtr<IDxcBlob> dxcBlob;
    ThrowIfFailed(results->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxcBlob), &outputName));

    ComPtr<ID3DBlob> d3dBlob;
    ThrowIfFailed(D3DCreateBlob(dxcBlob->GetBufferSize(), &d3dBlob));
    memcpy(d3dBlob->GetBufferPointer(), dxcBlob->GetBufferPointer(), dxcBlob->GetBufferSize());

    Log::Info("Shader", "Compiled ", filename, " [", entrypoint, ", ", shaderType, ']');
    return d3dBlob;
}
