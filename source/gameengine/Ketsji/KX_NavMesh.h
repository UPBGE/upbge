/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavMesh.h
 *  \ingroup ketsji
 *  A navigation mesh while the game runs: Detour's dtNavMesh (one tile for a navmesh object made
 *  in the editor, many for one built while playing), the queries every thread asks it, and the
 *  turn between the game's space (Z up) and Recast's (Y up).
 *
 *  Threads: the mesh is read by any number of threads at once (each takes its own query object,
 *  ThreadQuery); it is changed (tiles added or removed) only on the main thread while nobody reads.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MT_Vector3.h"

class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;
class KX_GameObject;

/** Detour's polygon reference (64 bits: the library's build option). */
using KX_NavPolyRef = uint64_t;

/** A navmesh object's polygons, in Recast's space (Y up), as the editor's bake stored them. */
struct KX_NavMeshPolygons {
  /** 3 floats a vertex. */
  std::vector<float> verts;
  /** `nvp * 2` a polygon: its vertices (0xffff after the last), then its neighbours (0xffff: a
   * wall). */
  std::vector<uint16_t> polys;
  int nvp = 6;
  /** Detail meshes, 4 a polygon: first detail vertex, detail vertex count (the polygon's own
   * vertices not counted), first triangle, triangle count. Empty: none (Detour makes one). */
  std::vector<uint32_t> detail_meshes;
  std::vector<float> detail_verts;
  /** 3 indices a triangle: below the polygon's vertex count its own vertices, then its detail
   * vertices. */
  std::vector<uint8_t> detail_tris;
  float walkable_height = 2.0f;
  float walkable_radius = 0.6f;
  float walkable_climb = 0.9f;
  /** Navigation links (Detour's off-mesh connections), Recast's space: 6 floats each (start,
   * end), a radius, both ways (1) or not (0), the link's number (KX_NavLink::user_id). */
  std::vector<float> link_verts;
  std::vector<float> link_radii;
  std::vector<uint8_t> link_both_ways;
  std::vector<uint32_t> link_ids;
};

class KX_NavMesh {
 public:
  /** Snapping a point onto the navmesh looks this far around it (Recast's space: x, up, z). */
  static constexpr float kSnapExtents[3] = {2.0f, 4.0f, 2.0f};
  /** Polygons one path may cross; longer paths end short (partial) and are searched again. */
  static constexpr int kMaxPathPolygons = 4096;

  ~KX_NavMesh();
  KX_NavMesh(const KX_NavMesh &) = delete;
  KX_NavMesh &operator=(const KX_NavMesh &) = delete;

  /** A navmesh object's polygons as one tile. Null (and the reason) when they are not a
   * navmesh. */
  static std::unique_ptr<KX_NavMesh> FromPolygons(const KX_NavMeshPolygons &polygons,
                                                  std::string &r_error);
  /** Takes a dtNavMesh made elsewhere (built while playing): it is freed with this. */
  static std::unique_ptr<KX_NavMesh> FromDetour(dtNavMesh *mesh);

  /** The object whose space the navmesh is in (its world position, turn and scale at every
   * query), or null: the world. */
  void SetOwner(const KX_GameObject *owner)
  {
    m_owner = owner;
  }
  const KX_GameObject *GetOwner() const
  {
    return m_owner;
  }

  /** The game's world -> Recast's space of the mesh, and back. */
  void ToNav(const MT_Vector3 &world, float r_nav[3]) const;
  MT_Vector3 ToWorld(const float nav[3]) const;
  /** A direction (a normal): turned, not moved. */
  MT_Vector3 NavDirectionToWorld(const float nav[3]) const;
  /** The owner's local space (Z up, unscaled by nothing): what the obstacle simulation stores. */
  static MT_Vector3 NavToLocal(const float nav[3])
  {
    return MT_Vector3(nav[0], nav[2], nav[1]);
  }

  dtNavMesh *Mesh()
  {
    return m_mesh;
  }
  const dtNavMesh *Mesh() const
  {
    return m_mesh;
  }
  /** This thread's query object (made at its first use). */
  dtNavMeshQuery *ThreadQuery() const;
  /** Every walkable polygon, every area at its cost. */
  const dtQueryFilter &DefaultFilter() const;

  /** The navmesh point nearest to `world` (within kSnapExtents of it). */
  bool NearestPoint(const MT_Vector3 &world,
                    MT_Vector3 &r_point,
                    KX_NavPolyRef *r_ref = nullptr,
                    const dtQueryFilter *filter = nullptr) const;
  /** The corners of the shortest way from `from` to `to`, both snapped onto the navmesh first,
   * the start first. False without a way. `r_complete` is false when the way ends short of `to`
   * (it can't be reached, or the search ran out of room: then it ends at the nearest point it
   * found). */
  bool FindPath(const MT_Vector3 &from,
                const MT_Vector3 &to,
                std::vector<MT_Vector3> &r_corners,
                bool *r_complete = nullptr,
                const dtQueryFilter *filter = nullptr) const;
  /** Walks a straight line along the navmesh from `from` toward `to`. True when a wall stops it:
   * `r_fraction` of the way (0..1), the wall's normal; false: the whole way is walkable. */
  bool Raycast(const MT_Vector3 &from,
               const MT_Vector3 &to,
               float &r_fraction,
               MT_Vector3 &r_hit,
               MT_Vector3 &r_normal,
               const dtQueryFilter *filter = nullptr) const;
  /** A random navmesh point reachable from `center` and within `radius` of it (0: anywhere
   * reachable). The same seed gives the same point. */
  bool RandomPoint(const MT_Vector3 &center,
                   float radius,
                   uint64_t seed,
                   MT_Vector3 &r_point,
                   const dtQueryFilter *filter = nullptr) const;
  /** The navmesh surface's normal under `world`. */
  bool SurfaceNormal(const MT_Vector3 &world, MT_Vector3 &r_normal) const;

  /** Edges without a neighbour (walls), in the owner's local space (Z up). */
  void ForEachWallEdge(
      const std::function<void(const MT_Vector3 &a, const MT_Vector3 &b)> &edge) const;

  enum class DrawMode { Walls, Polygons, Triangles };
  /** Lines in the world, within `range` of `center` when `range` > 0. */
  void Draw(DrawMode mode,
            const std::function<void(const MT_Vector3 &a, const MT_Vector3 &b)> &line,
            const MT_Vector3 &center = MT_Vector3(0.0f, 0.0f, 0.0f),
            float range = 0.0f) const;

  int PolygonCount() const;
  /** Raised whenever tiles change: paths across the old ones must be checked. */
  uint64_t GetTileStamp() const
  {
    return m_tileStamp.load(std::memory_order_relaxed);
  }
  void NoteTilesChanged()
  {
    m_tileStamp.fetch_add(1, std::memory_order_relaxed);
  }

  /** Tests and tools: the tile's polygon data in Recast's space. */
  void CollectPolygonsForTests(std::vector<std::vector<MT_Vector3>> &r_polygons) const;

 private:
  KX_NavMesh();

  dtNavMesh *m_mesh = nullptr;
  const KX_GameObject *m_owner = nullptr;
  /** One query object for each thread slot (KX_NavThreadSlot), made by that thread. */
  mutable std::vector<std::atomic<dtNavMeshQuery *>> m_queries;
  std::unique_ptr<dtQueryFilter> m_defaultFilter;
  std::atomic<uint64_t> m_tileStamp{0};
};

/** The calling thread's small index for per-thread navigation data (reused after the thread
 * ends). */
uint32_t KX_NavThreadSlot();
