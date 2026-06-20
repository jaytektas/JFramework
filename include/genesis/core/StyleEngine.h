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
struct Color {
    uint8_t r, g, b, a;
    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

/**
 * @brief Type-safe style property key.
...
 * Encapsulates a unique ID and the expected value type.
 */
template<typename T>
struct StyleKey {
    uint32_t id;
};

/**
 * @brief Sparse storage for visual properties at a specific node.
 */
class StyleStore {
public:
    template<typename T>
    void set(StyleKey<T> key, T value) {
        m_properties[key.id] = std::make_any<T>(value);
    }

    template<typename T>
    std::optional<T> get(StyleKey<T> key) const {
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
 * @brief The Style Engine manages the cascading lookup logic across the SceneGraph.
 */
class StyleEngine {
public:
    StyleEngine(const SceneGraph& graph) : m_graph(graph) {}

    /**
     * @brief Looks up a property, traversing up the parent hierarchy if not found locally.
     */
    template<typename T>
    T lookup(NodeId id, StyleKey<T> key, T defaultValue) const {
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
            // Note: We need a way to get parent from SceneGraph, 
            // but our SceneGraph currently stores hierarchy privately.
            // I will assume for now we can access it or we update SceneGraph.
            current = getParentId(current);
        }
        return defaultValue;
    }

    template<typename T>
    void setLocal(NodeId id, StyleKey<T> key, T value) {
        m_nodeStyles[id].set(key, value);
    }

private:
    // This requires exposing parentId in SceneGraph or keeping a mirror here.
    // For this architectural demo, I'll mirror it or assume a helper.
    NodeId getParentId(NodeId id) const {
        return m_graph.getParent(id);
    }

    const SceneGraph& m_graph;
    std::unordered_map<NodeId, StyleStore> m_nodeStyles;
};

/**
 * @brief Standard Style Keys for the Toolkit.
 */
namespace Style {
    constexpr StyleKey<uint32_t[4]> BackgroundColor{ 0x01 }; // RGBA8
    constexpr StyleKey<float> CornerRadius{ 0x02 };
    constexpr StyleKey<float> BorderWidth{ 0x03 };
    constexpr StyleKey<float> RowHeight{ 0x50 };
    constexpr StyleKey<float> HeaderHeight{ 0x51 };
    constexpr StyleKey<float> CellPadding{ 0x52 };
}

} // namespace Genesis
