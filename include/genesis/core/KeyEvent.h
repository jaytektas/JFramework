#pragma once

#include <cstdint>

namespace Genesis {

/** Keyboard input event passed from platform to the app each frame. */
struct KeyEvent {
    enum class Key : uint32_t {
        Unknown = 0,
        Tab, BackTab,       // focus navigation
        Return, Space,      // activation
        Escape,
        Backspace, Delete,
        Left, Right, Up, Down,
        Home, End,
        A=65, B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,
        _0=48,_1,_2,_3,_4,_5,_6,_7,_8,_9,
    };
    Key      key{Key::Unknown};
    uint32_t keysym{0};
    char     utf8[8]{};     // UTF-8 text for printable characters
    bool     shift{false}, ctrl{false}, alt{false};
    bool     pressed{true}; // false on release
};

} // namespace Genesis
