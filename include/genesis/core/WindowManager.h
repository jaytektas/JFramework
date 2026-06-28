#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Window registration and surface management happen on the render/main thread.

#include <vector>
#include <memory>
#include <unordered_map>
#include "SceneGraph.h"
#include "AiControlBus.h"

namespace Genesis {

/**
 * @brief Represents a managed application surface within Genesis Prime.
 */
struct AppSurface {
    uint32_t surfaceId;
    NodeId layoutNode;      // Corresponding node in the WM's global SceneGraph
    bool isAgenticFocused;  // Whether the AI ALE is currently optimizing this app
    float taskRelevance;    // Scalar (0.0 - 1.0) determined by AiControlBus
};

/**
 * @brief Superior Window Manager Core.
 * Orchestrates spatial layouts and AI-driven focus across all Genesis apps.
 */
class WindowManager {
public:
    WindowManager(SceneGraph& globalGraph, AiControlBus& aiBus) 
        : m_globalGraph(globalGraph), m_aiBus(aiBus), m_rootWorkspace(0) 
    {
        m_rootWorkspace = m_globalGraph.createNode("Genesis_Prime_Root");
    }

    /**
     * @brief Registers a new application surface into the spatial environment.
     */
    void registerApp(const std::string& appName) {
        NodeId appNode = m_globalGraph.createNode(appName);
        m_globalGraph.addChild(m_rootWorkspace, appNode);
        
        AppSurface surface{
            static_cast<uint32_t>(m_surfaces.size()),
            appNode,
            false,
            0.0f
        };
        m_surfaces[surface.surfaceId] = surface;
    }

    /**
     * @brief The Agentic Layout Pass.
     * Uses ALE logic to rearrange windows based on task relevance.
     */
    void performAgenticLayout() {
        // 1. Read task context from AiControlBus
        // 2. Adjust m_globalGraph.getLayout() parameters for each surface
        // 3. Trigger SceneGraph invalidation vector
    }

    /**
     * @brief Spatial Focus Switch.
     * Moves a surface to the foreground and adjusts the virtual camera.
     */
    void setFocus(uint32_t surfaceId) {
        // Implementation: Adjust Z-order and ALE priorities
    }

private:
    SceneGraph& m_globalGraph;
    AiControlBus& m_aiBus;
    NodeId m_rootWorkspace;
    std::unordered_map<uint32_t, AppSurface> m_surfaces;
};

} // namespace Genesis
