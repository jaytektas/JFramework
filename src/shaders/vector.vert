#version 450

// Anti-aliased vector geometry: per-vertex position + colour (uv reserved).
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;   // from R8G8B8A8_UNORM (normalised)

layout(push_constant) uniform PC {
    float vpW;
    float vpH;
    float pad0;
    float pad1;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    gl_Position = vec4(
        (inPos.x / pc.vpW) * 2.0 - 1.0,
        (inPos.y / pc.vpH) * 2.0 - 1.0,
        0.0, 1.0);
    fragColor = inColor;
    fragUV    = inUV;
}
