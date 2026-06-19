#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include "ApplicationCore.h"

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogAssetPipeline = Genesis::Log::Assets; }

namespace Genesis {

/**
 * @brief Opaque container for raw, cache-aligned asset data staged in background.
 */
struct StagedAsset {
    enum class Type { Raw, Font, Texture };
    Type type;
    std::vector<uint8_t> data;
    std::string identifier;
};

/**
 * @brief Lock-free Asynchronous Asset Pipeline Manager.
 * Offloads heavy I/O and processing to worker threads, injecting results
 * into the main loop via the task queue without frame stalling.
 */
class AssetManager {
public:
    AssetManager(Core::Application& app) : m_appContext(app), m_activeLoads(0) {}
    ~AssetManager() {
        // Ensure all background workers are joined or detached safely
    }

    // Disable unsafe resource duplication
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /**
     * @brief Initiates a non-blocking load of a resource.
     * @tparam T Callback type for handling the loaded asset on the main thread.
     */
    void requestAsset(const std::string& path, StagedAsset::Type type, std::function<void(std::shared_ptr<StagedAsset>)> onLoaded) {
        m_activeLoads.fetch_add(1, std::memory_order_relaxed);

        // Spawn a background worker (or pull from a thread pool in a full implementation)
        std::thread([this, path, type, onLoaded]() {
            try {
                qCInfo(LogAssetPipeline) << "Background loading initiated for asset: " << path << std::endl;
                
                // 1. Heavy I/O Operation (Simulated here)
                // In production: std::ifstream or native OS file descriptors
                auto staged = std::make_shared<StagedAsset>();
                staged->identifier = path;
                staged->type = type;
                
                // Simulate processing/decoding time (e.g., MSDF generation or PNG decoding)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                staged->data.resize(1024, 0xAA); // Dummy data

                // 2. Thread-Safe Injection into Main Thread Loop
                m_appContext.postToMainThread([staged, onLoaded, this]() {
                    if (onLoaded) {
                        onLoaded(staged);
                    }
                    m_activeLoads.fetch_sub(1, std::memory_order_release);
                    qCInfo(LogAssetPipeline) << "Asset transferred to main thread: " << staged->identifier << std::endl;
                });

            } catch (const std::exception& e) {
                qCWarning(LogAssetPipeline) << "Background asset load failed: " << e.what() << std::endl;
                m_activeLoads.fetch_sub(1, std::memory_order_release);
            }
        }).detach(); // Using detach for this architectural spec; pool-management preferred for production.
    }

    bool isIdle() const {
        return m_activeLoads.load(std::memory_order_acquire) == 0;
    }

private:
    Core::Application& m_appContext;
    std::atomic<uint32_t> m_activeLoads;
};

} // namespace Genesis
