#include "world.hpp"
#include <algorithm>
namespace
{
bool partial(Block b) { return b.shape == Slab || b.shape == Tile; }

bool visible(Block a, Block b, int face)
{
    if (!b.id || a.shape == Wall)
        return true;
    int ny = face == 0 ? 1 : face == 1 ? -1 : 0;
    bool upper = a.id & 1, otherUpper = b.id & 1;
    if (partial(a) && ((ny == 1 && !upper) || (ny == -1 && upper)))
        return true;
    bool cap = (ny == -1 && otherUpper) || (ny == 1 && !otherUpper);
    if (a.shape == Pillar)
        return !(ny && (((a.flags & Transparent) && (a.flags & Join)) ||
                        (partial(b) ? cap
                                    : b.shape != Wall && b.shape != Cross &&
                                          b.shape != Fence && !(b.flags & Transparent))));
    if (b.flags & Transparent)
    {
        if ((a.flags & Transparent) && (a.flags & Join) && a.material == b.material)
        {
            if (partial(b) && cap)
                return false;
            if (!ny && ((partial(a) && a.shape == b.shape) || (a.shape == Tile && b.shape == Slab)))
                return upper != otherUpper;
            if (b.shape == Cube)
                return false;
        }
        return true;
    }
    if (b.shape == Pillar || b.shape == Wall || b.shape == Cross || b.shape == Fence ||
        (a.shape == Slab && b.shape == Tile))
        return true;
    return partial(b) && (ny || !partial(a) || otherUpper != upper) && !cap;
}

int material(Block b, int face, bool alternate = false)
{
    int m = b.material;
    if (m < 0)
        return m;
    int top = b.flags & Top ? 1 : 0;
    int bot = b.flags & Bottom ? 1 : 0;
    int sid = b.flags & Side ? 1 : 0;
    if (face == 0 && top) m += top;
    if (face == 1 && bot) m += top + bot;
    if (alternate) m += top + bot + 1;
    if ((face == 2 || face == 3) && sid) m += top + bot + sid;
    if (face >= 2 && (b.flags & Lower) && b.shape == Slab && !(b.id & 1))
        m += top + bot + sid + 1;
    return m;
}

void emitQuad(std::vector<Quad> &dst, Vec lo, Vec hi, Block b, int face, bool twoSided = false)
{
    Vec p = lo;
    float w, h;
    if (face < 2)
    {
        w = hi.x - lo.x; h = hi.z - lo.z;
        if (!face) { p.x = hi.x; p.y = hi.y; }
    }
    else if (face < 4)
    {
        w = hi.z - lo.z; h = hi.y - lo.y;
        if (face == 2) { p.x = hi.x; p.z = hi.z; }
    }
    else
    {
        w = hi.x - lo.x; h = hi.y - lo.y;
        if (face == 4) p.x = hi.x;
        else           p.z = hi.z;
    }
    float u = b.shape == Pillar ? .35f : 0;
    float v = b.shape == Pillar && face < 2 ? .35f
            : partial(b) && face >= 2 && (b.id & 1) ? h : 0;
    dst.push_back({p, w, h, u, v, uint32_t(face), material(b, face)});
    if (twoSided)
    {
        auto back = dst.back();
        back.face += 8;
        dst.push_back(back);
    }
}

void plants(std::vector<Quad> &dst, Block b, Vec p)
{
    if (b.shape == Cross)
    {
        for (int i = 0; i < 2; ++i)
        {
            Quad q{{p.x, p.y, p.z + i}, 1, 1, 0, 0, uint32_t(6 + i),
                   material(b, 5, i && (b.flags & Alternate))};
            dst.push_back(q);
            q.face += 8;
            dst.push_back(q);
        }
        return;
    }
    for (float inset : {.25f, .75f})
    {
        emitQuad(dst, {p.x, p.y, p.z + inset}, {p.x + 1, p.y + 1, p.z + inset}, b, 5, true);
        emitQuad(dst, {p.x + inset, p.y, p.z}, {p.x + inset, p.y + 1, p.z + 1}, b, 3, true);
    }
    for (int face : {0, 1})
        if (b.flags & (face ? Bottom : Top))
            emitQuad(dst, p, {p.x + 1, p.y + 1, p.z + 1}, b, face, true);
}
} // namespace

void mesh(World const &world, int key, Map &out)
{
    constexpr int stride[]   = {256, 16, 1};
    constexpr int normals[][3] = {{0,1,0},{0,-1,0},{1,0,0},{-1,0,0},{0,0,-1},{0,0,1}};
    constexpr int wallFace[]   = {2, 3, 5, 4};

    const Chunk &chunk = *world.chunks[key];
    int base[] = {key >> 8 << 4, (key >> 4 & 15) << 4, (key & 15) << 4};
    uint32_t first = uint32_t(out.quads.size());
    std::vector<Quad> cutouts;

    for (int face = 0; face < 6; ++face)
    {
        int axis = face < 2 ? 1 : face < 4 ? 0 : 2;
        int u = axis == 0 ? 2 : 0, v = axis == 1 ? 2 : 1;
        int step = normals[face][axis];
        int su = stride[u], sv = stride[v], sa = stride[axis];
        int cAxis = (key / sa) & 15;

        for (int s = 0; s < 16; ++s)
        {
            int ns = s + step;
            bool inChunk = unsigned(ns) < 16;
            const Chunk *nb = inChunk ? &chunk
                : (unsigned(cAxis + step) < 16 ? world.chunks[key + step * sa].get() : nullptr);
            int curBase = s * sa;
            int nbBase  = (ns < 0 ? 15 : ns > 15 ? 0 : ns) * sa;

            uint16_t mask[16][16]{};
            for (int j = 0; j < 16; ++j)
            {
                int rowOff = j * sv;
                for (int i = 0; i < 16; ++i)
                {
                    int off = rowOff + i * su;
                    uint16_t idx = chunk[curBase + off];
                    if (!idx) continue;
                    auto b = world.palette[idx];
                    if (b.shape == Fence || b.shape == Cross)
                    {
                        if (!face)
                            plants(cutouts, b, {float(base[0] + i), float(base[1] + s), float(base[2] + j)});
                        continue;
                    }
                    if (b.shape == Wall && wallFace[b.facing] != face) continue;
                    uint16_t nbIdx = nb ? (*nb)[nbBase + off] : 0;
                    if (visible(b, world.palette[nbIdx], face))
                        mask[j][i] = idx;
                }
            }

            // Greedy quad merging.
            for (int j = 0; j < 16; ++j)
            {
                for (int i = 0; i < 16; ++i)
                {
                    uint16_t idx = mask[j][i];
                    if (!idx) continue;
                    auto b = world.palette[idx];
                    int w = 1, h = 1;
                    if (b.shape != Pillar)
                        while (i + w < 16 && mask[j][i + w] == idx) ++w;
                    if ((!partial(b) || v != 1) && (b.shape != Pillar || v == 1))
                        while (j + h < 16 &&
                               std::all_of(mask[j+h]+i, mask[j+h]+i+w, [=](auto t){ return t == idx; }))
                            ++h;
                    for (int k = 0; k < h; ++k) std::fill_n(mask[j+k]+i, w, uint16_t(0));

                    float lo[3], hi[3];
                    lo[axis] = float(base[axis] + s);
                    lo[u]    = float(base[u] + i);
                    lo[v]    = float(base[v] + j);
                    for (int k = 0; k < 3; ++k)
                        hi[k] = lo[k] + (k == u ? w : k == v ? h : 1);

                    if (partial(b))
                    {
                        float thickness = b.shape == Tile ? .1f : .5f;
                        if (b.id & 1) lo[1] += 1 - thickness;
                        hi[1] = lo[1] + thickness;
                    }
                    if (b.shape == Pillar)
                        for (int k : {0, 2}) { lo[k] += .35f; hi[k] -= .35f; }
                    if (b.shape == Wall)
                    {
                        float shift = -.98f * step;
                        lo[axis] += shift; hi[axis] += shift;
                    }
                    bool isCutout = (b.flags & Transparent) || b.shape == Wall;
                    emitQuad(isCutout ? cutouts : out.quads,
                             {lo[0],lo[1],lo[2]}, {hi[0],hi[1],hi[2]}, b, face);
                }
            }
        }
    }

    uint32_t opaqueCount = uint32_t(out.quads.size()) - first;
    out.quads.insert(out.quads.end(), cutouts.begin(), cutouts.end());
    if (uint32_t count = uint32_t(out.quads.size()) - first)
        out.batches.push_back({{float(base[0]+8), float(base[1]+8), float(base[2]+8)}, first, opaqueCount, count});
}
