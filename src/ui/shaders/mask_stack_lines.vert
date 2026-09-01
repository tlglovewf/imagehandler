#version 440

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(std140, binding = 0) uniform Camera {
    mat4 mvp;
    vec4 origin;
    vec4 parameters;
    vec4 startColor;
    vec4 endColor;
    vec4 selection;
} camera;

layout(location = 0) out vec3 color;

void main() {
    vec3 position = inPosition;
    if (camera.selection.y > 0.5) position.z *= camera.parameters.w;
    position.z *= camera.parameters.x;
    position -= camera.origin.xyz;
    gl_Position = camera.mvp * vec4(position, 1.0);
    color = inColor;
}
