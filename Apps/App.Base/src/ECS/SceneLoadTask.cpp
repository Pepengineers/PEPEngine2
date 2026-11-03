#include "App.Base/ECS/SceneLoadTask.h"
#include "App.Base/ECS/PreparedScene.h"

SceneLoadTask::SceneLoadTask(std::string const& scenePath, AppConfig const& config,
    SceneManagerModule& sceneManagerModule, RenderModule& renderModule)
        : _scenePath(scenePath),
        _config(config),
        _sceneManagerModule(&sceneManagerModule),
        _renderModule(&renderModule)
{
}

SceneLoadTask::~SceneLoadTask()
{
    Cancel();
    
    if (_preparedFuture.valid())
    {
        _preparedFuture.wait();
    }
}

void SceneLoadTask::Start()
{
    _cancelRequested.store(false, std::memory_order_release);
    SetStatus(0.0f, "Preparing scene");
    
    _preparedFuture = std::async(std::launch::async, [this]()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        return WorldLoader::PrepareScene(_scenePath, [this](float progress, const std::string& status)
        {
            //cpu preparation is only the first 75%
            SetStatus(progress * 0.75f, status);
        },
        [this]() noexcept
        {
            return _cancelRequested.load(std::memory_order_acquire);
        });
    });
}

void SceneLoadTask::Cancel() noexcept
{
    _cancelRequested.store(true, std::memory_order_release);
}

void SceneLoadTask::TickMainThread(std::chrono::milliseconds const& budget)
{
    if (_complete.load() || _failed.load())
    {
        return;
    }
    
    if (!_committer)
    {
        if (!_preparedFuture.valid() || _preparedFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            return;
        }
        
        try
        {
            std::unique_ptr<PreparedScene> prepared = _preparedFuture.get();
            if (!prepared)
            {
                Fail("CPU scene preparation failed");
                return;
            }
            
            _committer = WorldLoader::Committer::Create(std::move(prepared), *_sceneManagerModule, *_renderModule, _config);
            
            if (!_committer)
            {
                Fail("Could not create the scene committer");
                return;
            }
        }
        catch (const std::exception& exception)
        {
            Fail(exception.what());
            return;
        }
        catch (...)
        {
            Fail("Unknown exception during scene preparation");
            return;
        }
    }

    try
    {
        if (!_committer->Tick(budget) || _committer->HasFailed())
        {
            Fail(_committer->GetError());
            return;
        }
        
        SetStatus(0.75f + _committer->GetProgress() * 0.25f, _committer->GetStatus());
        
        if (_committer->IsComplete())
        {
            SetStatus(1.0f, "Complete");
            _complete.store(true);
        }
    }
    catch (const std::exception& exception)
    {
        Fail(exception.what());
    }
    catch (...)
    {
        Fail("Unknown exception while committing the scene");
    }
}

bool SceneLoadTask::IsComplete() const
{
    return _complete.load();
}

bool SceneLoadTask::HasFailed() const
{
    return _failed.load();
}

float SceneLoadTask::GetProgress() const
{
    return _progress.load();
}

std::string SceneLoadTask::GetStatus() const
{
    std::lock_guard<std::mutex> lock(_messageMutex);
    return _status;
}

std::string SceneLoadTask::GetError() const
{
    std::lock_guard<std::mutex> lock(_messageMutex);
    return _error;
}

void SceneLoadTask::SetStatus(float progress, std::string const& status)
{
    _progress.store((std::max)(0.0f, (std::min)(progress, 1.0f)));
    
    std::lock_guard<std::mutex> lock(_messageMutex);
    _status = status;
}

void SceneLoadTask::Fail(const std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(_messageMutex);
        _error = error;
        _status = "Failed";
    }
    
    _failed.store(true);
}
