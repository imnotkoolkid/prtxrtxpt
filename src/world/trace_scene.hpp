#pragma once
#include "world.hpp"

struct SurfaceMaterial
{
    int tile, shape, flags, orientation;
    float roughness, metal, ior, transmission;
    float emission[3], strength;
    float absorption[3], density;
};
static_assert(sizeof(SurfaceMaterial) == 64);

// Directory entries are {one-based page, low occupancy bits, high occupancy bits}.
// Each page packs two native palette indices per word in the map's X:Y:Z order.
class TraceScene
{
  public:
    static constexpr size_t directoryWords = 4096 * 3, pageWords = 2048;
    std::shared_ptr<World> world;
    std::vector<uint32_t> grid, lights;
    std::vector<SurfaceMaterial> materials;
    void load(std::shared_ptr<World>);

  private:
    void pack(int key, const Chunk &chunk);
};
