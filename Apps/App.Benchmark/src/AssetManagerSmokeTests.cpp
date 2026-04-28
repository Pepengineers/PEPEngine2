// AssetManagerSmokeTests.cpp

#include <App.Benchmark/AssetManagerSmokeTests.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <windows.h>

#include <Engine.Core/AssetManager.h>

namespace
{
	void OpenConsole()
	{
		// Do nothing if a console is already attached (e.g. launched from a terminal).
		if (GetConsoleWindow() != nullptr)
		{
			return;
		}

		// Allocate a new console window for this process.
		AllocConsole();

		// Redirect stdout, stderr and stdin to the new console window.
		// CONOUT$ and CONIN$ are special Windows device names for the console output and input.
		FILE* fileStream = nullptr;
		freopen_s(&fileStream, "CONOUT$", "w", stdout);
		freopen_s(&fileStream, "CONOUT$", "w", stderr);
		freopen_s(&fileStream, "CONIN$", "r", stdin);

		// Sync C++ streams (std::wcout) with C streams (stdout) after redirection.
		std::ios::sync_with_stdio();

		// Print booleans as "true"/"false" instead of "1"/"0".
		std::wcout << std::boolalpha;
		std::wcerr << std::boolalpha;

		// Print floating point numbers with 3 decimal places in fixed notation (e.g. 1.234).
		std::wcout.precision(3);
		std::wcout << std::fixed;
	}

	std::wstring ToWide(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	void PrintSeparator()
	{
		std::wcout << L"\n============================================================\n";
	}

	void PrintBounds(const wchar_t* label, const DirectX::BoundingBox& bounds)
	{
		std::wcout
			<< label
			<< L" center=("
			<< bounds.Center.x << L", "
			<< bounds.Center.y << L", "
			<< bounds.Center.z << L")"
			<< L", extents=("
			<< bounds.Extents.x << L", "
			<< bounds.Extents.y << L", "
			<< bounds.Extents.z << L")\n";
	}

	void PrintRegistryStats()
	{
		auto& assetManager = Engine::Core::AssetManager::GetInstance();

		std::wcout
			<< L"MeshRegistry: registered=" << assetManager.Meshes().GetRegisteredCount()
			<< L", loaded=" << assetManager.Meshes().GetLoadedCount()
			<< L"\n";

		std::wcout
			<< L"SceneRegistry: registered=" << assetManager.Scenes().GetRegisteredCount()
			<< L", loaded=" << assetManager.Scenes().GetLoadedCount()
			<< L"\n";

		std::wcout
			<< L"TextureRegistry: registered=" << assetManager.Textures().GetRegisteredCount()
			<< L", loaded=" << assetManager.Textures().GetLoadedCount()
			<< L"\n";
	}

	void PrintMeshSummary(const std::filesystem::path& path)
	{
		auto& assetManager = Engine::Core::AssetManager::GetInstance();

		PrintSeparator();
		std::wcout << L"[Mesh Import] " << path.generic_wstring() << L"\n";

		const Engine::Core::Mesh* mesh = assetManager.LoadMesh(path);
		if (mesh == nullptr)
		{
			std::wcout << L"Result: FAILED\n";
			PrintRegistryStats();
			return;
		}

		const Engine::Core::Mesh* cachedMesh = assetManager.LoadMesh(path);

		size_t totalVertices = 0;
		size_t totalIndices = 0;

		const std::vector<Engine::Core::SubMesh>& subMeshes = mesh->GetSubMeshes();
		for (size_t subMeshIndex = 0; subMeshIndex < subMeshes.size(); ++subMeshIndex)
		{
			const Engine::Core::SubMesh& subMesh = subMeshes[subMeshIndex];
			totalVertices += subMesh.GetVertexCount();
			totalIndices += subMesh.GetIndexCount();

			std::wcout
				<< L"SubMesh[" << subMeshIndex << L"]"
				<< L": vertices=" << subMesh.GetVertexCount()
				<< L", indices=" << subMesh.GetIndexCount()
				<< L", material=" << subMesh.MaterialIndex
				<< L", startVertex=" << subMesh.StartVertexLocation
				<< L", startIndex=" << subMesh.StartIndexLocation
				<< L"\n";

			PrintBounds(L"  bounds:", subMesh.Bounds);
		}

		std::wcout << L"Result: SUCCESS\n";
		std::wcout << L"SubMesh count: " << mesh->GetSubMeshCount() << L"\n";
		std::wcout << L"Total vertices: " << totalVertices << L"\n";
		std::wcout << L"Total indices: " << totalIndices << L"\n";
		PrintBounds(L"Mesh bounds:", mesh->GetBounds());
		std::wcout << L"Cache reuse: " << (mesh == cachedMesh) << L"\n";

		PrintRegistryStats();
	}

	void PrintSceneSummary(const std::filesystem::path& path)
	{
		auto& assetManager = Engine::Core::AssetManager::GetInstance();

		PrintSeparator();
		std::wcout << L"[Scene Import] " << path.generic_wstring() << L"\n";

		const Engine::Core::SceneAsset* scene = assetManager.LoadScene(path);
		if (scene == nullptr)
		{
			std::wcout << L"Result: FAILED\n";
			PrintRegistryStats();
			return;
		}

		const Engine::Core::SceneAsset* cachedScene = assetManager.LoadScene(path);

		size_t nodesWithMeshes = 0;
		size_t totalMeshRefs = 0;
		std::set<std::uint32_t> uniqueMeshHandleValues;

		for (size_t nodeIndex = 0; nodeIndex < scene->GetNodeCount(); ++nodeIndex)
		{
			const Engine::Core::SceneNode& node = scene->GetNode(nodeIndex);

			if (node.HasMeshes())
			{
				++nodesWithMeshes;
			}

			totalMeshRefs += node.Meshes.size();

			for (Engine::Core::MeshHandle meshHandle : node.Meshes)
			{
				if (meshHandle.IsValid())
				{
					uniqueMeshHandleValues.insert(meshHandle.GetValue());
				}
			}

			std::wcout
				<< L"Node[" << nodeIndex << L"]"
				<< L": name=\"" << ToWide(node.Name) << L"\""
				<< L", parent=" << node.ParentIndex
				<< L", children=" << node.ChildrenIndices.size()
				<< L", meshes=" << node.Meshes.size()
				<< L"\n";

			std::wcout
				<< L"  localTranslation=("
				<< node.LocalTranslation.x << L", "
				<< node.LocalTranslation.y << L", "
				<< node.LocalTranslation.z << L")\n";

			std::wcout
				<< L"  localRotation=("
				<< node.LocalRotation.x << L", "
				<< node.LocalRotation.y << L", "
				<< node.LocalRotation.z << L", "
				<< node.LocalRotation.w << L")\n";

			std::wcout
				<< L"  localScale=("
				<< node.LocalScale.x << L", "
				<< node.LocalScale.y << L", "
				<< node.LocalScale.z << L")\n";
		}

		std::wcout << L"Result: SUCCESS\n";
		std::wcout << L"Node count: " << scene->GetNodeCount() << L"\n";
		std::wcout << L"Root node count: " << scene->GetRootNodeCount() << L"\n";
		std::wcout << L"Nodes with meshes: " << nodesWithMeshes << L"\n";
		std::wcout << L"Total mesh references: " << totalMeshRefs << L"\n";
		std::wcout << L"Unique mesh handles: " << uniqueMeshHandleValues.size() << L"\n";
		std::wcout << L"Cache reuse: " << (scene == cachedScene) << L"\n";

		for (std::uint32_t handleValue : uniqueMeshHandleValues)
		{
			const Engine::Core::MeshHandle meshHandle(handleValue);

			const Engine::Core::MeshAssetLocator locator = assetManager.Meshes().GetLocator(meshHandle);
			const Engine::Core::Mesh* mesh = assetManager.Meshes().GetMesh(meshHandle);

			std::wcout << L"MeshHandle[" << handleValue << L"]";
			std::wcout << L": path=" << locator.SourcePath.generic_wstring();

			if (locator.HasSubAssetIndex())
			{
				std::wcout << L", subAssetIndex=" << locator.SubAssetIndex;
			}
			else
			{
				std::wcout << L", subAssetIndex=<none>";
			}

			if (mesh != nullptr)
			{
				std::wcout << L", subMeshCount=" << mesh->GetSubMeshCount() << L"\n";
				PrintBounds(L"  bounds:", mesh->GetBounds());
			}
			else
			{
				std::wcout << L", meshData=<null>\n";
			}
		}

		PrintRegistryStats();
	}

	const wchar_t* TextureDimensionToString(const Engine::Core::ETextureDimension dimension)
	{
		switch (dimension)
		{
		case Engine::Core::ETextureDimension::Texture1D:
			return L"Texture1D";

		case Engine::Core::ETextureDimension::Texture2D:
			return L"Texture2D";

		case Engine::Core::ETextureDimension::Texture3D:
			return L"Texture3D";

		case Engine::Core::ETextureDimension::TextureCube:
			return L"TextureCube";

		default:
			return L"Unknown";
		}
	}

	void PrintTextureSummary(const std::filesystem::path& path)
	{
		auto& assetManager = Engine::Core::AssetManager::GetInstance();

		PrintSeparator();
		std::wcout << L"[Texture Import] " << path.generic_wstring() << L"\n";

		const Engine::Core::Texture* texture = assetManager.LoadTexture(path);
		if (texture == nullptr)
		{
			std::wcout << L"Result: FAILED\n";
			PrintRegistryStats();
			return;
		}

		const Engine::Core::Texture* cachedTexture = assetManager.LoadTexture(path);
		const Engine::Core::Texture* fallbackTexture = assetManager.LoadTextureOrDefault(path);

		std::wcout << L"Result: SUCCESS\n";
		std::wcout << L"Dimension: " << TextureDimensionToString(texture->GetDimension()) << L"\n";
		std::wcout << L"Format: " << static_cast<int>(texture->GetFormat()) << L"\n";
		std::wcout << L"Width: " << texture->GetWidth() << L"\n";
		std::wcout << L"Height: " << texture->GetHeight() << L"\n";
		std::wcout << L"Depth: " << texture->GetDepth() << L"\n";
		std::wcout << L"Array size: " << texture->GetArraySize() << L"\n";
		std::wcout << L"Mip levels: " << texture->GetMipLevels() << L"\n";
		std::wcout << L"Cube map: " << texture->IsCubeMap() << L"\n";
		std::wcout << L"Subresource count: " << texture->GetSubresourceCount() << L"\n";
		std::wcout << L"Cache reuse: " << (texture == cachedTexture) << L"\n";
		std::wcout << L"Fallback path returns same texture: " << (texture == fallbackTexture) << L"\n";

		if (texture->GetSubresourceCount() > 0)
		{
			const Engine::Core::SubTexture& firstSubresource = texture->GetSubresource(0, 0);

			std::wcout << L"Subresource[0].Width: " << firstSubresource.Width << L"\n";
			std::wcout << L"Subresource[0].Height: " << firstSubresource.Height << L"\n";
			std::wcout << L"Subresource[0].RowPitch: " << firstSubresource.RowPitch << L"\n";
			std::wcout << L"Subresource[0].SlicePitch: " << firstSubresource.SlicePitch << L"\n";
			std::wcout << L"Subresource[0].DataSize: " << firstSubresource.Data.size() << L"\n";
		}

		PrintRegistryStats();
	}

	void PrintMissingTextureSummary(const std::filesystem::path& path)
	{
		auto& assetManager = Engine::Core::AssetManager::GetInstance();

		PrintSeparator();
		std::wcout << L"[Missing Texture] " << path.generic_wstring() << L"\n";

		const Engine::Core::Texture* strictTexture = assetManager.LoadTexture(path);
		const Engine::Core::Texture* fallbackTexture = assetManager.LoadTextureOrDefault(path);
		const Engine::Core::Texture* defaultTexture = assetManager.Textures().GetDefaultTexture();

		std::wcout << L"Strict load returned nullptr: " << (strictTexture == nullptr) << L"\n";
		std::wcout << L"Fallback returned nullptr: " << (fallbackTexture == nullptr) << L"\n";
		std::wcout << L"Fallback returned default texture: " << (fallbackTexture == defaultTexture) << L"\n";

		if (fallbackTexture != nullptr)
		{
			std::wcout << L"Default dimension: " << TextureDimensionToString(fallbackTexture->GetDimension()) << L"\n";
			std::wcout << L"Default format: " << static_cast<int>(fallbackTexture->GetFormat()) << L"\n";
			std::wcout << L"Default width: " << fallbackTexture->GetWidth() << L"\n";
			std::wcout << L"Default height: " << fallbackTexture->GetHeight() << L"\n";
			std::wcout << L"Default subresource count: " << fallbackTexture->GetSubresourceCount() << L"\n";
		}

		PrintRegistryStats();
	}

	void RunTextureImportSmokeTest()
	{
		const std::filesystem::path ddsPath = L"checkboard.dds";
		const std::filesystem::path pngPath = L"test_texture.png";
		const std::filesystem::path jpgPath2 = L"enot.jpg";
		const std::filesystem::path jpegPath = L"test_texture2.jpeg";
		const std::filesystem::path jpgPath = L"test_texture3.jpg";
		const std::filesystem::path missingPath = L"definitely_missing_texture.png";

		std::wcout << L"\nTexture import smoke test started.\n";
		std::wcout << L"Textures are resolved relative to Assets/Textures.\n";

		PrintTextureSummary(ddsPath);
		PrintTextureSummary(pngPath);
		PrintTextureSummary(jpgPath2);
		PrintTextureSummary(jpegPath);
		PrintTextureSummary(jpgPath);
		PrintMissingTextureSummary(missingPath);

		PrintSeparator();
		std::wcout << L"Texture import smoke test finished.\n";
	}

	void RunAssetImportSmokeTest()
	{
		const std::filesystem::path africanHeadPath = L"african_head.obj";
		const std::filesystem::path svidetelPath = L"Svidetel.fbx";
		const std::filesystem::path lowPolyCarPath = L"lowpolycar/source/low_poly_car_-_chevrolet_c10_pickup_1963.glb";

		std::wcout << L"Asset import smoke test started.\n";
		std::wcout << L"Models are resolved relative to Assets/Models.\n";

		PrintMeshSummary(africanHeadPath);
		PrintSceneSummary(africanHeadPath);

		PrintMeshSummary(svidetelPath);
		PrintSceneSummary(svidetelPath);

		PrintMeshSummary(lowPolyCarPath);
		PrintSceneSummary(lowPolyCarPath);

		PrintSeparator();
		std::wcout << L"Asset import smoke test finished.\n";
	}
}

void RunAssetManagerSmokeTests()
{
	OpenConsole();
	RunAssetImportSmokeTest();
	RunTextureImportSmokeTest();

	std::wcout << L"\nPress Enter to close...\n";
	std::wstring line;
	std::getline(std::wcin, line);
}