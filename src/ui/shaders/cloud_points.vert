#version 440

layout(location = 0) in vec2 inPixel;
layout(location = 1) in float inGray;

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
layout(location = 1) out vec2 localPosition;

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(1.0,1.0),
        vec2(-1.0,-1.0), vec2(1.0,1.0), vec2(-1.0,1.0));
    float heightFactor = mix(inGray, 1.0 - inGray, camera.invertHeight);
    vec3 position = vec3(inPixel.x * camera.pixelScale - camera.originX,
                         inPixel.y * camera.pixelScale - camera.originY,
                         heightFactor * camera.maximumHeight - camera.originZ);
    vec4 clip = camera.mvp * vec4(position, 1.0);
    vec2 corner = corners[gl_VertexIndex];
    clip.xy += corner * 2.0 * vec2(2.0 / camera.viewportSize.x,
                                   2.0 / camera.viewportSize.y) * clip.w;
    gl_Position = clip;
    localPosition = corner;
    color = mix(camera.startColor.rgb, camera.endColor.rgb, clamp(inGray, 0.0, 1.0));
}
