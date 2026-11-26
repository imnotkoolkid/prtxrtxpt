#include "world.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
namespace
{
using namespace winrt::Windows::Data::Json;
constexpr Block blocks[] = {
#include "blocks.inc"
};
struct Migration { int id, target; };
constexpr Migration migrations[] = {
#include "migration.inc"
};

template <class T, size_t N>
const T *lookup(const T (&table)[N], int id)
{
    auto it = std::lower_bound(table, table + N, id, [](const T &a, int b) { return a.id < b; });
    return it != table + N && it->id == id ? it : nullptr;
}

Block resolve(int id, bool legacy)
{
    int family = id / 100000;
    if (legacy)
    {
        int odd = (family == 1 || family == 5) && (id & 1);
        if (auto *e = lookup(migrations, id - odd)) id = e->target + odd;
        family = id / 100000;
    }
    int index = id % 100000;
    if (family >= 7 && family <= 11)
    {
        constexpr int shapes[] = {Cube, Slab, Wall, Tile, Pillar};
        int shape = shapes[family - 7], color = index, facing = 0;
        if (shape == Slab || shape == Tile) color = index / 2;
        if (shape == Wall && index > 0) { color = (index - 1) / 4 + 1; facing = (index - 1) % 4; }
        if (color >= 1 && color <= 4096) return {id, shape, -color, facing, 0};
    }
    int key = (family == 1 || family == 5) ? id & ~1 : id;
    if (auto *e = lookup(blocks, key)) { auto b = *e; b.id = id; return b; }
    return blocks[0];
}

uint64_t decode(winrt::hstring const &code)
{
    if (code.empty()) throw std::runtime_error("Empty block ID.");
    uint64_t n = 0;
    for (auto c : code)
    {
        int d = c >= L'0' && c <= L'9' ? c - L'0'
              : c >= L'a' && c <= L'z' ? c - L'a' + 10
              : c >= L'A' && c <= L'Z' ? c - L'A' + 36 : -1;
        if (d < 0 || n > (9007199254740991ULL - d) / 62)
            throw std::runtime_error("Invalid base62 block ID.");
        n = n * 62 + d;
    }
    return n;
}

int integer(double n, int max)
{
    if (!(n >= 0 && n <= max && n == std::floor(n)))
        throw std::runtime_error("Invalid version, chunk coordinate or run range.");
    return int(n);
}

bool position(IJsonValue value, Vec &pos)
{
    if (value.ValueType() != JsonValueType::Array) return false;
    auto a = value.GetArray();
    if (a.Size() < 3) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (a.GetAt(i).ValueType() != JsonValueType::Number) return false;
    pos = {float(a.GetNumberAt(0)), float(a.GetNumberAt(1)), float(a.GetNumberAt(2))};
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
}
} 

Map loadMap(std::wstring_view json, bool buildMesh)
{
    try
    {
        auto root = JsonObject::Parse(json);
        auto optInt = [&](const wchar_t *key, int fallback) {
            if (!root.HasKey(key) || root.Lookup(key).ValueType() == JsonValueType::Null)
                return fallback;
            return integer(root.GetNamedNumber(key), 4096);
        };
        int version = optInt(L"v", 3);
        if (version < 1 || version > 3 || optInt(L"chnkSize", 16) != 16)
            throw std::runtime_error("Expected a v1-v3 map with 16-voxel chunks.");

        Map out;
        if (root.HasKey(L"n") && root.Lookup(L"n").ValueType() == JsonValueType::String)
            out.name = winrt::to_string(root.GetNamedString(L"n"));
        out.world = std::make_shared<World>();
        World &world = *out.world;
        world.palette.reserve(64);
        int first = -1;
        std::unordered_map<int, uint16_t> ids;
        ids.reserve(64);

        for (auto value : root.GetNamedArray(L"chnks"))
        {
            auto entry = value.GetArray();
            int key = integer(entry.GetNumberAt(1), 4095);
            for (auto encoded : entry.GetArrayAt(0))
            {
                auto run  = encoded.GetArray();
                auto id   = decode(run.GetStringAt(0));
                int start = integer(run.GetNumberAt(1), 4095);
                int len   = integer(run.GetNumberAt(2), 4096 - start);
                if (!id || !len) continue;
                if (first < 0) first = key;
                auto block = resolve(id > 1199999 ? 1 : int(id), version == 1);
                auto [it, inserted] = ids.try_emplace(block.id, uint16_t(world.palette.size()));
                if (inserted) {
                    if (world.palette.size() >= 65536) throw std::runtime_error("Map palette is full.");
                    world.palette.push_back(block);
                }
                auto &chunk = world.chunks[key];
                if (!chunk) chunk = std::make_unique<Chunk>();
                std::fill_n(chunk->begin() + start, len, it->second);
            }
        }

        for (int key = 0; key < 4096; ++key)
            if (buildMesh && world.chunks[key]) mesh(world, key, out);

  
        bool spawned = false;
        if (root.HasKey(L"spwns") && root.Lookup(L"spwns").ValueType() == JsonValueType::Array)
            for (auto spawn : root.GetNamedArray(L"spwns"))
                if (position(spawn, out.camera.pos))
                {
                    auto a = spawn.GetArray();
                    if (a.Size() > 3 && a.GetAt(3).ValueType() == JsonValueType::Number)
                        out.camera.yaw = float(a.GetNumberAt(3));
                    spawned = true;
                    break;
                }

        if (!spawned)
        {
            Vec lo, hi;
            bool hasBounds = root.HasKey(L"mnVoxel") && root.HasKey(L"mxVoxel") &&
                             position(root.Lookup(L"mnVoxel"), lo) &&
                             position(root.Lookup(L"mxVoxel"), hi) &&
                             lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z;
            if (!hasBounds)
            {
                lo = first < 0 ? Vec{120, 64, 120}
                               : Vec{float(first >> 8 << 4), float((first >> 4 & 15) << 4),
                                     float((first & 15) << 4)};
                hi = {lo.x + 16, lo.y + 16, lo.z + 16};
            }
            float d = std::max({8.f, (hi.x - lo.x) / 3, (hi.y - lo.y) / 3});
            out.camera.pos   = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2 + .6f * d, hi.z + d};
            out.camera.pitch = -std::atan2(.6f * d, d + (hi.z - lo.z) / 2);
        }
        out.camera.pos.y += .775f; 
        return out;
    }
    catch (winrt::hresult_error const &)
    {
        throw std::runtime_error("Invalid map JSON. Check the chunk and run arrays.");
    }
}
