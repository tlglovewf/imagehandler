#version 440

layout(location = 0) in vec3 color;
layout(location = 1) in vec2 localPosition;
layout(location = 0) out vec4 fragColor;

void main() {
    float radius = dot(localPosition, localPosition);
    if (radius > 1.0) discard;
    float edge = 1.0 - smoothstep(0.72, 1.0, radius);
    fragColor = vec4(color, edge);
}
