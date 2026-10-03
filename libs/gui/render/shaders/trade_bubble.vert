#version 440
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(location=0) out vec2 circle;
layout(location=1) out vec4 tint;
layout(std140,binding=0) uniform buf { mat4 qt_Matrix; float qt_Opacity; } ubuf;
void main() {
    circle = uv;
    tint = color * ubuf.qt_Opacity;
    gl_Position = ubuf.qt_Matrix * vec4(position,0.0,1.0);
}
