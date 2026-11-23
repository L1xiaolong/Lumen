#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
layout(location = 0) out vec3 viewRay;

layout(std140, binding = 0) uniform Panorama {
    mat4 clipMatrix;
    mat4 cameraMatrix;
    vec4 lens;       // half-FOV tangents and inverse page-table side
    vec4 atlasInfo;  // inverse atlas width, inverse atlas height, first level, level count
    vec4 levels[8];  // raster width, raster height, first row in the page table
} panorama;

void main() {
    vec2 plane = vec2(2.0 * texCoord.x - 1.0, 1.0 - 2.0 * texCoord.y) * panorama.lens.xy;
    viewRay = (panorama.cameraMatrix * vec4(plane, 1.0, 0.0)).xyz;
    gl_Position = panorama.clipMatrix * vec4(position, 0.0, 1.0);
}
