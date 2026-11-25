#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
struct Vec
{
    float x{}, y{}, z{};
};
struct Camera
{
    Vec pos;
    float yaw{}, pitch{};
};
struct Quad
{
    Vec origin;
    float width, height, uOffset, vOffset;
    uint32_t face;
    int material;
};
struct Batch
{
    Vec center;
    uint32_t first, opaqueCount, count;
};
struct Map
{
    Camera camera;
    std::vector<Quad> quads;
    std::vector<Batch> batches;
};
Map loadMap(std::wstring_view json);
