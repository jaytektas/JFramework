// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JDialogButtonBox — the standard footer for a dialog with CUSTOM content.
//
// The framework's JDialog covers fixed kinds (Message / Confirm / Input / file pickers) and
// JNativeDialogWindow draws their footer, honouring JDialogOptions (okOnRight) and JDialogKeyBindings
// (Return accepts, Escape rejects). A dialog with bespoke content — a form, a table, an editor — could not
// use any of that, so each one re-implemented its own button row: different order, different placement,
// some with real JButtons and some with pushRectangle + a hit-test that fired on mouse-DOWN.
//
// This box is that footer, extracted once. A dialog declares BUTTONS BY ROLE and the box owns the rest:
//
//     box.addButton("Create", JDialogButtonBox::Role::Accept);   // Return activates it
//     box.addButton("Cancel", JDialogButtonBox::Role::Reject);   // Escape activates it
//     box.setBounds(footer);  box.populateRenderPrimitives(buf);
//
// Roles, not labels: "Create", "Save", "Open" ARE the right words for an accept button (Qt's HIG and the
// GNOME HIG both prefer a verb over a generic OK), so the box standardises ORDER, placement, keyboard and
// focus while leaving the wording to the dialog.
//
// The buttons are adopted, so they join the window's focus tree automatically: Tab reaches them, they show
// a focus ring, Space/Return activates, and a click completes on release-inside (press then slide off to
// cancel) because that is JControl's contract.

#include "JWidget.h"
#include "JButton.h"
#include "Dialog.h"        // JDialogOptions / JDialogKeyBindings

#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JDialogButtonBox : public JWidget {
public:
    // Accept      — the affirmative action (OK / Save / Create). Return activates it; at most one.
    // Reject      — the dismissive action (Cancel / Close). Escape activates it; at most one.
    // Destructive — a discarding action (Discard / Delete). Never the default; never bound to a key.
    // Action      — anything else (Apply, Add …), placed left of the standard pair.
    enum class Role { Accept, Reject, Destructive, Action };

    jf::JSignal<> onAccept;
    jf::JSignal<> onReject;

    explicit JDialogButtonBox(JSceneGraph& graph, JDialogOptions opts = {})
        : JWidget(graph, "JDialogButtonBox"), m_opts(opts) {}

    JButton* addButton(const std::string& label, Role role, float w = 84.f) {
        auto* b = adopt(std::make_unique<JButton>(m_graph, label, w, JStyle::current().buttonHeight));
        b->onClicked.connect([this, role] { _fire(role); });
        m_buttons.push_back({ b, role });
        return b;
    }

    // Natural width of the row, so a dialog can size/position its footer without knowing the layout rules.
    float naturalWidth() const {
        float w = 0.f;
        for (const auto& e : m_buttons) w += e.btn->bounds().width + kGap;
        return w > 0.f ? w - kGap : 0.f;
    }
    static float naturalHeight() { return JStyle::current().buttonHeight; }

    // Return accepts, Escape rejects — the bindings the framework already declares for dialogs, applied
    // here so every custom dialog gets them without repeating the wiring. Returns true if consumed.
    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        const JDialogKeyBindings kb;
        if (ke.key == kb.accept && _has(Role::Accept)) { _fire(Role::Accept); return true; }
        if (ke.key == kb.cancel && m_opts.closeOnEscape && _has(Role::Reject)) { _fire(Role::Reject); return true; }
        return false;
    }

    void handleMouseMove(float mx, float my) override    { for (auto& e : m_buttons) e.btn->handleMouseMove(mx, my); }
    void handleMousePress(float mx, float my) override   { for (auto& e : m_buttons) e.btn->handleMousePress(mx, my); }
    void handleMouseRelease(float mx, float my) override { for (auto& e : m_buttons) e.btn->handleMouseRelease(mx, my); }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        _layout();
        for (auto& e : m_buttons) e.btn->populateRenderPrimitives(buf);
    }

private:
    struct Entry { JButton* btn; Role role; };
    static constexpr float kGap = 8.f;

    bool _has(Role r) const {
        for (const auto& e : m_buttons) if (e.role == r) return true;
        return false;
    }
    void _fire(Role r) {
        if (r == Role::Accept) onAccept.emit();
        else if (r == Role::Reject) onReject.emit();
        // Destructive/Action buttons carry their own onClicked handlers from the caller.
    }

    // Right-aligned row. Order follows JDialogOptions::okOnRight — the platform convention lives in ONE
    // place instead of each dialog choosing for itself. Action/Destructive buttons sit left of the pair.
    void _layout() {
        const JRect b = bounds();
        std::vector<JButton*> tail;    // the standard pair, in final left-to-right order
        JButton *accept = nullptr, *reject = nullptr;
        std::vector<JButton*> lead;    // Action + Destructive, in declaration order
        for (auto& e : m_buttons) {
            if (e.role == Role::Accept) accept = e.btn;
            else if (e.role == Role::Reject) reject = e.btn;
            else lead.push_back(e.btn);
        }
        if (m_opts.okOnRight) { if (reject) tail.push_back(reject); if (accept) tail.push_back(accept); }
        else                  { if (accept) tail.push_back(accept); if (reject) tail.push_back(reject); }

        float total = 0.f;
        for (JButton* x : lead) total += x->bounds().width + kGap;
        for (JButton* x : tail) total += x->bounds().width + kGap;
        if (total > 0.f) total -= kGap;

        float x = b.x + b.width - total;          // right-aligned within the footer rect
        const float h = JStyle::current().buttonHeight;
        const float y = b.y + (b.height - h) * 0.5f;
        for (JButton* w : lead) { w->setBounds({ x, y, w->bounds().width, h }); x += w->bounds().width + kGap; }
        for (JButton* w : tail) { w->setBounds({ x, y, w->bounds().width, h }); x += w->bounds().width + kGap; }
    }

    JDialogOptions      m_opts;
    std::vector<Entry>  m_buttons;   // adopted -> owned here AND part of the focus tree
};

} // inline namespace jf
