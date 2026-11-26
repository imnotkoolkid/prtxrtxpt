vec3 guideNormal(float key) {
    const vec3 normals[8] = vec3[](vec3(0, 1, 0), vec3(0, -1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1),
                                   vec3(0, 0, -1), vec3(.707107, 0, .707107), vec3(.707107, 0, -.707107));
    return normals[uint(max(key, 0.)) & 7u];
}
float surfaceFootprint(float pixelWidth, vec3 view, vec3 normal) {
    return pixelWidth / max(.06, abs(dot(normalize(view), normal)));
}
