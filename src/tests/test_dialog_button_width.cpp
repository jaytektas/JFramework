// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A dialog button is as wide as its label needs (JButton::labelWidth), never under the theme's
// buttonMinWidth, and a dialog's window widens so its whole button row fits: a long cancel label
// ("Choose Another File…") can no longer spill out of its button or off the window.
#include <j/core/JButton.h>
#include <j/platforms/NativeDialogWindow.h>

#include <cassert>
#include <string>

using namespace jf;

int main() {
    const float min = JStyle::current().buttonMinWidth;
    assert(min > 0.f);
    assert(JButton::dialogButtonWidth({ "OK" }) == min);

    const std::string longLabel = "Choose Another File and Then Some More Words";
    const float need = JButton::labelWidth(longLabel);
    assert(need > min);
    assert(JButton::dialogButtonWidth({ "Import", longLabel }) == need);

    JDialogRequest confirm;
    confirm.kind                = JDialogRequest::JKind::Confirm;
    confirm.options.okLabel     = "Import";
    confirm.options.cancelLabel = longLabel;
    const uint32_t w = JNativeDialogWindow::calcWidth(confirm);
    assert(w >= JNativeDialogWindow::kW);
    assert(float(w) >= 2.f * need);                     // both buttons, at the long label's width, fit

    JDialogRequest message;
    message.kind = JDialogRequest::JKind::Message;
    assert(JNativeDialogWindow::calcWidth(message) == JNativeDialogWindow::kW);   // short labels: the usual width
    return 0;
}
