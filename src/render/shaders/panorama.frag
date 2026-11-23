#version 440

layout(location = 0) in vec3 viewRay;
layout(location = 0) out vec4 color;

layout(std140, binding = 0) uniform Panorama {
    mat4 clipMatrix;
    mat4 cameraMatrix;
    vec4 lens;
    vec4 atlasInfo;
    vec4 levels[8];
} panorama;

layout(binding = 1) uniform sampler2D previewTexture;
layout(binding = 2) uniform sampler2D tileAtlas;
layout(binding = 3) uniform sampler2D pageTable;

void main() {
    const float pi = 3.14159265358979323846;
    // A camera ray is linear across this quad, so only the spherical angles
    // need to be computed per fragment. atan also works with an unnormalized ray.
    float horizontalLength = length(viewRay.xz);
    float longitude = horizontalLength > 1e-6 ? atan(viewRay.x, viewRay.z) : 0.0;
    float latitude = atan(viewRay.y, max(horizontalLength, 1e-6));
    vec2 location = vec2(fract(0.5 + longitude / (2.0 * pi)),
                         clamp(0.5 - latitude / pi, 0.0, 1.0));
    int count = int(panorama.atlasInfo.w);
    if (count > 0) {
        vec2 dx = dFdx(location);
        vec2 dy = dFdy(location);
        dx.x -= floor(dx.x + 0.5);
        dy.x -= floor(dy.x + 0.5);
        vec2 sourceSize = panorama.levels[0].xy;
        float footprint = max(length(dx * sourceSize), length(dy * sourceSize));
        int first = int(max(panorama.atlasInfo.z, floor(log2(max(1.0, footprint)))));
        for (int level = first; level < count; ++level) {
            vec4 detail = panorama.levels[level];
            vec2 pixel = min(location * detail.xy, detail.xy - 0.001);
            ivec2 tile = ivec2(pixel / 512.0);
            vec2 entry = (vec2(tile) + vec2(0.5, detail.z + 0.5)) * panorama.lens.z;
            vec3 slot = texture(pageTable, entry).rgb;
            if (slot.b > 0.5) {
                vec2 atlasPixel = floor(slot.rg * 255.0 + 0.5) * 516.0
                    + pixel - vec2(tile) * 512.0 + 2.0;
                color = vec4(texture(tileAtlas, atlasPixel * panorama.atlasInfo.xy).rgb, 1.0);
                return;
            }
        }
    }
    color = vec4(texture(previewTexture, location).rgb, 1.0);
}
