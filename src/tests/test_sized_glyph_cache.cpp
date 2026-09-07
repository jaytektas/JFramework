// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A SIZE THE BACKEND CANNOT HOLD IS ASKED FOR ONCE.
//
// JTextHelper bakes a glyph atlas at a requested pixel size and keeps it, so large text is drawn from
// real glyphs rather than an upscaled 14px base. The keeping was conditional on the GPU handing back an
// atlas id — and GpuHal::createFontAtlas() answers 0 by default, which every backend but Vulkan
// inherited. So on the software renderer the bake ran, the upload failed, the result was dropped, and
// the next text draw did it all again: one studio session rasterised the same 512x512 atlas 669 times.
//
// The answer "this size is not available here" is as worth caching as the atlas itself. These tests hold
// that: one bake per (face, size) whether the upload works or not.
//
//   cmake --build build --target test_sized_glyph_cache && ./build/test_sized_glyph_cache

#include <j/core/JTextHelper.h>
#include <j/graphics/FontEngine.h>

#include <cassert>
#include <iostream>

using namespace jf;

static int builds = 0;

// A bake that reports what it did, over a real font so the atlas is genuinely valid.
static void wire(bool uploadWorks) {
    JTextHelper::invalidateSized();
    builds = 0;
    JTextHelper::s_buildSized = [](const std::string&, float px) -> JFontAtlas {
        ++builds;
        JFontEngine e;
        if (!e.loadSystemFont()) return {};
        return e.buildAtlas(px, 512, 512);
    };
    JTextHelper::s_uploadSized = [uploadWorks](const JFontAtlas&) -> uint32_t {
        return uploadWorks ? 7u : 0u;                 // 0 = this backend has no sized atlases
    };
    JTextHelper::s_freeSized = [] {};
}

int main() {
    {   // A backend that CAN hold them: baked once, then served from the cache.
        wire(true);
        const auto* a = JTextHelper::sizedFor("", 24);
        const auto* b = JTextHelper::sizedFor("", 24);
        if (builds == 0) { std::cout << "skipped (no system font)\n"; return 0; }
        assert(a && b && a == b);
        assert(builds == 1);
        std::cout << "cached backend: " << builds << " bake for two asks\n";
    }

    {   // A backend that CANNOT: still one bake. The caller gets nullptr both times and falls back to
        // scaling the base atlas — the same behaviour as before, minus the work done over and over.
        wire(false);
        for (int i = 0; i < 5; ++i)
            assert(JTextHelper::sizedFor("", 24) == nullptr);
        assert(builds == 1);
        std::cout << "unsupported backend: " << builds << " bake for five asks\n";
    }

    {   // Distinct sizes are distinct entries — the negative answer is per (face, size), not global.
        wire(false);
        JTextHelper::sizedFor("", 24);
        JTextHelper::sizedFor("", 48);
        JTextHelper::sizedFor("", 24);
        assert(builds == 2);
        std::cout << "two sizes: " << builds << " bakes\n";
    }

    {   // …and a re-wire (new window, different backend) forgets them, because the new one may be able
        // to hold what the old one could not.
        wire(false);
        JTextHelper::sizedFor("", 24);
        wire(true);
        assert(JTextHelper::sizedFor("", 24) != nullptr);
        assert(builds == 1);
        std::cout << "after re-wire: available again\n";
    }

    std::cout << "test_sized_glyph_cache passed\n";
    return 0;
}
