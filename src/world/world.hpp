#pragma once
#include "map.hpp"
#include <array>
#include <memory>

enum Shape { Cube, Slab, Wall, Cross, Fence, Tile, Pillar };
enum Flag  { Top = 1, Bottom = 2, Side = 4, Lower = 8, Alternate = 16, Transparent = 32, Join = 64 };

struct Block
{
    int id, shape, material, facing, flags;
};

using Chunk = std::array<uint16_t, 4096>;

struct World
{
    std::array<std::unique_ptr<Chunk>, 4096> chunks{};
    std::vector<Block> palette{{}};
};

void mesh(World const &world, int key, Map &out);
