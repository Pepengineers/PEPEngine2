#pragma once
#include <chrono>
#include <future>
#include <mutex>
#include <string>

#include "AppConfig.h"
#include "WorldLoader.h"

class WorldLoader;
class RenderModule;
class SceneManagerModule;

class SceneLoadTask
{
public:
    SceneLoadTask(std::string const& scenePath, AppConfig const& config, SceneManagerModule& sceneManagerModule, RenderModule& renderModule);
    
    ~SceneLoadTask();
    
    void Start();
    void Cancel() noexcept;
    void TickMainThread(std::chrono::milliseconds const& budget);
    
    bool IsComplete() const;
    bool HasFailed() const;
    float GetProgress() const;
    
    std::string GetStatus() const;
    std::string GetError() const;
    
private:
    void SetStatus(float progress, std::string const& status);
    void Fail(const std::string& error);
    
    std::string _scenePath;
    AppConfig _config;
    
    SceneManagerModule* _sceneManagerModule = nullptr;
    RenderModule* _renderModule = nullptr;
    
    std::atomic<bool> _cancelRequested{false};
    std::atomic<float> _progress{0.0f};
    std::atomic<bool> _complete{false};
    std::atomic<bool> _failed{false};
    
    mutable std::mutex _messageMutex;
    std::string _status = "Waiting";
    std::string _error;
    
    std::unique_ptr<WorldLoader::Committer> _committer;
    
    std::future<std::unique_ptr<PreparedScene>> _preparedFuture;
};
