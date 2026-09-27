// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// A window's FIRST focus. The dialog button row used to place its buttons only when it painted, so on the
// first frame — when a dialog gives its first focus — every button sat at (0,0) and the first one declared
// won the reading-order pick. Return on open then cancelled a form, discarded unsaved changes or started a
// firmware update. Now a dialog opens in its own content, and a dialog of buttons alone opens on its safe
// default: Accept, else Reject, never a destructive button.
#include <j/core/FocusManager.h>
#include <j/core/JLineEdit.h>
#include <j/core/JDialogButtonBox.h>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    using Role = jf::JDialogButtonBox::Role;

    std::puts("1. a form: the field, not the first-declared button");
    {
        jf::JSceneGraph g; jf::JFocusManager focus;
        jf::JLineEdit name(g, "");
        name.setBounds({ 10, 40, 300, 24 });
        jf::JDialogButtonBox box(g);
        jf::JButton* cancel = box.addButton("Cancel", Role::Reject, 90.f);
        box.addButton("Create", Role::Accept, 90.f);
        box.setBounds({ 10, 200, 300, 30 });
        check(cancel->bounds().y > 100.f, "the buttons are placed as soon as the row has bounds");
        focus.setFocusRoots({ &name, &box });
        focus.focusFirst();
        check(focus.focused() == &name, "the first focus is the name field");
    }

    std::puts("2. buttons only, Discard declared first: Save, never Discard");
    {
        jf::JSceneGraph g; jf::JFocusManager focus;
        jf::JDialogButtonBox box(g);
        box.addButton("Discard", Role::Destructive, 90.f);
        box.addButton("Cancel", Role::Reject, 90.f);
        jf::JButton* save = box.addButton("Save", Role::Accept, 90.f);
        box.setBounds({ 10, 200, 400, 30 });
        focus.setFocusRoots({ &box });
        focus.focusFirst();
        check(focus.focused() == save, "the first focus is the Accept button");
    }

    std::puts("3. a destructive action and a cancel: Cancel");
    {
        jf::JSceneGraph g; jf::JFocusManager focus;
        jf::JDialogButtonBox box(g);
        box.addButton("Update firmware", Role::Destructive, 150.f);
        jf::JButton* notNow = box.addButton("Not now", Role::Reject, 90.f);
        box.setBounds({ 10, 200, 400, 30 });
        focus.setFocusRoots({ &box });
        focus.focusFirst();
        check(focus.focused() == notNow, "the first focus is the Reject button");
    }

    std::puts("4. a disabled Accept: Reject");
    {
        jf::JSceneGraph g; jf::JFocusManager focus;
        jf::JDialogButtonBox box(g);
        jf::JButton* cancel = box.addButton("Cancel", Role::Reject, 90.f);
        jf::JButton* ok = box.addButton("OK", Role::Accept, 90.f);
        ok->setEnabled(false);
        box.setBounds({ 10, 200, 300, 30 });
        focus.setFocusRoots({ &box });
        focus.focusFirst();
        check(focus.focused() == cancel, "the first focus is Cancel while OK is greyed out");
    }

    std::puts("5. Tab still reaches the buttons");
    {
        jf::JSceneGraph g; jf::JFocusManager focus;
        jf::JLineEdit name(g, "");
        name.setBounds({ 10, 40, 300, 24 });
        jf::JDialogButtonBox box(g);
        jf::JButton* cancel = box.addButton("Cancel", Role::Reject, 90.f);
        box.addButton("OK", Role::Accept, 90.f);
        box.setBounds({ 10, 200, 300, 30 });
        focus.setFocusRoots({ &name, &box });
        focus.focusFirst();
        focus.nextFocus();
        check(focus.focused() == cancel, "Tab from the field goes to the row's first button");
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
