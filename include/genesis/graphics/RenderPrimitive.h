#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <algorithm>

#include <genesis/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogGraphicsEngine = Genesis::Log::Graphics; }

namespace Genesis {

// Opaque handle to a GPU-resident texture created via GpuHal::uploadTexture().
// 0 is the null/invalid handle.
using TextureHandle = uint32_t;
static constexpr TextureHandle kNullTexture = 0;

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

    // Scissor/clip rectangle in window pixels.  enabled=false means "no clip" (full window).
    struct ClipRect {
        float x{0.0f}, y{0.0f}, w{0.0f}, h{0.0f};
        bool  enabled{false};
    };

    // ---- Image draw data ----
    struct ImageData {
        float x{0}, y{0}, w{0}, h{0};   // destination rect in window pixels
        float u0{0}, v0{0}, u1{1}, v1{1}; // source UV rect (default = full texture)
        TextureHandle tex{kNullTexture};
        uint8_t tint[4]{255, 255, 255, 255}; // RGBA tint (255,255,255,255 = no tint)
    };

    // ---- Unified draw command ----
    struct DrawCommand {
        enum class Kind : uint8_t { Rect, Text, Image };
        Kind             kind{Kind::Rect};
        GpuPrimitiveInstance rect{};
        TextCall             text{};
        ImageData            image{};
        ClipRect             clip{};   // active clip when this command was recorded
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
        cmd.clip = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    void pushTextCall(TextCall call) {
        DrawCommand cmd;
        cmd.kind = DrawCommand::Kind::Text;
        cmd.text = std::move(call);
        cmd.clip = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    void pushImage(float x, float y, float w, float h, TextureHandle tex,
                   const uint8_t tint[4] = nullptr,
                   float u0 = 0.f, float v0 = 0.f, float u1 = 1.f, float v1 = 1.f)
    {
        if (tex == kNullTexture) return;
        DrawCommand cmd;
        cmd.kind      = DrawCommand::Kind::Image;
        cmd.image.x   = x; cmd.image.y  = y;
        cmd.image.w   = w; cmd.image.h  = h;
        cmd.image.u0  = u0; cmd.image.v0 = v0;
        cmd.image.u1  = u1; cmd.image.v1 = v1;
        cmd.image.tex = tex;
        if (tint) {
            for (int i = 0; i < 4; ++i) cmd.image.tint[i] = tint[i];
        }
        cmd.clip = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    // ---- Clip stack: scopes subsequent draws to a rectangle (nested clips intersect) ----
    void pushClip(float x, float y, float w, float h) {
        ClipRect c{x, y, w, h, true};
        if (!m_clipStack.empty()) {
            const ClipRect& p = m_clipStack.back();
            float x1 = std::max(c.x, p.x), y1 = std::max(c.y, p.y);
            float x2 = std::min(c.x + c.w, p.x + p.w), y2 = std::min(c.y + c.h, p.y + p.h);
            c.x = x1; c.y = y1; c.w = std::max(0.0f, x2 - x1); c.h = std::max(0.0f, y2 - y1);
        }
        m_clipStack.push_back(c);
    }
    void popClip() { if (!m_clipStack.empty()) m_clipStack.pop_back(); }
    ClipRect currentClip() const { return m_clipStack.empty() ? ClipRect{} : m_clipStack.back(); }

    // ---- Read API ----

    const std::vector<DrawCommand>& getCommands() const { return m_commands; }

    void clear() { m_commands.clear(); m_clipStack.clear(); }

private:
    std::vector<DrawCommand> m_commands;
    std::vector<ClipRect>    m_clipStack;
};

} // namespace Genesis
