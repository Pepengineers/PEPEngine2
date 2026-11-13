#include "CrossGPUBenchmark.h"

#include <Engine.RendererDX12/GDX12DeviceFactory.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include <ryml.hpp>
#include <ryml_std.hpp>

EDeviceRole ConsumerRole(const CrossGPUStage& stage)
{
    return stage.Producer == DEVICE_ROLE_PRIMARY ? DEVICE_ROLE_SECONDARY : DEVICE_ROLE_PRIMARY;
}

void AddCrossGPUStages(std::vector<CrossGPUStage>& stages, EDeviceRole producer, UINT size, UINT reads,
    const std::string& workload)
{
    const auto prefix = std::string(producer == DEVICE_ROLE_PRIMARY ? "primary_to_secondary_" : "secondary_to_primary_") + workload;
    for (const auto queue : {TransportQueuePolicy::ForceDirect, TransportQueuePolicy::ForceCopy})
    {
        for (const auto [storage, access] : {
            std::pair{MemoryType::Local, MemoryType::Local},
            std::pair{MemoryType::Shared, MemoryType::Local},
            std::pair{MemoryType::Shared, MemoryType::Shared},
            std::pair{MemoryType::Local, MemoryType::Shared}})
        {
            const bool noCopies = storage == MemoryType::Shared && access == MemoryType::Shared;
            if (noCopies && queue == TransportQueuePolicy::ForceCopy) { continue; }
            auto name = prefix + '_' + (storage == MemoryType::Local ? 'L' : 'S') + (access == MemoryType::Local ? 'L' : 'S');
            if (!noCopies) { name += queue == TransportQueuePolicy::ForceDirect ? "_direct" : "_copy"; }
            stages.push_back({std::move(name), size, reads, producer, storage, access, queue});
        }
    }
}

namespace
{
    constexpr auto StageDuration = std::chrono::seconds(7);
    constexpr float WarmupSeconds = 1.0f;
    constexpr unsigned FrameSlots = 3;

    class ClearAndPresentPass final : public GDX12RenderPass
    {
    public:
        void Describe(PassDescription& description) const override
        {
            auto& input = description.AddInputTexture("BackBuffer");
            input.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
            input.Access = DataAccess::WriteOnly;
            input.Usages = ResourceUsage::RenderTarget;
            auto& output = description.AddOutputTexture("Presented");
            output.AliasInput = "BackBuffer";
            output.Usages = ResourceUsage::Present;
        }

        void Execute(PassExecutionContext& context) override
        {
            context.GetCommandList()->ClearRenderTargetView(context.GetInputTexture("BackBuffer").GetTexture());
        }
    };

    struct AdapterCapabilities
    {
        std::string Name;
        bool RowMajorTextures;
    };

    struct StageResult
    {
        const char* Status = "pending";
        std::string Reason;
        std::string Report;
    };

    void WriteString(ryml::NodeRef node, const std::string& value)
    {
        node << value;
        node |= ryml::VAL_DQUO;
    }

    std::string UTF8(const std::wstring& value)
    {
        if (value.empty()) { return {}; }
        const auto length = static_cast<int>(value.size());
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
        std::string result(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(), bytes, nullptr, nullptr);
        return result;
    }

    class CrossGPUApp final : public App
    {
    public:
        CrossGPUApp(HINSTANCE instance, const char* name, bool textures, std::vector<CrossGPUStage> stages,
            ConfigureCrossGPUWorkload configure)
            : App(instance), _name(name), _textures(textures), _stages(std::move(stages)),
              _configure(configure), _results(_stages.size())
        {
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto folder = _name + "-" + std::to_string(timestamp) + "-" + std::to_string(GetCurrentProcessId());
            _directory = std::filesystem::path(SOLUTION_DIR) / "Reports" / folder;
        }

        ~CrossGPUApp() override
        {
            if (_listener) { _render->OnBenchmarkRunCompleted().RemoveListener(_listener); }
        }

        bool Initialize() override
        {
            if (!App::Initialize()) { return false; }
            _render = GetLocator().GetModule<RenderModule>().get();
            for (const auto& adapter : GDX12DeviceFactory::GetDeviceDescriptors())
            {
                ComPtr<ID3D12Device> device;
                ThrowIfFailed(D3D12CreateDevice(adapter.Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
                D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
                ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
                _adapters.push_back({UTF8(adapter.Name), options.CrossAdapterRowMajorTextureSupported != FALSE});
                if (_adapters.size() == 2) { break; }
            }

            std::filesystem::create_directories(_directory);
            _listener = _render->OnBenchmarkRunCompleted().AddListener([this]() { CompleteStage(); });
            StartStage();
            return true;
        }

        void RecordFailure(const std::string& reason)
        {
            std::fprintf(stderr, "%s\n", reason.c_str());
            OutputDebugStringA((reason + '\n').c_str());
            if (_stageIndex < _results.size())
            { _results[_stageIndex] = {"failed", reason, {}}; }
            _exitCode = 1;
            SaveSummary();
        }

        bool HasPendingStage() const { return _stageIndex < _stages.size(); }

    protected:
        bool AddModules() override
        {
            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
            return BenchmarkEngine::AddModules();
        }

        void Update(const GameTimer& timer) override
        {
            BenchmarkEngine::Update(timer);
            if (!_verified || _measuring) { return; }
            if (_verifiedAt < 0.0f) { _verifiedAt = timer.TotalTime(); }
            if (timer.TotalTime() - _verifiedAt < WarmupSeconds) { return; }
            _render->StartBenchmarkRun(_stages[_stageIndex].Name.c_str(), StageDuration, _directory);
            _measuring = true;
        }

    private:
        static void ConfigurePipeline(FrameGraph& graph, const FrameGraphInputs& inputs)
        {
            auto& app = *static_cast<CrossGPUApp*>(App::GetInstance());
            const auto& stage = app._stages[app._stageIndex];
            graph.SetFrameSlotResourcesCount(FrameSlots);
            graph.SetTransportQueuePolicy(stage.Queue);
            const auto read = app._configure(graph, stage, app._verified);
            const auto present = AddBenchmarkPass<ClearAndPresentPass>(graph, ConsumerRole(stage), "Present");
            graph.ConnectNodes(inputs.BackBuffer, present.GetInput("BackBuffer"));
            graph.AddExecutionDependency(read, present);
        }

        const AdapterCapabilities& Adapter(EDeviceRole role) const
        {
            return _adapters[role == DEVICE_ROLE_PRIMARY ? 0 : 1];
        }

        std::string SkipReason(const CrossGPUStage& stage) const
        {
            if (_adapters.size() < 2) { return "Two hardware adapters are required."; }
            if (!_textures) { return {}; }
            const bool producerSupported = Adapter(stage.Producer).RowMajorTextures;
            const bool consumerSupported = Adapter(ConsumerRole(stage)).RowMajorTextures;
            if (stage.Storage == MemoryType::Shared && (!producerSupported || !consumerSupported))
            { return "A shared ROW_MAJOR UAV texture requires both adapters to accept its resource flags."; }
            if (stage.Access == MemoryType::Shared && !consumerSupported)
            { return "The consumer cannot bind a shared ROW_MAJOR texture as an SRV."; }
            return {};
        }

        void StartStage()
        {
            _verified = false;
            _verifiedAt = -1.0f;
            _measuring = false;
            while (HasPendingStage())
            {
                const auto& stage = _stages[_stageIndex];
                auto reason = SkipReason(stage);
                if (reason.empty())
                {
                    _results[_stageIndex].Status = "running";
                    SaveSummary();
                    std::fprintf(stderr, "Starting %s\n", stage.Name.c_str());
                    try
                    {
                        _render->SetGraphicsPipeline(ConfigurePipeline);
                        return;
                    }
                    catch (const FrameGraphSharedResourceUnsupported& error)
                    {
                        reason = error.what();
                    }
                }
                std::fprintf(stderr, "Skipping %s: %s\n", stage.Name.c_str(), reason.c_str());
                _results[_stageIndex++] = {"skipped", std::move(reason), {}};
            }
            SaveSummary();
            PostQuitMessage(_exitCode);
        }

        void CompleteStage()
        {
            const auto& report = _render->GetLastBenchmarkReportPath();
            if (report.empty())
            {
                _results[_stageIndex] = {"failed", "Unable to save benchmarking report.", {}};
                _exitCode = 1;
            }
            else { _results[_stageIndex] = {"completed", {}, report.filename().string()}; }
            ++_stageIndex;
            SaveSummary();
            StartStage();
        }

        void SaveSummary() const
        {
            std::filesystem::create_directories(_directory);
            ryml::Tree tree;
            auto root = tree.rootref();
            root |= ryml::MAP;
            WriteString(root["benchmark"], _name);
            root["frame_slots"] << FrameSlots;
            root["stage_seconds"] << StageDuration.count();
            auto adapters = root["adapters"];
            adapters |= ryml::SEQ;
            for (size_t i = 0; i < _adapters.size(); ++i)
            {
                auto node = adapters.append_child();
                node |= ryml::MAP;
                node["role"] = i == 0 ? "primary" : "secondary";
                WriteString(node["name"], _adapters[i].Name);
                node["cross_adapter_row_major_texture_supported"] = _adapters[i].RowMajorTextures ? "true" : "false";
            }
            auto stages = root["stages"];
            stages |= ryml::SEQ;
            for (size_t i = 0; i < _stages.size(); ++i)
            {
                const auto& stage = _stages[i];
                const auto& result = _results[i];
                auto node = stages.append_child();
                node |= ryml::MAP;
                WriteString(node["name"], stage.Name);
                node["status"] = result.Status;
                node["producer"] = stage.Producer == DEVICE_ROLE_PRIMARY ? "primary" : "secondary";
                node["consumer"] = ConsumerRole(stage) == DEVICE_ROLE_PRIMARY ? "primary" : "secondary";
                node[_textures ? "texture_side" : "buffer_mib"] << stage.Size;
                node["reads"] << stage.Reads;
                node["storage"] = GetMemoryTypeName(stage.Storage);
                node["access"] = GetMemoryTypeName(stage.Access);
                const bool localToLocal = stage.Storage == MemoryType::Local && stage.Access == MemoryType::Local;
                node["transport"] = !_textures ? "buffer" : localToLocal ? "footprint_buffer" : "row_major_texture";
                if (stage.Storage == MemoryType::Shared && stage.Access == MemoryType::Shared)
                { node["copy_queue"] << nullptr; }
                else { node["copy_queue"] = stage.Queue == TransportQueuePolicy::ForceDirect ? "DIRECT" : "COPY"; }
                if (_textures && _adapters.size() == 2)
                {
                    std::string pair = Adapter(stage.Producer).RowMajorTextures ? "1/" : "0/";
                    pair += Adapter(ConsumerRole(stage)).RowMajorTextures ? '1' : '0';
                    WriteString(node["capability_pair"], pair);
                }
                if (!result.Reason.empty()) { WriteString(node["reason"], result.Reason); }
                if (!result.Report.empty()) { WriteString(node["report"], result.Report); }
            }
            const auto path = _directory / "run-summary.json";
            const auto temporary = _directory / "run-summary.json.tmp";
            std::ofstream stream;
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            stream.open(temporary, std::ios::binary);
            stream << ryml::emitrs_json<std::string>(tree) << '\n';
            stream.close();
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            { throw std::runtime_error("Unable to publish benchmark summary."); }
        }

        std::string _name;
        bool _textures;
        std::vector<CrossGPUStage> _stages;
        ConfigureCrossGPUWorkload _configure;
        std::vector<StageResult> _results;
        std::vector<AdapterCapabilities> _adapters;
        std::filesystem::path _directory;
        RenderModule* _render = nullptr;
        ListenerHandle _listener = 0;
        size_t _stageIndex = 0;
        bool _verified = false;
        float _verifiedAt = -1.0f;
        bool _measuring = false;
        int _exitCode = 0;
    };
}

int RunCrossGPUBenchmark(HINSTANCE instance, const char* name, bool textures,
    std::vector<CrossGPUStage> stages, ConfigureCrossGPUWorkload configure, const char* stagePrefix)
{
    // An optional stage name or prefix lets a failed stage be reproduced in isolation.
    if (*stagePrefix)
    {
        stages.erase(std::remove_if(stages.begin(), stages.end(), [stagePrefix](const CrossGPUStage& stage)
            { return stage.Name.rfind(stagePrefix, 0) != 0; }), stages.end());
        if (stages.empty())
        {
            OutputDebugStringA("ERROR: No benchmark stages match the requested prefix.\n");
            return 1;
        }
    }
    CrossGPUApp app(instance, name, textures, std::move(stages), configure);
    std::string failure;
    try
    {
        if (!app.Initialize()) { throw std::runtime_error("Unable to initialize benchmark application."); }
        const int result = app.Run();
        if (!app.HasPendingStage()) { return result; }
        failure = "Benchmark stopped before the active stage completed.";
    }
    catch (const DxException& error) { failure = UTF8(error.ToString()); }
    catch (const std::exception& error) { failure = error.what(); }
    try { app.RecordFailure(failure); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); }
    return 1;
}
