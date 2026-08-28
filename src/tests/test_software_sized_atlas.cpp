// THE SOFTWARE RENDERER CAN HOLD A SIZE-SPECIFIC GLYPH ATLAS.
//
// JTextCall::atlasId selects which atlas a run of glyphs was laid out against: 0 is the base UI font,
// anything else a resident atlas baked at a particular pixel size. Only the Vulkan backend implemented
// them; the software one inherited the base class's "unsupported" (createFontAtlas returns 0), which had
// a consequence well beyond blurry large text — JTextHelper only keeps a baked atlas once the upload
// hands back an id, so every text draw at a non-base size re-rasterised the whole glyph set and dropped
// it. One studio session under software rendering rebuilt the same atlas 669 times.
//
// The two atlases here are deliberately unmistakable: the base is fully transparent and the sized one
// fully opaque, so the pixels say which one was sampled rather than merely that something was drawn.
//
//   cmake --build build --target test_software_sized_atlas && ./build/test_software_sized_atlas

#include "../graphics/SoftwareGpuHal.h"

#include <cstdio>
#include <vector>

using namespace jf;

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

// One quad covering (100,100)-(140,140), sampling the whole atlas.
static JPrimitiveBuffer::JTextCall quad(uint32_t atlasId) {
    JPrimitiveBuffer::JTextCall call;
    call.color[0] = call.color[1] = call.color[2] = call.color[3] = 255;
    call.atlasId = atlasId;
    call.verts = { {100.f, 100.f, 0.f, 0.f}, {140.f, 100.f, 1.f, 0.f}, {140.f, 140.f, 1.f, 1.f},
                   {100.f, 100.f, 0.f, 0.f}, {140.f, 140.f, 1.f, 1.f}, {100.f, 140.f, 0.f, 1.f} };
    return call;
}

static bool painted(SoftwareGpuHal& hal, uint32_t atlasId) {
    JPrimitiveBuffer buf;
    uint8_t black[4] = { 0, 0, 0, 255 };
    buf.pushRectangle(90.f, 90.f, 60.f, 60.f, black);          // a known ground to paint over
    buf.pushTextCall(quad(atlasId));
    const auto frame = hal.beginFrame();
    hal.drawPrimitives(buf);
    uint32_t w = 0, h = 0;
    const std::vector<uint32_t>* px = hal.surfacePixels(frame.surfaceId, w, h);
    if (!px) return false;
    const uint32_t p = (*px)[120 * w + 120];                    // the middle of the quad
    return ((p >> 16) & 0xFF) > 250 && ((p >> 8) & 0xFF) > 250 && (p & 0xFF) > 250;
}

int main() {
    std::printf("=== software sized atlas ===\n");
    JNativeWindowHandle handle{};
    SoftwareGpuHal hal(handle);
    if (!hal.initialize()) { std::fprintf(stderr, "hal init failed\n"); return 1; }

    const std::vector<uint8_t> clear(64 * 64, 0);               // base: draws nothing
    const std::vector<uint8_t> solid(64 * 64, 255);             // sized: draws everything
    hal.uploadFontAtlas(clear.data(), 64, 64);

    const uint32_t id = hal.createFontAtlas(solid.data(), 64, 64);
    check("a sized atlas gets a real id", id != 0, "id=" + std::to_string(id));

    const uint32_t id2 = hal.createFontAtlas(solid.data(), 64, 64);
    check("ids are distinct", id2 != 0 && id2 != id);

    check("glyphs draw from the atlas the call names", painted(hal, id));
    check("…and atlasId 0 still means the base atlas", !painted(hal, 0));

    // A freed atlas is a miss, and a miss must be skipped rather than silently drawn from the base —
    // the UVs belong to the atlas that laid the glyphs out, so the base would sample the wrong glyphs.
    hal.freeFontAtlas(id);
    check("a freed atlas draws nothing", !painted(hal, id));
    check("…and its sibling is untouched", painted(hal, id2));

    hal.freeFontAtlases();
    check("freeing them all leaves none", !painted(hal, id2));

    // An id nobody ever handed out is the same kind of miss, not a crash.
    check("an unknown id is harmless", !painted(hal, 4242));

    std::printf("%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
