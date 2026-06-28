#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PC {
    vec4  color;   // tint (1,1,1,1 = no tint)
    float vpW;
    float vpH;
    float pad0;
    float pad1;
} pc;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = vec4(
        (inPos.x / pc.vpW) * 2.0 - 1.0,
        (inPos.y / pc.vpH) * 2.0 - 1.0,
        0.0, 1.0);
    fragUV    = inUV;
    fragColor = pc.color;
}
