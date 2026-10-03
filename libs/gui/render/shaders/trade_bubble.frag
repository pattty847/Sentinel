#version 440
layout(location=0) in vec2 circle;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 fragColor;
layout(std140,binding=0) uniform buf { mat4 qt_Matrix; float qt_Opacity; } ubuf;
void main() {
    float d = length(circle);
    float aa = max(fwidth(d),0.001);
    // The UV derivative gives one screen pixel; keep the outer pixel darker
    // without increasing opacity or obscuring the heatmap beneath the circle.
    float pixel = max(length(vec2(dFdx(d), dFdy(d))), 0.001);
    float ring = smoothstep(1.0-1.5*pixel, 1.0-0.5*pixel, d);
    fragColor = vec4(tint.rgb * mix(1.0, 0.45, ring), tint.a)
                * (1.0-smoothstep(1.0-aa,1.0,d));
}
