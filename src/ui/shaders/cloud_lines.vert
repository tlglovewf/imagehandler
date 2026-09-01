#version 440

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inOverlay;

layout(std140, binding = 0) uniform Camera {
    mat4 mvp;
    float pixelScale;
    float maximumHeight;
    float originX;
    float originY;
    float originZ;
    float invertHeight;
    vec2 viewportSize;
    vec4 startColor;
    vec4 endColor;
} camera;

layout(location = 0) out vec3 color;

void main() {
    gl_Position = inOverlay > 0.5 ? vec4(inPosition, 1.0)
                                  : camera.mvp * vec4(inPosition, 1.0);
    color = inColor;
}
