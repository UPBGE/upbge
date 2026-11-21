/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavAir.cpp
 *  \ingroup ketsji
 */

#include "KX_NavAir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <functional>
#include <queue>
#include <unordered_set>

#include "KX_NavBuild.h"
#include "LN_JobPool.h"

namespace {

constexpr int kBrick = 8;
/** Bricks a job of a build takes (single bricks cost more to hand out than to compute). */
constexpr size_t kBricksPerJob = 8;
/** A search's heuristic weight (Warframe's greedy search: faster, the smoothing straightens). */
constexpr float kGreedy = 1.2f;

struct V3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline V3 sub(const V3 &a, const V3 &b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float dot(const V3 &a, const V3 &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 vertex(const KX_NavGeometry &geometry, const int index)
{
  const float *v = &geometry.verts[size_t(index) * 3];
  return {v[0], v[1], v[2]};
}

/** The point of the triangle `abc` nearest `p` (Ericson, Real-Time Collision Detection, 5.1.5). */
V3 closest_point_triangle(const V3 &p, const V3 &a, const V3 &b, const V3 &c)
{
  const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const float d1 = dot(ab, ap), d2 = dot(ac, ap);
  V3 q;
  if (d1 <= 0.0f && d2 <= 0.0f) {
    q = a;
  }
  else {
    const V3 bp = sub(p, b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    const float vc = d1 * d4 - d3 * d2;
    const V3 cp = sub(p, c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    const float vb = d5 * d2 - d1 * d6;
    const float va = d3 * d6 - d5 * d4;
    if (d3 >= 0.0f && d4 <= d3) {
      q = b;
    }
    else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
      const float v = d1 / (d1 - d3);
      q = {a.x + ab.x * v, a.y + ab.y * v, a.z + ab.z * v};
    }
    else if (d6 >= 0.0f && d5 <= d6) {
      q = c;
    }
    else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
      const float w = d2 / (d2 - d6);
      q = {a.x + ac.x * w, a.y + ac.y * w, a.z + ac.z * w};
    }
    else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
      const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      q = {b.x + (c.x - b.x) * w, b.y + (c.y - b.y) * w, b.z + (c.z - b.z) * w};
    }
    else {
      const float denom = 1.0f / (va + vb + vc);
      const float v = vb * denom, w = vc * denom;
      q = {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
    }
  }
  return q;
}

/** The squared distance from `p` to the triangle `abc`. */
float point_triangle_distance2(const V3 &p, const V3 &a, const V3 &b, const V3 &c)
{
  const V3 d = sub(p, closest_point_triangle(p, a, b, c));
  return dot(d, d);
}

/** A cell of the surface triangles' grid (21 bits a side, round the origin). */
uint64_t cell_key(const int cx, const int cy, const int cz)
{
  constexpr int kHalf = 1 << 20;
  return uint64_t(uint32_t(cx + kHalf) & 0x1FFFFF) |
         (uint64_t(uint32_t(cy + kHalf) & 0x1FFFFF) << 21) |
         (uint64_t(uint32_t(cz + kHalf) & 0x1FFFFF) << 42);
}

inline bool bit(const uint64_t *words, const int x, const int y, const int z)
{
  return ((words[z] >> (x + kBrick * y)) & 1u) != 0;
}
inline void set_bit(uint64_t *words, const int x, const int y, const int z)
{
  words[z] |= uint64_t(1) << (x + kBrick * y);
}

void run_jobs(LN_JobPool *pool, const size_t count, const std::function<void(size_t)> &item)
{
  if (count == 0) {
    return;
  }
  const size_t jobs = (count + kBricksPerJob - 1) / kBricksPerJob;
  if (pool == nullptr || pool->GetHelperCount() == 0 || jobs < 2) {
    for (size_t i = 0; i < count; i++) {
      item(i);
    }
    return;
  }
  pool->Run(jobs, [&](const size_t job, uint32_t /*thread*/) {
    const size_t end = std::min(count, (job + 1) * kBricksPerJob);
    for (size_t i = job * kBricksPerJob; i < end; i++) {
      item(i);
    }
  });
}

}  // namespace

KX_NavAirMap::KX_NavAirMap(const Settings &settings) : m_settings(settings)
{
  m_settings.voxel = std::max(m_settings.voxel, 0.05f);
  m_settings.radius = std::max(m_settings.radius, 0.0f);
}

uint64_t KX_NavAirMap::BrickKey(const int bx, const int by, const int bz)
{
  return uint64_t(uint32_t(bx) & 0x1FFFFF) | (uint64_t(uint32_t(by) & 0x1FFFFF) << 21) |
         (uint64_t(uint32_t(bz) & 0x1FFFFF) << 42);
}

KX_NavAirMap::Index KX_NavAirMap::VoxelOf(const MT_Vector3 &p) const
{
  const float v = m_settings.voxel;
  return {int(std::floor((float(p[0]) - float(m_origin[0])) / v)),
          int(std::floor((float(p[1]) - float(m_origin[1])) / v)),
          int(std::floor((float(p[2]) - float(m_origin[2])) / v))};
}

MT_Vector3 KX_NavAirMap::CenterOf(const Index &i) const
{
  const float v = m_settings.voxel;
  return MT_Vector3(float(m_origin[0]) + (float(i.x) + 0.5f) * v,
                    float(m_origin[1]) + (float(i.y) + 0.5f) * v,
                    float(m_origin[2]) + (float(i.z) + 0.5f) * v);
}

MT_Vector3 KX_NavAirMap::Min() const
{
  return m_origin;
}

MT_Vector3 KX_NavAirMap::Max() const
{
  const float v = m_settings.voxel;
  return m_origin + MT_Vector3(float(m_size[0]) * v, float(m_size[1]) * v, float(m_size[2]) * v);
}

bool KX_NavAirMap::Holds(const MT_Vector3 &min, const MT_Vector3 &max) const
{
  if (m_open) {
    return false;
  }
  const MT_Vector3 lo = Min(), hi = Max();
  /* (A hair of slack: the box was made from these very points.) */
  const float slack = 1e-3f;
  for (int axis = 0; axis < 3; axis++) {
    if (float(min[axis]) < float(lo[axis]) - slack || float(max[axis]) > float(hi[axis]) + slack)
    {
      return false;
    }
  }
  return true;
}

size_t KX_NavAirMap::MemoryBytes() const
{
  /* (A map node holds the key, the brick and two pointers' worth of bookkeeping.) */
  return (m_bricks.size() + m_water.size()) * (sizeof(uint64_t) + sizeof(Brick) + 16);
}

bool KX_NavAirMap::VoxelIsWater(const Index &v) const
{
  auto it = m_water.find(BrickKey(v.x >> 3, v.y >> 3, v.z >> 3));
  return it != m_water.end() && (it->second.full || bit(it->second.solid, v.x & 7, v.y & 7, v.z & 7));
}

bool KX_NavAirMap::VoxelFree(const Index &v, const bool water_only) const
{
  if (m_open) {
    return !water_only;
  }
  if (v.z >= m_size[2]) {
    /* The sky above the box: free, never water. */
    return !water_only;
  }
  if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= m_size[0] || v.y >= m_size[1]) {
    return false;
  }
  auto it = m_bricks.find(BrickKey(v.x >> 3, v.y >> 3, v.z >> 3));
  if (it != m_bricks.end() &&
      (it->second.full || bit(it->second.solid, v.x & 7, v.y & 7, v.z & 7)))
  {
    return false;
  }
  return !water_only || VoxelIsWater(v);
}

bool KX_NavAirMap::BrickWholeFree(const int bx, const int by, const int bz) const
{
  if (bx < 0 || by < 0 || bz < 0 || (bx + 1) * kBrick > m_size[0] ||
      (by + 1) * kBrick > m_size[1] || (bz + 1) * kBrick > m_size[2])
  {
    return false;
  }
  return m_bricks.find(BrickKey(bx, by, bz)) == m_bricks.end();
}

bool KX_NavAirMap::Free(const MT_Vector3 &p, const bool water_only) const
{
  return VoxelFree(VoxelOf(p), water_only);
}

KX_NavAirMap::PieceMap KX_NavAirMap::PiecesOf(const KX_NavGeometry &geometry)
{
  PieceMap pieces;
  if (geometry.pieces.empty()) {
    return pieces;
  }
  /* Each piece's middle (its vertices' mean), then each face turned away from it. */
  std::unordered_map<int32_t, std::pair<V3, int>> middles;
  const int ntris = int(geometry.tris.size() / 3);
  for (int t = 0; t < ntris; t++) {
    const int32_t piece = geometry.pieces[size_t(t)];
    if (piece < 0) {
      continue;
    }
    auto &[sum, count] = middles[piece];
    for (int k = 0; k < 3; k++) {
      const V3 v = vertex(geometry, geometry.tris[size_t(t) * 3 + k]);
      sum.x += v.x;
      sum.y += v.y;
      sum.z += v.z;
      count++;
    }
  }
  for (int t = 0; t < ntris; t++) {
    const int32_t piece = geometry.pieces[size_t(t)];
    if (piece < 0) {
      continue;
    }
    const auto &[sum, count] = middles[piece];
    const V3 middle = {sum.x / float(count), sum.y / float(count), sum.z / float(count)};
    const V3 a = vertex(geometry, geometry.tris[size_t(t) * 3 + 0]);
    const V3 b = vertex(geometry, geometry.tris[size_t(t) * 3 + 1]);
    const V3 c = vertex(geometry, geometry.tris[size_t(t) * 3 + 2]);
    const V3 ab = sub(b, a), ac = sub(c, a);
    V3 n = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
    const float length = std::sqrt(dot(n, n));
    if (length < 1e-9f) {
      continue;
    }
    n = {n.x / length, n.y / length, n.z / length};
    if (dot(n, sub(middle, a)) > 0.0f) {
      n = {-n.x, -n.y, -n.z};
    }
    pieces[piece].push_back({MT_Vector3(a.x, a.y, a.z), MT_Vector3(n.x, n.y, n.z)});
  }
  /* (A piece with fewer than four faces encloses nothing.) */
  for (auto it = pieces.begin(); it != pieces.end();) {
    it = it->second.size() < 4 ? pieces.erase(it) : std::next(it);
  }
  return pieces;
}

void KX_NavAirMap::BinGeometry(const KX_NavGeometry &geometry,
                               const PieceMap &pieces,
                               Bins &r_tri_bins,
                               Bins &r_piece_bins) const
{
  const float v = m_settings.voxel;
  const float grow = m_settings.radius + 0.5f * v;
  const int bricks[3] = {(m_size[0] + kBrick - 1) / kBrick,
                         (m_size[1] + kBrick - 1) / kBrick,
                         (m_size[2] + kBrick - 1) / kBrick};
  auto add_box = [&](const float *lo, const float *hi, const int item, Bins &bins) {
    int b0[3], b1[3];
    for (int axis = 0; axis < 3; axis++) {
      const int i0 = int(std::floor((lo[axis] - float(m_origin[axis])) / v));
      const int i1 = int(std::floor((hi[axis] - float(m_origin[axis])) / v));
      b0[axis] = std::max(0, i0) / kBrick;
      b1[axis] = std::min(bricks[axis] * kBrick - 1, i1) / kBrick;
      if (i1 < 0 || i0 >= bricks[axis] * kBrick) {
        return;
      }
    }
    for (int bz = b0[2]; bz <= b1[2]; bz++) {
      for (int by = b0[1]; by <= b1[1]; by++) {
        for (int bx = b0[0]; bx <= b1[0]; bx++) {
          bins[BrickKey(bx, by, bz)].push_back(item);
        }
      }
    }
  };
  const int ntris = int(geometry.tris.size() / 3);
  for (int t = 0; t < ntris; t++) {
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int k = 0; k < 3; k++) {
      const float *p = &geometry.verts[size_t(geometry.tris[size_t(t) * 3 + k]) * 3];
      for (int axis = 0; axis < 3; axis++) {
        lo[axis] = std::min(lo[axis], p[axis] - grow);
        hi[axis] = std::max(hi[axis], p[axis] + grow);
      }
    }
    add_box(lo, hi, t, r_tri_bins);
  }
  /* A piece covers the bricks of its bounds (its inside; its surface is the triangles'). */
  std::unordered_map<int32_t, std::pair<std::array<float, 3>, std::array<float, 3>>> bounds;
  for (int t = 0; t < ntris && !geometry.pieces.empty(); t++) {
    const int32_t piece = geometry.pieces[size_t(t)];
    if (piece < 0 || pieces.find(piece) == pieces.end()) {
      continue;
    }
    auto found = bounds.find(piece);
    if (found == bounds.end()) {
      found = bounds.emplace(piece,
                             std::make_pair(std::array<float, 3>{FLT_MAX, FLT_MAX, FLT_MAX},
                                            std::array<float, 3>{-FLT_MAX, -FLT_MAX, -FLT_MAX}))
                  .first;
    }
    for (int k = 0; k < 3; k++) {
      const float *p = &geometry.verts[size_t(geometry.tris[size_t(t) * 3 + k]) * 3];
      for (int axis = 0; axis < 3; axis++) {
        found->second.first[axis] = std::min(found->second.first[axis], p[axis]);
        found->second.second[axis] = std::max(found->second.second[axis], p[axis]);
      }
    }
  }
  for (const auto &[piece, box] : bounds) {
    add_box(box.first.data(), box.second.data(), piece, r_piece_bins);
  }
}

void KX_NavAirMap::ComputeBricks(const KX_NavGeometry &geometry,
                                 const PieceMap &pieces,
                                 const std::vector<uint64_t> &keys,
                                 const Bins &tri_bins,
                                 const Bins &piece_bins,
                                 LN_JobPool *pool,
                                 std::vector<Brick> &r_bricks) const
{
  r_bricks.assign(keys.size(), Brick());
  const float v = m_settings.voxel;
  const float grow = m_settings.radius + 0.5f * v;
  const float grow2 = grow * grow;
  run_jobs(pool, keys.size(), [&](const size_t index) {
    const uint64_t key = keys[index];
    const int bx = int(key & 0x1FFFFF), by = int((key >> 21) & 0x1FFFFF),
              bz = int((key >> 42) & 0x1FFFFF);
    Brick &brick = r_bricks[index];
    const float corner[3] = {float(m_origin[0]) + float(bx * kBrick) * v,
                             float(m_origin[1]) + float(by * kBrick) * v,
                             float(m_origin[2]) + float(bz * kBrick) * v};
    /* The voxels of the brick a box meets (their middles within it). */
    auto range = [&](const float *lo, const float *hi, int *r0, int *r1) {
      for (int axis = 0; axis < 3; axis++) {
        r0[axis] = std::max(0, int(std::ceil((lo[axis] - corner[axis]) / v - 0.5f)));
        r1[axis] = std::min(kBrick - 1, int(std::floor((hi[axis] - corner[axis]) / v - 0.5f)));
        if (r0[axis] > r1[axis]) {
          return false;
        }
      }
      return true;
    };
    /* The middle of corner voxel `which` (bit 0: x high, 1: y high, 2: z high), axis `axis`. */
    auto corner_pos = [&](const int which, const int axis) {
      /* (Not `far`: <windows.h> makes it nothing.) */
      const int high = (which >> axis) & 1;
      return corner[axis] + (high ? float(kBrick) - 0.5f : 0.5f) * v;
    };
    /* The convex pieces first: a brick whose eight corner voxels are inside one is inside it
     * whole (a block's inside: no voxel by voxel, no triangle). */
    auto inside_piece = [](const std::vector<Plane> &faces, const MT_Vector3 &p) {
      for (const Plane &face : faces) {
        if (float((p - face.point).dot(face.normal)) > 1e-5f) {
          return false;
        }
      }
      return true;
    };
    auto pit = piece_bins.find(key);
    if (pit != piece_bins.end()) {
      for (const int piece : pit->second) {
        const std::vector<Plane> &faces = pieces.at(piece);
        bool whole = true;
        for (int corner = 0; corner < 8 && whole; corner++) {
          const MT_Vector3 p(corner_pos(corner, 0), corner_pos(corner, 1), corner_pos(corner, 2));
          whole = inside_piece(faces, p);
        }
        if (whole) {
          for (int z = 0; z < kBrick; z++) {
            brick.solid[z] = ~uint64_t(0);
          }
          brick.full = true;
          return;
        }
        for (int z = 0; z < kBrick; z++) {
          for (int y = 0; y < kBrick; y++) {
            for (int x = 0; x < kBrick; x++) {
              if (bit(brick.solid, x, y, z)) {
                continue;
              }
              const MT_Vector3 p(corner[0] + (float(x) + 0.5f) * v,
                                 corner[1] + (float(y) + 0.5f) * v,
                                 corner[2] + (float(z) + 0.5f) * v);
              if (inside_piece(faces, p)) {
                set_bit(brick.solid, x, y, z);
              }
            }
          }
        }
      }
    }
    auto it = tri_bins.find(key);
    if (it != tri_bins.end()) {
      for (const int t : it->second) {
        const V3 a = vertex(geometry, geometry.tris[size_t(t) * 3 + 0]);
        const V3 b = vertex(geometry, geometry.tris[size_t(t) * 3 + 1]);
        const V3 c = vertex(geometry, geometry.tris[size_t(t) * 3 + 2]);
        const float lo[3] = {std::min({a.x, b.x, c.x}) - grow,
                             std::min({a.y, b.y, c.y}) - grow,
                             std::min({a.z, b.z, c.z}) - grow};
        const float hi[3] = {std::max({a.x, b.x, c.x}) + grow,
                             std::max({a.y, b.y, c.y}) + grow,
                             std::max({a.z, b.z, c.z}) + grow};
        int r0[3], r1[3];
        if (!range(lo, hi, r0, r1)) {
          continue;
        }
        for (int z = r0[2]; z <= r1[2]; z++) {
          for (int y = r0[1]; y <= r1[1]; y++) {
            for (int x = r0[0]; x <= r1[0]; x++) {
              if (bit(brick.solid, x, y, z)) {
                continue;
              }
              const V3 p = {corner[0] + (float(x) + 0.5f) * v,
                            corner[1] + (float(y) + 0.5f) * v,
                            corner[2] + (float(z) + 0.5f) * v};
              if (point_triangle_distance2(p, a, b, c) <= grow2) {
                set_bit(brick.solid, x, y, z);
              }
            }
          }
        }
      }
    }
    brick.full = true;
    for (int z = 0; z < kBrick; z++) {
      brick.full = brick.full && brick.solid[z] == ~uint64_t(0);
    }
  });
}

void KX_NavAirMap::Build(const KX_NavGeometry &geometry,
                         const std::vector<KX_NavWaterVolume> &water,
                         LN_JobPool *pool)
{
  m_bricks.clear();
  m_water.clear();
  m_shell.clear();
  const int ntris = int(geometry.tris.size() / 3);
  float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (int t = 0; t < ntris; t++) {
    for (int k = 0; k < 3; k++) {
      const float *p = &geometry.verts[size_t(geometry.tris[size_t(t) * 3 + k]) * 3];
      for (int axis = 0; axis < 3; axis++) {
        lo[axis] = std::min(lo[axis], p[axis]);
        hi[axis] = std::max(hi[axis], p[axis]);
      }
    }
  }
  for (const KX_NavWaterVolume &volume : water) {
    for (int axis = 0; axis < 3; axis++) {
      float extent = 0.0f;
      for (int k = 0; k < 3; k++) {
        extent += std::fabs(float(volume.orientation[axis][k])) * float(volume.half[k]);
      }
      lo[axis] = std::min(lo[axis], float(volume.center[axis]) - extent);
      hi[axis] = std::max(hi[axis], float(volume.center[axis]) + extent);
    }
  }
  m_open = lo[0] > hi[0];
  if (m_surfaces) {
    SetSurfaceGeometry(geometry);
  }
  if (m_open) {
    m_size[0] = m_size[1] = m_size[2] = 0;
    return;
  }
  /* The level's sides and bottom exactly, its top and a headroom above (whole voxels; at most
   * 2^20 a side: keys hold 21 bits of bricks). */
  const float v = m_settings.voxel;
  m_origin = MT_Vector3(lo[0], lo[1], lo[2]);
  const float top = hi[2] + std::max(m_settings.headroom, 0.0f);
  m_size[0] = std::clamp(int(std::ceil((hi[0] - lo[0]) / v)), 1, 1 << 20);
  m_size[1] = std::clamp(int(std::ceil((hi[1] - lo[1]) / v)), 1, 1 << 20);
  m_size[2] = std::clamp(int(std::ceil((top - lo[2]) / v)), 1, 1 << 20);

  const PieceMap pieces = PiecesOf(geometry);
  Bins tri_bins, piece_bins;
  BinGeometry(geometry, pieces, tri_bins, piece_bins);
  std::vector<uint64_t> keys;
  keys.reserve(tri_bins.size() + piece_bins.size());
  for (const auto &item : tri_bins) {
    keys.push_back(item.first);
  }
  for (const auto &item : piece_bins) {
    if (tri_bins.find(item.first) == tri_bins.end()) {
      keys.push_back(item.first);
    }
  }
  std::sort(keys.begin(), keys.end());
  std::vector<Brick> bricks;
  ComputeBricks(geometry, pieces, keys, tri_bins, piece_bins, pool, bricks);
  for (size_t i = 0; i < keys.size(); i++) {
    const Brick &brick = bricks[i];
    bool any = false;
    for (int z = 0; z < kBrick && !any; z++) {
      any = brick.solid[z] != 0;
    }
    if (any) {
      m_bricks.emplace(keys[i], brick);
    }
  }
  SetWater(water);
  if (m_surfaces) {
    const int b0[3] = {0, 0, 0};
    const int b1[3] = {(m_size[0] - 1) / kBrick, (m_size[1] - 1) / kBrick, (m_size[2] - 1) / kBrick};
    std::vector<uint64_t> shell_keys;
    ShellKeysAround(b0, b1, shell_keys);
    ComputeShell(shell_keys, pool);
  }
}

void KX_NavAirMap::Rebuild(const MT_Vector3 &min,
                           const MT_Vector3 &max,
                           const KX_NavGeometry &geometry,
                           LN_JobPool *pool)
{
  if (m_open) {
    return;
  }
  const float v = m_settings.voxel;
  const float grow = m_settings.radius + v;
  int b0[3], b1[3];
  for (int axis = 0; axis < 3; axis++) {
    const int i0 = int(std::floor((float(min[axis]) - grow - float(m_origin[axis])) / v));
    const int i1 = int(std::floor((float(max[axis]) + grow - float(m_origin[axis])) / v));
    b0[axis] = std::max(0, i0) / kBrick;
    b1[axis] = std::min(m_size[axis] - 1, i1);
    if (b1[axis] < 0) {
      return;
    }
    b1[axis] /= kBrick;
  }
  std::vector<uint64_t> keys;
  for (int bz = b0[2]; bz <= b1[2]; bz++) {
    for (int by = b0[1]; by <= b1[1]; by++) {
      for (int bx = b0[0]; bx <= b1[0]; bx++) {
        keys.push_back(BrickKey(bx, by, bz));
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  const PieceMap pieces = PiecesOf(geometry);
  Bins tri_bins, piece_bins;
  BinGeometry(geometry, pieces, tri_bins, piece_bins);
  std::vector<Brick> bricks;
  ComputeBricks(geometry, pieces, keys, tri_bins, piece_bins, pool, bricks);
  for (size_t i = 0; i < keys.size(); i++) {
    const Brick &brick = bricks[i];
    bool any = false;
    for (int z = 0; z < kBrick && !any; z++) {
      any = brick.solid[z] != 0;
    }
    if (any) {
      m_bricks[keys[i]] = brick;
    }
    else {
      m_bricks.erase(keys[i]);
    }
  }
  if (m_surfaces) {
    /* (The shell of these bricks and those round them: a solid gone or come changes both.) */
    std::vector<uint64_t> shell_keys;
    ShellKeysAround(b0, b1, shell_keys);
    ComputeShell(shell_keys, pool);
  }
}

void KX_NavAirMap::SetWater(const std::vector<KX_NavWaterVolume> &volumes)
{
  m_water.clear();
  if (m_open) {
    return;
  }
  const float v = m_settings.voxel;
  for (const KX_NavWaterVolume &volume : volumes) {
    int i0[3], i1[3];
    for (int axis = 0; axis < 3; axis++) {
      float extent = 0.0f;
      for (int k = 0; k < 3; k++) {
        extent += std::fabs(float(volume.orientation[axis][k])) * float(volume.half[k]);
      }
      i0[axis] = std::max(0, int(std::floor((float(volume.center[axis]) - extent -
                                             float(m_origin[axis])) / v)));
      i1[axis] = std::min(m_size[axis] - 1,
                          int(std::floor((float(volume.center[axis]) + extent -
                                          float(m_origin[axis])) / v)));
    }
    const MT_Matrix3x3 into = volume.orientation.transposed();
    for (int z = i0[2]; z <= i1[2]; z++) {
      for (int y = i0[1]; y <= i1[1]; y++) {
        for (int x = i0[0]; x <= i1[0]; x++) {
          const MT_Vector3 local = into * (CenterOf({x, y, z}) - volume.center);
          if (std::fabs(float(local[0])) > float(volume.half[0]) ||
              std::fabs(float(local[1])) > float(volume.half[1]) ||
              std::fabs(float(local[2])) > float(volume.half[2]))
          {
            continue;
          }
          Brick &brick = m_water[BrickKey(x >> 3, y >> 3, z >> 3)];
          set_bit(brick.solid, x & 7, y & 7, z & 7);
        }
      }
    }
  }
}

bool KX_NavAirMap::LineFree(const MT_Vector3 &a, const MT_Vector3 &b, const bool water_only) const
{
  if (m_open) {
    return !water_only;
  }
  /* Amanatides and Woo's walk through the voxels the segment crosses. */
  Index cur = VoxelOf(a);
  const Index end = VoxelOf(b);
  if (!VoxelFree(cur, water_only)) {
    return false;
  }
  const float v = m_settings.voxel;
  const float d[3] = {float(b[0] - a[0]), float(b[1] - a[1]), float(b[2] - a[2])};
  int step[3];
  float t_max[3], t_delta[3];
  int *coord[3] = {&cur.x, &cur.y, &cur.z};
  for (int axis = 0; axis < 3; axis++) {
    if (std::fabs(d[axis]) < 1e-9f) {
      step[axis] = 0;
      t_max[axis] = FLT_MAX;
      t_delta[axis] = FLT_MAX;
      continue;
    }
    step[axis] = d[axis] > 0.0f ? 1 : -1;
    const float boundary = float(m_origin[axis]) +
                           float(*coord[axis] + (step[axis] > 0 ? 1 : 0)) * v;
    t_max[axis] = (boundary - float(a[axis])) / d[axis];
    t_delta[axis] = v / std::fabs(d[axis]);
  }
  const int steps = std::abs(end.x - cur.x) + std::abs(end.y - cur.y) + std::abs(end.z - cur.z);
  for (int i = 0; i < steps; i++) {
    int axis = 0;
    if (t_max[1] < t_max[axis]) {
      axis = 1;
    }
    if (t_max[2] < t_max[axis]) {
      axis = 2;
    }
    if (step[axis] == 0) {
      break;
    }
    *coord[axis] += step[axis];
    t_max[axis] += t_delta[axis];
    if (!VoxelFree(cur, water_only)) {
      return false;
    }
  }
  return VoxelFree(end, water_only);
}

bool KX_NavAirMap::NearestFree(const MT_Vector3 &p,
                               const float reach,
                               MT_Vector3 &r_point,
                               const bool water_only,
                               const float up_reach) const
{
  if (Free(p, water_only)) {
    r_point = p;
    return true;
  }
  if (m_open) {
    return false;
  }
  const float v = m_settings.voxel;
  const Index s = VoxelOf(p);
  /* The free voxel nearest `p` within `within`. */
  auto nearest = [&](const float within) {
    const int shells = int(std::ceil(within / v));
    float best = FLT_MAX;
    Index found;
    for (int r = 1; r <= shells; r++) {
      for (int dz = -r; dz <= r; dz++) {
        for (int dy = -r; dy <= r; dy++) {
          for (int dx = -r; dx <= r; dx++) {
            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) {
              continue;
            }
            const Index u = {s.x + dx, s.y + dy, s.z + dz};
            if (!VoxelFree(u, water_only)) {
              continue;
            }
            const float d2 = float((CenterOf(u) - p).length2());
            if (d2 < best) {
              best = d2;
              found = u;
            }
          }
        }
      }
      /* No voxel of a farther shell is nearer than r voxels. */
      if (best <= float(r) * v * float(r) * v) {
        break;
      }
    }
    if (best > within * within) {
      return false;
    }
    r_point = CenterOf(found);
    return true;
  };
  /* Touching a solid (within its grown reach: a flyer at a wall, a goal at a wall's foot): the
   * nearest free point, a little aside. */
  const float touching = std::min(reach, m_settings.radius + 2.0f * v);
  if (nearest(touching)) {
    return true;
  }
  /* Buried (in the ground, inside a building, beside the level below its top): straight up,
   * out on top of it (never into a free space under the ground). */
  if (!water_only) {
    const float up = std::max(reach, up_reach);
    for (int k = 1; float(k) * v <= up; k++) {
      const Index above = {s.x, s.y, s.z + k};
      if (VoxelFree(above, false)) {
        r_point = MT_Vector3(p[0], p[1], CenterOf(above)[2]);
        return true;
      }
    }
  }
  /* Else the nearest within the whole reach. */
  return reach > touching && nearest(reach);
}

bool KX_NavAirMap::FindPath(const MT_Vector3 &from,
                            const MT_Vector3 &to,
                            std::vector<MT_Vector3> &r_points,
                            bool *r_complete,
                            const bool water_only,
                            const int max_nodes) const
{
  r_points.clear();
  if (r_complete != nullptr) {
    *r_complete = false;
  }
  if (m_open) {
    if (water_only) {
      return false;
    }
    r_points.push_back(to);
    if (r_complete != nullptr) {
      *r_complete = true;
    }
    return true;
  }
  const float v = m_settings.voxel;
  const float snap = std::max(4.0f * v, 2.0f * m_settings.radius + v);
  /* Both ends onto free air (touching a solid: a little aside; inside a building or the ground:
   * on top of it, up to 64 m). */
  MT_Vector3 start, goal;
  if (!NearestFree(from, snap, start, water_only, 64.0f)) {
    return false;
  }
  const bool goal_free = NearestFree(to, snap * 2.0f, goal, water_only, 64.0f);
  if (!goal_free) {
    goal = to;
  }
  /* An end in the sky above the box (a flyer high up, or come out of the ground beside the
   * level): the search knows only the box; that end goes through the open sky to the box's top
   * layer, straight above the box's nearest point (across at its own height first: below the
   * box's top beside it is solid), and the search starts or ends there. */
  std::vector<MT_Vector3> lead_in, lead_out;
  const MT_Vector3 snapped_start = start, snapped_goal = goal;
  auto into_box = [&](const MT_Vector3 &p, MT_Vector3 &r_entry, std::vector<MT_Vector3> &r_legs) {
    const MT_Vector3 lo = Min(), hi = Max();
    r_entry = MT_Vector3(std::clamp(float(p[0]), float(lo[0]) + 0.5f * v, float(hi[0]) - 0.5f * v),
                         std::clamp(float(p[1]), float(lo[1]) + 0.5f * v, float(hi[1]) - 0.5f * v),
                         float(hi[2]) - 0.5f * v);
    if (!Free(r_entry) && !NearestFree(r_entry, snap, r_entry, false, 0.0f)) {
      return false;
    }
    r_legs.clear();
    const MT_Vector3 over(r_entry[0], r_entry[1], p[2]);
    if ((over - p).length2() > 1e-8f) {
      r_legs.push_back(over);
    }
    r_legs.push_back(r_entry);
    return true;
  };
  auto in_box = [&](const MT_Vector3 &p) {
    const Index i = VoxelOf(p);
    return i.x >= 0 && i.y >= 0 && i.z >= 0 && i.x < m_size[0] && i.y < m_size[1] &&
           i.z < m_size[2];
  };
  auto finish = [&](const std::vector<MT_Vector3> &raw, const bool reached) {
    /* Straightened: from each corner the farthest one it sees. */
    if ((snapped_start - from).length2() > 1e-8f) {
      r_points.push_back(snapped_start);
    }
    r_points.insert(r_points.end(), lead_in.begin(), lead_in.end());
    MT_Vector3 anchor = start;
    size_t i = 0;
    while (i < raw.size()) {
      size_t j = i;
      while (j + 1 < raw.size() && LineFree(anchor, raw[j + 1], water_only)) {
        j++;
      }
      r_points.push_back(raw[j]);
      anchor = raw[j];
      i = j + 1;
    }
    if (reached) {
      /* The way out of the box to a goal in the sky. */
      for (const MT_Vector3 &p : lead_out) {
        if (r_points.empty() || (r_points.back() - p).length2() > 1e-8f) {
          r_points.push_back(p);
        }
      }
    }
    if (r_complete != nullptr) {
      *r_complete = reached && goal_free;
    }
    return !r_points.empty() || reached;
  };
  if (goal_free && LineFree(start, goal, water_only)) {
    return finish({goal}, true);
  }
  if (!water_only && !in_box(start)) {
    MT_Vector3 entry;
    if (!into_box(start, entry, lead_in)) {
      return false;
    }
    start = entry;
  }
  if (!water_only && goal_free && !in_box(goal)) {
    /* The legs from the goal down into the box, flown the other way: up above the exit (when
     * the goal is not straight above it), then the goal. */
    MT_Vector3 exit;
    std::vector<MT_Vector3> legs;
    if (into_box(goal, exit, legs)) {
      if (legs.size() == 2) {
        lead_out.push_back(legs[0]);
      }
      lead_out.push_back(snapped_goal);
      goal = exit;
    }
  }
  if (goal_free && LineFree(start, goal, water_only)) {
    return finish({goal}, true);
  }

  /* Nodes: a voxel (x + nx (y + ny z)) of a brick holding solids or touching the box's sides,
   * or a whole brick of free air (the top bit set, then its key). */
  const uint64_t nx = uint64_t(m_size[0]), ny = uint64_t(m_size[1]);
  constexpr uint64_t kBrickNode = uint64_t(1) << 63;
  using Open = std::pair<float, uint64_t>;
  static const int kDirs[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const int bricks_n[3] = {(m_size[0] + kBrick - 1) / kBrick,
                           (m_size[1] + kBrick - 1) / kBrick,
                           (m_size[2] + kBrick - 1) / kBrick};
  auto brick_xyz = [](const uint64_t key, int *r) {
    r[0] = int(key & 0x1FFFFF);
    r[1] = int((key >> 21) & 0x1FFFFF);
    r[2] = int((key >> 42) & 0x1FFFFF);
  };
  auto inside = [&](const Index &i) {
    return i.x >= 0 && i.y >= 0 && i.z >= 0 && i.x < m_size[0] && i.y < m_size[1] &&
           i.z < m_size[2];
  };
  auto node_of = [&](const Index &i) -> uint64_t {
    if (!water_only && BrickWholeFree(i.x >> 3, i.y >> 3, i.z >> 3)) {
      return kBrickNode | BrickKey(i.x >> 3, i.y >> 3, i.z >> 3);
    }
    return uint64_t(i.x) + nx * (uint64_t(i.y) + ny * uint64_t(i.z));
  };
  auto position_of = [&](const uint64_t node) -> MT_Vector3 {
    if (node & kBrickNode) {
      int b[3];
      brick_xyz(node & ~kBrickNode, b);
      return m_origin + MT_Vector3((float(b[0]) + 0.5f) * kBrick * v,
                                   (float(b[1]) + 0.5f) * kBrick * v,
                                   (float(b[2]) + 0.5f) * kBrick * v);
    }
    const uint64_t z = node / (nx * ny), rest = node % (nx * ny);
    return CenterOf({int(rest % nx), int(rest / nx), int(z)});
  };
  auto brick_key_of_node = [&](const uint64_t node) -> uint64_t {
    if (node & kBrickNode) {
      return node & ~kBrickNode;
    }
    const uint64_t z = node / (nx * ny), rest = node % (nx * ny);
    return BrickKey(int(rest % nx) >> 3, int(rest / nx) >> 3, int(z) >> 3);
  };
  const Index start_voxel = VoxelOf(start), goal_voxel = VoxelOf(goal);
  const bool goal_inside = inside(goal_voxel);
  if (start_voxel.z >= m_size[2]) {
    /* Above the box (open sky) with no straight line: no search there. */
    return false;
  }

  /* The fine search: voxels, and whole bricks of free air as one node each; kept to `keep_to`'s
   * bricks when given. Its way (after `a`) into `r_raw`: to `b` when reached (true), else to
   * the nearest it came. */
  struct Entry {
    float g = 0.0f;
    uint64_t parent = 0;
    bool closed = false;
  };
  thread_local std::unordered_map<uint64_t, Entry> entries;
  auto fine_search = [&](const MT_Vector3 &a,
                         const MT_Vector3 &b,
                         const std::unordered_set<uint64_t> *keep_to,
                         const int budget,
                         std::vector<MT_Vector3> &r_raw) {
    r_raw.clear();
    const uint64_t a_node = node_of(VoxelOf(a));
    const Index b_voxel = VoxelOf(b);
    const uint64_t b_node = inside(b_voxel) ? node_of(b_voxel) : ~uint64_t(0);
    entries.clear();
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    entries[a_node] = {0.0f, a_node, false};
    open.push({float((position_of(a_node) - b).length()) * kGreedy, a_node});
    uint64_t best = a_node;
    float best_h = float((position_of(a_node) - b).length());
    bool reached = false;
    int expanded = 0;
    std::vector<uint64_t> next;
    while (!open.empty() && expanded < budget) {
      const uint64_t node = open.top().second;
      open.pop();
      float g_here;
      {
        Entry &entry = entries[node];
        if (entry.closed) {
          continue;
        }
        entry.closed = true;
        /* (Copied: the inserts below may move the entries.) */
        g_here = entry.g;
      }
      expanded++;
      if (node == b_node) {
        reached = true;
        best = node;
        break;
      }
      const MT_Vector3 here = position_of(node);
      const float h = float((here - b).length());
      if (h < best_h) {
        best_h = h;
        best = node;
      }
      next.clear();
      if (node & kBrickNode) {
        int c[3];
        brick_xyz(node & ~kBrickNode, c);
        for (const auto &d : kDirs) {
          const int nbx = c[0] + d[0], nby = c[1] + d[1], nbz = c[2] + d[2];
          if (!water_only && BrickWholeFree(nbx, nby, nbz)) {
            next.push_back(kBrickNode | BrickKey(nbx, nby, nbz));
            continue;
          }
          /* The free voxels of its face. */
          for (int i = 0; i < kBrick; i++) {
            for (int j = 0; j < kBrick; j++) {
              Index u;
              if (d[0] != 0) {
                u = {d[0] > 0 ? nbx * kBrick : nbx * kBrick + kBrick - 1,
                     c[1] * kBrick + i,
                     c[2] * kBrick + j};
              }
              else if (d[1] != 0) {
                u = {c[0] * kBrick + i,
                     d[1] > 0 ? nby * kBrick : nby * kBrick + kBrick - 1,
                     c[2] * kBrick + j};
              }
              else {
                u = {c[0] * kBrick + i,
                     c[1] * kBrick + j,
                     d[2] > 0 ? nbz * kBrick : nbz * kBrick + kBrick - 1};
              }
              if (u.z >= m_size[2] || !VoxelFree(u, water_only)) {
                continue;
              }
              next.push_back(node_of(u));
            }
          }
        }
      }
      else {
        const uint64_t z = node / (nx * ny), rest = node % (nx * ny);
        const Index c = {int(rest % nx), int(rest / nx), int(z)};
        for (const auto &d : kDirs) {
          const Index u = {c.x + d[0], c.y + d[1], c.z + d[2]};
          if (u.z >= m_size[2] || !VoxelFree(u, water_only)) {
            continue;
          }
          next.push_back(node_of(u));
        }
      }
      for (const uint64_t n : next) {
        if (keep_to != nullptr && keep_to->find(brick_key_of_node(n)) == keep_to->end()) {
          continue;
        }
        const float g = g_here + float((position_of(n) - here).length());
        auto it = entries.find(n);
        if (it != entries.end() && (it->second.closed || it->second.g <= g)) {
          continue;
        }
        entries[n] = {g, node, false};
        open.push({g + float((position_of(n) - b).length()) * kGreedy, n});
      }
    }
    for (uint64_t node = best; node != a_node; node = entries[node].parent) {
      r_raw.push_back(position_of(node));
    }
    std::reverse(r_raw.begin(), r_raw.end());
    if (reached) {
      if (r_raw.empty()) {
        r_raw.push_back(b);
      }
      else {
        r_raw.back() = b;
      }
    }
    return reached;
  };

  /* The coarse search (HPA*'s way, 2026-11-19: a city's way searched voxel by voxel ran out of
   * its budget 200 m short after 45 ms): over bricks, a step to the next one where a free voxel
   * of its face meets a free one across (read from the two bricks' masks; water from the
   * voxels), each step costing one and the estimate the bricks' Manhattan distance (exact in
   * the open: no spreading sideways). A way between voxels is one between their bricks too: the
   * coarse search having seen every brick it reaches without the goal's, none exists. */
  auto free_in = [&](const uint64_t *bits, const Index &w) {
    if (!inside(w)) {
      return false;
    }
    if (water_only) {
      return VoxelFree(w, true);
    }
    return bits == nullptr || !bit(bits, w.x & 7, w.y & 7, w.z & 7);
  };
  /* The free voxel pair of a face nearest its middle (the portal from brick `b` toward `d`),
   * false when none. */
  auto portal = [&](const int *b, const int *d, Index *r_u, Index *r_w) {
    const int nb[3] = {b[0] + d[0], b[1] + d[1], b[2] + d[2]};
    if (nb[0] < 0 || nb[1] < 0 || nb[2] < 0 || nb[0] >= bricks_n[0] || nb[1] >= bricks_n[1] ||
        nb[2] >= bricks_n[2])
    {
      return false;
    }
    auto here = m_bricks.find(BrickKey(b[0], b[1], b[2]));
    auto there = m_bricks.find(BrickKey(nb[0], nb[1], nb[2]));
    if ((here != m_bricks.end() && here->second.full) ||
        (there != m_bricks.end() && there->second.full))
    {
      return false;
    }
    const uint64_t *bits_here = here != m_bricks.end() ? here->second.solid : nullptr;
    const uint64_t *bits_there = there != m_bricks.end() ? there->second.solid : nullptr;
    int best_d = INT32_MAX;
    for (int i = 0; i < kBrick; i++) {
      for (int j = 0; j < kBrick; j++) {
        const int off = std::abs(2 * i - (kBrick - 1)) + std::abs(2 * j - (kBrick - 1));
        if (off >= best_d) {
          continue;
        }
        Index u;
        if (d[0] != 0) {
          u = {d[0] > 0 ? b[0] * kBrick + kBrick - 1 : b[0] * kBrick,
               b[1] * kBrick + i,
               b[2] * kBrick + j};
        }
        else if (d[1] != 0) {
          u = {b[0] * kBrick + i,
               d[1] > 0 ? b[1] * kBrick + kBrick - 1 : b[1] * kBrick,
               b[2] * kBrick + j};
        }
        else {
          u = {b[0] * kBrick + i,
               b[1] * kBrick + j,
               d[2] > 0 ? b[2] * kBrick + kBrick - 1 : b[2] * kBrick};
        }
        const Index w = {u.x + d[0], u.y + d[1], u.z + d[2]};
        if (free_in(bits_here, u) && free_in(bits_there, w)) {
          if (r_u == nullptr) {
            /* (Only whether it is open.) */
            return true;
          }
          best_d = off;
          *r_u = u;
          *r_w = w;
        }
      }
    }
    return best_d != INT32_MAX;
  };
  std::vector<uint64_t> chain;
  bool coarse_reached = false, unreachable = false;
  if (goal_inside) {
    struct Coarse {
      float g = 0.0f;
      uint64_t parent = 0;
      uint8_t dir = 0;
      bool closed = false;
    };
    thread_local std::unordered_map<uint64_t, Coarse> coarse;
    coarse.clear();
    const int gb[3] = {goal_voxel.x >> 3, goal_voxel.y >> 3, goal_voxel.z >> 3};
    const uint64_t start_key = BrickKey(start_voxel.x >> 3, start_voxel.y >> 3, start_voxel.z >> 3);
    const uint64_t goal_key = BrickKey(gb[0], gb[1], gb[2]);
    auto h_brick = [&](const uint64_t key) {
      int b[3];
      brick_xyz(key, b);
      return float(std::abs(b[0] - gb[0]) + std::abs(b[1] - gb[1]) + std::abs(b[2] - gb[2]));
    };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    /* (Weighted as the fine search: with unit steps and the Manhattan estimate every brick of
     * the box between the two ends ties, and a diagonal 460 m long spread over 300,000.) */
    coarse[start_key] = {0.0f, start_key, 0, false};
    open.push({h_brick(start_key) * kGreedy, start_key});
    uint64_t best = start_key;
    float best_h = h_brick(start_key);
    int expanded = 0;
    while (!open.empty() && expanded < max_nodes * 4) {
      const uint64_t key = open.top().second;
      open.pop();
      float g_here;
      {
        Coarse &entry = coarse[key];
        if (entry.closed) {
          continue;
        }
        entry.closed = true;
        g_here = entry.g;
      }
      expanded++;
      const float h = h_brick(key);
      if (h < best_h) {
        best_h = h;
        best = key;
      }
      if (key == goal_key) {
        best = key;
        coarse_reached = true;
        break;
      }
      int b[3];
      brick_xyz(key, b);
      for (int k = 0; k < 6; k++) {
        const int *d = kDirs[k];
        const int nb[3] = {b[0] + d[0], b[1] + d[1], b[2] + d[2]};
        if (nb[0] < 0 || nb[1] < 0 || nb[2] < 0 || nb[0] >= bricks_n[0] ||
            nb[1] >= bricks_n[1] || nb[2] >= bricks_n[2])
        {
          continue;
        }
        if (!(!water_only && BrickWholeFree(b[0], b[1], b[2]) &&
              BrickWholeFree(nb[0], nb[1], nb[2])) &&
            !portal(b, d, nullptr, nullptr))
        {
          continue;
        }
        const uint64_t next = BrickKey(nb[0], nb[1], nb[2]);
        const float g = g_here + 1.0f;
        auto it = coarse.find(next);
        if (it != coarse.end() && (it->second.closed || it->second.g <= g)) {
          continue;
        }
        coarse[next] = {g, key, uint8_t(k), false};
        open.push({g + h_brick(next) * kGreedy, next});
      }
    }
    unreachable = !coarse_reached && open.empty();
    for (uint64_t key = best;; key = coarse[key].parent) {
      chain.push_back(key);
      if (key == start_key) {
        break;
      }
    }
    std::reverse(chain.begin(), chain.end());
  }

  std::vector<MT_Vector3> raw;
  bool reached = false;
  if (coarse_reached) {
    /* The coarse way's portals (a free voxel pair of each face it crosses, nearest the face's
     * middle), joined by straight lines where they see each other, else by a fine search kept
     * to the two bricks and those round them: short searches only. */
    std::vector<MT_Vector3> waypoints;
    for (size_t i = 0; i + 1 < chain.size(); i++) {
      int b[3], nb[3];
      brick_xyz(chain[i], b);
      brick_xyz(chain[i + 1], nb);
      const int d[3] = {nb[0] - b[0], nb[1] - b[1], nb[2] - b[2]};
      Index u, w;
      if (portal(b, d, &u, &w)) {
        waypoints.push_back((CenterOf(u) + CenterOf(w)) * 0.5f);
      }
    }
    waypoints.push_back(goal);
    reached = true;
    MT_Vector3 last = start;
    std::vector<MT_Vector3> piece;
    for (const MT_Vector3 &point : waypoints) {
      if (LineFree(last, point, water_only)) {
        raw.push_back(point);
        last = point;
        continue;
      }
      std::unordered_set<uint64_t> round_ends;
      for (const MT_Vector3 &end : {last, point}) {
        const Index e = VoxelOf(end);
        const int eb[3] = {e.x >> 3, e.y >> 3, e.z >> 3};
        for (int dz = -1; dz <= 1; dz++) {
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              if (eb[0] + dx >= 0 && eb[1] + dy >= 0 && eb[2] + dz >= 0) {
                round_ends.insert(BrickKey(eb[0] + dx, eb[1] + dy, eb[2] + dz));
              }
            }
          }
        }
      }
      if (!fine_search(last, point, &round_ends, std::max(max_nodes / 4, 2000), piece)) {
        reached = false;
        break;
      }
      raw.insert(raw.end(), piece.begin(), piece.end());
      last = point;
    }
  }
  if (!reached && !unreachable) {
    /* (The portals' joins failed: a brick's free part split inside.) The fine search over the
     * coarse way's bricks and those round them, then everywhere. */
    std::unordered_set<uint64_t> corridor;
    for (const uint64_t key : chain) {
      int b[3];
      brick_xyz(key, b);
      corridor.insert(key);
      for (const auto &d : kDirs) {
        if (b[0] + d[0] >= 0 && b[1] + d[1] >= 0 && b[2] + d[2] >= 0) {
          corridor.insert(BrickKey(b[0] + d[0], b[1] + d[1], b[2] + d[2]));
        }
      }
    }
    reached = fine_search(start, goal, chain.empty() ? nullptr : &corridor, max_nodes, raw);
    if (!reached && !chain.empty()) {
      reached = fine_search(start, goal, nullptr, max_nodes, raw);
    }
  }
  else if (!reached) {
    /* No way: the nearest the coarse search came (its bricks), searched finely there. */
    std::unordered_set<uint64_t> corridor(chain.begin(), chain.end());
    fine_search(start, goal, &corridor, max_nodes, raw);
  }
  return finish(raw, reached);
}

/* -------------------------------------------------------------------- */
/** \name Surfaces: walls and ceilings (navigation phase 9, 2026-11-19)
 * \{ */

namespace {

void key_xyz(const uint64_t key, int *r)
{
  r[0] = int(key & 0x1FFFFF);
  r[1] = int((key >> 21) & 0x1FFFFF);
  r[2] = int((key >> 42) & 0x1FFFFF);
}

/** A ceiling's normal points at least this far down (an overhang steeper than 30 degrees past
 * the vertical is still a wall). */
constexpr float kCeilingCos = -0.5f;
/** A triangle over more cells than this is looked at every time (m_surfaceBig). */
constexpr int kMaxCellsATriangle = 1024;

}  // namespace

bool KX_NavAirMap::VoxelSolidLevel(const Index &v) const
{
  if (m_open || v.x < 0 || v.y < 0 || v.z < 0 || v.x >= m_size[0] || v.y >= m_size[1] ||
      v.z >= m_size[2])
  {
    return false;
  }
  auto it = m_bricks.find(BrickKey(v.x >> 3, v.y >> 3, v.z >> 3));
  return it != m_bricks.end() &&
         (it->second.full || bit(it->second.solid, v.x & 7, v.y & 7, v.z & 7));
}

int KX_NavAirMap::ShellKind(const Index &v) const
{
  if (v.x < 0 || v.y < 0 || v.z < 0) {
    return 0;
  }
  auto it = m_shell.find(BrickKey(v.x >> 3, v.y >> 3, v.z >> 3));
  if (it == m_shell.end()) {
    return 0;
  }
  const int x = v.x & 7, y = v.y & 7, z = v.z & 7;
  if (!bit(it->second.shell, x, y, z)) {
    return 0;
  }
  if (bit(it->second.floor, x, y, z)) {
    return SurfaceFloor;
  }
  if (bit(it->second.ceiling, x, y, z)) {
    return SurfaceCeiling;
  }
  return SurfaceWall;
}

int KX_NavAirMap::SurfaceKindAt(const MT_Vector3 &p) const
{
  return m_surfaces && !m_open ? ShellKind(VoxelOf(p)) : 0;
}

void KX_NavAirMap::ShellKeysAround(const int *b0, const int *b1, std::vector<uint64_t> &r_keys) const
{
  r_keys.clear();
  if (m_open) {
    return;
  }
  const int n[3] = {(m_size[0] + kBrick - 1) / kBrick,
                    (m_size[1] + kBrick - 1) / kBrick,
                    (m_size[2] + kBrick - 1) / kBrick};
  int lo[3], hi[3];
  size_t range = 1;
  for (int axis = 0; axis < 3; axis++) {
    lo[axis] = std::max(b0[axis] - 1, 0);
    hi[axis] = std::min(b1[axis] + 1, n[axis] - 1);
    if (lo[axis] > hi[axis]) {
      return;
    }
    range *= size_t(hi[axis] - lo[axis] + 1);
  }
  if (range <= 4 * (m_bricks.size() + m_shell.size()) + 64) {
    for (int bz = lo[2]; bz <= hi[2]; bz++) {
      for (int by = lo[1]; by <= hi[1]; by++) {
        for (int bx = lo[0]; bx <= hi[0]; bx++) {
          r_keys.push_back(BrickKey(bx, by, bz));
        }
      }
    }
  }
  else if (range <= (size_t(1) << 26)) {
    /* (A big box, most of it empty air: the bricks round the solid ones, and those that held a
     * shell, which may lose it; marked in a bitmap of the box's bricks, at most 8 MB.) */
    const size_t w = size_t(hi[0] - lo[0] + 1), h = size_t(hi[1] - lo[1] + 1);
    std::vector<uint64_t> marks((range + 63) / 64, 0);
    auto mark = [&](const int x, const int y, const int z) {
      if (x < lo[0] || y < lo[1] || z < lo[2] || x > hi[0] || y > hi[1] || z > hi[2]) {
        return;
      }
      const size_t i = size_t(x - lo[0]) + w * (size_t(y - lo[1]) + h * size_t(z - lo[2]));
      marks[i >> 6] |= uint64_t(1) << (i & 63);
    };
    for (const auto &item : m_bricks) {
      int b[3];
      key_xyz(item.first, b);
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            mark(b[0] + dx, b[1] + dy, b[2] + dz);
          }
        }
      }
    }
    for (const auto &item : m_shell) {
      int b[3];
      key_xyz(item.first, b);
      mark(b[0], b[1], b[2]);
    }
    for (size_t word = 0; word < marks.size(); word++) {
      uint64_t bits = marks[word];
      while (bits != 0) {
        const size_t i = (word << 6) + size_t(std::countr_zero(bits));
        bits &= bits - 1;
        const int x = int(i % w) + lo[0], y = int((i / w) % h) + lo[1], z = int(i / (w * h)) + lo[2];
        r_keys.push_back(BrickKey(x, y, z));
      }
    }
  }
  else {
    /* (A box too big for the bitmap: a set.) */
    auto in = [&](const int *b) {
      return b[0] >= lo[0] && b[1] >= lo[1] && b[2] >= lo[2] && b[0] <= hi[0] &&
             b[1] <= hi[1] && b[2] <= hi[2];
    };
    std::unordered_set<uint64_t> keys;
    for (const auto &item : m_bricks) {
      int b[3];
      key_xyz(item.first, b);
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            const int nb[3] = {b[0] + dx, b[1] + dy, b[2] + dz};
            if (in(nb)) {
              keys.insert(BrickKey(nb[0], nb[1], nb[2]));
            }
          }
        }
      }
    }
    for (const auto &item : m_shell) {
      int b[3];
      key_xyz(item.first, b);
      if (in(b)) {
        keys.insert(item.first);
      }
    }
    r_keys.assign(keys.begin(), keys.end());
  }
  std::sort(r_keys.begin(), r_keys.end());
}

void KX_NavAirMap::ComputeShell(const std::vector<uint64_t> &keys, LN_JobPool *pool)
{
  std::vector<ShellBrick> shells(keys.size());
  std::vector<uint8_t> any(keys.size(), 0);
  const int n[3] = {(m_size[0] + kBrick - 1) / kBrick,
                    (m_size[1] + kBrick - 1) / kBrick,
                    (m_size[2] + kBrick - 1) / kBrick};
  run_jobs(pool, keys.size(), [&](const size_t index) {
    int b[3];
    key_xyz(keys[index], b);
    /* The 27 bricks round it (null: no solid there, or beyond the box). */
    const Brick *round[3][3][3];
    bool some = false;
    for (int dz = 0; dz < 3; dz++) {
      for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
          const int nb[3] = {b[0] + dx - 1, b[1] + dy - 1, b[2] + dz - 1};
          round[dz][dy][dx] = nullptr;
          if (nb[0] < 0 || nb[1] < 0 || nb[2] < 0 || nb[0] >= n[0] || nb[1] >= n[1] ||
              nb[2] >= n[2])
          {
            continue;
          }
          auto it = m_bricks.find(BrickKey(nb[0], nb[1], nb[2]));
          if (it != m_bricks.end()) {
            round[dz][dy][dx] = &it->second;
            some = true;
          }
        }
      }
    }
    if (!some || (round[1][1][1] != nullptr && round[1][1][1]->full)) {
      return;
    }
    /* The level's solids of the brick and a voxel round it, as rows of bits (grid[z][y], bit x;
     * local 0 and 9 the neighbours' voxels; beyond the box: none). */
    uint16_t grid[10][10] = {};
    uint16_t x_in_box = 0;
    for (int lx = 0; lx < 10; lx++) {
      const int gx = b[0] * kBrick + lx - 1;
      if (gx >= 0 && gx < m_size[0]) {
        x_in_box |= uint16_t(1u << lx);
      }
    }
    for (int lz = 0; lz < 10; lz++) {
      const int gz = b[2] * kBrick + lz - 1;
      if (gz < 0 || gz >= m_size[2]) {
        continue;
      }
      const int rz = (gz >> 3) - b[2] + 1;
      for (int ly = 0; ly < 10; ly++) {
        const int gy = b[1] * kBrick + ly - 1;
        if (gy < 0 || gy >= m_size[1]) {
          continue;
        }
        const int ry = (gy >> 3) - b[1] + 1;
        uint16_t row = 0;
        for (int rx = 0; rx < 3; rx++) {
          const Brick *brick = round[rz][ry][rx];
          if (brick == nullptr) {
            continue;
          }
          const uint32_t bits = brick->full ?
                                    0xFFu :
                                    uint32_t((brick->solid[gz & 7] >> (kBrick * (gy & 7))) & 0xFFu);
          if (rx == 0) {
            row |= uint16_t((bits >> 7) & 1u);
          }
          else if (rx == 1) {
            row |= uint16_t(bits << 1);
          }
          else {
            row |= uint16_t((bits & 1u) << 9);
          }
        }
        grid[lz][ly] = uint16_t(row & x_in_box);
      }
    }
    /* Touching a solid (26 round): the solids grown by a voxel, row by row. */
    uint16_t grown_x[10][10], grown_xy[10][10];
    for (int lz = 0; lz < 10; lz++) {
      for (int ly = 0; ly < 10; ly++) {
        const uint16_t r = grid[lz][ly];
        grown_x[lz][ly] = uint16_t((r | (r << 1) | (r >> 1)) & 0x3FFu);
      }
      for (int ly = 0; ly < 10; ly++) {
        grown_xy[lz][ly] = uint16_t(grown_x[lz][ly] | (ly > 0 ? grown_x[lz][ly - 1] : 0) |
                                    (ly < 9 ? grown_x[lz][ly + 1] : 0));
      }
    }
    ShellBrick &out = shells[index];
    bool found = false;
    for (int z = 0; z < kBrick; z++) {
      const int gz = b[2] * kBrick + z;
      if (gz >= m_size[2]) {
        break;
      }
      for (int y = 0; y < kBrick; y++) {
        const int gy = b[1] * kBrick + y;
        if (gy >= m_size[1]) {
          break;
        }
        const uint16_t touching = uint16_t(grown_xy[z][y + 1] | grown_xy[z + 1][y + 1] |
                                           grown_xy[z + 2][y + 1]);
        /* Free voxels of the box touching a solid (local bits 1..8). */
        uint16_t shell_row = uint16_t(touching & ~grid[z + 1][y + 1] & x_in_box & 0x1FEu);
        while (shell_row != 0) {
          const int lx = std::countr_zero(uint32_t(shell_row));
          shell_row = uint16_t(shell_row & (shell_row - 1));
          const int x = lx - 1;
          /* Its normal points away from the solids round it. */
          float nx = 0.0f, ny = 0.0f, nz = 0.0f;
          for (int oz = -1; oz <= 1; oz++) {
            for (int oy = -1; oy <= 1; oy++) {
              const uint16_t r = grid[z + 1 + oz][y + 1 + oy];
              for (int ox = -1; ox <= 1; ox++) {
                if ((ox | oy | oz) == 0 || ((r >> (lx + ox)) & 1u) == 0) {
                  continue;
                }
                const float inv = 1.0f / std::sqrt(float(ox * ox + oy * oy + oz * oz));
                nx -= float(ox) * inv;
                ny -= float(oy) * inv;
                nz -= float(oz) * inv;
              }
            }
          }
          set_bit(out.shell, x, y, z);
          found = true;
          const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
          /* (Enclosed evenly: no way out, a wall.) */
          const float up = length > 1e-6f ? nz / length : 0.0f;
          if (up >= m_floorCos) {
            set_bit(out.floor, x, y, z);
          }
          else if (up <= kCeilingCos) {
            set_bit(out.ceiling, x, y, z);
          }
        }
      }
    }
    any[index] = found ? 1 : 0;
  });
  for (size_t i = 0; i < keys.size(); i++) {
    if (any[i]) {
      m_shell[keys[i]] = shells[i];
    }
    else {
      m_shell.erase(keys[i]);
    }
  }
}

void KX_NavAirMap::EnableSurfaces(const KX_NavGeometry &geometry,
                                  const float floor_cos,
                                  LN_JobPool *pool)
{
  m_surfaces = true;
  m_floorCos = std::clamp(floor_cos, 0.0f, 1.0f);
  SetSurfaceGeometry(geometry);
  m_shell.clear();
  if (m_open) {
    return;
  }
  const int b0[3] = {0, 0, 0};
  const int b1[3] = {(m_size[0] - 1) / kBrick, (m_size[1] - 1) / kBrick, (m_size[2] - 1) / kBrick};
  std::vector<uint64_t> keys;
  ShellKeysAround(b0, b1, keys);
  ComputeShell(keys, pool);
}

void KX_NavAirMap::SetSurfaceGeometry(const KX_NavGeometry &geometry)
{
  m_surfaceVerts = geometry.verts;
  m_surfaceTris = geometry.tris;
  BinSurfaceGeometry();
}

void KX_NavAirMap::BinSurfaceGeometry()
{
  m_surfaceCells.clear();
  m_surfaceBig.clear();
  m_cellSize = std::max(2.0f, m_settings.radius * 4.0f);
  const float inv = 1.0f / m_cellSize;
  const int ntris = int(m_surfaceTris.size() / 3);
  for (int t = 0; t < ntris; t++) {
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int k = 0; k < 3; k++) {
      const float *p = &m_surfaceVerts[size_t(m_surfaceTris[size_t(t) * 3 + k]) * 3];
      for (int axis = 0; axis < 3; axis++) {
        lo[axis] = std::min(lo[axis], p[axis]);
        hi[axis] = std::max(hi[axis], p[axis]);
      }
    }
    int c0[3], c1[3];
    int64_t cells = 1;
    for (int axis = 0; axis < 3; axis++) {
      c0[axis] = int(std::floor(lo[axis] * inv));
      c1[axis] = int(std::floor(hi[axis] * inv));
      cells *= int64_t(c1[axis] - c0[axis] + 1);
    }
    if (cells > kMaxCellsATriangle) {
      m_surfaceBig.push_back(t);
      continue;
    }
    for (int cz = c0[2]; cz <= c1[2]; cz++) {
      for (int cy = c0[1]; cy <= c1[1]; cy++) {
        for (int cx = c0[0]; cx <= c1[0]; cx++) {
          m_surfaceCells[cell_key(cx, cy, cz)].push_back(t);
        }
      }
    }
  }
}

bool KX_NavAirMap::NearestSurface(const MT_Vector3 &p,
                                  const float reach,
                                  MT_Vector3 &r_point,
                                  MT_Vector3 &r_normal,
                                  const MT_Vector3 *hint) const
{
  if (m_surfaceTris.empty() || !(reach > 0.0f)) {
    return false;
  }
  const V3 q = {float(p[0]), float(p[1]), float(p[2])};
  const auto corner = [&](const int t, const int k) {
    const float *v = &m_surfaceVerts[size_t(m_surfaceTris[size_t(t) * 3 + k]) * 3];
    return V3{v[0], v[1], v[2]};
  };
  float best = reach * reach;
  int best_t = -1;
  V3 best_point;
  /* (The nearest, ties to the lower triangle number: the same answer in any cell order.) */
  auto test = [&](const int t) {
    const V3 point = closest_point_triangle(q, corner(t, 0), corner(t, 1), corner(t, 2));
    const V3 d = sub(q, point);
    const float d2 = dot(d, d);
    if (d2 < best || (d2 == best && best_t >= 0 && t < best_t)) {
      best = d2;
      best_t = t;
      best_point = point;
    }
  };
  const float inv = 1.0f / m_cellSize;
  int c0[3], c1[3];
  for (int axis = 0; axis < 3; axis++) {
    c0[axis] = int(std::floor((float(p[axis]) - reach) * inv));
    c1[axis] = int(std::floor((float(p[axis]) + reach) * inv));
  }
  for (int cz = c0[2]; cz <= c1[2]; cz++) {
    for (int cy = c0[1]; cy <= c1[1]; cy++) {
      for (int cx = c0[0]; cx <= c1[0]; cx++) {
        auto it = m_surfaceCells.find(cell_key(cx, cy, cz));
        if (it == m_surfaceCells.end()) {
          continue;
        }
        for (const int t : it->second) {
          test(t);
        }
      }
    }
  }
  for (const int t : m_surfaceBig) {
    test(t);
  }
  if (best_t < 0) {
    return false;
  }
  r_point = MT_Vector3(best_point.x, best_point.y, best_point.z);
  const V3 d = sub(q, best_point);
  const float length = std::sqrt(dot(d, d));
  if (length > 1e-5f) {
    r_normal = MT_Vector3(d.x / length, d.y / length, d.z / length);
    return true;
  }
  /* On the surface: the triangle's face, toward the hint. */
  const V3 a = corner(best_t, 0), b = corner(best_t, 1), c = corner(best_t, 2);
  const V3 ab = sub(b, a), ac = sub(c, a);
  V3 face = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
  const float face_length = std::sqrt(dot(face, face));
  if (face_length < 1e-12f) {
    r_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
    return true;
  }
  r_normal = MT_Vector3(face.x / face_length, face.y / face_length, face.z / face_length);
  if (hint != nullptr && float(r_normal.dot(*hint)) < 0.0f) {
    r_normal = -r_normal;
  }
  return true;
}

bool KX_NavAirMap::SupportSurface(const MT_Vector3 &p,
                                  const float reach,
                                  const float touch,
                                  const MT_Vector3 &along,
                                  MT_Vector3 &r_point,
                                  MT_Vector3 &r_normal,
                                  const MT_Vector3 *hint) const
{
  if (m_surfaceTris.empty() || !(reach > 0.0f)) {
    return false;
  }
  const V3 q = {float(p[0]), float(p[1]), float(p[2])};
  const auto corner = [&](const int t, const int k) {
    const float *v = &m_surfaceVerts[size_t(m_surfaceTris[size_t(t) * 3 + k]) * 3];
    return V3{v[0], v[1], v[2]};
  };
  struct Candidate {
    int t;
    float d2;
    V3 point;
  };
  thread_local std::vector<Candidate> candidates;
  candidates.clear();
  float best = reach * reach;
  auto test = [&](const int t) {
    const V3 point = closest_point_triangle(q, corner(t, 0), corner(t, 1), corner(t, 2));
    const V3 d = sub(q, point);
    const float d2 = dot(d, d);
    if (d2 <= reach * reach) {
      candidates.push_back({t, d2, point});
      best = std::min(best, d2);
    }
  };
  const float inv = 1.0f / m_cellSize;
  int c0[3], c1[3];
  for (int axis = 0; axis < 3; axis++) {
    c0[axis] = int(std::floor((float(p[axis]) - reach) * inv));
    c1[axis] = int(std::floor((float(p[axis]) + reach) * inv));
  }
  for (int cz = c0[2]; cz <= c1[2]; cz++) {
    for (int cy = c0[1]; cy <= c1[1]; cy++) {
      for (int cx = c0[0]; cx <= c1[0]; cx++) {
        auto it = m_surfaceCells.find(cell_key(cx, cy, cz));
        if (it == m_surfaceCells.end()) {
          continue;
        }
        for (const int t : it->second) {
          test(t);
        }
      }
    }
  }
  for (const int t : m_surfaceBig) {
    test(t);
  }
  if (candidates.empty()) {
    return false;
  }
  /* Touching: within `touch`, or within a hair of the nearest. */
  const float limit = std::max(std::sqrt(best) + 0.03f, touch);
  const float along_length = float(along.length());
  const MT_Vector3 way = along_length > 1e-6f ? along / along_length :
                                                MT_Vector3(0.0f, 0.0f, 0.0f);
  /* Each touching candidate: its face's way out (its score: how much the way runs across it),
   * its point's way out (the normal handed back: round an edge it turns). */
  struct Scored {
    float score, align, d2;
    int t;
    MT_Vector3 point, normal;
  };
  thread_local std::vector<Scored> scored;
  scored.clear();
  float least = FLT_MAX;
  for (const Candidate &c : candidates) {
    if (c.d2 > limit * limit) {
      continue;
    }
    const V3 a = corner(c.t, 0), b = corner(c.t, 1), cc = corner(c.t, 2);
    const V3 ab = sub(b, a), ac = sub(cc, a);
    const V3 face = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
    const float face_length = std::sqrt(dot(face, face));
    if (face_length < 1e-12f) {
      continue;
    }
    const MT_Vector3 face_normal(face.x / face_length, face.y / face_length, face.z / face_length);
    MT_Vector3 normal;
    const V3 d = sub(q, c.point);
    const float length = std::sqrt(c.d2);
    if (length > 1e-5f) {
      normal = MT_Vector3(d.x / length, d.y / length, d.z / length);
    }
    else {
      normal = face_normal;
      if (hint != nullptr && float(normal.dot(*hint)) < 0.0f) {
        normal = -normal;
      }
    }
    /* (Its face's way, never the way from a point on its edge: the other triangle of a flat face
     * touches too, from its diagonal, a hair farther and slanted: chosen, it tilted a wall and a
     * Character walked up it for good.) */
    const float score = std::fabs(float(way.dot(face_normal)));
    const float align = hint != nullptr ? std::fabs(float(face_normal.dot(*hint))) : 0.0f;
    scored.push_back({score, align, c.d2, c.t, MT_Vector3(c.point.x, c.point.y, c.point.z),
                      normal});
    least = std::min(least, score);
  }
  /* The way along it best; on a near tie (its way along a wall at the wall's foot runs along
   * both) the surface it stands on (`hint`: by the nearer, a Character's capsule walking along a
   * corridor's wall stood on the wall); then the nearer; then the lower triangle number: the
   * same answer in any cell order. */
  const Scored *chosen = nullptr;
  for (const Scored &s : scored) {
    if (s.score > least + 0.05f) {
      continue;
    }
    if (chosen == nullptr || s.align > chosen->align + 1e-4f ||
        (s.align >= chosen->align - 1e-4f &&
         (s.d2 < chosen->d2 || (s.d2 == chosen->d2 && s.t < chosen->t))))
    {
      chosen = &s;
    }
  }
  if (chosen == nullptr) {
    return false;
  }
  r_point = chosen->point;
  r_normal = chosen->normal;
  return true;
}

bool KX_NavAirMap::NearestShell(const MT_Vector3 &p,
                                const float reach,
                                const int kinds,
                                MT_Vector3 &r_point) const
{
  if (!m_surfaces || m_open) {
    return false;
  }
  const float v = m_settings.voxel;
  const Index s = VoxelOf(p);
  if (ShellKind(s) & kinds) {
    r_point = CenterOf(s);
    return true;
  }
  const int shells = int(std::ceil(reach / v));
  float best = FLT_MAX;
  Index found;
  for (int r = 1; r <= shells; r++) {
    for (int dz = -r; dz <= r; dz++) {
      for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
          if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) {
            continue;
          }
          const Index u = {s.x + dx, s.y + dy, s.z + dz};
          if (!(ShellKind(u) & kinds)) {
            continue;
          }
          const float d2 = float((CenterOf(u) - p).length2());
          if (d2 < best) {
            best = d2;
            found = u;
          }
        }
      }
    }
    if (best <= float(r) * v * float(r) * v) {
      break;
    }
  }
  if (best > reach * reach) {
    return false;
  }
  r_point = CenterOf(found);
  return true;
}

bool KX_NavAirMap::SurfaceLineFree(const MT_Vector3 &a, const MT_Vector3 &b, const int kinds) const
{
  if (!m_surfaces || m_open) {
    return false;
  }
  /* On the shell of `kinds`, or free air just off it (one voxel: the shell's staircase on a
   * slanted face); never a solid, never a shell of another kind (a ceiling for one that climbs
   * walls only). */
  auto near_shell = [&](const Index &u) {
    const int kind = ShellKind(u);
    if (kind != 0) {
      return (kind & kinds) != 0;
    }
    if (!VoxelFree(u, false) || u.z >= m_size[2]) {
      return false;
    }
    static const int kDirs6[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto &d : kDirs6) {
      if (ShellKind({u.x + d[0], u.y + d[1], u.z + d[2]}) & kinds) {
        return true;
      }
    }
    return false;
  };
  /* Amanatides and Woo's walk, as LineFree's. */
  Index cur = VoxelOf(a);
  const Index end = VoxelOf(b);
  if (!near_shell(cur)) {
    return false;
  }
  const float v = m_settings.voxel;
  const float d[3] = {float(b[0] - a[0]), float(b[1] - a[1]), float(b[2] - a[2])};
  int step[3];
  float t_max[3], t_delta[3];
  int *coord[3] = {&cur.x, &cur.y, &cur.z};
  for (int axis = 0; axis < 3; axis++) {
    if (std::fabs(d[axis]) < 1e-9f) {
      step[axis] = 0;
      t_max[axis] = FLT_MAX;
      t_delta[axis] = FLT_MAX;
      continue;
    }
    step[axis] = d[axis] > 0.0f ? 1 : -1;
    const float boundary = float(m_origin[axis]) +
                           float(*coord[axis] + (step[axis] > 0 ? 1 : 0)) * v;
    t_max[axis] = (boundary - float(a[axis])) / d[axis];
    t_delta[axis] = v / std::fabs(d[axis]);
  }
  const int steps = std::abs(end.x - cur.x) + std::abs(end.y - cur.y) + std::abs(end.z - cur.z);
  for (int i = 0; i < steps; i++) {
    int axis = 0;
    if (t_max[1] < t_max[axis]) {
      axis = 1;
    }
    if (t_max[2] < t_max[axis]) {
      axis = 2;
    }
    if (step[axis] == 0) {
      break;
    }
    *coord[axis] += step[axis];
    t_max[axis] += t_delta[axis];
    if (!near_shell(cur)) {
      return false;
    }
  }
  return near_shell(end);
}

bool KX_NavAirMap::FindSurfacePath(const MT_Vector3 &from,
                                   const MT_Vector3 &to,
                                   const int kinds,
                                   const SurfaceCosts &costs,
                                   std::vector<MT_Vector3> &r_points,
                                   bool *r_complete,
                                   const float goal_reach,
                                   const int max_nodes) const
{
  r_points.clear();
  if (r_complete != nullptr) {
    *r_complete = false;
  }
  if (!m_surfaces || m_open || kinds == 0) {
    return false;
  }
  const float v = m_settings.voxel;
  /* Both ends onto the shell (the body sits nearer its surface than the shell: within its
   * radius and two voxels). */
  MT_Vector3 start, goal;
  if (!NearestShell(from, std::max(2.0f * m_settings.radius + 2.0f * v, 1.0f), kinds, start)) {
    return false;
  }
  const bool goal_found = NearestShell(to, std::max(goal_reach, 2.0f * v), kinds, goal);
  if (!goal_found) {
    goal = to;
  }
  auto cost_of = [&](const int kind) {
    return kind == SurfaceFloor ? costs.floor :
           kind == SurfaceCeiling ? costs.ceiling :
                                    costs.wall;
  };
  float least = FLT_MAX;
  if (kinds & SurfaceFloor) {
    least = std::min(least, costs.floor);
  }
  if (kinds & SurfaceWall) {
    least = std::min(least, costs.wall);
  }
  if (kinds & SurfaceCeiling) {
    least = std::min(least, costs.ceiling);
  }
  least = std::max(least, 1e-3f);
  auto finish = [&](const std::vector<MT_Vector3> &raw, const bool reached) {
    /* Straightened along the faces: from each corner the farthest one a straight move along
     * the shell reaches (found by doubling, then halving: one check a step, not every one). */
    MT_Vector3 anchor = start;
    if ((start - from).length2() > 1e-8f) {
      r_points.push_back(start);
    }
    size_t i = 0;
    while (i < raw.size()) {
      size_t good = i, step = 1;
      while (good + step < raw.size() && SurfaceLineFree(anchor, raw[good + step], kinds)) {
        good += step;
        step *= 2;
      }
      size_t bad = std::min(good + step, raw.size());
      while (bad - good > 1) {
        const size_t mid = good + (bad - good) / 2;
        if (SurfaceLineFree(anchor, raw[mid], kinds)) {
          good = mid;
        }
        else {
          bad = mid;
        }
      }
      r_points.push_back(raw[good]);
      anchor = raw[good];
      i = good + 1;
    }
    if (r_complete != nullptr) {
      *r_complete = reached && goal_found;
    }
    return !r_points.empty() || reached;
  };
  if (goal_found && SurfaceLineFree(start, goal, kinds)) {
    return finish({goal}, true);
  }

  const uint64_t nx = uint64_t(m_size[0]), ny = uint64_t(m_size[1]);
  const int bricks_n[3] = {(m_size[0] + kBrick - 1) / kBrick,
                           (m_size[1] + kBrick - 1) / kBrick,
                           (m_size[2] + kBrick - 1) / kBrick};
  using Open = std::pair<float, uint64_t>;
  /* A brick's voxels of `kinds` (8 words; all zero: none). */
  auto allowed_of = [&](const uint64_t key, uint64_t *r) {
    auto it = m_shell.find(key);
    if (it == m_shell.end()) {
      std::fill(r, r + 8, uint64_t(0));
      return false;
    }
    bool any = false;
    for (int z = 0; z < kBrick; z++) {
      const ShellBrick &s = it->second;
      uint64_t take = 0;
      if (kinds & SurfaceFloor) {
        take |= s.floor[z];
      }
      if (kinds & SurfaceCeiling) {
        take |= s.ceiling[z];
      }
      if (kinds & SurfaceWall) {
        take |= ~(s.floor[z] | s.ceiling[z]);
      }
      r[z] = s.shell[z] & take;
      any = any || r[z] != 0;
    }
    return any;
  };
  const Index start_voxel = VoxelOf(start), goal_voxel = VoxelOf(goal);

  /* The coarse search over bricks (as FindPath's): a step to a face neighbour where a voxel of
   * `kinds` on this side meets one across (straight or a voxel aside: the shell's staircase). */
  auto open_between = [&](const int *b, const int *d) {
    const int nb[3] = {b[0] + d[0], b[1] + d[1], b[2] + d[2]};
    if (nb[0] < 0 || nb[1] < 0 || nb[2] < 0 || nb[0] >= bricks_n[0] || nb[1] >= bricks_n[1] ||
        nb[2] >= bricks_n[2])
    {
      return false;
    }
    uint64_t here[8], there[8];
    if (!allowed_of(BrickKey(b[0], b[1], b[2]), here) ||
        !allowed_of(BrickKey(nb[0], nb[1], nb[2]), there))
    {
      return false;
    }
    const int axis = d[0] != 0 ? 0 : (d[1] != 0 ? 1 : 2);
    const int side_here = (d[axis] > 0) ? kBrick - 1 : 0;
    const int side_there = kBrick - 1 - side_here;
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (int i = 0; i < kBrick; i++) {
      for (int j = 0; j < kBrick; j++) {
        int u[3];
        u[axis] = side_here;
        u[a1] = i;
        u[a2] = j;
        if (!bit(here, u[0], u[1], u[2])) {
          continue;
        }
        for (int di = -1; di <= 1; di++) {
          for (int dj = -1; dj <= 1; dj++) {
            int w[3];
            w[axis] = side_there;
            w[a1] = i + di;
            w[a2] = j + dj;
            if (w[a1] < 0 || w[a1] >= kBrick || w[a2] < 0 || w[a2] >= kBrick) {
              continue;
            }
            if (bit(there, w[0], w[1], w[2])) {
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  static const int kDirs6[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  /* A step into a brick costs its cheapest surface of `kinds` (a brick of wall only, a wall's:
   * dear walls lead the coarse way round them, and the fine search is kept to it). */
  auto brick_cost = [&](const uint64_t key) {
    auto it = m_shell.find(key);
    if (it == m_shell.end()) {
      return least;
    }
    bool floor = false, wall = false, ceiling = false;
    for (int z = 0; z < kBrick; z++) {
      const ShellBrick &s = it->second;
      floor = floor || (s.shell[z] & s.floor[z]) != 0;
      ceiling = ceiling || (s.shell[z] & s.ceiling[z]) != 0;
      wall = wall || (s.shell[z] & ~(s.floor[z] | s.ceiling[z])) != 0;
    }
    float cost = FLT_MAX;
    if (floor && (kinds & SurfaceFloor)) {
      cost = std::min(cost, costs.floor);
    }
    if (wall && (kinds & SurfaceWall)) {
      cost = std::min(cost, costs.wall);
    }
    if (ceiling && (kinds & SurfaceCeiling)) {
      cost = std::min(cost, costs.ceiling);
    }
    return cost == FLT_MAX ? least : std::max(cost, 1e-3f);
  };
  std::vector<uint64_t> chain;
  bool coarse_reached = false, unreachable = false;
  const bool goal_inside = goal_voxel.x >= 0 && goal_voxel.y >= 0 && goal_voxel.z >= 0 &&
                           goal_voxel.x < m_size[0] && goal_voxel.y < m_size[1] &&
                           goal_voxel.z < m_size[2];
  if (goal_found && goal_inside) {
    struct Coarse {
      float g = 0.0f;
      uint64_t parent = 0;
      bool closed = false;
    };
    thread_local std::unordered_map<uint64_t, Coarse> coarse;
    coarse.clear();
    const int gb[3] = {goal_voxel.x >> 3, goal_voxel.y >> 3, goal_voxel.z >> 3};
    const uint64_t start_key = BrickKey(
        start_voxel.x >> 3, start_voxel.y >> 3, start_voxel.z >> 3);
    const uint64_t goal_key = BrickKey(gb[0], gb[1], gb[2]);
    /* (The estimate at the cheapest surface's cost: steps cost a surface's.) */
    auto h_brick = [&](const uint64_t key) {
      int b[3];
      key_xyz(key, b);
      return float(std::abs(b[0] - gb[0]) + std::abs(b[1] - gb[1]) + std::abs(b[2] - gb[2])) *
             least;
    };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    coarse[start_key] = {0.0f, start_key, false};
    open.push({h_brick(start_key) * kGreedy, start_key});
    uint64_t best = start_key;
    float best_h = h_brick(start_key);
    int expanded = 0;
    while (!open.empty() && expanded < max_nodes * 4) {
      const uint64_t key = open.top().second;
      open.pop();
      float g_here;
      {
        Coarse &entry = coarse[key];
        if (entry.closed) {
          continue;
        }
        entry.closed = true;
        g_here = entry.g;
      }
      expanded++;
      const float h = h_brick(key);
      if (h < best_h) {
        best_h = h;
        best = key;
      }
      if (key == goal_key) {
        best = key;
        coarse_reached = true;
        break;
      }
      int b[3];
      key_xyz(key, b);
      for (const auto &d : kDirs6) {
        if (!open_between(b, d)) {
          continue;
        }
        const uint64_t next = BrickKey(b[0] + d[0], b[1] + d[1], b[2] + d[2]);
        const float g = g_here + brick_cost(next);
        auto it = coarse.find(next);
        if (it != coarse.end() && (it->second.closed || it->second.g <= g)) {
          continue;
        }
        coarse[next] = {g, key, false};
        open.push({g + h_brick(next) * kGreedy, next});
      }
    }
    unreachable = !coarse_reached && open.empty();
    for (uint64_t key = best;; key = coarse[key].parent) {
      chain.push_back(key);
      if (key == start_key) {
        break;
      }
    }
    std::reverse(chain.begin(), chain.end());
  }

  /* The fine search over the shell's voxels (26 round, each step its length times the cost of
   * the surface it steps onto), kept to `keep_to`'s bricks when given; its way (after the
   * start) into `r_raw`: to the goal when reached, else to the nearest it came. */
  struct Entry {
    float g = 0.0f;
    uint64_t parent = 0;
    bool closed = false;
  };
  thread_local std::unordered_map<uint64_t, Entry> entries;
  auto node_of = [&](const Index &i) {
    return uint64_t(i.x) + nx * (uint64_t(i.y) + ny * uint64_t(i.z));
  };
  auto index_of = [&](const uint64_t node) {
    const uint64_t z = node / (nx * ny), rest = node % (nx * ny);
    return Index{int(rest % nx), int(rest / nx), int(z)};
  };
  auto fine_search = [&](const std::unordered_set<uint64_t> *keep_to,
                         const int budget,
                         std::vector<MT_Vector3> &r_raw) {
    r_raw.clear();
    entries.clear();
    const uint64_t a_node = node_of(start_voxel);
    const uint64_t b_node = goal_found && goal_inside ? node_of(goal_voxel) : ~uint64_t(0);
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    entries[a_node] = {0.0f, a_node, false};
    open.push({float((start - goal).length()) * least * kGreedy, a_node});
    uint64_t best = a_node;
    float best_h = float((start - goal).length());
    bool reached = false;
    int expanded = 0;
    while (!open.empty() && expanded < budget) {
      const uint64_t node = open.top().second;
      open.pop();
      float g_here;
      {
        Entry &entry = entries[node];
        if (entry.closed) {
          continue;
        }
        entry.closed = true;
        g_here = entry.g;
      }
      expanded++;
      if (node == b_node) {
        reached = true;
        best = node;
        break;
      }
      const Index c = index_of(node);
      const MT_Vector3 here = CenterOf(c);
      const float h = float((here - goal).length());
      if (h < best_h) {
        best_h = h;
        best = node;
      }
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            if ((dx | dy | dz) == 0) {
              continue;
            }
            const Index u = {c.x + dx, c.y + dy, c.z + dz};
            if (u.x < 0 || u.y < 0 || u.z < 0 || u.x >= m_size[0] || u.y >= m_size[1] ||
                u.z >= m_size[2])
            {
              continue;
            }
            const int kind = ShellKind(u);
            if (!(kind & kinds)) {
              continue;
            }
            if (keep_to != nullptr &&
                keep_to->find(BrickKey(u.x >> 3, u.y >> 3, u.z >> 3)) == keep_to->end())
            {
              continue;
            }
            const uint64_t n = node_of(u);
            const float g = g_here + std::sqrt(float(dx * dx + dy * dy + dz * dz)) * v *
                                         cost_of(kind);
            auto it = entries.find(n);
            if (it != entries.end() && (it->second.closed || it->second.g <= g)) {
              continue;
            }
            entries[n] = {g, node, false};
            open.push({g + float((CenterOf(u) - goal).length()) * least * kGreedy, n});
          }
        }
      }
    }
    for (uint64_t node = best; node != a_node; node = entries[node].parent) {
      r_raw.push_back(CenterOf(index_of(node)));
    }
    std::reverse(r_raw.begin(), r_raw.end());
    if (reached) {
      if (r_raw.empty()) {
        r_raw.push_back(goal);
      }
      else {
        r_raw.back() = goal;
      }
    }
    return reached;
  };

  std::vector<MT_Vector3> raw;
  bool reached = false;
  if (!unreachable) {
    /* The coarse way's bricks and those round them first (a tube: few voxels), then
     * everywhere. */
    std::unordered_set<uint64_t> corridor;
    for (const uint64_t key : chain) {
      int b[3];
      key_xyz(key, b);
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            if (b[0] + dx >= 0 && b[1] + dy >= 0 && b[2] + dz >= 0) {
              corridor.insert(BrickKey(b[0] + dx, b[1] + dy, b[2] + dz));
            }
          }
        }
      }
    }
    reached = fine_search(coarse_reached ? &corridor : nullptr, max_nodes, raw);
    if (!reached && coarse_reached) {
      reached = fine_search(nullptr, max_nodes, raw);
    }
  }
  else {
    /* No way: the nearest the coarse search came, searched finely there. */
    std::unordered_set<uint64_t> corridor(chain.begin(), chain.end());
    fine_search(&corridor, max_nodes, raw);
  }
  return finish(raw, reached);
}

/** \} */
