/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavAir.h
 *  \ingroup ketsji
 *
 * The air map (navigation phase 8, 2026-11-19): where a flyer of one size fits, made from the
 * level's static solid shapes. Voxels in bricks of 8 x 8 x 8 (a mask of 512 bits); only bricks
 * near a solid are kept, a brick holding none is free air and is searched as one big cell (a
 * sparse two-level grid: the sparse voxel octree's idea, Warframe 2015, with eight children a
 * side). A voxel is solid when its middle lies within the flyer's radius and half a voxel of a
 * triangle, or inside a closed convex piece (a box, a convex hull: KX_NavGeometry::pieces).
 *
 * The map is the level's box and a headroom above it. Beside the box (below its top) is solid:
 * a flyer never leaves the level's sides there, and the space under a terrain, open only round
 * its edges, stays sealed off. Above the box is free sky.
 *
 * Water: the voxels inside the Buoyancy Volume sensors (KX_NavWaterVolume); a swimmer keeps to
 * free voxels that are water.
 *
 * Everything here is read-only once built: any thread may ask while nothing builds.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

struct KX_NavGeometry;
class LN_JobPool;

/** A water volume: an oriented box in the world (a Buoyancy Volume sensor's bounds). */
struct KX_NavWaterVolume {
  MT_Vector3 center = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 half = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 orientation = MT_Matrix3x3::Identity();
};

class KX_NavAirMap {
 public:
  struct Settings {
    /** The flyer's radius (solids are grown by it). */
    float radius = 0.5f;
    /** A voxel's edge. */
    float voxel = 0.5f;
    /** How far above the level's top the map reaches (above it: free sky). */
    float headroom = 30.0f;
  };

  explicit KX_NavAirMap(const Settings &settings);

  /** Builds the map of `geometry` (the level in the game's space) and the `water` volumes (the
   * box holds both), on `pool`'s helpers (each brick by one thread: the same on any count). No
   * triangles and no water: open sky, no box. */
  void Build(const KX_NavGeometry &geometry,
             const std::vector<KX_NavWaterVolume> &water,
             LN_JobPool *pool);
  /** Builds again the bricks meeting a box (the world) from `geometry` (the level's triangles
   * reaching into the box grown by the radius); the map's box stays. */
  void Rebuild(const MT_Vector3 &min,
               const MT_Vector3 &max,
               const KX_NavGeometry &geometry,
               LN_JobPool *pool);
  /** The water: the voxels inside `volumes` (replaces the last). */
  void SetWater(const std::vector<KX_NavWaterVolume> &volumes);

  /** No level: everything free (open sky). */
  bool OpenSky() const
  {
    return m_open;
  }
  bool HasWater() const
  {
    return !m_water.empty();
  }
  float Radius() const
  {
    return m_settings.radius;
  }
  float Voxel() const
  {
    return m_settings.voxel;
  }
  /** The map's box (the world; meaningless for open sky). */
  MT_Vector3 Min() const;
  MT_Vector3 Max() const;
  /** Whether the box holds a box (the world): a level piece or water there is mapped by
   * Rebuild / SetWater; outside it (beside the level, above the headroom, any for open sky)
   * only a whole Build maps it. */
  bool Holds(const MT_Vector3 &min, const MT_Vector3 &max) const;
  size_t BrickCount() const
  {
    return m_bricks.size();
  }
  size_t MemoryBytes() const;

  /** Whether a flyer fits at `p` (and it is water, `water_only`). */
  bool Free(const MT_Vector3 &p, bool water_only = false) const;
  /** Whether a flyer fits all along the straight line from `a` to `b`. */
  bool LineFree(const MT_Vector3 &a, const MT_Vector3 &b, bool water_only = false) const;
  /** A free point for `p`: `p` itself when free; touching a solid (within the radius and two
   * voxels of free air: a flyer at a wall), the nearest, a little aside; buried (in the ground,
   * inside a building, beside the map below its top), straight above, up to `reach` or
   * `up_reach` when larger (never for water: a swimmer's); else the nearest within `reach`.
   * False when none. */
  bool NearestFree(const MT_Vector3 &p,
                   float reach,
                   MT_Vector3 &r_point,
                   bool water_only = false,
                   float up_reach = 0.0f) const;
  /** The way from `from` to `to` (each snapped to the nearest free point first): its corners
   * after `from`, smoothed (the next corner seen from the last), `to` last. False when no way
   * starts; `r_complete` false when it ends short of `to` (the nearest the search came: it ran
   * out of `max_nodes`, or `to` cannot be reached). */
  bool FindPath(const MT_Vector3 &from,
                const MT_Vector3 &to,
                std::vector<MT_Vector3> &r_points,
                bool *r_complete,
                bool water_only = false,
                int max_nodes = 20000) const;

  /** Walls and ceilings (navigation phase 9, 2026-11-19): crawlers move on the map's shell, the
   * free voxels touching a solid voxel of the level (26 round; the box's sides and bottom are no
   * level: nothing crawls on them), each a floor, a wall or a ceiling by the solids round it (its
   * normal points away from them). Made when a crawler of this size first asks
   * (EnableSurfaces), then kept with the bricks (Build, Rebuild + SetSurfaceGeometry). A
   * crawler's body stays at its own height from the level's true surface (NearestSurface, from
   * a copy of the level's triangles); the shell, a radius out, is where its search goes. */
  enum SurfaceKind : int { SurfaceFloor = 1, SurfaceWall = 2, SurfaceCeiling = 4 };
  /** A meter's cost on each kind of surface (a crawler's area costs "wall" and "ceiling"). */
  struct SurfaceCosts {
    float floor = 1.0f, wall = 1.0f, ceiling = 1.0f;
  };
  /** The surfaces of `geometry` (the whole level); a normal at least `floor_cos` up is a floor,
   * at least half down a ceiling, the rest walls. */
  void EnableSurfaces(const KX_NavGeometry &geometry, float floor_cos, LN_JobPool *pool);
  bool HasSurfaces() const
  {
    return m_surfaces;
  }
  /** The level's triangles again (after a Rebuild: the shell there is made again by Rebuild;
   * the true surface reads the whole level). */
  void SetSurfaceGeometry(const KX_NavGeometry &geometry);
  /** The level's surface point nearest `p` within `reach`, and the way from it out toward `p`
   * (unit; `p` on the surface: the triangle's face, toward `hint` when given). */
  bool NearestSurface(const MT_Vector3 &p,
                      float reach,
                      MT_Vector3 &r_point,
                      MT_Vector3 &r_normal,
                      const MT_Vector3 *hint = nullptr) const;
  /** The surface a crawler at `p` stands on: of the surfaces touching it (within `touch` of `p`,
   * or about as near as the nearest), the one its way `along` runs along best (in a corner up a
   * wall, the wall; on its way out across the floor, the floor; a Character's capsule touches
   * the wall beside it nearer than the floor under it); `along` zero: the nearest. */
  bool SupportSurface(const MT_Vector3 &p,
                      float reach,
                      float touch,
                      const MT_Vector3 &along,
                      MT_Vector3 &r_point,
                      MT_Vector3 &r_normal,
                      const MT_Vector3 *hint = nullptr) const;
  /** The kind of shell at `p` (a SurfaceKind; 0: none). */
  int SurfaceKindAt(const MT_Vector3 &p) const;
  /** Whether a straight move from `a` to `b` keeps to the shell of `kinds` (and the free air
   * just off it). */
  bool SurfaceLineFree(const MT_Vector3 &a, const MT_Vector3 &b, int kinds) const;
  /** The nearest shell point of `kinds` to `p` within `reach` (a voxel's middle). */
  bool NearestShell(const MT_Vector3 &p, float reach, int kinds, MT_Vector3 &r_point) const;
  /** The way over the shell of `kinds` from `from` to `to` (each snapped to the nearest shell
   * of `kinds`, `to` within `goal_reach`): its corners after `from` (voxel middles, smoothed
   * along the faces), the snapped `to` last. False when no way starts; `r_complete` false when
   * it ends short. */
  bool FindSurfacePath(const MT_Vector3 &from,
                       const MT_Vector3 &to,
                       int kinds,
                       const SurfaceCosts &costs,
                       std::vector<MT_Vector3> &r_points,
                       bool *r_complete,
                       float goal_reach = 4.0f,
                       int max_nodes = 20000) const;
  size_t SurfaceBrickCount() const
  {
    return m_shell.size();
  }

 private:
  struct Brick {
    /** Bit (x + 8 y) of word z: solid. */
    uint64_t solid[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool full = false;
  };
  /** A brick's shell (bit set: a shell voxel; floor / ceiling: its kind, else a wall). */
  struct ShellBrick {
    uint64_t shell[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t floor[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t ceiling[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  };
  struct Index {
    int x = 0, y = 0, z = 0;
  };
  /** A convex piece's face: a point inside the piece is behind every one. */
  struct Plane {
    MT_Vector3 point, normal;
  };
  using PieceMap = std::unordered_map<int32_t, std::vector<Plane>>;
  using Bins = std::unordered_map<uint64_t, std::vector<int>>;

  static uint64_t BrickKey(int bx, int by, int bz);
  Index VoxelOf(const MT_Vector3 &p) const;
  MT_Vector3 CenterOf(const Index &v) const;
  /** Inside the box's sides and bottom (above its top counts too: sky). */
  bool VoxelFree(const Index &v, bool water_only) const;
  bool VoxelIsWater(const Index &v) const;
  /** The convex pieces' faces (their numbers hold for one collection of the level only). */
  static PieceMap PiecesOf(const KX_NavGeometry &geometry);
  /** Each triangle into the bricks its grown bounds meet, each piece into those it covers. */
  void BinGeometry(const KX_NavGeometry &geometry,
                   const PieceMap &pieces,
                   Bins &r_tri_bins,
                   Bins &r_piece_bins) const;
  /** Fills `r_bricks` with the bricks of `keys` (each by one thread, alone). */
  void ComputeBricks(const KX_NavGeometry &geometry,
                     const PieceMap &pieces,
                     const std::vector<uint64_t> &keys,
                     const Bins &tri_bins,
                     const Bins &piece_bins,
                     LN_JobPool *pool,
                     std::vector<Brick> &r_bricks) const;
  /** A whole brick of free air (no solid voxel, all inside the box's sides): one search node. */
  bool BrickWholeFree(int bx, int by, int bz) const;
  /** A solid voxel of the level (inside the box; the box's sides are none). */
  bool VoxelSolidLevel(const Index &v) const;
  /** The shell's kind at a voxel (a SurfaceKind; 0: none). */
  int ShellKind(const Index &v) const;
  /** The shell of the bricks of `keys` (each by one thread, from the solids round it). */
  void ComputeShell(const std::vector<uint64_t> &keys, LN_JobPool *pool);
  /** The bricks whose shell a solid brick in [b0, b1] may touch (they and the 26 round). */
  void ShellKeysAround(const int *b0, const int *b1, std::vector<uint64_t> &r_keys) const;
  /** The triangle copy's cells (for NearestSurface). */
  void BinSurfaceGeometry();

  Settings m_settings;
  bool m_open = true;
  /** The box: voxel (0, 0, 0)'s corner and the voxel count of each side. */
  MT_Vector3 m_origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  int m_size[3] = {0, 0, 0};
  std::unordered_map<uint64_t, Brick> m_bricks;
  /** Water bricks (bit set: water). */
  std::unordered_map<uint64_t, Brick> m_water;
  /** Surfaces (phase 9): on, the floor's least normal z, the shell's bricks, the level's
   * triangles (a copy) in cells of m_cellSize. */
  bool m_surfaces = false;
  float m_floorCos = 0.7f;
  std::unordered_map<uint64_t, ShellBrick> m_shell;
  std::vector<float> m_surfaceVerts;
  std::vector<int> m_surfaceTris;
  float m_cellSize = 2.0f;
  Bins m_surfaceCells;
  /** Triangles too big for the cells (a ground of two triangles): looked at every time. */
  std::vector<int> m_surfaceBig;
};
