#pragma once

// JTextHelper — global font atlas + text layout for widgets. Extracted from BaseWidgets.h.

#include <string>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <map>
#include <functional>
#include "JStyle.h"
#include "../graphics/RenderPrimitive.h"
#include "../graphics/FontEngine.h"

inline namespace jf {

// ============================================================================
// JTextHelper — global font atlas + text layout for widgets
// ============================================================================

/**
 * Single shared JFontAtlas used by all widgets.
 * Set once at app startup: JTextHelper::setAtlas(fontEngine.buildAtlas(14.f));
 * After that, every populateRenderPrimitives call can emit real text glyphs.
 */
class JTextHelper {
public:
    static void setAtlas(JFontAtlas atlas) { get() = std::move(atlas); }
    static const JFontAtlas& atlas()       { return get(); }
    static bool hasAtlas()                { return get().valid; }

    // ---- Size-aware glyph cache (Qt-style crisp text at any size) ------------------------------
    // The base atlas above serves normal UI text (pushText). Large text (pushTextScaled) would blur
    // if it just upscaled the base bitmap, so instead we rasterise a fresh atlas AT the target pixel
    // size and draw from it 1:1. JAppWindow wires these three hooks to the font engine + GPU; if they
    // are unset (e.g. a headless/text-only build) pushTextScaled falls back to scaling the base atlas.
    static inline std::function<JFontAtlas(float px)>       s_buildSized;   // rasterise a whole atlas at px
    static inline std::function<uint32_t(const JFontAtlas&)> s_uploadSized;  // upload -> gpu atlas id (>0)
    static inline std::function<void()>                     s_freeSized;    // free every gpu sized atlas

    struct SizedAtlas { JFontAtlas atlas; uint32_t id{0}; };
    static std::map<int, SizedAtlas>& sizedCache() { static std::map<int, SizedAtlas> m; return m; }

    // Get-or-build the glyph atlas whose native size is `px`. Returns nullptr if the size machinery
    // isn't wired or the bake/upload failed (caller falls back to base-atlas scaling).
    static const SizedAtlas* sizedFor(int px) {
        if (!s_buildSized || !s_uploadSized) return nullptr;
        auto& cache = sizedCache();
        if (auto it = cache.find(px); it != cache.end()) return &it->second;
        JFontAtlas a = s_buildSized(static_cast<float>(px));
        if (!a.valid) return nullptr;
        uint32_t id = s_uploadSized(a);
        if (id == 0) return nullptr;
        SizedAtlas& slot = cache[px];
        slot.atlas = std::move(a); slot.id = id;
        return &slot;
    }

    // The base font changed → every cached sized atlas is stale (wrong face/metrics). Free them.
    static void invalidateSized() { if (s_freeSized) s_freeSized(); sizedCache().clear(); }

    /** Decode one UTF-8 codepoint from src[i], advance i, return codepoint. */
    static uint32_t _decodeUtf8(const std::string& s, size_t& i) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80)                       { i += 1; return b; }
        if ((b & 0xE0) == 0xC0 && i+1 < s.size()) {
            uint32_t cp = ((b & 0x1F) << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
            i += 2; return cp;
        }
        if ((b & 0xF0) == 0xE0 && i+2 < s.size()) {
            uint32_t cp = ((b & 0x0F) << 12)
                        | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6)
                        |  (static_cast<unsigned char>(s[i+2]) & 0x3F);
            i += 3; return cp;
        }
        if ((b & 0xF8) == 0xF0 && i+3 < s.size()) {
            uint32_t cp = ((b & 0x07) << 18)
                        | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 12)
                        | ((static_cast<unsigned char>(s[i+2]) & 0x3F) << 6)
                        |  (static_cast<unsigned char>(s[i+3]) & 0x3F);
            i += 4; return cp;
        }
        i += 1; return 0; // invalid byte — skip
    }

    /** Map codepoints outside the atlas to something that is in it. */
    static uint32_t _substitute(uint32_t cp) {
        if (cp >= 0x2013 && cp <= 0x2014) return '-';   // en/em dash
        if (cp == 0x2018 || cp == 0x2019) return '\'';  // smart single quotes
        if (cp == 0x201C || cp == 0x201D) return '"';   // smart double quotes
        if (cp == 0x2026)                 return '.';   // ellipsis → period
        if (cp == 0x00A0)                 return ' ';   // non-breaking space
        return '?';
    }

    /** Push 6 verts (2 triangles) per glyph into a JTextCall and add to buf. */
    static void pushText(JPrimitiveBuffer& buf,
                         float x, float y,
                         const std::string& text,
                         const uint8_t color[4],
                         float maxWidth = 0.0f)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty()) return;

        JPrimitiveBuffer::JTextCall call;
        std::copy(color, color + 4, call.color);

        float penX    = x;
        float baseline = y + atl.ascent;

        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;

            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { penX += atl.ascent * 0.35f; continue; }
            }
            const JGlyphInfo& g = it->second;

            if (maxWidth > 0.0f && (penX - x + g.advanceX) > maxWidth) break;

            penX += g.advanceX;

            // Whitespace or zero-size glyphs: advance pen only, no geometry
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            // Pixel-snap the glyph quad to the display grid. The atlas is packed and sampled 1:1, but a
            // fractional pen position (fractional advances + fractional ascent) makes the LINEAR sampler
            // smear each glyph across two texel columns/rows — the whole run looks soft/blurry. Snapping the
            // quad's top-left to integers makes every glyph land on the pixel grid (crisp), while penX stays
            // fractional above so letter spacing is unchanged.
            float gx = std::floor(penX - g.advanceX + g.bearingX + 0.5f);
            float gy = std::floor(baseline + g.bearingY + 0.5f);
            float gw = g.pixelW, gh = g.pixelH;

            call.verts.push_back({gx,      gy,      g.u0, g.v0});
            call.verts.push_back({gx + gw, gy,      g.u1, g.v0});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx,      gy + gh, g.u0, g.v1});
            call.verts.push_back({gx,      gy,      g.u0, g.v0});
        }

        if (!call.verts.empty()) buf.pushTextCall(std::move(call));
    }

    /** Push text scaled by `scale` about the atlas' native size. Advances/bearings/glyph quads all
     *  multiply by scale, so the single shared atlas can render large readouts (e.g. a value gauge)
     *  or fine print. Glyphs are sampled up/down by the GPU. maxWidth is measured in final (scaled) px. */
    static void pushTextScaled(JPrimitiveBuffer& buf,
                               float x, float y,
                               const std::string& text,
                               const uint8_t color[4],
                               float scale,
                               float maxWidth = 0.0f)
    {
        const auto& base = get();
        if (!base.valid || text.empty() || scale <= 0.0f) return;
        if (scale == 1.0f) { pushText(buf, x, y, text, color, maxWidth); return; }

        // Qt-style crisp scaling: rather than stretch the base bitmap, render from an atlas baked AT the
        // target pixel size whenever we'd otherwise UPSCALE (downscaling the base atlas is already crisp).
        // Sizes quantise to 4px steps so we don't bake a new atlas per fractional scale. `s` is the residual
        // scale on the chosen atlas — ~1.0 when a size-matched atlas is used, so glyphs sample ~1:1.
        const JFontAtlas* atlp = &base;
        float s = scale;
        uint32_t atlasId = 0;
        const float targetPx = base.pixelSize * scale;
        if (base.pixelSize > 0.0f && targetPx > base.pixelSize * 1.15f) {
            int key = static_cast<int>(std::lround(targetPx / 4.0f)) * 4;
            // Cap the baked size (a 128px atlas already holds every glyph crisp; beyond that a mild
            // upscale from 128px is still far better than upscaling the 14px base).
            key = std::clamp(key, static_cast<int>(std::ceil(base.pixelSize)), 128);
            if (const SizedAtlas* sz = sizedFor(key)) {
                atlp = &sz->atlas; atlasId = sz->id;
                s = (sz->atlas.pixelSize > 0.0f) ? targetPx / sz->atlas.pixelSize : 1.0f;
            }
        }
        const JFontAtlas& atl = *atlp;

        JPrimitiveBuffer::JTextCall call;
        std::copy(color, color + 4, call.color);
        call.atlasId = atlasId;

        float penX     = x;
        float baseline = y + atl.ascent * s;

        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { penX += atl.ascent * 0.35f * s; continue; }
            }
            const JGlyphInfo& g = it->second;

            if (maxWidth > 0.0f && (penX - x + g.advanceX * s) > maxWidth) break;
            penX += g.advanceX * s;
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            float gx = penX - g.advanceX * s + g.bearingX * s;
            float gy = baseline + g.bearingY * s;
            float gw = g.pixelW * s, gh = g.pixelH * s;

            call.verts.push_back({gx,      gy,      g.u0, g.v0});
            call.verts.push_back({gx + gw, gy,      g.u1, g.v0});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx + gw, gy + gh, g.u1, g.v1});
            call.verts.push_back({gx,      gy + gh, g.u0, g.v1});
            call.verts.push_back({gx,      gy,      g.u0, g.v0});
        }
        if (!call.verts.empty()) buf.pushTextCall(std::move(call));
    }

    /** Rendered width of `text` at `scale` (advances scale linearly). */
    static float measureWidthScaled(const std::string& text, float scale) {
        return measureWidth(text) * scale;
    }

    // Text rotated 90° (for vertical tab bars). cw=true reads top->bottom (tilt head right);
    // cw=false reads bottom->top. (x,y) is the rotation pivot; the run advances along the
    // screen Y axis. maxLen caps the run length (in the unrotated text width).
    static void pushTextVertical(JPrimitiveBuffer& buf,
                                 float x, float y,
                                 const std::string& text,
                                 const uint8_t color[4],
                                 float maxLen = 0.0f,
                                 bool  cw = true)
    {
        const auto& atl = get();
        if (!atl.valid || text.empty()) return;

        JPrimitiveBuffer::JTextCall call;
        std::copy(color, color + 4, call.color);

        // Lay out in local (lx along the run, ly across the baseline), then rotate to screen.
        auto rot = [&](float lx, float ly, float& sx, float& sy) {
            if (cw) { sx = x - ly; sy = y + lx; }   // CW 90°
            else    { sx = x + ly; sy = y - lx; }   // CCW 90°
        };

        float pen = 0.f;
        float baseline = atl.ascent;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) {
                cp = _substitute(cp);
                it = atl.glyphs.find(cp);
                if (it == atl.glyphs.end()) { pen += atl.ascent * 0.35f; continue; }
            }
            const JGlyphInfo& g = it->second;
            if (maxLen > 0.0f && (pen + g.advanceX) > maxLen) break;
            pen += g.advanceX;
            if (g.pixelW < 0.5f || g.pixelH < 0.5f) continue;

            float lx = pen - g.advanceX + g.bearingX;
            float ly = baseline + g.bearingY;
            float gw = g.pixelW, gh = g.pixelH;

            float ax, ay, bx, by, cx, cy, dx, dy;
            rot(lx,      ly,      ax, ay);
            rot(lx + gw, ly,      bx, by);
            rot(lx + gw, ly + gh, cx, cy);
            rot(lx,      ly + gh, dx, dy);
            call.verts.push_back({ax, ay, g.u0, g.v0});
            call.verts.push_back({bx, by, g.u1, g.v0});
            call.verts.push_back({cx, cy, g.u1, g.v1});
            call.verts.push_back({cx, cy, g.u1, g.v1});
            call.verts.push_back({dx, dy, g.u0, g.v1});
            call.verts.push_back({ax, ay, g.u0, g.v0});
        }
        if (!call.verts.empty()) buf.pushTextCall(std::move(call));
    }

    /** Measure rendered width of a UTF-8 string using the shared atlas. */
    static float measureWidth(const std::string& text) {
        const auto& atl = get();
        if (!atl.valid) return static_cast<float>(text.size()) * 8.0f;
        float w = 0.0f;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = _decodeUtf8(text, i);
            if (cp == 0) continue;
            auto it = atl.glyphs.find(cp);
            if (it == atl.glyphs.end()) { cp = _substitute(cp); it = atl.glyphs.find(cp); }
            if (it != atl.glyphs.end()) w += it->second.advanceX;
            else w += atl.ascent * 0.35f;
        }
        return w;
    }

    static float lineHeight() {
        return hasAtlas() ? get().lineHeight : 16.0f;
    }

private:
    static JFontAtlas& get() { static JFontAtlas s; return s; }
};

} // inline namespace jf
