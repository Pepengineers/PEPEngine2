#pragma once
#include <filesystem>
#include <functional>

struct PreparedScene;
struct AppConfig;
class RenderModule;
class SceneManagerModule;
class World;

using SceneLoadProgressCallback = std::function<void(float, const std::string&)>;
using SceneLoadCancellationCallback = std::function<bool()>;

class WorldLoader
{
public:
    static std::unique_ptr<PreparedScene> PrepareScene(const std::string& scenePath, 
                                                        SceneLoadProgressCallback progressCallback, 
                                                        SceneLoadCancellationCallback cancellationCallback = {});
    
    class Committer
    {
    public:
        static std::unique_ptr<Committer> Create(std::unique_ptr<PreparedScene> preparedScene, 
                                                    SceneManagerModule& sceneManagerModule, 
                                                    RenderModule& renderModule,
                                                    const AppConfig& config);
        ~Committer();
        
        Committer(Committer&&) noexcept;
        Committer& operator=(Committer&&) noexcept;
        
        Committer(const Committer&) = delete;
        Committer& operator=(const Committer&) = delete;
        
        bool Tick(std::chrono::milliseconds budget);
        
        bool IsComplete() const;
        bool HasFailed() const;
        float GetProgress() const;
        std::string GetStatus() const;
        std::string GetError() const;
        
    private:
        struct Impl;
        
        explicit Committer(std::unique_ptr<Impl> impl);
        
        std::unique_ptr<Impl> _impl;
    };
    
    static bool LoadFromFile(World& world, const std::filesystem::path& path);

    static bool SaveToFile(World& world, const std::filesystem::path& path);
};