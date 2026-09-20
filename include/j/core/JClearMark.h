// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JClearMark — the framework's ONE clear/remove mark: the diagonal ✕ a clearable JLineEdit, a clearable
// JPickerField and a row's remove button all draw. JCloseButton is the other half of the pair and stays
// separate on purpose: that one is WINDOW chrome, a chip with a cross bar, and it means close the window.
// This one means unset this value / take this row away.
//
// It existed three times over before this, in three shapes. JLineEdit drew anti-aliased crossed lines on
// a JVectorCanvas; JPickerField stamped a seven-step loop of 2px squares; and two studio pages set a
// JButton's LABEL to U+2715, which the font atlas does not pack — so the mark came out of _substitute as
// '?' and a remove button read as a question. A mark is DRAWN, not written: what a control means must not
// depend on which codepoints a font happens to carry.
//
// Static, because the callers are a mix of widgets painting their own box and immediate-mode window
// chrome. `box` is the target area; the ✕ is centred in it and sized from it, so no caller invents a
// radius of its own and two marks side by side are the same mark.

#include "SceneGraph.h"
#include "../graphics/VectorGraphics.h"

inline namespace jf {

class JClearMark {
public:
    // Hover both brightens the ✕ and gives it a disc, so it reads as its own pressable target rather
    // than as decoration on the field it sits in.
    static void draw(JPrimitiveBuffer& buf, const JRect& box, JColor tint, bool hovered) {
        const float s  = (box.width < box.height) ? box.width : box.height;
        const float cx = box.x + box.width * 0.5f;
        const float cy = box.y + box.height * 0.5f;
        const float r  = s * 0.28f;

        if (hovered)
            buf.pushRectangle(cx - s * 0.5f, cy - s * 0.5f, s, s,
                              withAlpha(tint, 38).data(), s * 0.5f);

        JVectorCanvas vc;
        vc.setAntiAlias(1.0f);
        const JPaint p{withAlpha(tint, hovered ? 235 : 165)};
        vc.drawLine(cx - r, cy - r, cx + r, cy + r, 1.4f, p);
        vc.drawLine(cx + r, cy - r, cx - r, cy + r, 1.4f, p);
        vc.flush(buf);
    }
};

} // inline namespace jf
