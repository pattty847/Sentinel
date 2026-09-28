#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;

layout(location = 0) out vec2 v_texcoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;
    vec4 params2;
};

void main() {
    v_texcoord = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
