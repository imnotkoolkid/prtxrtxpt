#include "trace_scene.hpp"

namespace
{
SurfaceMaterial describe(Block b)
{
    SurfaceMaterial m{b.material, b.shape,   b.flags, b.facing * 2 + (b.id & 1), .76f, 0, 1.5f,
                      0,          {1, 1, 1}, 0,       {.04f, .025f, .015f},      1};
    // Native atlas tile identifiers shared by the cube, slab, tile and pillar variants.
    switch (b.material)
    {
    // Rough dielectrics: loose ground, pavement, cloth and paper.
    case 18:  // Dirt
    case 20:  // Sand
    case 44:  // Blue carpet
    case 69:  // Anti-slip surface
    case 70:  // Large anti-slip surface
    case 74:  // Dark asphalt
    case 93:  // Snow
    case 98:  // Rope
    case 102: // Cardboard box
    case 108: // Red carpet
    case 111: // Old temple gravel
    case 130: // Mud block
    case 206: // Small cardboard box
    case 233: // Dry dirt
    case 244: // Big gravel
    case 253: // Orange sand
    case 261: // Gravel
    case 262: // Alternate orange sand
        m.roughness = .93f;
        break;
    // Opaque timber and foliage are dielectrics with broader highlights.
    case 21:  // Oak log
    case 31:  // Dark oak log
    case 35:  // Dark oak stump
    case 51:  // Foliage 1
    case 52:  // Foliage 1 with purple flowers
    case 57:  // Foliage 2
    case 126: // Magic wood log
    case 347: // Conifer needles
    case 348: // Conifer log
    case 371: // Foliage 3
        m.roughness = .84f;
        break;
    case 24:  // Oak planks
    case 25:  // Oak box and door bottom
    case 34:  // Dark oak planks
    case 122: // Magic wood planks
    case 323: // Dark oak planks without nails
    case 328: // Dark oak floor pattern I
    case 342: // Dark oak floor pattern II
    case 346: // Conifer planks
    case 372: // Dark oak floorboards A
    case 373: // Dark oak floorboards B
        m.roughness = .57f;
        break;
    // Smooth opaque finishes; the atlas still controls color and cutouts.
    case 42:  // Window frame
    case 112: // Chiseled quartz
    case 113: // Quartz column
        m.roughness = .48f;
        break;
    case 43:  // Black and white checks
    case 106: // Purple tiles
    case 116: // Quartz tiles
    case 134: // Green tiles
    case 174: // Blue tiles
        m.roughness = .30f;
        break;
    // Painted pipes and fittings have a dielectric outer coating.
    case 183: // Trash can
    case 197: // Yellow barrel
    case 200: // Black barrel
    case 203: // Black grate
    case 230: // Nuclear barrel
    case 282: // Blue pipe
    case 285: // Red pipe
    case 288: // Yellow pipe
    case 291: // Black pipe
        m.roughness = .45f;
        break;
    case 303: // Smooth blue pipe
    case 306: // Smooth red pipe
    case 309: // Smooth yellow pipe
    case 312: // Smooth black pipe
        m.roughness = .30f;
        break;
    // Exposed metal. The atlas albedo supplies the reflection tint.
    case 60:  // Steel ladder
    case 61:  // Iron bars
    case 63:  // Iron door top
    case 64:  // Iron door bottom
    case 65:  // Iron fence
    case 66:  // Iron meshed fence
    case 67:  // Metal fence
    case 68:  // Prison bars
    case 87:  // Chain
    case 177: // Manhole cover
    case 178: // Sewer grate
    case 187: // Ventilation grate
    case 316: // Damaged steel ladder
    case 336: // Rail Z
    case 338: // Rail X
        m.metal = 1;
        m.roughness = .58f;
        break;
    case 273: // Copper pipe
    case 276: // Steel pipe
    case 279: // Iron pipe
    case 392: // Steel shutters upper
    case 393: // Steel shutters lower
    case 394: // Iron shutters upper
    case 395: // Iron shutters lower
        m.metal = 1;
        m.roughness = .39f;
        break;
    case 294: // Smooth copper pipe
    case 297: // Smooth steel pipe
    case 300: // Smooth iron pipe
        m.metal = 1;
        m.roughness = .24f;
        break;
    case 41:
    case 264:
    case 265:
        m.transmission = 1;
        m.roughness = 0;
        m.ior = 1.52f;
        break;
    case 138: // Ice with occasional diffuse scattering at the interface.
        m.transmission = .94f;
        m.ior = 1.31f;
        m.roughness = .18f;
        m.absorption[0] = .18f;
        m.absorption[1] = .055f;
        m.absorption[2] = .022f;
        break;
    case 71: // Oxidized sheet metal.
        m.metal = .55f;
        m.roughness = .72f;
        break;
    case 180:
    case 181:
    case 182:
    case 191:
    case 215:
    case 216:
    case 228:
        m.roughness = .42f; // Painted steel has a dielectric top coat.
        break;
    case 72:
    case 117:
    case 118:
    case 119:
    case 172:
    case 194:
    case 204:
    case 205:
    case 217:
    case 62:
    case 137:
        m.metal = 1;
        m.roughness = b.material == 137 ? .2f : .32f;
        break;
    case 135:
    case 86:
    case 89:
    case 90:
    case 91:
        m.strength = 9;
        break;
    }
    return m;
}
} // namespace
void TraceScene::pack(int key, const Chunk &chunk)
{
    uint32_t mask[2]{};
    std::array<uint32_t, pageWords> words{};
    for (unsigned i = 0; i < chunk.size(); ++i)
    {
        unsigned index = chunk[i];
        if (!index || materials[index].tile == 1)
            continue; // Invisible map barriers.
        unsigned x = i >> 8, y = (i >> 4) & 15, z = i & 15;
        unsigned bit = (x >> 2) * 16 + (y >> 2) * 4 + (z >> 2);
        mask[bit >> 5] |= 1u << (bit & 31);
        words[i >> 1] |= index << ((i & 1) * 16);
        if (materials[index].strength > 0)
        {
            unsigned wx = (key >> 8) * 16 + x, wy = ((key >> 4) & 15) * 16 + y, wz = (key & 15) * 16 + z;
            lights.push_back((wx << 16) | (wy << 8) | wz);
        }
    }
    if (mask[0] || mask[1])
    {
        grid[key * 3] = uint32_t((grid.size() - directoryWords) / pageWords + 1);
        grid[key * 3 + 1] = mask[0];
        grid[key * 3 + 2] = mask[1];
        grid.insert(grid.end(), words.begin(), words.end());
    }
}

void TraceScene::load(std::shared_ptr<World> next)
{
    world = std::move(next);
    materials.clear();
    materials.reserve(world->palette.size());
    for (auto block : world->palette)
        materials.push_back(describe(block));

    lights.clear();
    grid.assign(directoryWords, 0);
    for (int key = 0; key < int(world->chunks.size()); ++key)
        if (const auto &chunk = world->chunks[key])
            pack(key, *chunk);
}
