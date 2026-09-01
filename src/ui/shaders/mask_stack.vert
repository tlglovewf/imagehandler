#version 440

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inLayer;
layout(location = 3) in float inFaceKind;

layout(std140, binding = 0) uniform Camera {
    mat4 mvp;
    vec4 origin;
    vec4 parameters;
    vec4 startColor;
    vec4 endColor;
    vec4 selection;
} camera;

layout(location = 0) out vec3 color;
layout(location = 1) out float layerCoordinate;
layout(location = 2) out float faceKind;

void main() {
    vec3 position = inPosition;
    if (inFaceKind > 1.5) position.z *= camera.parameters.w;
    position.z *= camera.parameters.x;
    position -= camera.origin.xyz;
    gl_Position = camera.mvp * vec4(position, 1.0);

    float denominator = max(1.0, camera.parameters.z - 1.0);
    float factor = clamp((inLayer - 1.0) / denominator, 0.0, 1.0);
    vec3 base = mix(camera.startColor.rgb, camera.endColor.rgb, factor);
    vec3 lightDirection = normalize(vec3(-0.35, -0.45, 0.82));
    float lighting = 0.42 + 0.58 * max(0.0, dot(normalize(inNormal), lightDirection));
    if (inFaceKind > 0.5 && inFaceKind < 1.5) lighting *= 0.72;
    if (abs(inLayer - camera.selection.x) < 0.45 && inFaceKind > 1.5)
        base = vec3(1.0, 0.78, 0.08);
    color = base * lighting;
    layerCoordinate = inPosition.z / max(0.000001, camera.parameters.y);
    faceKind = inFaceKind;
}
