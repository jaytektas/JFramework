#include <j/core/JButton.h>
#include <j/core/Animator.h>
#include <j/core/Splitter.h>
#include <j/core/ImageWidget.h>
#include <j/platform/Clipboard.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/GpuHal.h>
#include <cassert>
#include <iostream>
#include <cstring>
#include <unordered_map>

using namespace jf;
using namespace jf;

// ---------------------------------------------------------------------------
// Minimal mock HAL for texture tests
// ---------------------------------------------------------------------------
class MockHal : public JGpuHal {
public:
    bool initialize() override { return true; }
    void resizeSurface(GpuSurfaceId, uint32_t, uint32_t) override {}
    JGpuFrameContext beginFrame(GpuSurfaceId sid) override { JGpuFrameContext c; c.surfaceId = sid; return c; }
    bool uploadFontAtlas(const uint8_t*, uint32_t, uint32_t) override { return true; }
    void drawPrimitives(const JPrimitiveBuffer&) override {}
    void submitAndPresentFrame(const JGpuFrameContext&) override {}
    void waitIdle() override {}
    JGpuApiType getBackendType() const noexcept override { return JGpuApiType::Software; }
    GpuSurfaceId createSurface(const JNativeWindowHandle&, uint32_t, uint32_t) override { return 1; }
    void destroySurface(GpuSurfaceId) override {}

    TextureHandle uploadTexture(const uint8_t*, uint32_t w, uint32_t h) override {
        TextureHandle h2 = m_next++;
        m_textures[h2] = {w, h};
        return h2;
    }
    void releaseTexture(TextureHandle tex) override { m_textures.erase(tex); }

    bool hasTexture(TextureHandle tex) const { return m_textures.count(tex) > 0; }
    size_t textureCount() const { return m_textures.size(); }

private:
    struct TexInfo { uint32_t w, h; };
    std::unordered_map<TextureHandle, TexInfo> m_textures;
    TextureHandle m_next{1};
};

// ---------------------------------------------------------------------------
// JAnimator
// ---------------------------------------------------------------------------

void test_animator_linear() {
    JAnimatedFloat af(0.0f);
    af.animateTo(100.0f, 200.0f, JEasing::Linear); // 200ms

    bool changed = af.advance(0.1f); // 100ms = halfway
    assert(changed);
    assert(af.current() >= 49.0f && af.current() <= 51.0f);
    assert(!af.isDone());

    af.advance(0.1f); // another 100ms = done
    assert(af.isDone());
    assert(af.current() == 100.0f);
    std::cout << "test_animator_linear passed\n";
}

void test_animator_snap() {
    JAnimatedFloat af(5.0f);
    af.set(42.0f);
    assert(af.current() == 42.0f);
    assert(af.isDone());
    std::cout << "test_animator_snap passed\n";
}

void test_animator_easing_values() {
    // Test key easing properties: t=0 → 0, t=1 → 1
    for (auto e : {JEasing::Linear, JEasing::EaseIn, JEasing::EaseOut, JEasing::EaseInOut,
                   JEasing::EaseInCubic, JEasing::EaseOutCubic, JEasing::EaseInOutCubic,
                   JEasing::EaseOutElastic, JEasing::EaseInBounce, JEasing::EaseOutBounce}) {
        float v0 = applyEasing(0.0f, e);
        float v1 = applyEasing(1.0f, e);
        assert(std::abs(v0) < 0.001f);
        assert(std::abs(v1 - 1.0f) < 0.001f);
    }
    std::cout << "test_animator_easing_values passed\n";
}

void test_animated_color() {
    JAnimatedColor c(255, 0, 0, 255);   // red
    uint8_t target[4] = {0, 0, 255, 255}; // blue
    c.animateTo(target, 100.0f, JEasing::Linear);

    c.advance(0.05f); // half way
    uint8_t out[4];
    c.fill(out);
    assert(out[0] > 100 && out[0] < 155); // r moving from 255→0
    assert(out[2] > 100 && out[2] < 155); // b moving from 0→255
    assert(!c.isDone());
    std::cout << "test_animated_color passed\n";
}

void test_animator_manager() {
    JAnimator anim;
    size_t a = anim.add(0.0f);
    size_t b = anim.add(100.0f);

    anim.animateTo(a, 50.0f, 100.0f);
    anim.animateTo(b, 0.0f, 100.0f);

    anim.advance(0.05f);
    assert(anim.value(a) > 0.0f && anim.value(a) < 50.0f);
    assert(anim.value(b) > 0.0f && anim.value(b) < 100.0f);
    assert(!anim.isDone());

    anim.advance(0.1f);
    assert(anim.isDone());
    assert(anim.value(a) == 50.0f);
    assert(anim.value(b) == 0.0f);

    assert(anim.value(999) == 0.0f); // out of range → 0
    std::cout << "test_animator_manager passed\n";
}

// ---------------------------------------------------------------------------
// JClipboard (smoke test — skip if xclip/xsel not installed)
// ---------------------------------------------------------------------------

void test_clipboard_roundtrip() {
    // Use a unique string to avoid interference from the OS clipboard.
    const std::string text = "genesis-clipboard-test-42";
    JClipboard::setText(text);
    std::string got = JClipboard::getText();
    // If clipboard tools are installed the round-trip should work.
    // If not (CI without xclip/xsel), getText() returns "" — that's acceptable.
    if (!got.empty())
        assert(got == text);
    std::cout << "test_clipboard_roundtrip passed"
              << (got.empty() ? " (no clipboard tool — skipped check)" : "") << "\n";
}

// ---------------------------------------------------------------------------
// JSplitter
// ---------------------------------------------------------------------------

void test_splitter_fractions() {
    JSceneGraph graph;
    JSplitter split(graph, JSplitter::JOrientation::Horizontal, 600.0f, 400.0f);

    // Create two dummy widget nodes to act as panes.
    JButton pane1(graph, "Pane1", 300.0f, 400.0f);
    JButton pane2(graph, "Pane2", 300.0f, 400.0f);

    split.addPane(&pane1);
    split.addPane(&pane2);

    const auto& panes = split.panes();
    assert(panes.size() == 2);
    // Fractions should sum to ~1.0
    float sum = panes[0].fraction + panes[1].fraction;
    assert(std::abs(sum - 1.0f) < 0.001f);
    std::cout << "test_splitter_fractions passed\n";
}

void test_splitter_layout() {
    JSceneGraph graph;
    // Set splitter's own bounding box via layout
    JSplitter split(graph, JSplitter::JOrientation::Horizontal, 600.0f, 400.0f);
    {
        auto& l = graph.getLayout(split.getNodeId());
        l.boundingBox = {0, 0, 600, 400};
    }

    JButton pane1(graph, "A", 10.0f, 10.0f);
    JButton pane2(graph, "B", 10.0f, 10.0f);
    split.addPane(&pane1, 0.4f);
    split.addPane(&pane2, 0.6f);
    split.layout();

    // pane1 should occupy 40% of (600 - 5) = ~238px
    const auto& bb1 = graph.getLayoutConst(pane1.getNodeId()).boundingBox;
    const auto& bb2 = graph.getLayoutConst(pane2.getNodeId()).boundingBox;
    assert(bb1.x == 0.0f);
    assert(bb1.width > 230.0f && bb1.width < 242.0f);
    assert(bb2.x > 238.0f); // starts after pane1 + divider
    assert(bb1.height == 400.0f);
    std::cout << "test_splitter_layout passed\n";
}

// ---------------------------------------------------------------------------
// RenderPrimitive — Image
// ---------------------------------------------------------------------------

void test_renderprimitive_image() {
    JPrimitiveBuffer buf;

    // pushImage with null handle should not add a command.
    buf.pushImage(0, 0, 100, 100, kNullTexture);
    assert(buf.getCommands().empty());

    // pushImage with a valid handle adds one command.
    TextureHandle fake = 1;
    buf.pushImage(10, 20, 200, 150, fake);
    assert(buf.getCommands().size() == 1);
    const auto& cmd = buf.getCommands()[0];
    assert(cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Image);
    assert(cmd.image.tex == fake);
    assert(cmd.image.x == 10.0f);
    assert(cmd.image.y == 20.0f);
    assert(cmd.image.w == 200.0f);
    assert(cmd.image.h == 150.0f);
    // Default tint is white
    assert(cmd.image.tint[0] == 255 && cmd.image.tint[3] == 255);
    // Default UV rect is 0,0,1,1
    assert(cmd.image.u0 == 0.0f && cmd.image.v1 == 1.0f);
    std::cout << "test_renderprimitive_image passed\n";
}

void test_renderprimitive_image_tint() {
    JPrimitiveBuffer buf;
    const uint8_t red[4] = {255, 0, 0, 200};
    buf.pushImage(0, 0, 50, 50, 1u, red, 0.1f, 0.2f, 0.8f, 0.9f);
    assert(buf.getCommands().size() == 1);
    const auto& img = buf.getCommands()[0].image;
    assert(img.tint[0] == 255 && img.tint[1] == 0);
    assert(img.tint[3] == 200);
    assert(img.u0 == 0.1f && img.v0 == 0.2f);
    assert(img.u1 == 0.8f && img.v1 == 0.9f);
    std::cout << "test_renderprimitive_image_tint passed\n";
}

// ---------------------------------------------------------------------------
// JGpuHal texture interface
// ---------------------------------------------------------------------------

void test_hal_texture_interface() {
    MockHal hal;

    uint8_t rgba[16] = {
        255,   0,   0, 255,
          0, 255,   0, 255,
          0,   0, 255, 255,
        255, 255, 255, 255,
    };

    TextureHandle tex = hal.uploadTexture(rgba, 2, 2);
    assert(tex != kNullTexture);
    assert(hal.hasTexture(tex));
    assert(hal.textureCount() == 1);

    hal.releaseTexture(tex);
    assert(!hal.hasTexture(tex));
    assert(hal.textureCount() == 0);
    std::cout << "test_hal_texture_interface passed\n";
}

void test_image_widget_render() {
    MockHal hal;
    uint8_t rgba[4] = {255, 128, 0, 255};
    TextureHandle tex = hal.uploadTexture(rgba, 1, 1);

    JSceneGraph graph;
    JImageWidget img(graph, tex, 100.0f, 80.0f);
    {
        auto& l = graph.getLayout(img.getNodeId());
        l.boundingBox = {10, 20, 100, 80};
    }

    JPrimitiveBuffer buf;
    img.populateRenderPrimitives(buf);
    assert(buf.getCommands().size() == 1);
    const auto& cmd = buf.getCommands()[0];
    assert(cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Image);
    assert(cmd.image.tex == tex);
    assert(cmd.image.x == 10.0f && cmd.image.y == 20.0f);
    assert(cmd.image.w == 100.0f && cmd.image.h == 80.0f);

    img.releaseTexture(hal);
    assert(!hal.hasTexture(tex));
    std::cout << "test_image_widget_render passed\n";
}

// ---------------------------------------------------------------------------

int main() {
    test_animator_linear();
    test_animator_snap();
    test_animator_easing_values();
    test_animated_color();
    test_animator_manager();

    test_clipboard_roundtrip();

    test_splitter_fractions();
    test_splitter_layout();

    test_renderprimitive_image();
    test_renderprimitive_image_tint();

    test_hal_texture_interface();
    test_image_widget_render();

    std::cout << "All new-feature tests passed!\n";
    return 0;
}
