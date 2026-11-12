#version 440
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(location=3) in float halfLength;
layout(location=4) in vec4 ringColor;
layout(location=5) in float ringWidth;
layout(location=2) out float capsuleHalfLength;
layout(location=3) out vec4 ringTint;
layout(location=4) out float strokeWidth;
layout(location=0) out vec2 circle;
layout(location=1) out vec4 tint;
layout(std140,binding=0) uniform buf { mat4 qt_Matrix; float qt_Opacity; } ubuf;
void main() {
    circle = uv;
    ringTint = ringColor * ubuf.qt_Opacity;
    strokeWidth = ringWidth;
    capsuleHalfLength = halfLength;
    tint = color * ubuf.qt_Opacity;
    gl_Position = ubuf.qt_Matrix * vec4(position,0.0,1.0);
}
