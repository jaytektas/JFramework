#pragma once

// jf::createPlatformWindow — construct the native window for the current OS without the
// caller pulling in xcb/win32 headers. Defined in src/platforms/PlatformFactory.cpp
// (compiled into the platform library), declared here against the abstract JPlatformWindow.

#include <j/core/ApplicationCore.h>   // jf::JPlatformWindow
#include <j/core/PlatformCommon.h>    // jf::JPlatformWindowStyle

#include <memory>
#include <string>

inline namespace jf {

std::unique_ptr<JPlatformWindow> createPlatformWindow(
    const std::string& title, uint32_t width, uint32_t height,
    int x = 100, int y = 100,
    JPlatformWindowStyle style = JPlatformWindowStyle::Normal);

}  // namespace jf
