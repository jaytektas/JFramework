#pragma once

#include <cstdint>

inline namespace jf {

/** Keyboard input event passed from platform to the app each frame. */
struct JKeyEvent {
    enum class JKey : uint32_t {
        Unknown = 0,
        Tab, BackTab,       // focus navigation
        Return, Space,      // activation
        Escape,
        Backspace, Delete,
        Left, Right, Up, Down,
        Home, End,
        PageUp = 0xF0, PageDown,   // paging keys (explicit values so they never shift the letter/digit block below)
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,   // function keys (14..25 — before A=65)
        A=65, B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,
        _0=48,_1,_2,_3,_4,_5,_6,_7,_8,_9,
    };
    JKey      key{JKey::Unknown};
    uint32_t keysym{0};
    char     utf8[8]{};     // UTF-8 text for printable characters
    bool     shift{false}, ctrl{false}, alt{false};
    bool     pressed{true}; // false on release
};

// ---- Tab navigation, spelled ONCE ----------------------------------------------------------------------
// Shift+Tab does not arrive the same way on every platform: X11 sends its own keysym (XK_ISO_Left_Tab, mapped
// to JKey::BackTab), while Win32 (VK_TAB) and macOS (kVK_Tab) send plain Tab with the shift modifier set. Code
// that implements a focus domain must accept BOTH spellings, or backward navigation works on one platform and
// silently walks FORWARD on the others. Testing on Linux alone will not reveal it.
inline bool jIsTabNav(const JKeyEvent& ke) {
    return ke.key == JKeyEvent::JKey::Tab || ke.key == JKeyEvent::JKey::BackTab;
}
// +1 forward, -1 backward.
inline int jTabNavDir(const JKeyEvent& ke) {
    return (ke.key == JKeyEvent::JKey::BackTab || ke.shift) ? -1 : 1;
}

} // inline namespace jf
