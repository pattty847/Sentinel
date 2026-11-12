#version 440
layout(location=0) in vec2 circle;
layout(location=1) in vec4 tint;
layout(location=2) in float capsuleHalfLength;
layout(location=3) in vec4 ringTint;
layout(location=4) in float strokeWidth;
layout(location=0) out vec4 fragColor;
layout(std140,binding=0) uniform buf { mat4 qt_Matrix; float qt_Opacity; } ubuf;
void main() {
    float d = length(vec2(circle.x, max(abs(circle.y) - capsuleHalfLength, 0.0)));
    float aa = max(fwidth(d),0.001);
    // Width is normalized from logical pixels by the CPU radius. DPR scales
    // the whole geometry; fwidth only controls device-pixel antialiasing.
    float ring = strokeWidth > 0.0 ? smoothstep(1.0-strokeWidth-aa, 1.0-strokeWidth, d) : 0.0;
    fragColor = mix(tint, ringTint, ring) * (1.0-smoothstep(1.0-aa,1.0,d));
}
