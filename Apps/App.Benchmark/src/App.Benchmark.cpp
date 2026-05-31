// App.Benchmark.cpp

#include <Engine.Core/BenchmarkEngine.h>
#include <App.Base/App.h>

#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/Registries/TextureRegistry.h>

namespace
{
	bool CheckOrReport(const bool condition, const std::wstring& message)
	{
		if (condition)
		{
			return true;
		}

		OutputDebugStringW((L"[AllocatorTest] FAILED: " + message + L"\n").c_str());
		MessageBoxW(nullptr, message.c_str(), L"Allocator test failed", MB_OK | MB_ICONERROR);
		assert(false);
		return false;
	}

	bool RunTextureRegistryAllocatorTest()
	{
		Engine::Core::TextureRegistry registry;

		const std::filesystem::path pathA = L"allocator_test_a.png";
		const std::filesystem::path pathB = L"allocator_test_b.png";

		const Engine::Core::TextureHandle handleA = registry.Register(pathA);
		if (!CheckOrReport(handleA.IsValid(), L"TextureRegistry::Register(pathA) must return a valid handle.")) return false;
		if (!CheckOrReport(registry.GetRegisteredCount() == 1, L"After Register(pathA), registered count must be 1.")) return false;
		if (!CheckOrReport(registry.FindHandle(pathA) == handleA, L"FindHandle(pathA) must return the same handle.")) return false;

		const Engine::Core::Texture* cachedA = registry.Cache(handleA, std::make_unique<Engine::Core::Texture>());
		if (!CheckOrReport(cachedA != nullptr, L"Cache(handleA, texture) must return a non-null pointer.")) return false;
		if (!CheckOrReport(registry.GetTexture(handleA) == cachedA, L"GetTexture(handleA) must return the cached texture.")) return false;
		if (!CheckOrReport(registry.GetLoadedCount() == 1, L"After caching texture A, loaded count must be 1.")) return false;

		registry.Unload(handleA);
		if (!CheckOrReport(registry.GetTexture(handleA) == nullptr, L"After Unload(handleA), texture data must be gone.")) return false;
		if (!CheckOrReport(registry.FindHandle(pathA) == handleA, L"Unload(handleA) must not unregister the asset.")) return false;
		if (!CheckOrReport(registry.GetRegisteredCount() == 1, L"After Unload(handleA), registered count must stay 1.")) return false;
		if (!CheckOrReport(registry.GetLoadedCount() == 0, L"After Unload(handleA), loaded count must become 0.")) return false;

		if (!CheckOrReport(registry.Unregister(handleA), L"Unregister(handleA) must succeed.")) return false;
		if (!CheckOrReport(!registry.FindHandle(pathA).IsValid(), L"After Unregister(handleA), pathA must no longer be registered.")) return false;
		if (!CheckOrReport(registry.GetRegisteredCount() == 0, L"After Unregister(handleA), registered count must become 0.")) return false;
		if (!CheckOrReport(registry.GetTexture(handleA) == nullptr, L"Old handleA must no longer resolve after unregister.")) return false;

		const Engine::Core::TextureHandle handleB = registry.Register(pathB);
		if (!CheckOrReport(handleB.IsValid(), L"TextureRegistry::Register(pathB) must return a valid handle.")) return false;
		if (!CheckOrReport(handleB.GetValue() == handleA.GetValue(), L"Freed texture slot must be reused.")) return false;
		if (!CheckOrReport(handleB.GetGeneration() == handleA.GetGeneration() + 1, L"Reused texture slot must increment generation.")) return false;
		if (!CheckOrReport(handleB != handleA, L"Reused handle must differ from stale handle because generation changed.")) return false;
		if (!CheckOrReport(registry.FindHandle(pathB) == handleB, L"FindHandle(pathB) must return the reused handle with new generation.")) return false;

		const Engine::Core::Texture* cachedB = registry.Cache(handleB, std::make_unique<Engine::Core::Texture>());
		if (!CheckOrReport(cachedB != nullptr, L"Cache(handleB, texture) must return a non-null pointer.")) return false;
		if (!CheckOrReport(registry.GetTexture(handleB) == cachedB, L"GetTexture(handleB) must return the new cached texture.")) return false;
		if (!CheckOrReport(registry.GetTexture(handleA) == nullptr, L"Stale texture handleA must stay invalid after slot reuse.")) return false;

		return true;
	}

	bool RunMeshRegistryAllocatorTest()
	{
		Engine::Core::MeshRegistry registry;

		Engine::Core::MeshAssetLocator locator0 = {};
		locator0.SourcePath = L"allocator_test_mesh.fbx";
		locator0.SubAssetIndex = 0;

		Engine::Core::MeshAssetLocator locator1 = {};
		locator1.SourcePath = L"allocator_test_mesh.fbx";
		locator1.SubAssetIndex = 1;

		const Engine::Core::MeshHandle handle0 = registry.Register(locator0);
		if (!CheckOrReport(handle0.IsValid(), L"MeshRegistry::Register(locator0) must return a valid handle.")) return false;

		const Engine::Core::MeshHandle handle1 = registry.Register(locator1);
		if (!CheckOrReport(handle1.IsValid(), L"MeshRegistry::Register(locator1) must return a valid handle.")) return false;
		if (!CheckOrReport(handle1 != handle0, L"Different mesh sub-assets must get different handles.")) return false;

		if (!CheckOrReport(registry.FindHandle(locator0) == handle0, L"FindHandle(locator0) must return handle0.")) return false;
		if (!CheckOrReport(registry.FindHandle(locator1) == handle1, L"FindHandle(locator1) must return handle1.")) return false;

		if (!CheckOrReport(registry.Unregister(handle0), L"Unregister(handle0) must succeed.")) return false;

		const Engine::Core::MeshHandle handle0b = registry.Register(locator0);
		if (!CheckOrReport(handle0b.IsValid(), L"Re-registering locator0 must return a valid handle.")) return false;
		if (!CheckOrReport(handle0b.GetValue() == handle0.GetValue(), L"Freed mesh slot must be reused.")) return false;
		if (!CheckOrReport(handle0b.GetGeneration() == handle0.GetGeneration() + 1, L"Reused mesh slot must increment generation.")) return false;
		if (!CheckOrReport(handle0b != handle0, L"Reused mesh handle must differ from stale mesh handle.")) return false;

		const Engine::Core::Mesh* cachedMesh = registry.Cache(handle0b, std::make_unique<Engine::Core::Mesh>());
		if (!CheckOrReport(cachedMesh != nullptr, L"Cache(handle0b, mesh) must return a non-null pointer.")) return false;
		if (!CheckOrReport(registry.GetMesh(handle0b) == cachedMesh, L"GetMesh(handle0b) must return the new cached mesh.")) return false;
		if (!CheckOrReport(registry.GetMesh(handle0) == nullptr, L"Stale mesh handle0 must stay invalid after slot reuse.")) return false;

		return true;
	}

	void RunAllocatorSmokeTests()
	{
		if (!RunTextureRegistryAllocatorTest())
		{
			return;
		}

		if (!RunMeshRegistryAllocatorTest())
		{
			return;
		}

		OutputDebugStringW(L"[AllocatorTest] All allocator/generation tests passed.\n");
	}
}

class BenchmarkApp final : public App
{
public:
    BenchmarkApp(HINSTANCE hInstance);
    BenchmarkApp(const BenchmarkApp& rhs) = delete;
    BenchmarkApp& operator =(const BenchmarkApp& rhs) = delete;
    ~BenchmarkApp() override;

    bool Initialize() override;

protected:
    void OnResize() override;
    void Update(const GameTimer& gameTimer) override;
    void Render(const GameTimer& gameTimer) override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    UNREFERENCED_PARAMETER(prevInstance);
    UNREFERENCED_PARAMETER(cmdLine);
    UNREFERENCED_PARAMETER(showCmd);

    try
    {
        BenchmarkApp TheApp(hInstance);
        if (!TheApp.Initialize())
        {
            return 0;
        }

        return TheApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"App Init Failed", MB_OK);
        return 0;
    }
}

BenchmarkApp::BenchmarkApp(HINSTANCE hInstance)
    : App(hInstance)
{
}

BenchmarkApp::~BenchmarkApp()
= default;

bool BenchmarkApp::Initialize()
{
    if (!App::Initialize())
    {
        return false;
    }

#if defined(DEBUG) || defined(_DEBUG)
	RunAllocatorSmokeTests();
#endif

    return true;
}

void BenchmarkApp::OnResize()
{
    App::OnResize();
}

void BenchmarkApp::Update(const GameTimer& gameTimer)
{
    App::Update(gameTimer);
}

void BenchmarkApp::Render(const GameTimer& gameTimer)
{
    App::Render(gameTimer);
}

void BenchmarkApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    SetCapture(GetWindow()->GetWindowHandle());
}

void BenchmarkApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    ReleaseCapture();
}

void BenchmarkApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
}