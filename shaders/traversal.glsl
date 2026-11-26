layout(std430, binding = 1) readonly buffer VoxelGrid { uint grid[]; };
struct Material {
    ivec4 block;
    vec4 optics, emission, absorption;
};
layout(std430, binding = 2) readonly buffer Materials { Material palette[]; };
layout(std430, binding = 3) readonly buffer Emitters { uint lights[]; };
layout(binding = 4) uniform sampler2D atlas;
const uint TABLE = 12288u;
const int CUBE = 0, SLAB = 1, WALL = 2, CROSS = 3, FENCE = 4, TILE = 5, PILLAR = 6;
struct Hit {
    float t;
    uint id;
    vec3 n, albedo;
    ivec3 cell;
};
Hit miss(float t) { return Hit(t, 0u, vec3(0), vec3(0), ivec3(0)); }
uint address(ivec3 c) { return uint(c.x * 256 + c.y * 16 + c.z); }
uint voxel(uint page, ivec3 c) {
    uint i = address(c & 15);
    return (grid[TABLE + (page - 1u) * 2048u + (i >> 1u)] >> ((i & 1u) * 16u)) & 65535u;
}
uint lookup(ivec3 c) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(256))))
        return 0u;
    uint page = grid[address(c >> 4) * 3u];
    return page == 0u ? 0u : voxel(page, c);
}
void bounds(ivec4 b, out vec3 lo, out vec3 hi) {
    lo = vec3(0);
    hi = vec3(1);
    if (b.y == SLAB || b.y == TILE) {
        float h = b.y == SLAB ? .5 : .1;
        lo.y = float(b.w & 1) * (1. - h);
        hi.y = lo.y + h;
    } else if (b.y == PILLAR) {
        lo.xz = vec2(.35);
        hi.xz = vec2(.65);
    }
}
uint volumeAt(vec3 position) {
    ivec3 cell = ivec3(floor(position));
    uint id = lookup(cell);
    if (id == 0u)
        return 0u;
    ivec4 b = palette[id].block;
    if (b.y == WALL || b.y == CROSS || b.y == FENCE)
        return 0u;
    vec3 lo, hi;
    bounds(b, lo, hi);
    vec3 local = fract(position);
    return all(greaterThanEqual(local, lo)) && all(lessThanEqual(local, hi)) ? id : 0u;
}
int least(vec3 t) { return t.x <= min(t.y, t.z) ? 0 : (t.y <= t.z ? 1 : 2); }
vec3 axis(int a) { return vec3(a == 0, a == 1, a == 2); }
void boxInterval(vec3 o, vec3 d, vec3 inv, vec3 lo, vec3 hi, out vec3 nearT, out vec3 farT) {
    vec3 a = (lo - o) * inv, b = (hi - o) * inv;
    bvec3 parallel = lessThan(abs(d), vec3(1e-12));
    nearT = mix(min(a, b), vec3(-INF), parallel);
    farT = mix(max(a, b), vec3(INF), parallel);
    for (int i = 0; i < 3; ++i)
        if (parallel[i] && (o[i] < lo[i] || o[i] > hi[i]))
            farT[i] = -INF;
}
vec4 surfaceColor(ivec4 b, vec3 n, vec3 p, bool alternate) {
    int tile = b.x, flags = b.z;
    ivec2 pixel;
    if (tile < 0) {
        int i = -tile - 1;
        pixel = ivec2(i >> 4, 496 + (i & 15));
    } else {
        int top = flags & 1, bot = (flags >> 1) & 1, side = (flags >> 2) & 1;
        vec2 uv = vec2(n.z < -.99 ? 1. - p.x : p.x, p.y);
        if (abs(n.x) > .99) {
            uv.x = n.x > 0 ? 1. - p.z : p.z;
            tile += side * (top + bot + 1);
        }
        if (abs(n.y) > .99) {
            uv = vec2(n.y > 0 ? 1. - p.x : p.x, p.z);
            tile += n.y > 0 ? top : bot * (top + bot);
        } else if (b.y == SLAB && (b.w & 1) == 0 && (flags & 8) != 0)
            tile += top + bot + side + 1;
        if (alternate && (flags & 16) != 0)
            tile += top + bot + 1;
        int i = tile - 1;
        ivec2 texel = ivec2(clamp(uv, 0., .999999) * 16.);
        pixel = ivec2((i & 31) * 16, (i >> 5) * 16) + ivec2(texel.x, 15 - texel.y);
    }
    return texelFetch(atlas, pixel, 0);
}
void candidate(inout Hit h, ivec3 cell, uint id, Material m, float t, vec3 p, vec3 n, bool alternate) {
    if (t <= .000015 || t >= h.t || any(lessThan(p, vec3(-.00005))) || any(greaterThan(p, vec3(1.00005))))
        return;
    if (m.optics.w > 0.) {
        vec3 outside = p + vec3(cell) + n * BIAS;
        ivec3 other = ivec3(floor(outside));
        if (any(notEqual(other, cell))) {
            uint adj = volumeAt(outside);
            if (adj != 0u && palette[adj].optics.w > 0. && palette[adj].optics.z == m.optics.z)
                return;
            // A glass/opaque shared face belongs to the opaque substrate.
            if (adj != 0u && palette[adj].optics.w == 0. && (palette[adj].block.z & 32) == 0) {
                p = clamp(vec3(cell - other) + p, 0., 1.);
                cell = other;
                n = -n;
                id = adj;
                m = palette[adj];
            }
        }
    }
    vec4 texel = surfaceColor(m.block, n, p, alternate);
    if (texel.a < .5 && m.optics.w == 0.)
        return;
    vec3 color = mix(pow((texel.rgb + .055) / 1.055, vec3(2.4)), texel.rgb / 12.92,
                     lessThanEqual(texel.rgb, vec3(.04045)));
    if (m.optics.w > 0.)
        color = mix(vec3(.98), color, .22 * texel.a);
    h = Hit(t, id, n, min(color, vec3(.99)), cell);
}
Hit intersectCell(vec3 o, vec3 d, vec3 inv, ivec3 cell, uint id, float end) {
    Hit h = miss(end + .00004);
    Material m = palette[id];
    ivec4 b = m.block;
    if (b.y == WALL || b.y == CROSS || b.y == FENCE) {
        int count = b.y == CROSS ? 2 : (b.y == FENCE ? 6 : 1);
        for (int i = 0; i < count; ++i) {
            vec3 n;
            float k;
            if (b.y == WALL) {
                int side = b.w >> 1;
                n = axis(side < 2 ? 0 : 2);
                k = (side & 1) == 0 ? .02 : .98;
                if ((side & 1) != 0) {
                    n = -n;
                    k = -k;
                }
            } else if (b.y == CROSS) {
                n = normalize(vec3(1, 0, i == 0 ? -1 : 1));
                k = i == 0 ? 0 : n.x;
            } else if (i < 4) {
                n = axis(i < 2 ? 0 : 2);
                k = (i & 1) == 0 ? .25 : .75;
            } else {
                if ((b.z & (1 << (i - 4))) == 0)
                    continue;
                n = vec3(0, 1, 0);
                k = i == 4 ? 1. : 0.;
            }
            float denom = dot(n, d);
            if (abs(denom) > 1e-10) {
                float t = (k - dot(n, o)) / denom;
                candidate(h, cell, id, m, t, o + t * d, n, b.y == CROSS && i == 1);
            }
        }
    } else {
        vec3 lo, hi, a, bFar;
        bounds(b, lo, hi);
        boxInterval(o, d, inv, lo, hi, a, bFar);
        int i = least(-a), j = least(bFar);
        if (a[i] <= bFar[j]) {
            candidate(h, cell, id, m, a[i], o + d * a[i], -sign(d[i]) * axis(i), false);
            if (h.id == 0u)
                candidate(h, cell, id, m, bFar[j], o + d * bFar[j], sign(d[j]) * axis(j), false);
        }
    }
    return h;
}
// A negative miss is a truncated ray. Only a proven world exit contributes environment.
Hit trace(vec3 o, vec3 d, bool visibility) {
    bvec3 parallel = lessThan(abs(d), vec3(1e-12));
    vec3 inv = 1. / mix(d, vec3(1.), parallel), a, b;
    boxInterval(o, d, inv, vec3(0), vec3(256), a, b);
    float t = max(0., max(a.x, max(a.y, a.z))), exitT = min(b.x, min(b.y, b.z));
    if (exitT <= t)
        return miss(INF);
    ivec3 cell = clamp(ivec3(floor(o + d * t)), ivec3(0), ivec3(255));
    ivec3 stepDir = ivec3(sign(d)), positive = ivec3(greaterThan(d, vec3(0)));
    vec3 delta = mix(abs(inv), vec3(INF), parallel);
    vec3 edges = mix((vec3(cell + positive) - o) * inv, vec3(INF), parallel);
    uint lastKey = ~0u, page = 0u;
    uvec2 mask = uvec2(0);
    for (int i = 0; i < f.limits.z; ++i) {
        if (t > f.limits.w)
            break;
        uint key = address(cell >> 4);
        if (key != lastKey) {
            uint base = key * 3u;
            page = grid[base];
            mask = uvec2(grid[base + 1u], grid[base + 2u]);
            lastKey = key;
        }
        ivec3 local = (cell & 15) >> 2;
        uint bit = uint(local.x * 16 + local.y * 4 + local.z);
        int size = page == 0u ? 16 : ((mask[bit >> 5] & (1u << (bit & 31u))) == 0u ? 4 : 1);
        ivec3 base = cell & ~(size - 1);
        vec3 boundary = size == 1 ? edges : mix((vec3(base + positive * size) - o) * inv, vec3(INF), parallel);
        float next = min(boundary.x, min(boundary.y, boundary.z));
        if (size == 1) {
            uint id = voxel(page, cell);
            if (id != 0u) {
                ivec4 block = palette[id].block;
                if (visibility && block.y == CUBE && (block.z & 32) == 0 && palette[id].optics.w == 0.)
                    return Hit(t, id, vec3(0), vec3(0), cell);
                Hit hit = intersectCell(o - vec3(cell), d, inv, cell, id, min(next, float(f.limits.w)));
                if (hit.id != 0u)
                    return hit;
            }
        }
        if (next > f.limits.w)
            break;
        if (next >= exitT)
            return miss(INF);
        bvec3 crossed = lessThanEqual(boundary, vec3(next));
        if (size == 1) {
            cell += mix(ivec3(0), stepDir, crossed);
            edges += mix(vec3(0), delta, crossed);
        } else {
            cell = clamp(ivec3(floor(o + next * d)), base, base + size - 1);
            cell = mix(cell, base + positive * size - (1 - positive), crossed);
            edges = mix((vec3(cell + positive) - o) * inv, vec3(INF), parallel);
        }
        if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(256))))
            return miss(INF);
        t = next;
    }
    return miss(-1.);
}
