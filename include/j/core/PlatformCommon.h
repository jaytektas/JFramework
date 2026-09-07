// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

#include <cstdint>

inline namespace jf {

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

} // inline namespace jf
