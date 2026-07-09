// Headless test for the widget accessibility surface (JWidget::a11yNode() +
// the JA11yNotify change hook). Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/a11y_test.cpp -o /tmp/a11y_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/BaseWidgets.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <cstring>

using namespace jf;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static bool has(uint32_t flags, uint32_t bit) { return (flags & bit) != 0; }

int main() {
    JSceneGraph graph;

    // 1) JButton — role Button, name from label, focusable, not disabled.
    {
        JButton b(graph, "Save");
        JA11yNode n = b.a11yNode();
        check("button role == Button",       n.roleId == JA11yRole::Button);
        check("button name == label",        std::string(n.name) == "Save");
        check("button role string == JButton", std::string(n.role) == "JButton");
        check("button is focusable",         has(n.stateFlags, JA11yFocusable));
        check("button not disabled",         !has(n.stateFlags, JA11yDisabled));
    }

    // 2) JCheckBox — Checked flag flips with setChecked; tri-state -> Mixed.
    {
        JCheckBox c(graph, "Enable turbo");
        JA11yNode n0 = c.a11yNode();
        check("checkbox role == CheckBox",   n0.roleId == JA11yRole::CheckBox);
        check("checkbox name == label",      std::string(n0.name) == "Enable turbo");
        check("checkbox starts unchecked",   !has(n0.stateFlags, JA11yChecked));

        c.setChecked(true);
        JA11yNode n1 = c.a11yNode();
        check("checkbox Checked after setChecked(true)", has(n1.stateFlags, JA11yChecked));
        check("checkbox value == checked",   std::string(n1.value) == "checked");

        c.setChecked(false);
        check("checkbox Checked clears",     !has(c.a11yNode().stateFlags, JA11yChecked));

        c.setTristate(true);
        c.setCheckState(JCheckBox::PartiallyChecked);
        JA11yNode n2 = c.a11yNode();
        check("tri-state partial -> Mixed",  has(n2.stateFlags, JA11yMixed));
        check("tri-state partial not Checked", !has(n2.stateFlags, JA11yChecked));
        check("tri-state value == mixed",    std::string(n2.value) == "mixed");
    }

    // 3) JLineEdit Normal — Editable, value carries the text.
    {
        JLineEdit e(graph, "Name");   // placeholder = accessible name
        e.setText("Ada");
        JA11yNode n = e.a11yNode();
        check("lineedit role == TextField", n.roleId == JA11yRole::TextField);
        check("lineedit name == placeholder", std::string(n.name) == "Name");
        check("lineedit Editable",          has(n.stateFlags, JA11yEditable));
        check("lineedit not Protected",     !has(n.stateFlags, JA11yProtected));
        check("lineedit value == text",     std::string(n.value) == "Ada");
    }

    // 4) JLineEdit Password — Editable + Protected, value NOT leaked.
    {
        JLineEdit e(graph, "Password");
        e.setEchoMode(JLineEdit::Password);
        e.setText("hunter2");
        JA11yNode n = e.a11yNode();
        check("password Editable",          has(n.stateFlags, JA11yEditable));
        check("password Protected",         has(n.stateFlags, JA11yProtected));
        check("password value does NOT leak text",
              std::strstr(n.value, "hunter2") == nullptr && std::string(n.value).empty());
        // The plaintext is still held by the widget, just not exposed via a11y.
        check("password text still stored",  e.text() == "hunter2");
    }

    // 5) JSlider — value/min/max present in the numeric range.
    {
        JSlider s(graph);
        s.setValue(0.25f);
        JA11yNode n = s.a11yNode();
        check("slider role == Slider",       n.roleId == JA11yRole::Slider);
        check("slider hasRange",             n.hasRange);
        check("slider curValue == 0.25",     feq(n.curValue, 0.25f));
        check("slider minValue == 0",        feq(n.minValue, 0.0f));
        check("slider maxValue == 1",        feq(n.maxValue, 1.0f));
        check("slider value string == 0.25", std::string(n.value) == "0.25");
    }

    // 6) JSpinBox — value/min/max reflect the configured range.
    {
        JSpinBox sp(graph, -10, 50);
        sp.setValue(7);
        JA11yNode n = sp.a11yNode();
        check("spinbox role == SpinBox",     n.roleId == JA11yRole::SpinBox);
        check("spinbox curValue == 7",       feq(n.curValue, 7.0f));
        check("spinbox minValue == -10",     feq(n.minValue, -10.0f));
        check("spinbox maxValue == 50",      feq(n.maxValue, 50.0f));
        check("spinbox Editable",            has(n.stateFlags, JA11yEditable));
    }

    // 7) JProgressBar — read-only numeric range.
    {
        JProgressBar p(graph);
        p.setProgress(0.5f);
        JA11yNode n = p.a11yNode();
        check("progress role == ProgressBar", n.roleId == JA11yRole::ProgressBar);
        check("progress curValue == 0.5",    feq(n.curValue, 0.5f));
        check("progress ReadOnly",           has(n.stateFlags, JA11yReadOnly));
    }

    // 8) Disabled widget reports Disabled and drops Focusable-focus semantics.
    {
        JButton b(graph, "Off");
        b.setEnabled(false);
        JA11yNode n = b.a11yNode();
        check("disabled button Disabled",    has(n.stateFlags, JA11yDisabled));
    }

    // 9) Bounds mirror the widget geometry.
    {
        JButton b(graph, "Geo");
        b.setGeometry(JRect{12.f, 34.f, 100.f, 28.f});
        JA11yNode n = b.a11yNode();
        check("a11y bounds mirror geometry",
              feq(n.x, 12.f) && feq(n.y, 34.f) && feq(n.width, 100.f) && feq(n.height, 28.f));
    }

    // 10) The change-notify hook fires with the mutated widget.
    {
        JWidget* fired = nullptr;
        int count = 0;
        JWidget::s_a11yNotifyHook = [&](JWidget* w) { fired = w; ++count; };

        JSlider s(graph);
        int before = count;
        s.setValue(0.9f);                     // a real value change
        check("notify hook fired on value change", count == before + 1);
        check("notify hook carried the mutated widget", fired == &s);

        // A no-op change must NOT fire.
        int after = count;
        s.setValue(0.9f);                     // same value → clamped no-op
        check("notify hook silent on no-op change", count == after);

        // Checkbox toggle also notifies.
        JCheckBox c(graph, "Notify me");
        fired = nullptr;
        c.setChecked(true);
        check("notify hook fired on checkbox change", fired == &c);

        JWidget::s_a11yNotifyHook = nullptr;  // unhook (avoid dangling into later frames)
    }

    // 11) JComboBox — value is the current text.
    {
        JComboBox cb(graph, {"Red", "Green", "Blue"});
        cb.setCurrentIndex(2);
        JA11yNode n = cb.a11yNode();
        check("combo role == ComboBox",      n.roleId == JA11yRole::ComboBox);
        check("combo value == current text", std::string(n.value) == "Blue");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
