#pragma once

// Thread-safety: MAIN THREAD ONLY.
// JWindow registration and surface management happen on the render/main thread.

#include <vector>
#include <memory>
#include <unordered_map>
#include "SceneGraph.h"

inline namespace jf {

/**
 * @brief Represents a managed application surface within Genesis Prime.
 */
struct JAppSurface {
    uint32_t surfaceId;
    NodeId layoutNode;      // Corresponding node in the WM's global JSceneGraph
    bool isFocused;         // Whether this surface currently holds focus
    float taskRelevance;    // Scalar (0.0 - 1.0) layout priority
};

/**
 * @brief Superior JWindow Manager Core.
 * Orchestrates spatial layouts and focus across all Genesis apps.
 */
class JWindowManager {
public:
    explicit JWindowManager(JSceneGraph& globalGraph)
        : m_globalGraph(globalGraph), m_rootWorkspace(0)
    {
        m_rootWorkspace = m_globalGraph.createNode("Genesis_Prime_Root");
    }

    /**
     * @brief Registers a new application surface into the spatial environment.
     */
    void registerApp(const std::string& appName) {
        NodeId appNode = m_globalGraph.createNode(appName);
        m_globalGraph.addChild(m_rootWorkspace, appNode);
        
        JAppSurface surface{
            static_cast<uint32_t>(m_surfaces.size()),
            appNode,
            false,
            0.0f
        };
        m_surfaces[surface.surfaceId] = surface;
    }

    /**
     * @brief The Layout Pass.
     * Rearranges windows based on task relevance.
     */
    void performLayout() {
        // 1. Adjust m_globalGraph.getLayout() parameters for each surface
        // 2. Trigger JSceneGraph invalidation vector
    }

    /**
     * @brief Spatial Focus Switch.
     * Moves a surface to the foreground and adjusts the virtual camera.
     */
    void setFocus(uint32_t surfaceId) {
        // Implementation: Adjust Z-order and priorities
    }

private:
    JSceneGraph& m_globalGraph;
    NodeId m_rootWorkspace;
    std::unordered_map<uint32_t, JAppSurface> m_surfaces;
};

} // inline namespace jf
