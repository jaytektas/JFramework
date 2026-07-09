// no_aibus_test.cpp — proves the framework builds and drives widgets with the
// AI Control Bus feature fully removed.
//
//   g++ -std=c++20 -I<repo>/include tests/no_aibus_test.cpp -o /tmp/no_aibus_test

#include <j/core/BaseWidgets.h>
#include <j/core/FocusManager.h>
#include <j/app/JAppWindow.h>

#include <iostream>

int main() {
    using namespace jf;

    JSceneGraph graph;

    JLineEdit edit(graph);
    edit.setText("hello");

    JSpinBox spin(graph);
    spin.setValue(3);

    // Give the line edit focus and drive a key event through it.
    edit.setFocused(true);
    JKeyEvent ke{};
    ke.pressed = true;
    ke.utf8[0] = 'X';
    (void)edit.handleKeyEvent(ke);

    // Exercise the reference-resolution seam that outlived the AI bus.
    JVariant v = spin.getRef("value");
    (void)v;

    std::cout << "framework compiles + runs without AI bus: PASS\n";
    return 0;
}
