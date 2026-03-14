#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Device;

struct IDxcCompiler3;
struct IDxcUtils;
struct IDxcIncludeHandler;

class GDX12ShaderCompiler
{
public:
	~GDX12ShaderCompiler();

	//singleton instance
	static GDX12ShaderCompiler& GetInstance();

	static void Shutdown();

	void Initialize();

	ComPtr<ID3DBlob> CompileShader(GDX12Device* device, const std::wstring& filename, const D3D_SHADER_MACRO* defines, 
		const std::string& entrypoint, const std::string& shaderType);

private:
	GDX12ShaderCompiler();

	std::string GetShaderTargetForModel(GDX12Device* device, const std::string& shaderType);

	ComPtr<ID3DBlob> CompileShaderDXC(const std::wstring& filename, const D3D_SHADER_MACRO* defines,
		const std::string& entrypoint, const std::string& shaderType);

	ComPtr<ID3DBlob> CompileShaderFXC(const std::wstring& filename, const D3D_SHADER_MACRO* defines,
		const std::string& entrypoint, const std::string& shaderType);

	static GDX12ShaderCompiler* _instance;

	ComPtr<IDxcCompiler3> _dxcCompiler;
	ComPtr<IDxcUtils> _dxcUtils;
	ComPtr<IDxcIncludeHandler> _dxcIncludeHandler;
};