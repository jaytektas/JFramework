#pragma once

#include <cstdint>

namespace Genesis {

enum class JPlatformWindowStyle : uint8_t { 
    Normal, 
    Borderless, 
    Popup 
};

enum class JPlatformCursor : uint8_t {
    Default,
    ResizeLeftRight,
    ResizeUpDown,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight
};

} // namespace Genesis
