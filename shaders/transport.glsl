const float sunWidth = .0000108; // 1 - cos(0.266 degrees)
vec3 sky(vec3 wi) {
    // Single scattering in an atmospheric column, with wavelength-dependent
    // molecular extinction and a forward-scattering aerosol component.
    float height = max(.045, wi.y), day = smoothstep(-.1, .18, f.sunDir.y);
    float mu = clamp(dot(wi, f.sunDir.xyz), -1., 1.);
    vec3 rayleigh = vec3(.058, .135, .301), extinction = rayleigh + vec3(.022);
    float molecularPhase = .75 * (1. + mu * mu);
    float aerosolPhase = .51 / pow(max(.09, 1.49 - 1.4 * mu), 1.5);
    vec3 scatter = (rayleigh * molecularPhase + .022 * aerosolPhase) / extinction;
    vec3 atmosphere = (1. - exp(-extinction / height)) * scatter;
    vec3 ground = vec3(.09, .085, .078);
    return f.skyTint.rgb * mix(ground, atmosphere, smoothstep(-.08, .02, wi.y)) * day;
}
float powerWeight(float a, float b) {
    float r = b / max(a, 1e-20);
    return 1. / (1. + r * r);
}
vec3 emission(Hit h) {
    Material m = palette[h.id];
    return h.albedo * m.emission.rgb * m.emission.a * f.emissive.rgb;
}
vec3 schlick(vec3 f0, float cosine) { return f0 + (1. - f0) * pow(1. - clamp(cosine, 0., 1.), 5.); }
float dielectric(float cosine, float eta) {
    float sint2 = eta * eta * (1. - cosine * cosine);
    if (sint2 >= 1.)
        return 1.;
    float ct = sqrt(1. - sint2);
    vec2 r = vec2(eta * cosine - ct, cosine - eta * ct) / vec2(eta * cosine + ct, cosine + eta * ct);
    return dot(r, r) * .5;
}
struct Bsdf {
    vec3 n, wo, color, f0;
    float a2, metal, pGloss;
    bool diffuse, gloss;
};
Bsdf makeBsdf(Hit h, vec3 n, vec3 wo, bool diffuse, bool gloss) {
    Material m = palette[h.id];
    vec3 f0 = mix(vec3(pow((m.optics.z - 1.) / (m.optics.z + 1.), 2.)), h.albedo, m.optics.y);
    diffuse = diffuse && m.optics.y < 1.;
    float p = gloss ? (diffuse ? clamp(luminance(schlick(f0, dot(n, wo))), .08, .92) : 1.) : 0.;
    return Bsdf(n, wo, h.albedo, f0, pow(max(.03, m.optics.x), 4.), m.optics.y, p, diffuse, gloss);
}
float smith(float cosTheta, float a2) {
    return 2. * cosTheta / (cosTheta + sqrt(a2 + (1. - a2) * cosTheta * cosTheta));
}
// Return BSDF times incident cosine and the mixture's solid-angle density.
vec3 evaluate(Bsdf b, vec3 wi, out float pdf, out vec3 spec) {
    float ni = dot(b.n, wi), no = max(1e-5, dot(b.n, b.wo));
    pdf = 0.;
    spec = vec3(0);
    if (ni <= 0.)
        return vec3(0);
    vec3 h = normalize(wi + b.wo);
    float nh = max(0., dot(b.n, h)), oh = max(.000001, dot(b.wo, h));
    float q = nh * nh * (b.a2 - 1.) + 1.;
    float D = b.a2 / (PI * q * q);
    vec3 result = vec3(0);
    if (b.gloss) {
        result = schlick(b.f0, oh) * D * smith(no, b.a2) * smith(ni, b.a2) / (4. * no);
        spec = result;
        pdf += b.pGloss * D * smith(no, b.a2) / (4. * no);
    }
    if (b.diffuse) {
        result += b.color * (1. - b.metal) * (1. - schlick(b.f0, no)) * (1. - schlick(b.f0, ni)) * ni / PI;
        pdf += (1. - b.pGloss) * ni / PI;
    }
    return result;
}
vec3 sampleBsdf(Bsdf b, out bool glossy) {
    glossy = random() < b.pGloss;
    float u = random();
    if (!glossy)
        return cone(b.n, sqrt(u), sqrt(1. - u));
    // Sample the visible GGX normal distribution in stretched tangent space.
    vec3 t = normalize(cross(abs(b.n.y) > .95 ? vec3(1, 0, 0) : vec3(0, 1, 0), b.n));
    mat3 frame = mat3(t, cross(b.n, t), b.n);
    float alpha = sqrt(b.a2);
    vec3 v = normalize((transpose(frame) * b.wo) * vec3(alpha, alpha, 1));
    vec3 t1 = v.z < .99999 ? normalize(cross(vec3(0, 0, 1), v)) : vec3(1, 0, 0);
    float phi = 2. * PI * random(), x = sqrt(u) * cos(phi), y = sqrt(u) * sin(phi);
    y = mix(sqrt(max(0., 1. - x * x)), y, .5 * (1. + v.z));
    vec3 nh = x * t1 + y * cross(v, t1) + sqrt(max(0., 1. - x * x - y * y)) * v;
    vec3 h = frame * normalize(vec3(nh.xy * alpha, max(0., nh.z)));
    return reflect(-b.wo, h);
}
// Uniform solid angle around a sphere enclosing one selected emitter voxel.
// Testing the actual first hit handles cutouts, partial blocks and occluded faces.
float lightWidth(vec3 from, ivec3 cell, out vec3 direction) {
    vec3 offset = vec3(cell) + .5 - from;
    float d2 = dot(offset, offset);
    direction = d2 > 1e-12 ? offset * inversesqrt(d2) : vec3(0, 1, 0);
    if (d2 <= .7501)
        return 2.;
    float s2 = .7501 / d2;
    return s2 / (1. + sqrt(1. - s2));
}
vec3 sampleCap(vec3 dir, float width) {
    float w = width * random();
    return cone(dir, 1. - w, sqrt(w * (2. - w)));
}
float hitWeight(float pathPdf, float lightPdf, int samples) {
    if (pathPdf < 0.)
        return 1.; // camera or delta event
    return enabled(MIS) ? powerWeight(pathPdf, float(samples) * lightPdf) : 0.;
}
vec3 environment(vec3 d, float pathPdf, vec3 prevNormal) {
    vec3 L = sky(d) * hitWeight(pathPdf, max(dot(prevNormal, d), 0.) / PI, f.sampling.x);
    if (dot(d, f.sunDir.xyz) >= 1. - sunWidth && (enabled(SOFT) || pathPdf < 0.)) {
        float pdf = 1. / (2. * PI * sunWidth);
        L += f.solar.rgb * pdf * hitWeight(pathPdf, pdf, f.sampling.x);
    }
    return L;
}
bool visible(vec3 p, vec3 d) {
    vec3 start = p + (enabled(CONTACT) ? 0. : .5) * d;
    Hit h = trace(start, d, true);
    return h.id == 0u && h.t > 0.;
}
vec3 directResponse(Bsdf full, Bsdf continuation, vec3 wi, float density, vec3 light, inout vec3 gloss) {
    float pdf;
    vec3 spec;
    vec3 allLobes = evaluate(full, wi, pdf, spec);
    if (enabled(MIS) && density > 0.) {
        vec3 activeSpec;
        vec3 activeLobes = evaluate(continuation, wi, pdf, activeSpec);
        float complement = 1. - powerWeight(density, pdf);
        allLobes -= activeLobes * complement;
        spec -= activeSpec * complement;
    }
    gloss += light * spec;
    return light * allLobes;
}
vec3 direct(Bsdf b, Bsdf continuation, vec3 p, out vec3 gloss) {
    vec3 L = vec3(0);
    gloss = vec3(0);
    int count = f.sampling.x;
    for (int i = 0; i < count; ++i) {
        if (any(greaterThan(f.solar.rgb, vec3(0)))) {
            vec3 wi = enabled(SOFT) ? sampleCap(f.sunDir.xyz, sunWidth) : f.sunDir.xyz;
            if (dot(b.n, wi) > 0. && visible(p, wi))
                L += directResponse(b, continuation, wi, enabled(SOFT) ? count / (2. * PI * sunWidth) : 0.,
                                    f.solar.rgb / count, gloss);
        }
        if (any(greaterThan(f.skyTint.rgb, vec3(0)))) {
            float c = sqrt(max(random(), 1e-8));
            vec3 wi = cone(b.n, c, sqrt(1. - c * c));
            float density = count * c / PI;
            if (visible(p, wi))
                L += directResponse(b, continuation, wi, density, sky(wi) / density, gloss);
        }
    }
    if (enabled(LIGHTS) && f.budgets.w > 0 && any(greaterThan(f.emissive.rgb, vec3(0)))) {
        count = f.sampling.y;
        for (int i = 0; i < count; ++i) {
            uint packed = lights[min(int(random() * f.budgets.w), f.budgets.w - 1)];
            ivec3 cell = ivec3(packed >> 16, (packed >> 8) & 255u, packed & 255u);
            vec3 dir;
            float width = lightWidth(p, cell, dir);
            vec3 wi = sampleCap(dir, width);
            if (dot(b.n, wi) <= 0.)
                continue;
            Hit h = trace(p, wi, false);
            if (h.id == 0u || any(notEqual(h.cell, cell)))
                continue;
            float lightPdf = 1. / (2. * PI * width * f.budgets.w);
            L += directResponse(b, continuation, wi, count * lightPdf, emission(h) / (count * lightPdf), gloss);
        }
    }
    return L;
}
vec3 transport(vec3 o, vec3 d, out vec3 specular) {
    vec3 primary = vec3(0), specRatio = vec3(0);
    specular = vec3(0);
    vec3 L = vec3(0), weight = vec3(1), sigma = vec3(0), previousN = vec3(0);
    float pathPdf = -1.;
    ivec3 used = ivec3(0);
    for (int bounce = 0; bounce <= f.limits.y; ++bounce) {
        Hit h = trace(o, d, false);
        if (h.id == 0u) {
            if (h.t > 0.)
                L += weight * environment(d, pathPdf, previousN);
            break;
        }
        Material m = palette[h.id];
        vec3 p = o + h.t * d;
        bool entering = dot(h.n, d) < 0.;
        vec3 n = entering ? h.n : -h.n;
        weight *= exp(-sigma * h.t);
        if (m.emission.a > 0.) {
            float w = 1.;
            if (enabled(LIGHTS) && pathPdf >= 0. && f.budgets.w > 0) {
                vec3 unused;
                float width = lightWidth(o, h.cell, unused);
                w = hitWeight(pathPdf, 1. / (2. * PI * width * f.budgets.w), f.sampling.y);
            }
            L += weight * emission(h) * w;
        }
        if (bounce == f.limits.y)
            break;
        if (bounce == 1)
            weight *= f.lens.w;
        if (m.optics.w == 1. || (m.optics.w > 0. && random() < m.optics.w)) {
            if (bounce == 0) {
                primary = L;
                specRatio = vec3(1);
            }
            uint outside = volumeAt(p + h.n * BIAS);
            Material adjacent = palette[outside];
            float exteriorIOR = adjacent.optics.w > 0. ? adjacent.optics.z : 1.;
            float eta = entering ? exteriorIOR / m.optics.z : m.optics.z / exteriorIOR;
            float F = dielectric(clamp(dot(n, -d), 0., 1.), eta);
            if (random() < F) {
                if (++used.y > f.budgets.y)
                    break;
                d = reflect(d, n);
                o = p + n * BIAS;
            } else {
                if (++used.z > f.budgets.z)
                    break;
                d = normalize(refract(d, n, eta));
                o = p - n * BIAS;
                weight *= eta * eta;
                sigma = entering ? (m.absorption.rgb - log(max(h.albedo, vec3(.001)))) * m.absorption.a
                                 : (adjacent.optics.w > 0. ? adjacent.absorption.rgb * adjacent.absorption.a
                                                           : vec3(0));
            }
            pathPdf = -1.;
        } else {
            bool allowD = used.x < f.budgets.x && enabled(GI), allowS = used.y < f.budgets.y;
            Bsdf bs = makeBsdf(h, n, -d, allowD, allowS);
            o = p + n * BIAS;
            // Evaluate both direct lobes, weighting only those sampled by continuation.
            vec3 directGloss;
            L += weight * direct(makeBsdf(h, n, -d, true, true), bs, o, directGloss);
            if (bounce == 0) {
                primary = L;
                specular = directGloss;
            }
            if (!bs.diffuse && !bs.gloss)
                break;
            bool glossy;
            d = sampleBsdf(bs, glossy);
            vec3 gloss;
            vec3 response = evaluate(bs, d, pathPdf, gloss);
            if (bounce == 0)
                specRatio = gloss / max(response, vec3(1e-20));
            if (pathPdf <= 0.)
                break;
            weight *= response / pathPdf;
            if (glossy)
                ++used.y;
            else
                ++used.x;
            previousN = n;
        }
        if (enabled(RR) && bounce >= 2) {
            float survive = clamp(max(weight.x, max(weight.y, weight.z)), .08, .98);
            if (random() >= survive)
                break;
            weight /= survive;
        }
    }
    specular = clamp(specular + (L - primary) * specRatio, vec3(0), L);
    return L;
}
