#pragma once

#include <unordered_map>
#include <any>
#include <optional>
#include <cstdint>

inline namespace jf {

/**
 * @brief Type-safe style property key. Encapsulates a unique ID and the
 *        expected value type, so get/set on a JStyleStore can't be mistyped.
 */
template<typename T>
struct JStyleKey {
    uint32_t id;
};

/**
 * @brief Sparse storage for visual properties at a single scene-graph node.
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

} // inline namespace jf
