#version 450

// Push constants layout exactly mirrors GpuPrimitiveInstance (48 bytes, std430)
layout(push_constant) uniform PC {
    vec4  rectBounds;        // offset  0: x, y, w, h in pixels
    uint  colorPacked;       // offset 16: fill RGBA8 little-endian
    uint  borderColorPacked; // offset 20: border RGBA8 little-endian
    float borderRadius;      // offset 24
    float borderWidth;       // offset 28
    uint  primitiveType;     // offset 32
    float vpW;               // offset 36: viewport width  (padding[0])
    float vpH;               // offset 40: viewport height (padding[1])
    float _pad;              // offset 44
} pc;

layout(location = 0) out vec2 fragLocalPos; // pixel offset within rect

void main() {
    // Two-triangle quad built purely from vertex index — no vertex buffer needed
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 1.0),
        vec2(0.0, 1.0),
        vec2(0.0, 0.0)
    );

    vec2 uv       = corners[gl_VertexIndex];
    vec2 pixelPos = pc.rectBounds.xy + uv * pc.rectBounds.zw;
    fragLocalPos  = uv * pc.rectBounds.zw;

    // Screen-space pixels → Vulkan NDC (Y points down in both)
    gl_Position = vec4(
        (pixelPos.x / pc.vpW) * 2.0 - 1.0,
        (pixelPos.y / pc.vpH) * 2.0 - 1.0,
        0.0, 1.0
    );
}
