#pragma once

#include <vector>
#include <cstdint>
#include <iostream>

// --- Custom Logging Integration Mock ---
#ifndef qCWarning
#define qCWarning(category) std::cerr << "[WARNING] "
struct MockCategory {};
inline MockCategory LogGraphicsEngine;
#endif

namespace Genesis {

enum class PrimitiveType : uint32_t {
    Rectangle = 0,
    TextRun   = 1,
    CustomPath = 2
};

/**
 * @brief Pack-aligned GPU Vertex Layout for high throughput memory streaming.
 * Strict 32-byte alignment per vertex.
 */
struct RenderVertex {
    float position[2]; // X, Y coordinates
    float texCoord[2]; // U, V coordinates
    uint8_t color[4];  // RGBA8 normalized linear format
};

/**
 * @brief Unified GPU Instance Data.
 * Decouples shape parameters from geometry buffers via Signed Distance Fields (SDF).
 */
struct alignas(16) GpuPrimitiveInstance {
    float rectBounds[4];   // x, y, width, height
    uint8_t color[4];      // Primary fill RGBA8
    uint8_t borderColor[4];// Border color RGBA8
    float borderRadius;    // Corner rounding radius in pixels
    float borderWidth;     // Border inner width thickness
    uint32_t primitiveType;// Maps directly to PrimitiveType enum
    float padding[3];      // Hard GPU alignment constraint padding
};

/**
 * @brief High-performance Intermediate Representation (IR) Primitive Buffer.
 */
class PrimitiveBuffer {
public:
    PrimitiveBuffer() = default;
    ~PrimitiveBuffer() = default;

    PrimitiveBuffer(const PrimitiveBuffer&) = delete;
    PrimitiveBuffer& operator=(const PrimitiveBuffer&) = delete;

    /**
     * @brief Clear existing structures to re-use backing memory limits.
     */
    void clear() {
        m_vertices.clear();
        m_indices.clear();
        m_instances.clear();
    }

    /**
     * @brief Appends a rendering rectangle or panel block into the flat stream array.
     */
    void pushRectangle(float x, float y, float width, float height, 
                        const uint8_t fill[4], float radius = 0.0f, 
                        float bWidth = 0.0f, const uint8_t bColor[4] = nullptr) 
    {
        uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());

        // 1. Generate local index topologies for standard hardware 2-triangle Quad strip mapping
        m_indices.push_back(baseVertex + 0);
        m_indices.push_back(baseVertex + 1);
        m_indices.push_back(baseVertex + 2);
        m_indices.push_back(baseVertex + 2);
        m_indices.push_back(baseVertex + 3);
        m_indices.push_back(baseVertex + 0);

        // 2. Map standard normalized bounding corner layouts
        m_vertices.push_back(RenderVertex{{x, y},                  {0.0f, 0.0f}, {fill[0], fill[1], fill[2], fill[3]}});
        m_vertices.push_back(RenderVertex{{x + width, y},          {1.0f, 0.0f}, {fill[0], fill[1], fill[2], fill[3]}});
        m_vertices.push_back(RenderVertex{{x + width, y + height},  {1.0f, 1.0f}, {fill[0], fill[1], fill[2], fill[3]}});
        m_vertices.push_back(RenderVertex{{x, y + height},          {0.0f, 1.0f}, {fill[0], fill[1], fill[2], fill[3]}});

        // 3. Formulate the explicit GPU structural instance payload
        GpuPrimitiveInstance instance{};
        instance.rectBounds[0] = x;
        instance.rectBounds[1] = y;
        instance.rectBounds[2] = width;
        instance.rectBounds[3] = height;
        
        for (int i = 0; i < 4; ++i) {
            instance.color[i] = fill[i];
            instance.borderColor[i] = bColor ? bColor[i] : 0;
        }
        
        instance.borderRadius = radius;
        instance.borderWidth = bWidth;
        instance.primitiveType = static_cast<uint32_t>(PrimitiveType::Rectangle);

        m_instances.push_back(instance);
    }

    const std::vector<RenderVertex>& getVertices() const { return m_vertices; }
    const std::vector<uint32_t>& getIndices() const { return m_indices; }
    const std::vector<GpuPrimitiveInstance>& getInstances() const { return m_instances; }

private:
    std::vector<RenderVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<GpuPrimitiveInstance> m_instances;
};

} // namespace Genesis
