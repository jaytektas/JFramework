// The line edit's clear button (JLineEdit::setClearButtonEnabled): an ✕ inside the right edge that
// empties a search box in one click.
//
// It is a BUTTON drawn inside a text field, so the ways it goes wrong are all about the two fighting
// over one press. Pinned here:
//   - it shows only when there is something to clear, and only where it is enabled
//   - a press on it empties the field and reports the change (a filter wired to onTextChanged resets)
//   - a press on it does NOT also place a caret or start a drag-selection in the text it just removed
//   - a press on the TEXT is untouched by any of this
//   - with the button off, that same spot is ordinary text again
//
// Compile:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party tests/lineedit_clear_test.cpp -o /tmp/le_clear_test
// Prints PASS/FAIL per case; exits non-zero on any failure.

#include "j/core/JLineEdit.h"
#include <cstdio>
#include <string>

using namespace jf;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[clear-btn] %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    JSceneGraph graph;
    JLineEdit ed(graph, "Filter…", 200.0f, 22.0f);
    ed.setBounds({0.0f, 0.0f, 200.0f, 22.0f});

    int changes = 0;
    std::string last;
    ed.onTextChanged.connect([&](const std::string& s) { ++changes; last = s; });

    // --- when it shows ------------------------------------------------------------------------------
    ed.setText("boost");
    check(!ed.clearButtonVisible(),        "no ✕ until the field opts in");
    ed.setClearButtonEnabled(true);
    check(ed.clearButtonVisible(),         "enabled + text → the ✕ is shown");
    ed.setText("");
    check(!ed.clearButtonVisible(),        "nothing to clear → no ✕ (it would be a dead target)");
    ed.setText("boost");
    ed.setReadOnly(true);
    check(!ed.clearButtonVisible(),        "a read-only field shows no ✕ — it could not honour it");
    ed.setReadOnly(false);

    // The ✕ sits inside the right edge, vertically centred. Press its centre.
    const float bx = 200.0f, by = 11.0f;
    const float clearX = bx - 6.0f - 7.0f;   // ≈ padding + half the box: comfortably inside the target

    // --- clicking it --------------------------------------------------------------------------------
    changes = 0;
    ed.handleMousePress(clearX, by);
    check(ed.text().empty(),               "a press on the ✕ empties the field");
    check(changes == 1 && last.empty(),    "…and reports it once via onTextChanged (a bound filter resets)");
    check(!ed.hasSelection(),              "…leaving no selection in the text it removed");
    ed.handleMouseRelease(clearX, by);

    // --- it must not double as a caret placement ----------------------------------------------------
    ed.setText("boost by gear");
    ed.handleMousePress(clearX, by);
    ed.handleMouseMove(clearX - 40.0f, by);      // a drag that WOULD select, had the press started one
    check(ed.text().empty(),               "the press cleared rather than starting a drag-selection");
    check(!ed.hasSelection(),              "…and dragging afterwards selects nothing (there is nothing)");
    ed.handleMouseRelease(clearX - 40.0f, by);

    // --- pressing the text is unaffected ------------------------------------------------------------
    ed.setText("boost by gear");
    changes = 0;
    ed.handleMousePress(20.0f, by);
    check(ed.text() == "boost by gear",    "a press on the TEXT leaves the value alone");
    check(changes == 0,                    "…and reports no change");
    ed.handleMouseRelease(20.0f, by);

    // --- switched off, the same spot is just text ---------------------------------------------------
    ed.setClearButtonEnabled(false);
    ed.handleMousePress(clearX, by);
    check(ed.text() == "boost by gear",    "with the ✕ off, that spot is ordinary text again");
    ed.handleMouseRelease(clearX, by);

    std::printf("\n[clear-btn] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
