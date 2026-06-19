#version 450

layout(location = 0) in vec2 fragLocalPos;

layout(push_constant) uniform PC {
    vec4  rectBounds;
    uint  colorPacked;
    uint  borderColorPacked;
    float borderRadius;
    float borderWidth;
    uint  primitiveType;
    float vpW;
    float vpH;
    float _pad;
} pc;

layout(location = 0) out vec4 outColor;

vec4 unpackRGBA(uint p) {
    return vec4(float(p & 0xFFu), float((p >> 8u) & 0xFFu),
                float((p >> 16u) & 0xFFu), float((p >> 24u) & 0xFFu)) / 255.0;
}

// 2D SDF for a rounded rectangle centered at origin, half-extents b, radius r
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec4 fill   = unpackRGBA(pc.colorPacked);
    vec4 border = unpackRGBA(pc.borderColorPacked);

    vec2 halfExt = pc.rectBounds.zw * 0.5;
    vec2 center  = fragLocalPos - halfExt;

    float d = sdRoundedBox(center, halfExt, pc.borderRadius);

    // Discard fully-outside pixels (avoids overdraw and handles rectangles with no radius)
    if (d > 0.5) discard;

    // Anti-aliased shape coverage
    float coverage = 1.0 - smoothstep(-0.5, 0.5, d);

    // Inset border: d in range (-borderWidth, 0) → border zone
    vec4 c = fill;
    if (pc.borderWidth > 0.0 && border.a > 0.0 && d > -pc.borderWidth) {
        float t = smoothstep(-pc.borderWidth, -pc.borderWidth + 1.0, d);
        c = mix(fill, border, t);
    }

    outColor = vec4(c.rgb, c.a * coverage);
}
