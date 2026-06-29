#pragma once

#include <unordered_map>
#include <any>
#include <memory>
#include <optional>
#include <cstdint>
#include <iostream>
#include "SceneGraph.h"
namespace Genesis {

/**
 * @brief Normalized RGBA8 linear color primitive.
 */
struct JColor {
    uint8_t r, g, b, a;
    bool operator==(const JColor& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

/**
 * @brief JType-safe style property key.
...
 * Encapsulates a unique ID and the expected value type.
 */
template<typename T>
struct JStyleKey {
    uint32_t id;
};

/**
 * @brief Sparse storage for visual properties at a specific node.
 */
class JStyleStore {
public:
    template<typename T>
    void set(JStyleKey<T> key, T value) {
        m_properties[key.id] = std::make_any<T>(value);
    }

    template<typename T>
    std::optional<T> get(JStyleKey<T> key) const {
        auto it = m_properties.find(key.id);
        if (it != m_properties.end()) {
            return std::any_cast<T>(it->second);
        }
        return std::nullopt;
    }

    bool has(uint32_t keyId) const {
        return m_properties.count(keyId) > 0;
    }

private:
    std::unordered_map<uint32_t, std::any> m_properties;
};

/**
 * @brief The JStyle Engine manages the cascading lookup logic across the JSceneGraph.
 */
class JStyleEngine {
public:
    JStyleEngine(const JSceneGraph& graph) : m_graph(graph) {}

    /**
     * @brief Looks up a property, traversing up the parent hierarchy if not found locally.
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
            // Traverse up the hierarchy
            // Note: We need a way to get parent from JSceneGraph, 
            // but our JSceneGraph currently stores hierarchy privately.
            // I will assume for now we can access it or we update JSceneGraph.
            current = getParentId(current);
        }
        return defaultValue;
    }

    template<typename T>
    void setLocal(NodeId id, JStyleKey<T> key, T value) {
        m_nodeStyles[id].set(key, value);
    }

private:
    // This requires exposing parentId in JSceneGraph or keeping a mirror here.
    // For this architectural demo, I'll mirror it or assume a helper.
    NodeId getParentId(NodeId id) const {
        return m_graph.getParent(id);
    }

    const JSceneGraph& m_graph;
    std::unordered_map<NodeId, JStyleStore> m_nodeStyles;
};

/**
 * @brief Standard JStyle Keys for the Toolkit.
 */
namespace JStyle {
    constexpr JStyleKey<uint32_t[4]> BackgroundColor{ 0x01 }; // RGBA8
    constexpr JStyleKey<float> CornerRadius{ 0x02 };
    constexpr JStyleKey<float> BorderWidth{ 0x03 };
    constexpr JStyleKey<float> RowHeight{ 0x50 };
    constexpr JStyleKey<float> HeaderHeight{ 0x51 };
    constexpr JStyleKey<float> CellPadding{ 0x52 };
}

} // namespace Genesis
