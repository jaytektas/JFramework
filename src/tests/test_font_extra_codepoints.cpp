// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JFontEngine::addCodepoints: a language's letters beyond the built-in ranges
// (Cyrillic, Chinese) packed into the atlas, made taller to hold them; an
// atlas built without asking stays as it was.
#undef NDEBUG
#include <j/graphics/FontEngine.h>
#include <cassert>
#include <iostream>

using namespace jf;

int main() {
    JFontEngine engine;
    if (!engine.loadSystemFont()) {
        std::cout << "no system font: skipped\n";
        return 0;
    }
    const JFontAtlas plain = engine.buildAtlas(14.0f);
    assert(plain.valid && plain.height == 256 && !plain.glyphs.count(0x0416));
    // Cyrillic Zhe and the Chinese for "middle", and a run of Cyrillic.
    std::vector<uint32_t> cps = { 0x0416, 0x4E2D };
    for (uint32_t cp = 0x0410; cp <= 0x044F; ++cp) cps.push_back(cp);
    JFontEngine::addCodepoints(cps);
    const JFontAtlas extra = engine.buildAtlas(14.0f);
    assert(extra.valid && extra.height > 256);
    assert(extra.glyphs.count(0x0416) && extra.glyphs.at(0x0416).pixelW > 0);   // DejaVu has Cyrillic
    std::cout << "Chinese glyph " << (extra.glyphs.count(0x4E2D) ? "packed" : "not available on this system") << "\n";
    std::cout << "test_font_extra_codepoints passed: " << extra.width << "x" << extra.height << ", " << extra.glyphs.size()
              << " glyphs\n";
    return 0;
}
