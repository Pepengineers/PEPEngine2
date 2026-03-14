#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

#include "Engine.RendererDX12/GDX12Device.h"

#include <filesystem>

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
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_dxcCompiler));
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&_dxcUtils));
    _dxcUtils->CreateDefaultIncludeHandler(&_dxcIncludeHandler);
}

ComPtr<ID3DBlob> GDX12ShaderCompiler::CompileShader(std::shared_ptr<GDX12Device> device, const std::wstring& filename, const D3D_SHADER_MACRO* defines, const std::string& entrypoint, const std::string& shaderType)
{
    std::string target = GetShaderTargetForModel(device, shaderType);

    if (device->GetDeviceFeatures().MaxShaderModel == D3D_SHADER_MODEL_5_1)
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

std::string GDX12ShaderCompiler::GetShaderTargetForModel(std::shared_ptr<GDX12Device> device, const std::string& shaderType)
{
    D3D_SHADER_MODEL targetModel = device->GetDeviceFeatures().MaxShaderModel;

    UINT modelMajor = (targetModel >> 4) & 0xF;
    UINT modelMinor = targetModel & 0xF;

    std::string targetVersion;

    switch (modelMajor) {
    case 6:
        if (modelMinor >= 8 && targetModel >= D3D_SHADER_MODEL_6_8) { targetVersion = "6_8"; }
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
    HRESULT hr = _dxcCompiler->Compile(
        &sourceBuffer,
        arguments.data(),
        (UINT32)arguments.size(),
        _dxcIncludeHandler.Get(),
        IID_PPV_ARGS(&results));

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(hr)) results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (errors != nullptr && errors->GetStringLength() > 0)
    {
        //print a bunch of info if shader doesnt want to compile
        OutputDebugStringA("Shader compilation warnings/errors:\n");
        OutputDebugStringA(errors->GetStringPointer());

        if (errors->GetStringLength() > 0) {
            OutputDebugStringA("\n=== Shader Compilation Details ===\n");
            OutputDebugStringA(("Shader: " + std::string(filename.begin(), filename.end()) + "\n").c_str());
            OutputDebugStringA(("Entry point: " + entrypoint + "\n").c_str());
            OutputDebugStringA(("Target: " + std::string(shaderType.begin(), shaderType.end()) + "\n").c_str());
        }
    }

    ComPtr<IDxcBlobUtf16> outputName;
    HRESULT compileStatus;
    if (SUCCEEDED(results->GetStatus(&compileStatus)) && FAILED(compileStatus))
    {
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

    return d3dBlob;
}

ComPtr<ID3DBlob> GDX12ShaderCompiler::CompileShaderFXC(const std::wstring& filename, const D3D_SHADER_MACRO* defines, const std::string& entrypoint, const std::string& shaderType)
{
    UINT compileFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)  
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = S_OK;

    std::wstring prettyPathToShader = std::filesystem::weakly_canonical(filename).wstring();
    ComPtr<ID3DBlob> byteCode = nullptr;
    ComPtr<ID3DBlob> errors;
    hr = D3DCompileFromFile(prettyPathToShader.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint.c_str(), shaderType.c_str(), compileFlags, 0, &byteCode, &errors);

    std::wstring compilerLog;
    if (errors) 
    {
        const char* ptr = (const char*)errors->GetBufferPointer();
        size_t len = errors->GetBufferSize();
        int need = MultiByteToWideChar(CP_ACP, 0, ptr, (int)len, nullptr, 0);
        compilerLog.resize(need);
        MultiByteToWideChar(CP_ACP, 0, ptr, (int)len, compilerLog.data(), need);
    }

    if (FAILED(hr)) 
    {
        std::wstringstream det;

        std::wstring strCopy = prettyPathToShader;
        for (auto& ch : strCopy) if (ch == L' ') ch = L'\u00A0';
        prettyPathToShader = strCopy;

        det << L"Path to the shader: " << prettyPathToShader << L"\n";
        if (!compilerLog.empty()) det << L"\n" << compilerLog;

        ThrowIfFailed(hr);
    }

    return byteCode;
}