#include <j/graphics/FontEngine.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_font_load_system() {
    JFontEngine engine;
    bool ok = engine.loadSystemFont();
    // System fonts should be available in CI; warn but don't assert if missing
    if (!ok) {
        std::cout << "test_font_load_system: no system font found (skipped)\n";
        return;
    }
    assert(engine.isLoaded());
    std::cout << "test_font_load_system: loaded " << engine.path() << "\n";
    std::cout << "test_font_load_system passed\n";
}

void test_atlas_build() {
    JFontEngine engine;
    if (!engine.loadSystemFont()) {
        std::cout << "test_atlas_build: skipped (no system font)\n";
        return;
    }
    auto atlas = engine.buildAtlas(14.0f, 512, 256);
    assert(atlas.valid);
    assert(!atlas.bitmap.empty());
    assert(atlas.bitmap.size() == 512u * 256u);
    assert(atlas.glyphs.count('A') || atlas.glyphs.count('a'));
    assert(atlas.lineHeight > 0.0f);
    std::cout << "test_atlas_build: " << atlas.glyphs.size() << " glyphs, "
              << "lineHeight=" << atlas.lineHeight << "\n";
    std::cout << "test_atlas_build passed\n";
}

void test_text_measure() {
    JFontEngine engine;
    if (!engine.loadSystemFont()) {
        std::cout << "test_text_measure: skipped\n";
        return;
    }
    auto atlas = engine.buildAtlas(14.0f);
    float w = engine.measureWidth("Hello", atlas);
    assert(w > 0.0f);
    // "Hello" should be wider than "Hi"
    float w2 = engine.measureWidth("Hi", atlas);
    assert(w > w2);
    std::cout << "test_text_measure: 'Hello'=" << w << " 'Hi'=" << w2 << "\n";
    std::cout << "test_text_measure passed\n";
}

int main() {
    test_font_load_system();
    test_atlas_build();
    test_text_measure();
    std::cout << "All JFontEngine tests passed!\n";
    return 0;
}
