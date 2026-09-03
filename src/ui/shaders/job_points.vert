#version 440

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inId;
layout(location = 2) in vec3 inColor;

layout(std140, binding = 0) uniform Camera {
    mat4 mvp;
    float selectedId;
    float hoveredId;
    vec2 viewportSize;
} camera;

layout(location = 0) out vec3 color;
layout(location = 1) out vec2 localPosition;

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(1.0,1.0),
        vec2(-1.0,-1.0), vec2(1.0,1.0), vec2(-1.0,1.0));
    bool selected = abs(inId - camera.selectedId) < 0.25;
    bool hovered = abs(inId - camera.hoveredId) < 0.25;
    float size = selected ? 7.0 : (hovered ? 5.5 : 3.0);
    vec4 clip = camera.mvp * vec4(inPosition, 1.0);
    vec2 corner = corners[gl_VertexIndex];
    clip.xy += corner * size * vec2(2.0 / camera.viewportSize.x,
                                     2.0 / camera.viewportSize.y) * clip.w;
    gl_Position = clip;
    localPosition = corner;
    color = selected ? vec3(1.0, 0.82, 0.0)
                     : (hovered ? vec3(1.0, 0.45, 0.12) : inColor);
}
