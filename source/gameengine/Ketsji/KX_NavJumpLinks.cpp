/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavJumpLinks.cpp
 *  \ingroup ketsji
 */

#include "KX_NavJumpLinks.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "KX_Detour.h"
#include "KX_NavBuild.h"

namespace {

/** Along an open edge, a sample every this much (m). */
constexpr float kSampleStep = 0.5f;
/** The landing is looked for outward in steps of this much (m). */
constexpr float kLandingStep = 0.2f;
/** The start is pulled this far inside its polygon (Detour snaps a link's ends onto it). */
constexpr float kInset = 0.05f;
/** The body tested along the arc: spheres of this part of its radius (the navmesh keeps its
 * middle a whole radius from the walls: a jump may pass as near). */
constexpr float kBodyPart = 0.85f;
/** Walking is "about as short" up to this many times the jump's length plus kWalkSlack (m). */
constexpr float kWalkFactor = 2.0f;
constexpr float kWalkSlack = 3.0f;
/** The level's triangles in cells of this much (m) on the plan. */
constexpr float kGridCell = 1.0f;

/** Recast's space (Y up) <-> the game's (Z up). */
inline MT_Vector3 game_of(const float *nav)
{
  return MT_Vector3(nav[0], nav[2], nav[1]);
}
inline void nav_of(const MT_Vector3 &game, float *out)
{
  out[0] = float(game[0]);
  out[1] = float(game[2]);
  out[2] = float(game[1]);
}

/** The level's triangles bucketed on the plan inside a box (the region the tile's jumps can
 * reach: nothing is asked outside it), for sphere and surface tests. Flat arrays (each cell's
 * triangles one after another): built for every tile searched. */
class TriangleGrid {
 public:
  TriangleGrid(const KX_NavGeometry &geometry,
               const std::vector<int> *triangles,
               const float min_x,
               const float min_y,
               const float max_x,
               const float max_y)
      : m_geometry(geometry)
  {
    m_x0 = cell(min_x);
    m_y0 = cell(min_y);
    m_nx = std::max(cell(max_x) - m_x0 + 1, 1);
    m_ny = std::max(cell(max_y) - m_y0 + 1, 1);
    const int n = triangles ? int(triangles->size()) : int(geometry.tris.size() / 3);
    std::vector<int> range(size_t(n) * 4);
    m_start.assign(size_t(m_nx) * size_t(m_ny) + 1, 0);
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < n; i++) {
        const int t = triangles ? (*triangles)[size_t(i)] : i;
        int *r = &range[size_t(i) * 4];
        if (pass == 0) {
          float minx = FLT_MAX, maxx = -FLT_MAX, miny = FLT_MAX, maxy = -FLT_MAX;
          for (int k = 0; k < 3; k++) {
            const float *p = vertex(t, k);
            minx = std::min(minx, p[0]);
            maxx = std::max(maxx, p[0]);
            miny = std::min(miny, p[1]);
            maxy = std::max(maxy, p[1]);
          }
          r[0] = std::max(cell(minx), m_x0) - m_x0;
          r[1] = std::min(cell(maxx), m_x0 + m_nx - 1) - m_x0;
          r[2] = std::max(cell(miny), m_y0) - m_y0;
          r[3] = std::min(cell(maxy), m_y0 + m_ny - 1) - m_y0;
          if (r[0] > r[1] || r[2] > r[3]) {
            r[0] = 1; /* Outside the box: never asked about. */
            r[1] = 0;
            continue;
          }
          if (int64_t(r[1] - r[0] + 1) * int64_t(r[3] - r[2] + 1) > 1024) {
            /* A huge triangle (a floor): kept apart, tested always. */
            m_large.push_back(t);
            r[0] = 1;
            r[1] = 0;
            continue;
          }
        }
        for (int y = r[2]; y <= r[3] && r[0] <= r[1]; y++) {
          for (int x = r[0]; x <= r[1]; x++) {
            const size_t c = size_t(y) * size_t(m_nx) + size_t(x);
            if (pass == 0) {
              m_start[c + 1]++;
            }
            else {
              m_items[size_t(m_fill[c]++)] = t;
            }
          }
        }
      }
      if (pass == 0) {
        for (size_t c = 1; c < m_start.size(); c++) {
          m_start[c] += m_start[c - 1];
        }
        m_items.resize(size_t(m_start.back()));
        m_fill.assign(m_start.begin(), m_start.end() - 1);
      }
    }
  }

  /** Whether a sphere touches a triangle. */
  bool SphereHits(const MT_Vector3 &center, const float radius) const
  {
    const float c[3] = {float(center[0]), float(center[1]), float(center[2])};
    const float r2 = radius * radius;
    const int x0 = std::max(cell(c[0] - radius) - m_x0, 0);
    const int x1 = std::min(cell(c[0] + radius) - m_x0, m_nx - 1);
    const int y0 = std::max(cell(c[1] - radius) - m_y0, 0);
    const int y1 = std::min(cell(c[1] + radius) - m_y0, m_ny - 1);
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        const size_t c_index = size_t(y) * size_t(m_nx) + size_t(x);
        for (int i = m_start[c_index]; i < m_start[c_index + 1]; i++) {
          if (distance2(c, m_items[size_t(i)]) < r2) {
            return true;
          }
        }
      }
    }
    for (const int t : m_large) {
      if (distance2(c, t) < r2) {
        return true;
      }
    }
    return false;
  }

  /** Whether a surface lies under or over `p` on the plan, from `below` under it to `above` over
   * it (a triangle standing on its side covers nothing). */
  bool SurfaceNear(const MT_Vector3 &p, const float below, const float above) const
  {
    const float x = float(p[0]), y = float(p[1]), z0 = float(p[2]) - below,
                z1 = float(p[2]) + above;
    auto covers = [&](const int t) {
      const float *a = vertex(t, 0), *b = vertex(t, 1), *c = vertex(t, 2);
      const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
      if (std::fabs(d) < 1e-6f) {
        return false;
      }
      const float l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
      const float l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
      const float l3 = 1.0f - l1 - l2;
      if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) {
        return false;
      }
      const float z = l1 * a[2] + l2 * b[2] + l3 * c[2];
      return z >= z0 && z <= z1;
    };
    const int cx = cell(x) - m_x0, cy = cell(y) - m_y0;
    if (cx >= 0 && cx < m_nx && cy >= 0 && cy < m_ny) {
      const size_t c_index = size_t(cy) * size_t(m_nx) + size_t(cx);
      for (int i = m_start[c_index]; i < m_start[c_index + 1]; i++) {
        if (covers(m_items[size_t(i)])) {
          return true;
        }
      }
    }
    for (const int t : m_large) {
      if (covers(t)) {
        return true;
      }
    }
    return false;
  }

 private:
  static int cell(const float v)
  {
    return int(std::floor(v / kGridCell));
  }
  const float *vertex(const int t, const int k) const
  {
    return &m_geometry.verts[size_t(m_geometry.tris[size_t(t) * 3 + k]) * 3];
  }
  /** The squared distance from `p` to triangle `t` (Ericson's closest point). */
  float distance2(const float p[3], const int t) const
  {
    const float *a = vertex(t, 0), *b = vertex(t, 1), *c = vertex(t, 2);
    float ab[3], ac[3], ap[3];
    for (int i = 0; i < 3; i++) {
      ab[i] = b[i] - a[i];
      ac[i] = c[i] - a[i];
      ap[i] = p[i] - a[i];
    }
    auto dot = [](const float *u, const float *v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
    auto dist2 = [&](const float q[3]) {
      const float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
      return dot(d, d);
    };
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
      return dist2(a);
    }
    float bp[3];
    for (int i = 0; i < 3; i++) {
      bp[i] = p[i] - b[i];
    }
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
      return dist2(b);
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
      const float v = d1 / (d1 - d3);
      const float q[3] = {a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]};
      return dist2(q);
    }
    float cp[3];
    for (int i = 0; i < 3; i++) {
      cp[i] = p[i] - c[i];
    }
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
      return dist2(c);
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
      const float w = d2 / (d2 - d6);
      const float q[3] = {a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]};
      return dist2(q);
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
      const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      const float q[3] = {b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2])};
      return dist2(q);
    }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    const float q[3] = {a[0] + ab[0] * v + ac[0] * w,
                        a[1] + ab[1] * v + ac[1] * w,
                        a[2] + ab[2] * v + ac[2] * w};
    return dist2(q);
  }

  const KX_NavGeometry &m_geometry;
  int m_x0 = 0, m_y0 = 0, m_nx = 1, m_ny = 1;
  /** Cell c's triangles: m_items[m_start[c] .. m_start[c + 1]). */
  std::vector<int> m_start, m_fill, m_items;
  std::vector<int> m_large;
};

/** Whether the body (feet at `feet`) is clear of the level: spheres from just above what it
 * climbs to its top. */
bool body_clear(const TriangleGrid &grid,
                const MT_Vector3 &feet,
                const KX_NavJumpLinkSpec &spec)
{
  const float r = spec.radius * kBodyPart;
  const float bottom = std::max(spec.climb, 0.05f) + 0.05f + r;
  const float top = std::max(spec.height - r, bottom);
  const int n = std::max(1, int(std::ceil((top - bottom) / std::max(r, 0.05f))));
  for (int i = 0; i <= n; i++) {
    const float z = bottom + (top - bottom) * float(i) / float(n);
    if (grid.SphereHits(feet + MT_Vector3(0.0f, 0.0f, z), r)) {
      return false;
    }
  }
  return true;
}

/** Whether the jump's arc from `start` to `end` is clear for the body. */
bool arc_clear(const TriangleGrid &grid,
               const MT_Vector3 &start,
               const MT_Vector3 &end,
               const KX_NavJumpLinkSpec &spec)
{
  const float clearance = KX_NavJumpClearance(start, end);
  /* Samples about half a radius apart along the way (plan and height). */
  const float length = float((end - start).length()) + 2.0f * clearance;
  const int n = std::max(4, int(std::ceil(length / std::max(spec.radius * 0.5f, 0.05f))));
  for (int i = 1; i < n; i++) {
    if (!body_clear(grid, KX_NavJumpArc(start, end, clearance, float(i) / float(n)), spec)) {
      return false;
    }
  }
  return true;
}

/** How far the level's real ledge lies from a link's end along `dir` (on the plan): where the
 * surface it stands on ends or a wall rises (above half its climb), at most `most`. The navmesh
 * keeps about a body's width and a cell or two from every ledge, so a 2 m gap is a jump of 3 m
 * or more; the gap a game maker sees is the jump less both margins. */
float ledge_margin(const TriangleGrid &grid,
                   const MT_Vector3 &from,
                   const MT_Vector3 &dir,
                   const float most,
                   const KX_NavJumpLinkSpec &spec)
{
  constexpr float kStep = 0.05f;
  /* The navmesh lies up to a cell or so above the real surface. */
  const float below = 0.5f * spec.climb + 0.15f;
  const MT_Vector3 knee(0.0f, 0.0f, 0.5f * spec.climb + 0.05f);
  for (float d = kStep; d <= most + 1e-4f; d += kStep) {
    const MT_Vector3 p = from + dir * d;
    if (!grid.SurfaceNear(p, below, 0.2f) || grid.SphereHits(p + knee, 0.04f)) {
      return d - 0.5f * kStep;
    }
  }
  return most;
}

struct Candidate {
  MT_Vector3 start, end;
  dtPolyRef start_ref = 0, end_ref = 0;
  bool drop = false;
  float gap = 0.0f;
};

/** Walking only: no link (a link found before would judge itself "as short" and go away when its
 * tile is searched again; a ladder is no walk). */
class WalkFilter : public dtQueryFilter {
 public:
  bool passFilter(const dtPolyRef ref, const dtMeshTile *tile, const dtPoly *poly) const override
  {
    return poly->getType() != DT_POLYTYPE_OFFMESH_CONNECTION &&
           dtQueryFilter::passFilter(ref, tile, poly);
  }
};

/** Whether walking from the start's polygon to the landing's is about as short as the jump
 * (Havok's filter: such a link only slows the searches). */
bool walk_about_as_short(dtNavMeshQuery &query, const Candidate &c)
{
  WalkFilter filter;
  float s[3], e[3];
  nav_of(c.start, s);
  nav_of(c.end, e);
  /* A walk that short crosses only polygons within its length of the start (on the plan): a
   * landing outside them has none (most jumps: no search over the level). */
  const float bound = kWalkFactor * float((c.end - c.start).length()) + kWalkSlack;
  {
    constexpr int kCircle = 256;
    dtPolyRef refs[kCircle], parents[kCircle];
    float costs[kCircle];
    int n = 0;
    const dtStatus status = query.findPolysAroundCircle(
        c.start_ref, s, bound, &filter, refs, parents, costs, &n, kCircle);
    if (dtStatusFailed(status)) {
      return false;
    }
    if (!dtStatusDetail(status, DT_BUFFER_TOO_SMALL) &&
        std::find(refs, refs + n, c.end_ref) == refs + n)
    {
      return false;
    }
  }
  dtPolyRef path[256];
  int npath = 0;
  const dtStatus status = query.findPath(c.start_ref, c.end_ref, s, e, &filter, path, &npath, 256);
  if (dtStatusFailed(status) || npath == 0 || path[npath - 1] != c.end_ref ||
      dtStatusDetail(status, DT_PARTIAL_RESULT))
  {
    return false;
  }
  float straight[32 * 3];
  int nstraight = 0;
  if (dtStatusFailed(query.findStraightPath(
          s, e, path, npath, straight, nullptr, nullptr, &nstraight, 32)))
  {
    return false;
  }
  float walk = 0.0f;
  for (int i = 1; i < nstraight; i++) {
    walk += dtVdist(&straight[(i - 1) * 3], &straight[i * 3]);
  }
  if (nstraight >= 32) {
    return false; /* Many corners: long. */
  }
  return walk <= bound;
}

}  // namespace

float KX_NavJumpClearance(const MT_Vector3 &start, const MT_Vector3 &end)
{
  const float dx = float(end[0] - start[0]), dy = float(end[1] - start[1]);
  const float plan = std::sqrt(dx * dx + dy * dy);
  return std::clamp(0.25f + 0.15f * plan, 0.3f, 1.5f);
}

MT_Vector3 KX_NavJumpArc(const MT_Vector3 &start,
                         const MT_Vector3 &end,
                         const float clearance,
                         const float t)
{
  /* z(t) = z0 + v t - g t^2 / 2 on t in 0..1 with its top `clearance` above the higher end:
   * h1 = top - z0, h2 = top - z1, sqrt(g) = sqrt(2 h1) + sqrt(2 h2), v = sqrt(2 g h1). */
  const float z0 = float(start[2]), z1 = float(end[2]);
  const float top = std::max(z0, z1) + std::max(clearance, 0.0f);
  const float a = std::sqrt(2.0f * std::max(top - z0, 0.0f));
  const float b = std::sqrt(2.0f * std::max(top - z1, 0.0f));
  const float u = a + b;
  MT_Vector3 p = start + (end - start) * t;
  if (u > 1e-6f) {
    const float g = u * u, v = u * a;
    p[2] = z0 + v * t - 0.5f * g * t * t;
  }
  return p;
}

bool KX_NavJumpLaunch(const MT_Vector3 &start,
                      const MT_Vector3 &end,
                      const float clearance,
                      const float gravity,
                      float &r_up_speed,
                      float &r_plan_speed,
                      float &r_time)
{
  if (!(gravity > 1e-4f)) {
    return false;
  }
  const float z0 = float(start[2]), z1 = float(end[2]);
  const float top = std::max(z0, z1) + std::max(clearance, 0.0f);
  const float h1 = std::max(top - z0, 0.0f), h2 = std::max(top - z1, 0.0f);
  r_up_speed = std::sqrt(2.0f * gravity * h1);
  r_time = r_up_speed / gravity + std::sqrt(2.0f * h2 / gravity);
  const float dx = float(end[0] - start[0]), dy = float(end[1] - start[1]);
  r_plan_speed = r_time > 1e-4f ? std::sqrt(dx * dx + dy * dy) / r_time : 0.0f;
  return r_time > 1e-4f;
}

bool KX_NavJumpReachable(const float jump_speed,
                         const float plan_speed,
                         const float gravity,
                         const float climb,
                         const float gap,
                         const float rise)
{
  if (!(gravity > 1e-4f) || !(jump_speed > 0.0f)) {
    /* No jump: only what it walks over. */
    return gap <= 0.0f && rise <= climb;
  }
  /* The top a full jump reaches; the feet must clear a higher end by a hand's breadth. */
  const float top = jump_speed * jump_speed / (2.0f * gravity);
  if (rise > climb + top - 0.1f) {
    return false;
  }
  /* In the air: up to the top, then down to the landing's height (down a drop: longer). */
  const float time = jump_speed / gravity +
                     std::sqrt(2.0f * std::max(top - rise, 0.0f) / gravity);
  return gap <= plan_speed * time;
}

float KX_NavJumpLinkReach(const KX_NavJumpLinkSpec &spec)
{
  /* The landing search (the gap and two ledge margins), then the arc's body and a metre. */
  return std::min(spec.gap + 2.0f * (2.0f * spec.radius + 0.5f), spec.tile_width) +
         2.0f * spec.radius + 1.0f;
}

void KX_NavFindJumpLinks(const dtNavMesh &mesh,
                         dtNavMeshQuery &query,
                         const int tile_x,
                         const int tile_y,
                         const KX_NavJumpLinkSpec &spec,
                         const KX_NavGeometry &geometry,
                         const std::vector<int> *triangles,
                         std::vector<KX_NavFoundLink> &r_links)
{
  r_links.clear();
  dtQueryFilter filter;
  const dtMeshTile *tiles[32];
  const int ntiles = mesh.getTilesAt(tile_x, tile_y, tiles, 32);
  const float reach_up = spec.climb + spec.up;
  /* The navmesh's margin from a ledge (a body's width and a cell or two) at most; a landing is
   * looked for out to the gap and both margins. */
  const float most_margin = 2.0f * spec.radius + 0.5f;
  const float search = std::min(spec.gap + 2.0f * most_margin, spec.tile_width);
  const float first_step = std::max(spec.radius * 0.5f, kLandingStep);
  /* The level's triangles where this tile's jumps may reach (the game's plan). */
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  for (int ti = 0; ti < ntiles; ti++) {
    if (tiles[ti] != nullptr && tiles[ti]->header != nullptr) {
      min_x = std::min(min_x, tiles[ti]->header->bmin[0]);
      min_y = std::min(min_y, tiles[ti]->header->bmin[2]);
      max_x = std::max(max_x, tiles[ti]->header->bmax[0]);
      max_y = std::max(max_y, tiles[ti]->header->bmax[2]);
    }
  }
  if (min_x > max_x) {
    return;
  }
  const float around = KX_NavJumpLinkReach(spec);
  const TriangleGrid grid(
      geometry, triangles, min_x - around, min_y - around, max_x + around, max_y + around);

  /* The polygons along a sample's way out (one query a sample: 2026-11-19, a query a step cost
   * most of the search), then the landing among them at `d` out: the one nearest the start's
   * height in reach, or (`level` given) the one within 0.3 m of `level`. */
  constexpr int kAlong = 128;
  dtPolyRef along[kAlong];
  int nalong = 0;
  auto gather = [&](const MT_Vector3 &start, const MT_Vector3 &out) {
    const MT_Vector3 a = start + out * (kInset + first_step);
    const MT_Vector3 b = start + out * (kInset + search);
    const float lo = float(start[2]) - spec.drop, hi = float(start[2]) + reach_up;
    float center[3];
    nav_of(MT_Vector3((a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (lo + hi) * 0.5f), center);
    const float half[3] = {std::fabs(float(b[0] - a[0])) * 0.5f + 0.05f,
                           (hi - lo) * 0.5f + 0.05f,
                           std::fabs(float(b[1] - a[1])) * 0.5f + 0.05f};
    nalong = 0;
    query.queryPolygons(center, half, &filter, along, &nalong, kAlong);
  };
  auto landing_height = [&](const MT_Vector3 &start,
                            const MT_Vector3 &out,
                            const dtPolyRef start_ref,
                            const float d,
                            const float level,
                            float &r_height,
                            dtPolyRef &r_ref) {
    const MT_Vector3 plan = start + out * (kInset + d);
    float point[3];
    nav_of(MT_Vector3(plan[0], plan[1], start[2]), point);
    bool any = false;
    float best = FLT_MAX;
    for (int r = 0; r < nalong; r++) {
      float h = 0.0f;
      if (along[r] == start_ref || dtStatusFailed(query.getPolyHeight(along[r], point, &h))) {
        continue;
      }
      const float rise = h - float(start[2]);
      if (rise > reach_up || rise < -spec.drop) {
        continue;
      }
      if (level != FLT_MAX ? std::fabs(h - level) < 0.3f : std::fabs(rise) < best) {
        best = std::fabs(rise);
        r_height = h;
        r_ref = along[r];
        any = true;
        if (level != FLT_MAX) {
          break;
        }
      }
    }
    return any;
  };
  /* Whether a link like `c` is kept already within Spacing (one per Spacing along a ledge, the
   * same landing side). Judged before the arc and the ledges are measured: the same links as
   * thinning those that pass (only kept links count), for a quarter of the tests. */
  std::vector<Candidate> kept;
  auto near_kept = [&](const Candidate &c) {
    for (const Candidate &k : kept) {
      const float ds = float((MT_Vector3(c.start[0], c.start[1], 0.0f) -
                              MT_Vector3(k.start[0], k.start[1], 0.0f))
                                 .length());
      if (ds < spec.spacing && c.drop == k.drop &&
          std::fabs(float(c.end[2] - k.end[2])) < 0.5f &&
          float((c.end - k.end).length()) < spec.spacing + 0.5f)
      {
        return true;
      }
    }
    return false;
  };
  for (int ti = 0; ti < ntiles; ti++) {
    const dtMeshTile *tile = tiles[ti];
    if (tile == nullptr || tile->header == nullptr) {
      continue;
    }
    const dtPolyRef base = mesh.getPolyRefBase(tile);
    for (int pi = 0; pi < tile->header->polyCount; pi++) {
      const dtPoly &poly = tile->polys[pi];
      if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3) {
        continue;
      }
      float centroid[3] = {0.0f, 0.0f, 0.0f};
      for (int k = 0; k < poly.vertCount; k++) {
        dtVadd(centroid, centroid, &tile->verts[poly.verts[k] * 3]);
      }
      dtVscale(centroid, centroid, 1.0f / float(poly.vertCount));
      for (int j = 0; j < poly.vertCount; j++) {
        /* An open edge: no neighbour (a tile's border edge is a portal: it has one). */
        if (poly.neis[j] != 0) {
          continue;
        }
        const float *va = &tile->verts[poly.verts[j] * 3];
        const float *vb = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
        const float ex = vb[0] - va[0], ez = vb[2] - va[2];
        const float length = std::sqrt(ex * ex + ez * ez);
        if (length < 0.05f) {
          continue;
        }
        /* Outward on the plan (away from the polygon's middle). */
        float nx = ez / length, nz = -ex / length;
        const float mx = (va[0] + vb[0]) * 0.5f - centroid[0];
        const float mz = (va[2] + vb[2]) * 0.5f - centroid[2];
        if (nx * mx + nz * mz < 0.0f) {
          nx = -nx;
          nz = -nz;
        }
        const MT_Vector3 out(nx, nz, 0.0f);
        const int samples = std::max(1, int(std::ceil(length / kSampleStep)));
        for (int s = 0; s < samples; s++) {
          const float f = (float(s) + 0.5f) / float(samples);
          float edge_point[3];
          dtVlerp(edge_point, va, vb, f);
          const MT_Vector3 start = game_of(edge_point) - out * kInset;
          /* A wall higher than any ledge it could jump up to: nothing to find there. */
          if (grid.SphereHits(start + out * (spec.radius + 0.15f) +
                                  MT_Vector3(0.0f, 0.0f, reach_up + 0.1f + spec.radius * kBodyPart),
                              spec.radius * kBodyPart))
          {
            continue;
          }
          gather(start, out);
          if (nalong == 0) {
            continue;
          }
          /* The nearest landing outward (the one nearest the start's height there), its near
           * edge found to 2.5 cm; kept when no link like it is kept within Spacing, the gap to
           * it is within the limit and the body's arc reaches it. */
          const dtPolyRef start_ref = base + dtPolyRef(pi);
          float miss = 0.0f;
          for (float d = first_step; d <= search + 1e-4f; d += kLandingStep) {
            float h = 0.0f;
            dtPolyRef end_ref = 0;
            if (!landing_height(start, out, start_ref, d, FLT_MAX, h, end_ref)) {
              miss = d;
              continue;
            }
            float hit = d;
            for (int i = 0; i < 3; i++) {
              const float mid = 0.5f * (miss + hit);
              float mh = 0.0f;
              dtPolyRef mref = 0;
              if (landing_height(start, out, start_ref, mid, h, mh, mref)) {
                hit = mid;
                h = mh;
                end_ref = mref;
              }
              else {
                miss = mid;
              }
            }
            const MT_Vector3 plan = start + out * (kInset + hit);
            Candidate pick;
            pick.start = start;
            pick.end = MT_Vector3(plan[0], plan[1], h);
            pick.start_ref = start_ref;
            pick.end_ref = end_ref;
            pick.drop = h - float(start[2]) < -spec.climb;
            if (near_kept(pick)) {
              break;
            }
            const float travel = float(
                MT_Vector3(pick.end[0] - start[0], pick.end[1] - start[1], 0.0f).length());
            pick.gap = std::max(travel -
                                    ledge_margin(grid, start, out, most_margin, spec) -
                                    ledge_margin(grid, pick.end, -out, most_margin, spec),
                                0.0f);
            if (pick.gap <= spec.gap + 1e-4f && arc_clear(grid, start, pick.end, spec)) {
              kept.push_back(pick);
            }
            break;
          }
        }
      }
    }
  }
  /* The walking alternative judged for those kept. */
  for (const Candidate &c : kept) {
    if (walk_about_as_short(query, c)) {
      continue;
    }
    KX_NavFoundLink link;
    link.start = c.start;
    link.end = c.end;
    link.drop = c.drop;
    link.gap = c.gap;
    r_links.push_back(link);
  }
}
