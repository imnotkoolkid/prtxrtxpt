layout(std140, binding = 0) uniform Frame {
    vec4 camera, previous; // xyz position, yaw
    vec4 lens;             // pitch, previous pitch, display aspect, indirect multiplier
    ivec4 limits;          // spp, total bounces, steps, distance
    ivec4 budgets;         // diffuse, specular, transmission, light count
    ivec4 sampling;        // direct samples, emitter samples, flags, history valid
    uvec4 sequence;        // frame, stationary
    vec4 sunDir, solar, skyTint, emissive;
}
f;
const int RR = 1, MIS = 2, ADAPT = 4, GI = 8, SOFT = 16, CONTACT = 32, LIGHTS = 64;
bool enabled(int bit) { return (f.sampling.z & bit) != 0; }
const float PI = 3.141592653589793, INF = 1e30, BIAS = .00012;
uint seed;
float random() {
    seed = seed * 747796405u + 2891336453u;
    uint v = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((v ^ (v >> 22u)) >> 8u) * (1. / 16777216.);
}
vec3 cone(vec3 axis, float cosine, float sine) {
    vec3 u = normalize(cross(abs(axis.y) > .95 ? vec3(1, 0, 0) : vec3(0, 1, 0), axis));
    float phi = 2. * PI * random();
    return axis * cosine + sine * (u * cos(phi) + cross(axis, u) * sin(phi));
}
mat3 basis(float yaw, float pitch) {
    vec3 right = vec3(cos(yaw), 0, -sin(yaw));
    vec3 forward = vec3(-sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch));
    return mat3(right, cross(right, forward), forward);
}
float luminance(vec3 rgb) { return dot(rgb, vec3(.2126, .7152, .0722)); }
