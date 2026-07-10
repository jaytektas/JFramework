#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <algorithm>

#include <j/core/muted_logging_mock.h>
namespace { inline constexpr auto& LogGraphicsEngine = jf::Log::Graphics; }

inline namespace jf {

// Opaque handle to a GPU-resident texture created via JGpuHal::uploadTexture().
// 0 is the null/invalid handle.
using TextureHandle = uint32_t;
static constexpr TextureHandle kNullTexture = 0;

enum class JPrimitiveType : uint32_t {
    Rectangle = 0,
    TextRun   = 1,
    CustomPath = 2
};

/**
 * @brief Pack-aligned GPU Vertex Layout for high throughput memory streaming.
 * Strict 32-byte alignment per vertex.
 */
struct JRenderVertex {
    float position[2]; // X, Y coordinates
    float texCoord[2]; // U, V coordinates
    uint8_t color[4];  // RGBA8 normalized linear format
};

/**
 * @brief Unified GPU Instance Data.
 * Decouples shape parameters from geometry buffers via Signed Distance Fields (SDF).
 */
struct alignas(16) JGpuPrimitiveInstance {
    float rectBounds[4];    // x, y, width, height
    uint8_t color[4];       // Primary fill RGBA8
    uint8_t borderColor[4]; // Border color RGBA8
    float borderRadius;     // Corner rounding radius in pixels
    float borderWidth;      // Border inner width thickness
    uint32_t primitiveType; // Maps directly to JPrimitiveType enum
    float padding[3];       // Hard GPU alignment constraint padding
};

/**
 * @brief Ordered draw command buffer.
 *
 * Every pushRectangle / pushTextCall appends one JDrawCommand to a single
 * sequence.  The HAL walks that sequence in order, batching consecutive
 * same-pipeline commands for efficiency.  This guarantees correct painter's-
 * algorithm z-ordering regardless of how many pipelines are interleaved.
 */
class JPrimitiveBuffer {
public:
    JPrimitiveBuffer() = default;
    ~JPrimitiveBuffer() = default;

    JPrimitiveBuffer(const JPrimitiveBuffer&) = delete;
    JPrimitiveBuffer& operator=(const JPrimitiveBuffer&) = delete;

    // ---- Text types ----
    struct JTextVertex { float x, y, u, v; };
    struct JTextCall {
        uint8_t color[4]{};
        std::vector<JTextVertex> verts;
        uint32_t atlasId{0};   // 0 = the base UI font atlas; >0 = a size-specific glyph atlas (crisp large text)
    };

    // Scissor/clip rectangle in window pixels.  enabled=false means "no clip" (full window).
    struct JClipRect {
        float x{0.0f}, y{0.0f}, w{0.0f}, h{0.0f};
        bool  enabled{false};
    };

    // ---- Image draw data ----
    struct JImageData {
        float x{0}, y{0}, w{0}, h{0};   // destination rect in window pixels
        float u0{0}, v0{0}, u1{1}, v1{1}; // source UV rect (default = full texture)
        TextureHandle tex{kNullTexture};
        uint8_t tint[4]{255, 255, 255, 255}; // RGBA tint (255,255,255,255 = no tint)
    };

    // ---- Vector geometry (anti-aliased 2D paths, per-vertex color) ----
    struct JGeometryData {
        std::vector<JRenderVertex> verts;            // triangle list (count % 3 == 0)
        TextureHandle             tex{kNullTexture}; // optional; null = solid per-vertex color
    };

    // ---- Unified draw command ----
    struct JDrawCommand {
        enum class JKind : uint8_t { JRect, Text, Image, Geometry };
        JKind             kind{JKind::JRect};
        JGpuPrimitiveInstance rect{};
        JTextCall             text{};
        JImageData            image{};
        JGeometryData         geom{};
        JClipRect             clip{};   // active clip when this command was recorded
    };

    // ---- Push API ----

    void pushRectangle(float x, float y, float width, float height,
                       const uint8_t fill[4], float radius = 0.0f,
                       float bWidth = 0.0f, const uint8_t bColor[4] = nullptr)
    {
        JDrawCommand cmd;
        cmd.kind = JDrawCommand::JKind::JRect;
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
        cmd.rect.primitiveType = static_cast<uint32_t>(JPrimitiveType::Rectangle);
        cmd.clip = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    // Draw a dashed rectangle OUTLINE (no fill) by emitting short filled segments along each edge. The solid
    // primitive stroke can't dash, so this is how a caller gets a dashed border — selection rubber-band,
    // viewport guides, dashed panel borders. dashLen/gapLen are in pixels (defaults give a 4-on 2-off dash for
    // a 1px pen); thickness is the stroke width. Corners are covered by both edges (harmless overlap).
    void pushDashedRect(float x, float y, float width, float height, const uint8_t color[4],
                        float thickness = 1.0f, float dashLen = 4.0f, float gapLen = 2.0f)
    {
        const float period = dashLen + gapLen;
        if (!color || period <= 0.0f || width <= 0.0f || height <= 0.0f) return;
        for (float sx = x; sx < x + width; sx += period) {            // top + bottom edges
            const float remain = x + width - sx, len = dashLen < remain ? dashLen : remain;
            pushRectangle(sx, y, len, thickness, color);
            pushRectangle(sx, y + height - thickness, len, thickness, color);
        }
        for (float sy = y; sy < y + height; sy += period) {          // left + right edges
            const float remain = y + height - sy, len = dashLen < remain ? dashLen : remain;
            pushRectangle(x, sy, thickness, len, color);
            pushRectangle(x + width - thickness, sy, thickness, len, color);
        }
    }

    void pushTextCall(JTextCall call) {
        JDrawCommand cmd;
        cmd.kind = JDrawCommand::JKind::Text;
        cmd.text = std::move(call);
        cmd.clip = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    void pushImage(float x, float y, float w, float h, TextureHandle tex,
                   const uint8_t tint[4] = nullptr,
                   float u0 = 0.f, float v0 = 0.f, float u1 = 1.f, float v1 = 1.f)
    {
        if (tex == kNullTexture) return;
        JDrawCommand cmd;
        cmd.kind      = JDrawCommand::JKind::Image;
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

    // Push a triangle-list of anti-aliased vector geometry (see VectorGraphics.h).
    // verts.size() must be a multiple of 3. tex defaults to solid per-vertex color.
    void pushGeometry(std::vector<JRenderVertex> verts, TextureHandle tex = kNullTexture) {
        if (verts.size() < 3) return;
        JDrawCommand cmd;
        cmd.kind       = JDrawCommand::JKind::Geometry;
        cmd.geom.verts = std::move(verts);
        cmd.geom.tex   = tex;
        cmd.clip       = currentClip();
        m_commands.push_back(std::move(cmd));
    }

    // ---- Clip stack: scopes subsequent draws to a rectangle (nested clips intersect) ----
    void pushClip(float x, float y, float w, float h) {
        JClipRect c{x, y, w, h, true};
        if (!m_clipStack.empty()) {
            const JClipRect& p = m_clipStack.back();
            float x1 = std::max(c.x, p.x), y1 = std::max(c.y, p.y);
            float x2 = std::min(c.x + c.w, p.x + p.w), y2 = std::min(c.y + c.h, p.y + p.h);
            c.x = x1; c.y = y1; c.w = std::max(0.0f, x2 - x1); c.h = std::max(0.0f, y2 - y1);
        }
        m_clipStack.push_back(c);
    }
    void popClip() { if (!m_clipStack.empty()) m_clipStack.pop_back(); }
    JClipRect currentClip() const { return m_clipStack.empty() ? JClipRect{} : m_clipStack.back(); }

    // ---- Read API ----

    const std::vector<JDrawCommand>& getCommands() const { return m_commands; }

    void clear() { m_commands.clear(); m_clipStack.clear(); }

private:
    std::vector<JDrawCommand> m_commands;
    std::vector<JClipRect>    m_clipStack;
};

} // inline namespace jf
