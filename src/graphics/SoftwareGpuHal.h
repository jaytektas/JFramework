#pragma once
#include <genesis/graphics/GpuHal.h>
#include <mutex>
#include <map>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#else
#include <xcb/xcb.h>
#endif

namespace Genesis {

struct SoftwareTexture {
    std::vector<uint32_t> pixels; // packed ARGB (same as SoftwareSurface format)
    uint32_t w{0}, h{0};
};

struct SoftwareSurface {
#if defined(_WIN32)
    HWND hwnd{nullptr};
#else
    xcb_connection_t* conn{nullptr};
    xcb_window_t wid{0};
#endif
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint32_t> pixels;
};

class SoftwareGpuHal : public GpuHal {
public:
    SoftwareGpuHal(const NativeWindowHandle& windowHandle)
        : m_mainHandle(windowHandle) {}

    ~SoftwareGpuHal() override {
        waitIdle();
    }

    bool initialize() override {
        m_nextSurfaceId = 1;
        
        SoftwareSurface surf;
        surf.width = 800;
        surf.height = 600;
        surf.pixels.assign(800 * 600, 0xFF1E1E23);
#if defined(_WIN32)
        surf.hwnd = static_cast<HWND>(m_mainHandle.windowPointer);
#else
        surf.conn = static_cast<xcb_connection_t*>(m_mainHandle.connectionPointer);
        surf.wid = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(m_mainHandle.windowPointer));
#endif
        m_surfaces[kPrimarySurface] = std::move(surf);
        return true;
    }

    void resizeSurface(GpuSurfaceId sid, uint32_t width, uint32_t height) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surfaces.find(sid);
        if (it != m_surfaces.end()) {
            it->second.width = width;
            it->second.height = height;
            it->second.pixels.assign(width * height, 0xFF1E1E23);
            qCInfo(Genesis::Log::Graphics) << "SoftwareGpuHal: resizeSurface sid=" << sid << " to " << width << "x" << height << "\n";
        }
    }

    GpuFrameContext beginFrame(GpuSurfaceId sid = kPrimarySurface) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        GpuFrameContext ctx{};
        ctx.frameIndex = m_frameIndex++;
        ctx.surfaceId = sid;
        m_activeSurfaceId = sid;
        return ctx;
    }

    bool uploadFontAtlas(const uint8_t* pixels, uint32_t w, uint32_t h) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fontAtlas.assign(pixels, pixels + (w * h));
        m_fontAtlasW = w;
        m_fontAtlasH = h;
        return true;
    }

    inline uint32_t packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
               (static_cast<uint32_t>(b));
    }

    inline uint32_t blend(uint32_t bg, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (a == 255) return packColor(r, g, b, 255);
        if (a == 0) return bg;
        uint8_t bg_r = (bg >> 16) & 0xFF;
        uint8_t bg_g = (bg >> 8) & 0xFF;
        uint8_t bg_b = bg & 0xFF;

        uint32_t out_r = (r * a + bg_r * (255 - a)) / 255;
        uint32_t out_g = (g * a + bg_g * (255 - a)) / 255;
        uint32_t out_b = (b * a + bg_b * (255 - a)) / 255;
        return packColor(out_r, out_g, out_b, 255);
    }

    void drawPrimitives(const PrimitiveBuffer& buffer) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surfaces.find(m_activeSurfaceId);
        if (it == m_surfaces.end()) {
            qCWarning(Genesis::Log::Graphics) << "SoftwareGpuHal: drawPrimitives on inactive surface " << m_activeSurfaceId << "\n";
            return;
        }

        SoftwareSurface& surf = it->second;
        if (surf.width == 0 || surf.height == 0) return;
        qCDebug(Genesis::Log::Graphics) << "SoftwareGpuHal: drawPrimitives surf=" << m_activeSurfaceId << ", commands=" << buffer.getCommands().size() << "\n";

        for (const auto& cmd : buffer.getCommands()) {
            int cx1 = 0, cy1 = 0, cx2 = surf.width, cy2 = surf.height;
            if (cmd.clip.enabled) {
                cx1 = std::max(0, static_cast<int>(cmd.clip.x));
                cy1 = std::max(0, static_cast<int>(cmd.clip.y));
                cx2 = std::min(static_cast<int>(surf.width), static_cast<int>(cmd.clip.x + cmd.clip.w));
                cy2 = std::min(static_cast<int>(surf.height), static_cast<int>(cmd.clip.y + cmd.clip.h));
            }

            if (cx1 >= cx2 || cy1 >= cy2) continue;

            if (cmd.kind == PrimitiveBuffer::DrawCommand::Kind::Rect) {
                float rx = cmd.rect.rectBounds[0];
                float ry = cmd.rect.rectBounds[1];
                float rw = cmd.rect.rectBounds[2];
                float rh = cmd.rect.rectBounds[3];
                float r  = cmd.rect.borderRadius;
                float bw = cmd.rect.borderWidth;

                // SDF rect center and half-extents (matches rect.frag sdRoundedBox inputs)
                float ocx = rx + rw * 0.5f, ocy = ry + rh * 0.5f;
                float hw  = rw * 0.5f,      hh  = rh * 0.5f;

                // Rasterise 1px beyond bounds for AA fringe
                int x1 = std::max(cx1, static_cast<int>(rx) - 1);
                int y1 = std::max(cy1, static_cast<int>(ry) - 1);
                int x2 = std::min(cx2, static_cast<int>(rx + rw) + 2);
                int y2 = std::min(cy2, static_cast<int>(ry + rh) + 2);

                uint8_t fr = cmd.rect.color[0], fg = cmd.rect.color[1],
                        fb = cmd.rect.color[2], fa = cmd.rect.color[3];
                uint8_t bcr = cmd.rect.borderColor[0], bcg = cmd.rect.borderColor[1],
                        bcb = cmd.rect.borderColor[2], bca = cmd.rect.borderColor[3];
                bool hasBorder = (bw > 0.f && bca > 0);

                for (int py = y1; py < y2; ++py) {
                    for (int px = x1; px < x2; ++px) {
                        // Pixel centre (matches GLSL fragLocalPos convention)
                        float lx = (px + 0.5f) - rx;
                        float ly = (py + 0.5f) - ry;
                        float cx = lx - hw, cy = ly - hh;

                        // sdRoundedBox — exact translation of the GLSL shader
                        float qx = std::abs(cx) - hw + r;
                        float qy = std::abs(cy) - hh + r;
                        float mqx = std::max(qx, 0.f), mqy = std::max(qy, 0.f);
                        float d = std::sqrt(mqx*mqx + mqy*mqy)
                                + std::min(std::max(qx, qy), 0.f) - r;

                        if (d > 0.5f) continue;   // outside — matches shader discard

                        // AA coverage: 1 - smoothstep(-0.5, 0.5, d)
                        float coverage = 1.0f - std::clamp(d + 0.5f, 0.f, 1.f);

                        // Border blend: smoothstep(-bw, -bw+1, d) → mix(fill, border, t)
                        float out_r, out_g, out_b, out_a;
                        if (hasBorder && d > -bw) {
                            float t = std::clamp(d + bw, 0.f, 1.f);
                            out_r = fr + t * (bcr - (int)fr);
                            out_g = fg + t * (bcg - (int)fg);
                            out_b = fb + t * (bcb - (int)fb);
                            out_a = fa + t * (bca - (int)fa);
                        } else {
                            out_r = fr; out_g = fg; out_b = fb; out_a = fa;
                        }

                        uint8_t a = static_cast<uint8_t>(out_a * coverage);
                        if (a == 0) continue;
                        uint32_t& dest = surf.pixels[py * surf.width + px];
                        dest = blend(dest,
                                     static_cast<uint8_t>(out_r),
                                     static_cast<uint8_t>(out_g),
                                     static_cast<uint8_t>(out_b), a);
                    }
                }
            } else if (cmd.kind == PrimitiveBuffer::DrawCommand::Kind::Image) {
                const auto& img = cmd.image;
                auto texIt = m_textures.find(img.tex);
                if (texIt == m_textures.end()) continue;
                const SoftwareTexture& tex = texIt->second;
                if (tex.w == 0 || tex.h == 0) continue;

                int ix1 = std::max(cx1, static_cast<int>(img.x));
                int iy1 = std::max(cy1, static_cast<int>(img.y));
                int ix2 = std::min(cx2, static_cast<int>(img.x + img.w));
                int iy2 = std::min(cy2, static_cast<int>(img.y + img.h));

                for (int py = iy1; py < iy2; ++py) {
                    float ty = (img.h > 0.001f) ? (py - img.y) / img.h : 0.f;
                    float v  = img.v0 + ty * (img.v1 - img.v0);
                    int texY = std::clamp(static_cast<int>(v * tex.h), 0, static_cast<int>(tex.h) - 1);

                    for (int px = ix1; px < ix2; ++px) {
                        float tx = (img.w > 0.001f) ? (px - img.x) / img.w : 0.f;
                        float u  = img.u0 + tx * (img.u1 - img.u0);
                        int texX = std::clamp(static_cast<int>(u * tex.w), 0, static_cast<int>(tex.w) - 1);

                        uint32_t p = tex.pixels[texY * tex.w + texX];
                        uint8_t tr = (p >> 16) & 0xFF;
                        uint8_t tg = (p >> 8)  & 0xFF;
                        uint8_t tb =  p         & 0xFF;
                        uint8_t ta = (p >> 24)  & 0xFF;

                        // Apply tint
                        tr = static_cast<uint8_t>((tr * img.tint[0]) / 255);
                        tg = static_cast<uint8_t>((tg * img.tint[1]) / 255);
                        tb = static_cast<uint8_t>((tb * img.tint[2]) / 255);
                        ta = static_cast<uint8_t>((ta * img.tint[3]) / 255);

                        if (ta == 0) continue;
                        uint32_t& dest = surf.pixels[py * surf.width + px];
                        dest = blend(dest, tr, tg, tb, ta);
                    }
                }
            } else if (cmd.kind == PrimitiveBuffer::DrawCommand::Kind::Text) {
                if (m_fontAtlas.empty()) continue;
                const auto& call = cmd.text;
                
                for (size_t i = 0; i < call.verts.size(); i += 6) {
                    if (i + 5 >= call.verts.size()) break;
                    float x1 = call.verts[i].x;
                    float y1 = call.verts[i].y;
                    float x2 = call.verts[i + 2].x;
                    float y2 = call.verts[i + 2].y;
                    
                    float u0 = call.verts[i].u;
                    float v0 = call.verts[i].v;
                    float u1 = call.verts[i + 2].u;
                    float v1 = call.verts[i + 2].v;

                    int drawX1 = std::max(cx1, static_cast<int>(std::floor(x1)));
                    int drawY1 = std::max(cy1, static_cast<int>(std::floor(y1)));
                    int drawX2 = std::min(cx2, static_cast<int>(std::ceil(x2)));
                    int drawY2 = std::min(cy2, static_cast<int>(std::ceil(y2)));

                    for (int py = drawY1; py < drawY2; ++py) {
                        float ty = (y2 - y1 > 0.001f) ? (py - y1) / (y2 - y1) : 0.f;
                        float v = v0 + ty * (v1 - v0);
                        int texY = std::clamp(static_cast<int>(v * m_fontAtlasH), 0, static_cast<int>(m_fontAtlasH) - 1);
                        
                        for (int px = drawX1; px < drawX2; ++px) {
                            float tx = (x2 - x1 > 0.001f) ? (px - x1) / (x2 - x1) : 0.f;
                            float u = u0 + tx * (u1 - u0);
                            int texX = std::clamp(static_cast<int>(u * m_fontAtlasW), 0, static_cast<int>(m_fontAtlasW) - 1);
                            
                            uint8_t alpha = m_fontAtlas[texY * m_fontAtlasW + texX];
                            if (alpha > 0) {
                                uint8_t blended_a = (static_cast<uint32_t>(call.color[3]) * alpha) / 255;
                                uint32_t& dest = surf.pixels[py * surf.width + px];
                                dest = blend(dest, call.color[0], call.color[1], call.color[2], blended_a);
                            }
                        }
                    }
                }
            }
        }
    }

    void submitAndPresentFrame(const GpuFrameContext& context) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_surfaces.find(context.surfaceId);
        if (it == m_surfaces.end()) return;

        SoftwareSurface& surf = it->second;
        if (surf.width == 0 || surf.height == 0) return;

#if defined(_WIN32)
        if (surf.hwnd) {
            HDC hdc = GetDC(surf.hwnd);
            if (hdc) {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = surf.width;
                bmi.bmiHeader.biHeight = -static_cast<int>(surf.height);
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                StretchDIBits(hdc, 0, 0, surf.width, surf.height,
                              0, 0, surf.width, surf.height,
                              surf.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                ReleaseDC(surf.hwnd, hdc);
            }
        }
#else
        if (surf.conn && surf.wid != 0) {
            xcb_gcontext_t gc = xcb_generate_id(surf.conn);
            xcb_create_gc(surf.conn, gc, surf.wid, 0, nullptr);
            xcb_put_image(surf.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, surf.wid, gc,
                          surf.width, surf.height, 0, 0, 0, 24,
                          surf.width * surf.height * 4, reinterpret_cast<const uint8_t*>(surf.pixels.data()));
            xcb_free_gc(surf.conn, gc);
            xcb_flush(surf.conn);
        }
#endif
    }

    void waitIdle() override {}

    GpuApiType getBackendType() const noexcept override { return GpuApiType::Software; }

    GpuSurfaceId createSurface(const NativeWindowHandle& window, uint32_t w, uint32_t h) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        GpuSurfaceId sid = m_nextSurfaceId++;
        SoftwareSurface surf;
        surf.width = w;
        surf.height = h;
        surf.pixels.assign(w * h, 0xFF1E1E23);

#if defined(_WIN32)
        surf.hwnd = static_cast<HWND>(window.windowPointer);
#else
        surf.conn = static_cast<xcb_connection_t*>(window.connectionPointer);
        surf.wid = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(window.windowPointer));
#endif

        m_surfaces[sid] = std::move(surf);
        qCInfo(Genesis::Log::Graphics) << "SoftwareGpuHal: createSurface sid=" << sid << " w=" << w << " h=" << h << "\n";
        return sid;
    }

    void destroySurface(GpuSurfaceId sid) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_surfaces.erase(sid);
        qCInfo(Genesis::Log::Graphics) << "SoftwareGpuHal: destroySurface sid=" << sid << "\n";
    }

    TextureHandle uploadTexture(const uint8_t* rgba, uint32_t w, uint32_t h) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        TextureHandle handle = m_nextTexHandle++;
        SoftwareTexture& tex = m_textures[handle];
        tex.w = w;
        tex.h = h;
        tex.pixels.resize(w * h);
        for (uint32_t i = 0; i < w * h; ++i) {
            tex.pixels[i] = packColor(rgba[i*4+0], rgba[i*4+1], rgba[i*4+2], rgba[i*4+3]);
        }
        return handle;
    }

    void releaseTexture(TextureHandle tex) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_textures.erase(tex);
    }

private:
    NativeWindowHandle m_mainHandle;
    std::mutex m_mutex;
    uint32_t m_frameIndex{0};
    uint32_t m_nextSurfaceId{1};
    GpuSurfaceId m_activeSurfaceId{0};
    std::map<GpuSurfaceId, SoftwareSurface> m_surfaces;

    std::vector<uint8_t> m_fontAtlas;
    uint32_t m_fontAtlasW{0};
    uint32_t m_fontAtlasH{0};

    std::unordered_map<TextureHandle, SoftwareTexture> m_textures;
    TextureHandle m_nextTexHandle{1};
};

} // namespace Genesis
