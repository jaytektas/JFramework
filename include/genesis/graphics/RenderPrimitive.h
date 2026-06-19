#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogGraphicsEngine = Genesis::Log::Graphics; }

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
    float rectBounds[4];    // x, y, width, height
    uint8_t color[4];       // Primary fill RGBA8
    uint8_t borderColor[4]; // Border color RGBA8
    float borderRadius;     // Corner rounding radius in pixels
    float borderWidth;      // Border inner width thickness
    uint32_t primitiveType; // Maps directly to PrimitiveType enum
    float padding[3];       // Hard GPU alignment constraint padding
};

/**
 * @brief Ordered draw command buffer.
 *
 * Every pushRectangle / pushTextCall appends one DrawCommand to a single
 * sequence.  The HAL walks that sequence in order, batching consecutive
 * same-pipeline commands for efficiency.  This guarantees correct painter's-
 * algorithm z-ordering regardless of how many pipelines are interleaved.
 */
class PrimitiveBuffer {
public:
    PrimitiveBuffer() = default;
    ~PrimitiveBuffer() = default;

    PrimitiveBuffer(const PrimitiveBuffer&) = delete;
    PrimitiveBuffer& operator=(const PrimitiveBuffer&) = delete;

    // ---- Text types ----
    struct TextVertex { float x, y, u, v; };
    struct TextCall {
        uint8_t color[4]{};
        std::vector<TextVertex> verts;
    };

    // ---- Unified draw command ----
    struct DrawCommand {
        enum class Kind : uint8_t { Rect, Text };
        Kind             kind{Kind::Rect};
        GpuPrimitiveInstance rect{};
        TextCall             text{};
    };

    // ---- Push API ----

    void pushRectangle(float x, float y, float width, float height,
                       const uint8_t fill[4], float radius = 0.0f,
                       float bWidth = 0.0f, const uint8_t bColor[4] = nullptr)
    {
        DrawCommand cmd;
        cmd.kind = DrawCommand::Kind::Rect;
        cmd.rect.rectBounds[0] = x;
        cmd.rect.rectBounds[1] = y;
        cmd.rect.rectBounds[2] = width;
        cmd.rect.rectBounds[3] = height;
        for (int i = 0; i < 4; ++i) {
            cmd.rect.color[i]       = fill[i];
            cmd.rect.borderColor[i] = bColor ? bColor[i] : 0;
        }
        cmd.rect.borderRadius  = radius;
        cmd.rect.borderWidth   = bWidth;
        cmd.rect.primitiveType = static_cast<uint32_t>(PrimitiveType::Rectangle);
        m_commands.push_back(std::move(cmd));
    }

    void pushTextCall(TextCall call) {
        DrawCommand cmd;
        cmd.kind = DrawCommand::Kind::Text;
        cmd.text = std::move(call);
        m_commands.push_back(std::move(cmd));
    }

    // ---- Read API ----

    const std::vector<DrawCommand>& getCommands() const { return m_commands; }

    void clear() { m_commands.clear(); }

private:
    std::vector<DrawCommand> m_commands;
};

} // namespace Genesis
