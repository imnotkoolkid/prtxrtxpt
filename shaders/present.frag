#version 450
layout(binding = 0) uniform sampler2D radiance;
layout(binding = 1) uniform sampler2D geometry;
layout(binding = 2) uniform sampler2D specular;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    ivec2 size = textureSize(radiance, 0);
    vec2 p = uv * vec2(size) - .5, f = fract(p);
    ivec2 base = ivec2(floor(p));
    float face = texelFetch(geometry, clamp(ivec2(uv * size), ivec2(0), size - 1), 0).w;
    vec3 hdr = vec3(0);
    float total = 0.;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            ivec2 q = clamp(base + ivec2(x, y), ivec2(0), size - 1);
            float w = (x == 0 ? 1. - f.x : f.x) * (y == 0 ? 1. - f.y : f.y);
            if (texelFetch(geometry, q, 0).w != face)
                w *= .08;
            hdr += w * (texelFetch(radiance, q, 0).rgb + texelFetch(specular, q, 0).rgb);
            total += w;
        }
    vec3 c = hdr / max(total, .000001);
    c = clamp(c * (2.51 * c + .03) / (c * (2.43 * c + .59) + .14), 0., 1.);
    color = vec4(mix(1.055 * pow(c, vec3(1. / 2.4)) - .055, 12.92 * c, lessThan(c, vec3(.0031308))), 1);
}
