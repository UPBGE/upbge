/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavMesh.cpp
 *  \ingroup ketsji
 */

#include "KX_NavMesh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>

#include "KX_Detour.h"
#include "KX_GameObject.h"
#include "MT_Matrix3x3.h"
#include "MT_Transform.h"

static_assert(sizeof(dtPolyRef) == sizeof(KX_NavPolyRef), "Detour is built with DT_POLYREF64");

/* -------------------------------------------------------------------- */
/** \name Thread slots
 * \{ */

namespace {

/** Threads that may query at once (the main thread, the job pool's helpers, Jolt's are not). */
constexpr uint32_t kMaxThreadSlots = 512;
/** A search looks at this many polygons at most (each query object's node pool). */
constexpr int kQueryNodes = 8192;

std::mutex g_slotMutex;
std::vector<uint32_t> g_freeSlots;
uint32_t g_nextSlot = 0;

struct SlotHolder {
  uint32_t slot = UINT32_MAX;
  ~SlotHolder()
  {
    if (slot != UINT32_MAX) {
      std::lock_guard<std::mutex> lock(g_slotMutex);
      g_freeSlots.push_back(slot);
    }
  }
};
thread_local SlotHolder t_slot;

/** Detour's random point search takes a plain function: its state is the thread's. */
thread_local uint64_t t_randomState = 0;

float nav_random_float()
{
  /* SplitMix64. */
  uint64_t z = (t_randomState += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  /* 24 bits: [0, 1). */
  return float(z >> 40) * (1.0f / 16777216.0f);
}

}  // namespace

uint32_t KX_NavThreadSlot()
{
  if (t_slot.slot == UINT32_MAX) {
    std::lock_guard<std::mutex> lock(g_slotMutex);
    if (!g_freeSlots.empty()) {
      /* The lowest free slot first: a pool of N threads keeps slots 0..N. */
      auto lowest = std::min_element(g_freeSlots.begin(), g_freeSlots.end());
      t_slot.slot = *lowest;
      g_freeSlots.erase(lowest);
    }
    else {
      t_slot.slot = g_nextSlot++;
    }
  }
  return t_slot.slot;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Making
 * \{ */

KX_NavMesh::KX_NavMesh() : m_queries(kMaxThreadSlots), m_defaultFilter(new dtQueryFilter())
{
  for (std::atomic<dtNavMeshQuery *> &query : m_queries) {
    query.store(nullptr, std::memory_order_relaxed);
  }
  m_defaultFilter->setIncludeFlags(0xffff);
  m_defaultFilter->setExcludeFlags(0);
}

KX_NavMesh::~KX_NavMesh()
{
  for (std::atomic<dtNavMeshQuery *> &query : m_queries) {
    if (dtNavMeshQuery *q = query.load(std::memory_order_relaxed)) {
      dtFreeNavMeshQuery(q);
    }
  }
  if (m_mesh) {
    dtFreeNavMesh(m_mesh);
  }
}

std::unique_ptr<KX_NavMesh> KX_NavMesh::FromDetour(dtNavMesh *mesh)
{
  std::unique_ptr<KX_NavMesh> navmesh(new KX_NavMesh());
  navmesh->m_mesh = mesh;
  return navmesh;
}

namespace {

float dist_point_segment_2d_sq(const float *p, const float *a, const float *b)
{
  const float abx = b[0] - a[0], abz = b[2] - a[2];
  const float dx = p[0] - a[0], dz = p[2] - a[2];
  const float d = abx * abx + abz * abz;
  float t = abx * dx + abz * dz;
  if (d > 0.0f) {
    t /= d;
  }
  t = std::clamp(t, 0.0f, 1.0f);
  const float ex = a[0] + t * abx - p[0], ez = a[2] + t * abz - p[2];
  return ex * ex + ez * ez;
}

/** Recast's getEdgeFlags (RecastMeshDetail.cpp): 1 when the edge lies on the polygon's boundary
 * (Detour's DT_DETAIL_EDGE_BOUNDARY). */
unsigned char detail_edge_flags(const float *va,
                                const float *vb,
                                const float *vpoly,
                                const int npoly,
                                const float threshold_sq)
{
  for (int i = 0, j = npoly - 1; i < npoly; j = i++) {
    if (dist_point_segment_2d_sq(va, &vpoly[j * 3], &vpoly[i * 3]) < threshold_sq &&
        dist_point_segment_2d_sq(vb, &vpoly[j * 3], &vpoly[i * 3]) < threshold_sq)
    {
      return 1;
    }
  }
  return 0;
}

}  // namespace

std::unique_ptr<KX_NavMesh> KX_NavMesh::FromPolygons(const KX_NavMeshPolygons &polygons,
                                                     std::string &r_error)
{
  const int nvp = polygons.nvp;
  const int nverts = int(polygons.verts.size() / 3);
  if (nvp < 3 || nvp > DT_VERTS_PER_POLYGON) {
    r_error = "polygons of " + std::to_string(nvp) + " vertices (Detour takes 3 to 6)";
    return nullptr;
  }
  const int npolys = int(polygons.polys.size() / size_t(nvp * 2));
  if (nverts == 0 || npolys == 0) {
    r_error = "no polygons";
    return nullptr;
  }
  if (nverts >= 0xffff) {
    r_error = "more than 65,534 vertices";
    return nullptr;
  }
  const bool has_detail = !polygons.detail_meshes.empty();
  if (has_detail && polygons.detail_meshes.size() != size_t(npolys) * 4) {
    r_error = "detail meshes that don't match the polygons";
    return nullptr;
  }

  /* Bounds of every vertex, detail vertices included. */
  float bmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  auto grow = [&](const float *v) {
    for (int k = 0; k < 3; k++) {
      bmin[k] = std::min(bmin[k], v[k]);
      bmax[k] = std::max(bmax[k], v[k]);
    }
  };
  for (int i = 0; i < nverts; i++) {
    grow(&polygons.verts[size_t(i) * 3]);
  }
  for (size_t i = 0; i + 2 < polygons.detail_verts.size(); i += 3) {
    grow(&polygons.detail_verts[i]);
  }

  /* Detour stores polygon vertices as 16-bit steps from the bounds' corner: the steps are as
   * small as the size allows, never above a centimetre unless the navmesh is over 600 m. */
  const float extent = std::max({bmax[0] - bmin[0], bmax[2] - bmin[2], 1e-3f});
  const float cs = std::max(0.001f, extent / 60000.0f);
  const float ch = std::max(0.001f, (bmax[1] - bmin[1]) / 60000.0f);
  std::vector<unsigned short> quantized(size_t(nverts) * 3);
  std::vector<float> snapped(size_t(nverts) * 3);
  for (int i = 0; i < nverts; i++) {
    const float *v = &polygons.verts[size_t(i) * 3];
    unsigned short *q = &quantized[size_t(i) * 3];
    q[0] = (unsigned short)std::lround((v[0] - bmin[0]) / cs);
    q[1] = (unsigned short)std::lround((v[1] - bmin[1]) / ch);
    q[2] = (unsigned short)std::lround((v[2] - bmin[2]) / cs);
    float *s = &snapped[size_t(i) * 3];
    s[0] = bmin[0] + q[0] * cs;
    s[1] = bmin[1] + q[1] * ch;
    s[2] = bmin[2] + q[2] * cs;
  }

  std::vector<unsigned short> polys(polygons.polys.begin(), polygons.polys.end());
  std::vector<unsigned short> poly_flags(size_t(npolys), 1);
  std::vector<unsigned char> poly_areas(size_t(npolys), 0);

  /* Detail meshes as Recast hands them over: each one's vertices start with its polygon's. */
  std::vector<unsigned int> detail_meshes;
  std::vector<float> detail_verts;
  std::vector<unsigned char> detail_tris;
  if (has_detail) {
    detail_meshes.resize(size_t(npolys) * 4);
    const size_t tri_count = polygons.detail_tris.size() / 3;
    detail_tris.resize(tri_count * 4);
    const float threshold_sq = (2.0f * cs) * (2.0f * cs) + 1e-6f;
    for (int p = 0; p < npolys; p++) {
      const unsigned short *poly = &polys[size_t(p) * nvp * 2];
      int nv = 0;
      float vpoly[DT_VERTS_PER_POLYGON * 3];
      while (nv < nvp && poly[nv] != 0xffff) {
        if (poly[nv] >= nverts) {
          r_error = "a polygon names a vertex that doesn't exist";
          return nullptr;
        }
        dtVcopy(&vpoly[nv * 3], &snapped[size_t(poly[nv]) * 3]);
        nv++;
      }
      const uint32_t *src = &polygons.detail_meshes[size_t(p) * 4];
      const uint32_t vbase = src[0], vcount = src[1], tbase = src[2], tcount = src[3];
      if (size_t(vbase + vcount) * 3 > polygons.detail_verts.size() ||
          size_t(tbase + tcount) > tri_count || nv + vcount > 255)
      {
        r_error = "a detail mesh beyond its data";
        return nullptr;
      }
      unsigned int *dst = &detail_meshes[size_t(p) * 4];
      dst[0] = (unsigned int)(detail_verts.size() / 3);
      dst[1] = (unsigned int)(nv + vcount);
      dst[2] = tbase;
      dst[3] = tcount;
      detail_verts.insert(detail_verts.end(), vpoly, vpoly + nv * 3);
      detail_verts.insert(detail_verts.end(),
                          polygons.detail_verts.begin() + size_t(vbase) * 3,
                          polygons.detail_verts.begin() + size_t(vbase + vcount) * 3);
      const float *mesh_verts = &detail_verts[size_t(dst[0]) * 3];
      for (uint32_t t = tbase; t < tbase + tcount; t++) {
        const uint8_t *tri = &polygons.detail_tris[size_t(t) * 3];
        unsigned char *out = &detail_tris[size_t(t) * 4];
        for (int k = 0; k < 3; k++) {
          if (tri[k] >= nv + vcount) {
            r_error = "a detail triangle names a vertex that doesn't exist";
            return nullptr;
          }
          out[k] = tri[k];
        }
        const float *va = &mesh_verts[tri[0] * 3], *vb = &mesh_verts[tri[1] * 3],
                    *vc = &mesh_verts[tri[2] * 3];
        out[3] = (unsigned char)(detail_edge_flags(va, vb, vpoly, nv, threshold_sq) |
                                 (detail_edge_flags(vb, vc, vpoly, nv, threshold_sq) << 2) |
                                 (detail_edge_flags(vc, va, vpoly, nv, threshold_sq) << 4));
      }
    }
  }

  dtNavMeshCreateParams params;
  memset(&params, 0, sizeof(params));
  params.verts = quantized.data();
  params.vertCount = nverts;
  params.polys = polys.data();
  params.polyAreas = poly_areas.data();
  params.polyFlags = poly_flags.data();
  params.polyCount = npolys;
  params.nvp = nvp;
  if (has_detail) {
    params.detailMeshes = detail_meshes.data();
    params.detailVerts = detail_verts.data();
    params.detailVertsCount = int(detail_verts.size() / 3);
    params.detailTris = detail_tris.data();
    params.detailTriCount = int(detail_tris.size() / 4);
  }
  std::vector<unsigned char> link_dirs, link_areas;
  std::vector<unsigned short> link_flags;
  const int link_count = int(polygons.link_ids.size());
  if (link_count > 0 && polygons.link_verts.size() == size_t(link_count) * 6 &&
      polygons.link_radii.size() == size_t(link_count) &&
      polygons.link_both_ways.size() == size_t(link_count))
  {
    for (int i = 0; i < link_count; i++) {
      link_dirs.push_back(polygons.link_both_ways[size_t(i)] ? DT_OFFMESH_CON_BIDIR : 0);
      link_areas.push_back(0);
      link_flags.push_back(1);
    }
    params.offMeshConVerts = polygons.link_verts.data();
    params.offMeshConRad = polygons.link_radii.data();
    params.offMeshConDir = link_dirs.data();
    params.offMeshConAreas = link_areas.data();
    params.offMeshConFlags = link_flags.data();
    params.offMeshConUserID = polygons.link_ids.data();
    params.offMeshConCount = link_count;
  }
  params.walkableHeight = polygons.walkable_height;
  params.walkableRadius = polygons.walkable_radius;
  params.walkableClimb = polygons.walkable_climb;
  params.tileX = 0;
  params.tileY = 0;
  params.tileLayer = 0;
  dtVcopy(params.bmin, bmin);
  dtVcopy(params.bmax, bmax);
  params.cs = cs;
  params.ch = ch;
  params.buildBvTree = true;

  unsigned char *data = nullptr;
  int data_size = 0;
  if (!dtCreateNavMeshData(&params, &data, &data_size)) {
    r_error = "Detour could not make a navmesh of the polygons";
    return nullptr;
  }
  dtNavMesh *mesh = dtAllocNavMesh();
  if (mesh == nullptr || dtStatusFailed(mesh->init(data, data_size, DT_TILE_FREE_DATA))) {
    dtFree(data);
    dtFreeNavMesh(mesh);
    r_error = "Detour could not load the navmesh";
    return nullptr;
  }
  return FromDetour(mesh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spaces
 * \{ */

void KX_NavMesh::ToNav(const MT_Vector3 &world, float r_nav[3]) const
{
  MT_Vector3 local = world;
  if (m_owner != nullptr) {
    MT_Matrix3x3 orientation = m_owner->NodeGetWorldOrientation();
    const MT_Vector3 &scale = m_owner->NodeGetWorldScaling();
    orientation.scale(scale[0], scale[1], scale[2]);
    MT_Transform transform(m_owner->NodeGetWorldPosition(), orientation);
    MT_Transform inverse;
    inverse.invert(transform);
    local = inverse(world);
  }
  r_nav[0] = float(local[0]);
  r_nav[1] = float(local[2]);
  r_nav[2] = float(local[1]);
}

MT_Vector3 KX_NavMesh::ToWorld(const float nav[3]) const
{
  const MT_Vector3 local(nav[0], nav[2], nav[1]);
  if (m_owner == nullptr) {
    return local;
  }
  MT_Matrix3x3 orientation = m_owner->NodeGetWorldOrientation();
  const MT_Vector3 &scale = m_owner->NodeGetWorldScaling();
  orientation.scale(scale[0], scale[1], scale[2]);
  return MT_Transform(m_owner->NodeGetWorldPosition(), orientation)(local);
}

MT_Vector3 KX_NavMesh::NavDirectionToWorld(const float nav[3]) const
{
  const MT_Vector3 local(nav[0], nav[2], nav[1]);
  if (m_owner == nullptr) {
    return local;
  }
  return (m_owner->NodeGetWorldOrientation() * local).safe_normalized();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Queries
 * \{ */

dtNavMeshQuery *KX_NavMesh::ThreadQuery() const
{
  if (m_mesh == nullptr) {
    return nullptr;
  }
  const uint32_t slot = KX_NavThreadSlot();
  if (slot >= m_queries.size()) {
    return nullptr;
  }
  dtNavMeshQuery *query = m_queries[slot].load(std::memory_order_acquire);
  if (query == nullptr) {
    /* Only this thread writes its slot. */
    query = dtAllocNavMeshQuery();
    if (query == nullptr || dtStatusFailed(query->init(m_mesh, kQueryNodes))) {
      dtFreeNavMeshQuery(query);
      return nullptr;
    }
    m_queries[slot].store(query, std::memory_order_release);
  }
  return query;
}

const dtQueryFilter &KX_NavMesh::DefaultFilter() const
{
  return *m_defaultFilter;
}

bool KX_NavMesh::NearestPoint(const MT_Vector3 &world,
                              MT_Vector3 &r_point,
                              KX_NavPolyRef *r_ref,
                              const dtQueryFilter *filter) const
{
  dtNavMeshQuery *query = ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  float pos[3], nearest[3];
  ToNav(world, pos);
  dtPolyRef ref = 0;
  if (dtStatusFailed(query->findNearestPoly(
          pos, kSnapExtents, filter ? filter : m_defaultFilter.get(), &ref, nearest)) ||
      ref == 0)
  {
    return false;
  }
  r_point = ToWorld(nearest);
  if (r_ref) {
    *r_ref = ref;
  }
  return true;
}

bool KX_NavMesh::FindPath(const MT_Vector3 &from,
                          const MT_Vector3 &to,
                          std::vector<MT_Vector3> &r_corners,
                          bool *r_complete,
                          const dtQueryFilter *filter) const
{
  r_corners.clear();
  if (r_complete) {
    *r_complete = false;
  }
  dtNavMeshQuery *query = ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  const dtQueryFilter *f = filter ? filter : m_defaultFilter.get();
  float spos[3], epos[3], snear[3], enear[3];
  ToNav(from, spos);
  ToNav(to, epos);
  dtPolyRef sref = 0, eref = 0;
  query->findNearestPoly(spos, kSnapExtents, f, &sref, snear);
  query->findNearestPoly(epos, kSnapExtents, f, &eref, enear);
  if (sref == 0 || eref == 0) {
    return false;
  }
  std::vector<dtPolyRef> polys(kMaxPathPolygons);
  int npolys = 0;
  const dtStatus status = query->findPath(
      sref, eref, snear, enear, f, polys.data(), &npolys, kMaxPathPolygons);
  if (dtStatusFailed(status) || npolys == 0) {
    return false;
  }
  /* A way that ends short goes to the point of its last polygon nearest the goal. */
  float end[3];
  dtVcopy(end, enear);
  const bool complete = polys[size_t(npolys - 1)] == eref;
  if (!complete) {
    bool over = false;
    query->closestPointOnPoly(polys[size_t(npolys - 1)], enear, end, &over);
  }
  std::vector<float> straight(size_t(kMaxPathPolygons) * 3);
  int nstraight = 0;
  query->findStraightPath(snear,
                          end,
                          polys.data(),
                          npolys,
                          straight.data(),
                          nullptr,
                          nullptr,
                          &nstraight,
                          kMaxPathPolygons,
                          0);
  if (nstraight == 0) {
    return false;
  }
  r_corners.reserve(size_t(nstraight));
  for (int i = 0; i < nstraight; i++) {
    r_corners.push_back(ToWorld(&straight[size_t(i) * 3]));
  }
  if (r_complete) {
    *r_complete = complete;
  }
  return true;
}

bool KX_NavMesh::Raycast(const MT_Vector3 &from,
                         const MT_Vector3 &to,
                         float &r_fraction,
                         MT_Vector3 &r_hit,
                         MT_Vector3 &r_normal,
                         const dtQueryFilter *filter) const
{
  r_fraction = 1.0f;
  r_hit = to;
  r_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  dtNavMeshQuery *query = ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  const dtQueryFilter *f = filter ? filter : m_defaultFilter.get();
  float spos[3], epos[3], snear[3];
  ToNav(from, spos);
  ToNav(to, epos);
  dtPolyRef sref = 0;
  query->findNearestPoly(spos, kSnapExtents, f, &sref, snear);
  if (sref == 0) {
    return false;
  }
  float t = 0.0f, normal[3] = {0.0f, 0.0f, 0.0f};
  dtPolyRef visited[256];
  int nvisited = 0;
  query->raycast(sref, snear, epos, f, &t, normal, visited, &nvisited, 256);
  if (t > 1.0f) {
    return false;
  }
  r_fraction = t;
  float hit[3];
  dtVlerp(hit, snear, epos, t);
  /* The hit lies on the last polygon crossed: take its height there. */
  if (nvisited > 0) {
    float height = 0.0f;
    if (dtStatusSucceed(query->getPolyHeight(visited[nvisited - 1], hit, &height))) {
      hit[1] = height;
    }
  }
  r_hit = ToWorld(hit);
  r_normal = NavDirectionToWorld(normal);
  return true;
}

bool KX_NavMesh::RandomPoint(const MT_Vector3 &center,
                             const float radius,
                             const uint64_t seed,
                             MT_Vector3 &r_point,
                             const dtQueryFilter *filter) const
{
  dtNavMeshQuery *query = ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  const dtQueryFilter *f = filter ? filter : m_defaultFilter.get();
  float pos[3], snapped[3], out[3];
  ToNav(center, pos);
  dtPolyRef ref = 0, out_ref = 0;
  query->findNearestPoly(pos, kSnapExtents, f, &ref, snapped);
  if (ref == 0) {
    return false;
  }
  t_randomState = seed;
  if (radius <= 0.0f) {
    if (dtStatusFailed(query->findRandomPoint(f, nav_random_float, &out_ref, out)) ||
        out_ref == 0)
    {
      return false;
    }
    r_point = ToWorld(out);
    return true;
  }
  /* The radius in the navmesh's space: an owner's scale shrinks or grows it. */
  float local_radius = radius;
  if (m_owner != nullptr) {
    const MT_Vector3 &scale = m_owner->NodeGetWorldScaling();
    const float s = std::max({float(std::fabs(scale[0])), float(std::fabs(scale[1])), 1e-6f});
    local_radius = radius / s;
  }
  /* Detour picks a polygon touching the circle, then a point of it: the point may lie past the
   * radius. A few more tries, else the try nearest the centre. */
  float best[3];
  float best_d = FLT_MAX;
  for (int attempt = 0; attempt < 8; attempt++) {
    if (dtStatusFailed(query->findRandomPointAroundCircle(
            ref, snapped, local_radius, f, nav_random_float, &out_ref, out)) ||
        out_ref == 0)
    {
      continue;
    }
    const float d = dtVdist2D(out, snapped);
    if (d <= local_radius) {
      r_point = ToWorld(out);
      return true;
    }
    if (d < best_d) {
      best_d = d;
      dtVcopy(best, out);
    }
  }
  if (best_d == FLT_MAX) {
    return false;
  }
  /* Big polygons: every try past the radius. The nearest one drawn in to a random distance
   * within the radius, walked to along the navmesh from the centre (so it is reachable). */
  const float u = nav_random_float();
  const float want = local_radius * std::sqrt(std::max(u, 0.01f));
  float toward[3];
  dtVlerp(toward, snapped, best, std::min(want / std::max(best_d, 1e-6f), 1.0f));
  float walked[3];
  dtPolyRef visited[16];
  int nvisited = 0;
  if (dtStatusFailed(
          query->moveAlongSurface(ref, snapped, toward, f, walked, visited, &nvisited, 16)) ||
      nvisited == 0)
  {
    r_point = ToWorld(snapped);
    return true;
  }
  float height = walked[1];
  if (dtStatusSucceed(query->getPolyHeight(visited[nvisited - 1], walked, &height))) {
    walked[1] = height;
  }
  r_point = ToWorld(walked);
  return true;
}

bool KX_NavMesh::SurfaceNormal(const MT_Vector3 &world, MT_Vector3 &r_normal) const
{
  dtNavMeshQuery *query = ThreadQuery();
  if (query == nullptr) {
    return false;
  }
  float pos[3], snapped[3];
  ToNav(world, pos);
  dtPolyRef ref = 0;
  query->findNearestPoly(pos, kSnapExtents, m_defaultFilter.get(), &ref, snapped);
  if (ref == 0) {
    return false;
  }
  const dtMeshTile *tile = nullptr;
  const dtPoly *poly = nullptr;
  if (dtStatusFailed(m_mesh->getTileAndPolyByRef(ref, &tile, &poly)) ||
      poly->getType() != DT_POLYTYPE_GROUND)
  {
    return false;
  }
  const unsigned int ip = (unsigned int)(poly - tile->polys);
  const dtPolyDetail *pd = &tile->detailMeshes[ip];
  float best = FLT_MAX;
  float normal[3] = {0.0f, 1.0f, 0.0f};
  for (int j = 0; j < pd->triCount; j++) {
    const unsigned char *t = &tile->detailTris[(pd->triBase + j) * 4];
    const float *v[3];
    for (int k = 0; k < 3; k++) {
      v[k] = t[k] < poly->vertCount ?
                 &tile->verts[poly->verts[t[k]] * 3] :
                 &tile->detailVerts[(pd->vertBase + (t[k] - poly->vertCount)) * 3];
    }
    /* The triangle under the point, else the nearest one in the plane. */
    const float d = std::min({dist_point_segment_2d_sq(snapped, v[0], v[1]),
                              dist_point_segment_2d_sq(snapped, v[1], v[2]),
                              dist_point_segment_2d_sq(snapped, v[2], v[0])});
    float tri[9];
    for (int k = 0; k < 3; k++) {
      dtVcopy(&tri[k * 3], v[k]);
    }
    const bool inside = dtPointInPolygon(snapped, tri, 3);
    const float score = inside ? -1.0f : d;
    if (score < best) {
      best = score;
      float e0[3], e1[3];
      dtVsub(e0, v[1], v[0]);
      dtVsub(e1, v[2], v[0]);
      dtVcross(normal, e0, e1);
      if (normal[1] < 0.0f) {
        dtVscale(normal, normal, -1.0f);
      }
      dtVnormalize(normal);
    }
  }
  r_normal = NavDirectionToWorld(normal);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Walls and drawing
 * \{ */

namespace {

/** Whether polygon edge `edge` leads to a neighbour (in its tile, or a linked one). */
bool edge_has_neighbour(const dtMeshTile *tile, const dtPoly *poly, const int edge)
{
  const unsigned short nei = poly->neis[edge];
  if (nei == 0) {
    return false;
  }
  if ((nei & DT_EXT_LINK) == 0) {
    return true;
  }
  for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
    if (tile->links[k].edge == edge) {
      return true;
    }
  }
  return false;
}

}  // namespace

void KX_NavMesh::ForEachWallEdge(
    const std::function<void(const MT_Vector3 &a, const MT_Vector3 &b)> &edge) const
{
  if (m_mesh == nullptr) {
    return;
  }
  const dtNavMesh *mesh = m_mesh;
  for (int i = 0; i < mesh->getMaxTiles(); i++) {
    const dtMeshTile *tile = mesh->getTile(i);
    if (tile == nullptr || tile->header == nullptr) {
      continue;
    }
    for (int p = 0; p < tile->header->polyCount; p++) {
      const dtPoly *poly = &tile->polys[p];
      if (poly->getType() != DT_POLYTYPE_GROUND) {
        continue;
      }
      for (int j = 0, k = poly->vertCount - 1; j < poly->vertCount; k = j++) {
        if (edge_has_neighbour(tile, poly, k)) {
          continue;
        }
        edge(NavToLocal(&tile->verts[poly->verts[k] * 3]),
             NavToLocal(&tile->verts[poly->verts[j] * 3]));
      }
    }
  }
}

void KX_NavMesh::Draw(const DrawMode mode,
                      const std::function<void(const MT_Vector3 &a, const MT_Vector3 &b)> &line,
                      const MT_Vector3 &center,
                      const float range) const
{
  if (m_mesh == nullptr) {
    return;
  }
  const dtNavMesh *mesh = m_mesh;
  const float range_sq = range * range;
  for (int i = 0; i < mesh->getMaxTiles(); i++) {
    const dtMeshTile *tile = mesh->getTile(i);
    if (tile == nullptr || tile->header == nullptr) {
      continue;
    }
    for (int p = 0; p < tile->header->polyCount; p++) {
      const dtPoly *poly = &tile->polys[p];
      if (poly->getType() != DT_POLYTYPE_GROUND) {
        continue;
      }
      if (range > 0.0f &&
          (ToWorld(&tile->verts[poly->verts[0] * 3]) - center).length2() > range_sq)
      {
        continue;
      }
      if (mode == DrawMode::Triangles) {
        const dtPolyDetail *pd = &tile->detailMeshes[p];
        for (int j = 0; j < pd->triCount; j++) {
          const unsigned char *t = &tile->detailTris[(pd->triBase + j) * 4];
          MT_Vector3 v[3];
          for (int k = 0; k < 3; k++) {
            v[k] = ToWorld(t[k] < poly->vertCount ?
                               &tile->verts[poly->verts[t[k]] * 3] :
                               &tile->detailVerts[(pd->vertBase + (t[k] - poly->vertCount)) *
                                                  3]);
          }
          for (int k = 0; k < 3; k++) {
            line(v[k], v[(k + 1) % 3]);
          }
        }
        continue;
      }
      for (int j = 0, k = poly->vertCount - 1; j < poly->vertCount; k = j++) {
        if (mode == DrawMode::Walls && edge_has_neighbour(tile, poly, k)) {
          continue;
        }
        line(ToWorld(&tile->verts[poly->verts[k] * 3]), ToWorld(&tile->verts[poly->verts[j] * 3]));
      }
    }
  }
}

int KX_NavMesh::PolygonCount() const
{
  if (m_mesh == nullptr) {
    return 0;
  }
  const dtNavMesh *mesh = m_mesh;
  int count = 0;
  for (int i = 0; i < mesh->getMaxTiles(); i++) {
    const dtMeshTile *tile = mesh->getTile(i);
    if (tile != nullptr && tile->header != nullptr) {
      count += tile->header->polyCount;
    }
  }
  return count;
}

void KX_NavMesh::CollectPolygonsForTests(std::vector<std::vector<MT_Vector3>> &r_polygons) const
{
  r_polygons.clear();
  if (m_mesh == nullptr) {
    return;
  }
  const dtNavMesh *mesh = m_mesh;
  for (int i = 0; i < mesh->getMaxTiles(); i++) {
    const dtMeshTile *tile = mesh->getTile(i);
    if (tile == nullptr || tile->header == nullptr) {
      continue;
    }
    for (int p = 0; p < tile->header->polyCount; p++) {
      const dtPoly *poly = &tile->polys[p];
      if (poly->getType() != DT_POLYTYPE_GROUND) {
        continue;
      }
      std::vector<MT_Vector3> polygon;
      for (int j = 0; j < poly->vertCount; j++) {
        polygon.push_back(ToWorld(&tile->verts[poly->verts[j] * 3]));
      }
      r_polygons.push_back(std::move(polygon));
    }
  }
}

/** \} */
