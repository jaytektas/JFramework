#pragma once

#include <cstdint>

namespace Genesis {

enum class PlatformWindowStyle : uint8_t { 
    Normal, 
    Borderless, 
    Popup 
};

enum class PlatformCursor : uint8_t {
    Default,
    ResizeLeftRight,
    ResizeUpDown,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight
};

} // namespace Genesis
