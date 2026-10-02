// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// An image drawn through a mesh (JPrimitiveBuffer::pushImageMesh) on the software renderer, the
// reference for the GPU path: a texture mapped one-to-one comes out texel for texel, a mesh with
// its texture coordinates swapped mirrors it, and a single triangle leaves the rest untouched.
#include "../graphics/SoftwareGpuHal.h"

#include <cstdio>
#include <cstdlib>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

static int redAt(SoftwareGpuHal& hal, GpuSurfaceId sid, int x, int y) {
    uint32_t w = 0, h = 0;
    const std::vector<uint32_t>* px = hal.surfacePixels(sid, w, h);
    return px ? int(((*px)[size_t(y) * w + size_t(x)] >> 16) & 0xFF) : -1;
}

int main() {
    std::printf("=== image mesh ===\n");
    JNativeWindowHandle handle{};
    SoftwareGpuHal hal(handle);
    if (!hal.initialize()) { std::fprintf(stderr, "hal init failed\n"); return 1; }

    // 64 x 64, red rising 4 a texel left to right.
    constexpr int kN = 64;
    std::vector<uint8_t> rgba(kN * kN * 4);
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) {
            uint8_t* p = &rgba[size_t(y * kN + x) * 4];
            p[0] = uint8_t(x * 4); p[1] = 0; p[2] = 0; p[3] = 255;
        }
    const TextureHandle tex = hal.uploadTexture(rgba.data(), kN, kN);
    using V = JPrimitiveBuffer::JImageVertex;
    const float x0 = 100, y0 = 100, x1 = x0 + kN, y1 = y0 + kN;

    {   // One to one: texel for texel.
        JPrimitiveBuffer buf;
        buf.pushImageMesh(tex, { V{ x0, y0, 0, 0 }, V{ x1, y0, 1, 0 }, V{ x0, y1, 0, 1 },
                                 V{ x1, y0, 1, 0 }, V{ x1, y1, 1, 1 }, V{ x0, y1, 0, 1 } });
        auto frame = hal.beginFrame();
        hal.drawPrimitives(buf);
        bool exact = true;
        for (int i = 0; i < kN; i += 7) exact = exact && redAt(hal, frame.surfaceId, int(x0) + i, int(y0) + 20) == i * 4;
        check("a one-to-one mesh draws each texel where it is", exact);
    }
    {   // Mirrored: the texture coordinates swapped left to right.
        JPrimitiveBuffer buf;
        buf.pushImageMesh(tex, { V{ x0, y0, 1, 0 }, V{ x1, y0, 0, 0 }, V{ x0, y1, 1, 1 },
                                 V{ x1, y0, 0, 0 }, V{ x1, y1, 0, 1 }, V{ x0, y1, 1, 1 } });
        auto frame = hal.beginFrame();
        hal.drawPrimitives(buf);
        check("a mirrored mesh mirrors the picture",
              redAt(hal, frame.surfaceId, int(x0) + 1, int(y0) + 20) > 240 && redAt(hal, frame.surfaceId, int(x1) - 2, int(y0) + 20) < 12);
    }
    {   // One triangle (the top-left half): the other half is left as it was.
        JPrimitiveBuffer buf;
        buf.pushImageMesh(tex, { V{ x0, y0, 0, 0 }, V{ x1, y0, 1, 0 }, V{ x0, y1, 0, 1 } });
        auto frame = hal.beginFrame();
        const int before = redAt(hal, frame.surfaceId, int(x1) - 3, int(y1) - 3);
        hal.drawPrimitives(buf);
        check("outside its triangles a mesh draws nothing", redAt(hal, frame.surfaceId, int(x1) - 3, int(y1) - 3) == before);
        check("inside them it draws", redAt(hal, frame.surfaceId, int(x0) + 40, int(y0) + 3) == 160);
    }
    // Less than a triangle: nothing pushed.
    JPrimitiveBuffer none;
    none.pushImageMesh(tex, { V{ 0, 0, 0, 0 }, V{ 1, 0, 1, 0 } });
    check("fewer than three vertices push nothing", none.getCommands().empty());

    std::printf(fails ? "=== %d FAILED ===\n" : "=== image meshes draw what they say ===\n", fails);
    return fails ? 1 : 0;
}
