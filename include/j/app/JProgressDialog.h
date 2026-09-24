// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JProgressDialog — "this will take a while, and here is how far it has got".
//
// Modal, but NOT a nested loop: JAppWindow::openModal renders it from the main frame loop, so whatever
// it is reporting on — a download, a transfer over a serial link — keeps running underneath. A dialog
// that pumped its own loop would stall the very work it was drawing.
//
// Driven from outside: the code watching the work calls setProgress()/setNote()/dismiss() through
// active(), because the modal stack owns the dialog, not the code that opened it.
//
//     win.openModal<JProgressDialog>("Downloading jscope 0.2.0", "jscope-0.2.0-x86_64.AppImage");
//     ... later, from the transfer's progress signal:
//     if (auto* d = JProgressDialog::active()) d->setProgress(got, total);

#include <j/app/JDialogWindow.h>
#include <j/core/JLabel.h>
#include <j/core/JProgressBar.h>
#include <j/core/JStyle.h>

#include <cstdio>
#include <memory>
#include <string>

inline namespace jf {

class JProgressDialog : public JDialogWindow {
public:
    static constexpr uint32_t kW = 460, kH = 150;   // openModal's contract: the size is known statically

    JProgressDialog(std::string title, std::string what,
                    JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : JDialogWindow(title, kW, kH, hal, sx, sy, parent)
    {
        const float cw = kW - 2 * pad();
        m_what = std::make_unique<JLabel>(graph(), what, cw);
        m_bar  = std::make_unique<JProgressBar>(graph(), cw, JStyle::current().progressHeight);
        m_note = std::make_unique<JLabel>(graph(), "starting\xE2\x80\xA6", cw);
        add(m_what.get()); add(m_bar.get()); add(m_note.get());
        s_active = this;
    }
    ~JProgressDialog() override { if (s_active == this) s_active = nullptr; }

    // The dialog currently on screen, or nullptr.
    static JProgressDialog* active() { return s_active; }

    // `total` <= 0: the size is not known, so only the count moves.
    void setProgress(long long bytes, long long total) {
        if (total > 0) m_bar->setProgress(float(double(bytes) / double(total)));
        char b[96];
        std::snprintf(b, sizeof(b), "%lld / %lld KB", bytes / 1024, total / 1024);
        m_note->setText(b);
    }
    void setNote(const std::string& s) { m_note->setText(s); }

    // The work is finished, one way or the other. It stops being active() AT ONCE, not when the modal
    // stack gets round to destroying it: a caller that dismisses one box and opens the next straight
    // after must get the new box, not the one on its way out.
    void dismiss() { if (s_active == this) s_active = nullptr; close(); }

protected:
    void layout(float w, float) override {
        const JStyle& st = JStyle::current();
        const float x = pad(), cw = w - 2 * pad(), gap = 3 * st.spacing;
        float y = contentTop();
        m_what->setBounds({ x, y, cw, st.labelHeight });    y += st.labelHeight + gap;
        m_bar ->setBounds({ x, y, cw, st.progressHeight }); y += st.progressHeight + gap;
        m_note->setBounds({ x, y, cw, st.labelHeight });
    }

private:
    static float pad() { return 2 * JStyle::current().fieldPadding; }

    inline static JProgressDialog* s_active = nullptr;
    std::unique_ptr<JLabel>       m_what, m_note;
    std::unique_ptr<JProgressBar> m_bar;
};

} // inline namespace jf
