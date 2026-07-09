#pragma once

#include "SceneGraph.h"     // NodeId, JSceneGraph, InvalidNodeId
#include "JStyleStore.h"    // JStyleStore + JStyleKey (the engine's per-node storage)
#include <unordered_map>

inline namespace jf {

/**
 * @brief The style engine manages the cascading property lookup across the JSceneGraph.
 *        A property set on a node is inherited by descendants unless overridden.
 */
class JStyleEngine {
public:
    JStyleEngine(const JSceneGraph& graph) : m_graph(graph) {}

    /**
     * @brief Look up a property, traversing up the parent hierarchy if not found locally.
     */
    template<typename T>
    T lookup(NodeId id, JStyleKey<T> key, T defaultValue) const {
        NodeId current = id;
        while (current != InvalidNodeId) {
            auto it = m_nodeStyles.find(current);
            if (it != m_nodeStyles.end()) {
                auto val = it->second.get(key);
                if (val.has_value()) {
                    return val.value();
                }
            }
            current = m_graph.getParent(current);
        }
        return defaultValue;
    }

    template<typename T>
    void setLocal(NodeId id, JStyleKey<T> key, T value) {
        m_nodeStyles[id].set(key, value);
    }

private:
    const JSceneGraph& m_graph;
    std::unordered_map<NodeId, JStyleStore> m_nodeStyles;
};

} // inline namespace jf
