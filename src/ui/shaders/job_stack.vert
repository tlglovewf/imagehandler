#version 440

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inLayer;
layout(location = 3) in float inFaceKind;
layout(location = 4) in vec3 inInstancePosition;
layout(location = 5) in float inInstanceId;
layout(location = 6) in float inLayerCount;
layout(location = 7) in vec3 inInstanceScale;

layout(std140, binding = 0) uniform Camera {
    mat4 mvp;
    float selectedId;
    float hoveredId;
    vec2 viewportSize;
    float stackExplode;
} camera;

layout(location = 0) out vec3 color;

void main() {
    vec3 local = inPosition * inInstanceScale;
    if (inFaceKind > 1.5) local.z *= camera.stackExplode;
    gl_Position = camera.mvp * vec4(local + inInstancePosition, 1.0);

    float factor = clamp((inLayer - 1.0) / max(1.0, inLayerCount - 1.0), 0.0, 1.0);
    vec3 base = mix(vec3(0.10, 0.48, 1.0), vec3(0.12, 0.92, 0.38), factor);
    if (abs(inInstanceId - camera.selectedId) < 0.25)
        base = vec3(1.0, 0.78, 0.08);
    else if (abs(inInstanceId - camera.hoveredId) < 0.25)
        base = vec3(1.0, 0.45, 0.12);
    vec3 lightDirection = normalize(vec3(-0.35, -0.45, 0.82));
    float lighting = 0.42 + 0.58 * max(0.0, dot(normalize(inNormal), lightDirection));
    if (inFaceKind > 0.5 && inFaceKind < 1.5) lighting *= 0.72;
    color = base * lighting;
}
