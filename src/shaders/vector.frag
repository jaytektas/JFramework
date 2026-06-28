#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    // Straight-alpha colour; the AA fringe carries alpha 0 at outer edges.
    outColor = fragColor;
}
