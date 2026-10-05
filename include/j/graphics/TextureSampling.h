// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

inline namespace jf {

// How an image is sampled when it is drawn larger or smaller than it is
// (JGpuHal::uploadTexture): Smooth blends neighbouring pixels; Sharp shows
// each pixel as a hard-edged block (nearest neighbour), as a pixel inspector
// or a camera view zoomed in wants.
enum class JTextureSampling { Smooth, Sharp };

} // inline namespace jf
