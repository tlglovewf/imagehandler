#version 440

layout(location = 0) in vec3 color;
layout(location = 1) in float layerCoordinate;
layout(location = 2) in float faceKind;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 shaded = color;
    if (faceKind > 0.5 && faceKind < 1.5) {
        float distanceToLayer = min(fract(layerCoordinate), 1.0 - fract(layerCoordinate));
        float seam = 1.0 - smoothstep(0.0, max(fwidth(layerCoordinate) * 1.5, 0.008),
                                      distanceToLayer);
        shaded *= mix(1.0, 0.36, seam);
    }
    fragColor = vec4(shaded, 1.0);
}
