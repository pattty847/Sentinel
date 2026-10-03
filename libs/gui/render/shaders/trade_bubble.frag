#version 440
layout(location=0) in vec2 circle;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 fragColor;
layout(std140,binding=0) uniform buf { mat4 qt_Matrix; float qt_Opacity; } ubuf;
void main() {
    float d = length(circle);
    float aa = max(fwidth(d),0.001);
    fragColor = tint * (1.0-smoothstep(1.0-aa,1.0,d));
}
