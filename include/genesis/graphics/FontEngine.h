#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <genesis/core/muted_logging_mock.h>

// stb_truetype: implementation compiled once in FontEngine.cpp
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace Genesis {

/** Per-glyph metrics in both pixel space and normalised atlas UV space. */
struct GlyphInfo {
    // Atlas UV (0..1)
    float u0, v0, u1, v1;
    // Size in pixels
    float pixelW, pixelH;
    // Bearing from pen position
    float bearingX, bearingY;
    // Horizontal advance
    float advanceX;
};

/** CPU-side result from FontEngine::buildAtlas().  Upload to GPU once. */
struct FontAtlas {
    std::vector<uint8_t> bitmap;      // R8 grayscale, row-major
    uint32_t width  {512};
    uint32_t height {256};
    float    pixelSize{0.0f};         // the size baked into this atlas
    float    ascent{0.0f};
    float    descent{0.0f};
    float    lineHeight{0.0f};
    std::unordered_map<uint32_t, GlyphInfo> glyphs; // codepoint → info
    bool valid{false};
};

/**
 * @brief FontEngine — loads a TTF, rasterises a glyph atlas via stb_truetype.
 *
 * Usage:
 *   FontEngine fe;
 *   fe.loadFromFile("/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf");
 *   auto atlas = fe.buildAtlas(14.0f);   // bake at 14 px
 *   // upload atlas.bitmap to GPU as R8 texture, then call measureText / layout
 */
class FontEngine {
public:
    FontEngine()  = default;
    ~FontEngine() = default;

    FontEngine(const FontEngine&)            = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // ---- Loading ----

    bool loadFromFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            qCWarning(Genesis::Log::Graphics) << "FontEngine: cannot open " << path << "\n";
            return false;
        }
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        m_fontData.resize(sz);
        f.read(reinterpret_cast<char*>(m_fontData.data()), sz);

        int offset = stbtt_GetFontOffsetForIndex(m_fontData.data(), 0);
        if (!stbtt_InitFont(&m_info, m_fontData.data(), offset)) {
            qCWarning(Genesis::Log::Graphics) << "FontEngine: stbtt_InitFont failed for " << path << "\n";
            m_fontData.clear();
            return false;
        }
        m_loaded = true;
        m_path   = path;
        qCInfo(Genesis::Log::Graphics) << "FontEngine: loaded " << path << "\n";
        return true;
    }

    /** Auto-detect a usable system font. */
    bool loadSystemFont() {
        static const char* kCandidates[] = {
#if defined(_WIN32)
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
#else
            "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
            nullptr
        };
        for (const char** p = kCandidates; *p; ++p)
            if (loadFromFile(*p)) return true;
        qCWarning(Genesis::Log::Graphics) << "FontEngine: no system font found\n";
        return false;
    }

    bool isLoaded() const { return m_loaded; }
    const std::string& path() const { return m_path; }

    // ---- Atlas building ----

    /**
     * Rasterise all printable ASCII (32-126) + Latin-1 Supplement (160-255)
     * into a tightly-packed R8 atlas.  Call once per desired pixel size.
     */
    FontAtlas buildAtlas(float pixelSize, uint32_t atlasW = 512, uint32_t atlasH = 256) const {
        FontAtlas atlas;
        if (!m_loaded) return atlas;

        atlas.width     = atlasW;
        atlas.height    = atlasH;
        atlas.pixelSize = pixelSize;
        atlas.bitmap.assign(atlasW * atlasH, 0);

        float scale = stbtt_ScaleForPixelHeight(&m_info, pixelSize);

        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&m_info, &ascent, &descent, &lineGap);
        atlas.ascent     = ascent     * scale;
        atlas.descent    = descent    * scale;
        atlas.lineHeight = (ascent - descent + lineGap) * scale;

        // Simple shelf packer
        uint32_t penX = 1, penY = 1, shelfH = 0;

        auto packRange = [&](uint32_t from, uint32_t to) {
            for (uint32_t cp = from; cp <= to; ++cp) {
                int w, h, xoff, yoff;
                uint8_t* bmp = stbtt_GetCodepointBitmap(
                    &m_info, 0.0f, scale, static_cast<int>(cp), &w, &h, &xoff, &yoff);
                if (!bmp) {
                    // Whitespace: no pixels, but we still need the advance width
                    int adv, lsb;
                    stbtt_GetCodepointHMetrics(&m_info, static_cast<int>(cp), &adv, &lsb);
                    GlyphInfo gi{};
                    gi.advanceX = adv * scale;
                    atlas.glyphs[cp] = gi;
                    continue;
                }

                if (penX + w + 1 >= atlasW) { penX = 1; penY += shelfH + 1; shelfH = 0; }
                if (penY + h + 1 >= atlasH) { stbtt_FreeBitmap(bmp, nullptr); continue; }

                // Blit into atlas
                for (int row = 0; row < h; ++row)
                    std::memcpy(&atlas.bitmap[(penY + row) * atlasW + penX],
                                bmp + row * w, static_cast<size_t>(w));

                GlyphInfo gi{};
                gi.u0 = static_cast<float>(penX)   / atlasW;
                gi.v0 = static_cast<float>(penY)   / atlasH;
                gi.u1 = static_cast<float>(penX+w) / atlasW;
                gi.v1 = static_cast<float>(penY+h) / atlasH;
                gi.pixelW   = static_cast<float>(w);
                gi.pixelH   = static_cast<float>(h);
                gi.bearingX = static_cast<float>(xoff);
                gi.bearingY = static_cast<float>(yoff);

                int adv, lsb;
                stbtt_GetCodepointHMetrics(&m_info, static_cast<int>(cp), &adv, &lsb);
                gi.advanceX = adv * scale;

                atlas.glyphs[cp] = gi;
                shelfH = std::max(shelfH, static_cast<uint32_t>(h));
                penX += static_cast<uint32_t>(w) + 1;
                stbtt_FreeBitmap(bmp, nullptr);
            }
        };

        packRange(32, 126);   // printable ASCII
        packRange(160, 255);  // Latin-1 supplement
        packRange(0x2018, 0x201F); // typographic quotes

        atlas.valid = true;
        qCInfo(Genesis::Log::Graphics) << "FontEngine: atlas " << atlasW << "x" << atlasH
                                        << " at " << pixelSize << "px, "
                                        << atlas.glyphs.size() << " glyphs\n";
        return atlas;
    }

    // ---- Text metrics ----

    /** Measure rendered pixel width of a UTF-8 string using the given atlas. */
    float measureWidth(const std::string& text, const FontAtlas& atlas) const {
        float x = 0.0f;
        for (unsigned char c : text) {
            auto it = atlas.glyphs.find(c);
            if (it != atlas.glyphs.end()) x += it->second.advanceX;
        }
        return x;
    }

private:
    bool           m_loaded{false};
    std::string    m_path;
    std::vector<uint8_t> m_fontData;
    stbtt_fontinfo m_info{};
};

} // namespace Genesis
