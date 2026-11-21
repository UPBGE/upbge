/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavBuild.cpp
 *  \ingroup ketsji
 *
 *  The scene's own navmesh: tiles of kTileCells x kTileCells cells, each voxelized from the
 *  level's triangles by Recast into compressed layers that Detour's tile cache keeps; the tile
 *  cache turns layers into navmesh tiles, cutting the still obstacles out of them (only the tiles
 *  an obstacle touches are made again: microseconds). One tile cache and navmesh per agent size
 *  (Recast shrinks the walkable area by the agent's radius when it voxelizes).
 *
 *  Whole Level: every tile when the game starts (all threads). Around NPCs: only the tiles
 *  within the Build Radius of the agents, the nearest first, kTilesPerStep a step; tiles past the
 *  Remove Radius leave the navmesh but their layers are kept, and come back at once. Both: tiles
 *  a changed level piece touches are made again from the physics' shapes.
 *
 *  Voxelizing runs on the job pool's threads, a tile each; everything else (the physics' shapes,
 *  the tile cache, the navmesh) on the main thread, tiles in a fixed order: the navmesh is the
 *  same on any number of threads.
 */

#include "KX_NavBuild.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "CM_Message.h"
#include "DNA_scene_types.h"
#include "EXP_ListValue.h"
#include "EXP_Value.h"
#include "KX_Detour.h"
#include "KX_GameObject.h"
#include "KX_NavJumpLinks.h"
#include "KX_NavMesh.h"
#include "KX_Scene.h"
#include "LN_JobPool.h"
#include "PHY_IPhysicsEnvironment.h"

namespace {

/** Cells a tile is wide (at the default 0.3 m cells: 19.2 m). */
constexpr int kTileCells = 64;
constexpr int kMaxLayers = 32;
/** Around NPCs: tiles made a step at most (after the first step). */
constexpr int kTilesPerStep = 16;
/** Agent classes (radius, climb, slope) that get their own navmesh; more share the nearest one
 * they surely can walk (a larger radius, a lower climb and slope). */
constexpr int kMaxSizeClasses = 6;
constexpr int kMaxObstacles = 4096;
/** Around NPCs: dropped tiles kept (oldest forgotten first). */
constexpr size_t kMaxParkedTiles = 4096;
/** The whole level is found inside this box. */
constexpr float kLevelReach = 1.0e6f;

using TileKey = std::pair<int, int>;

struct TileKeyHash {
  size_t operator()(const TileKey &key) const
  {
    return std::hash<uint64_t>()((uint64_t(uint32_t(key.first)) << 32) | uint32_t(key.second));
  }
};

/** Layers are kept as they are (no compression: a 64 x 64 layer is 16 KB). */
struct RawCompressor : public dtTileCacheCompressor {
  int maxCompressedSize(const int size) override
  {
    return size;
  }
  dtStatus compress(const unsigned char *buffer,
                    const int size,
                    unsigned char *compressed,
                    const int /*max*/,
                    int *compressed_size) override
  {
    memcpy(compressed, buffer, size_t(size));
    *compressed_size = size;
    return DT_SUCCESS;
  }
  dtStatus decompress(const unsigned char *compressed,
                      const int compressed_size,
                      unsigned char *buffer,
                      const int max_size,
                      int *size) override
  {
    if (compressed_size > max_size) {
      return DT_FAILURE | DT_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, compressed, size_t(compressed_size));
    *size = compressed_size;
    return DT_SUCCESS;
  }
};

/** The tile cache's scratch memory while it makes a navmesh tile (main thread). */
struct LinearAlloc : public dtTileCacheAlloc {
  std::vector<unsigned char> buffer;
  size_t top = 0;
  LinearAlloc() : buffer(size_t(16) << 20) {}
  void reset() override
  {
    top = 0;
  }
  void *alloc(const size_t size) override
  {
    const size_t aligned = (size + 15) & ~size_t(15);
    if (top + aligned > buffer.size()) {
      return nullptr;
    }
    void *p = &buffer[top];
    top += aligned;
    return p;
  }
  void free(void * /*ptr*/) override {}
};

/** Off-mesh connections in Recast's space, as Detour's create parameters take them. */
struct LinkArrays {
  std::vector<float> verts;
  std::vector<float> radii;
  std::vector<unsigned char> dirs;
  std::vector<unsigned char> areas;
  std::vector<unsigned short> flags;
  std::vector<unsigned int> ids;

  void clear()
  {
    verts.clear();
    radii.clear();
    dirs.clear();
    areas.clear();
    flags.clear();
    ids.clear();
  }
  size_t size() const
  {
    return ids.size();
  }
  void add(const float *a,
           const float *b,
           const float radius,
           const unsigned char dir,
           const unsigned char area,
           const unsigned int id)
  {
    verts.insert(verts.end(), a, a + 3);
    verts.insert(verts.end(), b, b + 3);
    radii.push_back(radius);
    dirs.push_back(dir);
    areas.push_back(area);
    flags.push_back(1);
    ids.push_back(id);
  }
  void append(const LinkArrays &other, const size_t i)
  {
    add(&other.verts[i * 6],
        &other.verts[i * 6 + 3],
        other.radii[i],
        other.dirs[i],
        other.areas[i],
        other.ids[i]);
  }
};

/** Areas and flags of each new navmesh polygon, and the links: the hand-placed ones (`links`)
 * and those found for the class whose tiles are being made (`found`, 2026-11-19: each class
 * finds its own, by its body), those starting in the tile handed over. */
struct MeshProcess : public dtTileCacheMeshProcess {
  LinkArrays links;
  /** Each tile's found links (by its start's tile), as Detour takes them. */
  const std::map<TileKey, LinkArrays> *found = nullptr;
  LinkArrays tile;

  void process(dtNavMeshCreateParams *params,
               unsigned char *areas,
               unsigned short *flags) override
  {
    for (int i = 0; i < params->polyCount; i++) {
      /* The build's 1 + id back to the id (ground 0). */
      areas[i] = areas[i] > 0 ? (unsigned char)(areas[i] - 1) : 0;
      flags[i] = 1;
    }
    tile.clear();
    for (size_t i = 0; i < links.size(); i++) {
      tile.append(links, i);
    }
    if (found != nullptr) {
      /* This tile's found links (Detour keeps a link in its start's tile; every layer of the
       * tile gets them, Detour keeps those touching the layer). */
      auto it = found->find(TileKey(params->tileX, params->tileY));
      if (it != found->end()) {
        for (size_t i = 0; i < it->second.size(); i++) {
          tile.append(it->second, i);
        }
      }
    }
    /* Room for the landings of links leaving the tile (2026-11-19). Detour joins a link's end
     * to its landing with a link of the start's tile, but sets room aside only for link ends
     * inside a tile (dtCreateNavMeshData counts them): a tile with links leaving it ran out of
     * room and left their ends unjoined, silently (6 of 19 links of a test level: 3 of 5 drops
     * off a platform, every jump back over a gap; NPCs walked 6 m to a link that worked). A
     * link with only its end in the tile sets room aside and is kept by no tile (its start is
     * in no build it is handed to): one for each link leaving, ending at that link's start. */
    const float *bmin = params->bmin, *bmax = params->bmax;
    auto inside = [&](const float *p) {
      return p[0] >= bmin[0] && p[0] < bmax[0] && p[2] >= bmin[2] && p[2] < bmax[2];
    };
    const size_t real = tile.size();
    for (size_t i = 0; i < real; i++) {
      float start[3], end[3];
      dtVcopy(start, &tile.verts[i * 6]);
      dtVcopy(end, &tile.verts[i * 6 + 3]);
      if (inside(start) && !inside(end)) {
        const float outside[3] = {bmax[0] + 1000.0f, start[1], start[2]};
        tile.add(outside, start, 0.05f, 0, 0, 0);
      }
    }
    if (tile.size() > 0) {
      params->offMeshConVerts = tile.verts.data();
      params->offMeshConRad = tile.radii.data();
      params->offMeshConDir = tile.dirs.data();
      params->offMeshConAreas = tile.areas.data();
      params->offMeshConFlags = tile.flags.data();
      params->offMeshConUserID = tile.ids.data();
      params->offMeshConCount = int(tile.size());
    }
  }
};

/** One layer's tile cache data (made with dtAlloc: the tile cache takes it). */
struct LayerData {
  unsigned char *data = nullptr;
  int size = 0;
};

struct ObstacleState {
  dtObstacleRef ref = 0;
  MT_Vector3 center, half;
  float yaw = 0.0f;
  bool round = false;
};

/** A navmesh's class: what its agents can walk, in Recast's units (cells, whole degrees). */
struct ClassKey {
  int radius_cells = 1;
  int climb_cells = 0;
  int slope_degrees = 45;
  bool operator==(const ClassKey &other) const
  {
    return radius_cells == other.radius_cells && climb_cells == other.climb_cells &&
           slope_degrees == other.slope_degrees;
  }
};

struct SizeClass {
  ClassKey key;
  int radius_cells = 1;
  float radius = 0.5f;
  /** The highest ledge (m) and steepest slope (radians) its agents walk. */
  float climb = 0.9f;
  float max_slope = 0.785398f;
  dtTileCache *cache = nullptr;
  std::unique_ptr<KX_NavMesh> navmesh;
  /** Tiles whose layers are in the cache. */
  std::set<TileKey> tiles;
  /** Around NPCs: dropped tiles' layers, for a return without voxelizing. */
  std::map<TileKey, std::vector<std::vector<unsigned char>>> parked;
  std::deque<TileKey> parked_order;
  std::unordered_map<const KX_GameObject *, ObstacleState> obstacles;
  /** Around NPCs (2026-11-19): each agent's cell (a quarter tile) as counted, and how many
   * agents' cells want each tile (build radius) and keep it (remove radius); the wanted tiles not
   * there yet, with how near they were first wanted. */
  struct Cover {
    int cx = 0, cy = 0;
    uint64_t seen = 0;
  };
  std::unordered_map<uint64_t, Cover> covers;
  std::unordered_map<TileKey, int, TileKeyHash> want_count, keep_count;
  std::map<TileKey, float> pending;
  uint64_t cover_serial = 0;
  /** Jump links found (2026-11-19): each tile's (by their start), and the same as Detour takes
   * them (MeshProcess::found). */
  std::map<TileKey, std::vector<uint32_t>> found_by_tile;
  std::map<TileKey, LinkArrays> found;
  bool level_built = false;
  bool started = false;
  bool warned_obstacles = false;

  ~SizeClass()
  {
    dtFreeTileCache(cache);
  }
};

/** A tile's voxelizing settings. */
rcConfig tile_config(const KX_Navigation::Settings &settings, const SizeClass &size)
{
  rcConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.cs = settings.cell_size;
  cfg.ch = settings.cell_height;
  cfg.walkableSlopeAngle = size.max_slope * 57.2957795f;
  cfg.walkableHeight = std::max(int(std::ceil(settings.agent_height / cfg.ch)), 3);
  cfg.walkableClimb = int(std::floor(size.climb / cfg.ch));
  cfg.walkableRadius = size.radius_cells;
  cfg.maxEdgeLen = int(settings.edge_max_len / cfg.cs);
  cfg.maxSimplificationError = settings.edge_max_error;
  cfg.minRegionArea = int(settings.region_min_size * settings.region_min_size);
  cfg.mergeRegionArea = int(settings.region_merge_size * settings.region_merge_size);
  cfg.maxVertsPerPoly = std::clamp(settings.verts_per_poly, 3, DT_VERTS_PER_POLYGON);
  cfg.tileSize = kTileCells;
  cfg.borderSize = cfg.walkableRadius + 3;
  cfg.width = cfg.tileSize + cfg.borderSize * 2;
  cfg.height = cfg.width;
  cfg.detailSampleDist = settings.detail_sample_dist < 0.9f ? 0.0f :
                                                               cfg.cs * settings.detail_sample_dist;
  cfg.detailSampleMaxError = cfg.ch * settings.detail_sample_max_error;
  return cfg;
}

}  // namespace

struct KX_NavBuildState {
  KX_Navigation::Settings settings;
  KX_NavGeometrySource source;
  RawCompressor compressor;
  LinearAlloc alloc;
  MeshProcess process;
  std::vector<std::unique_ptr<SizeClass>> classes;
  std::set<TileKey> dirty;
  /** Tiles to make again from their layers (links came, went or moved there). */
  std::set<TileKey> relink;
  float tile_width = 19.2f;
  KX_NavBuildStats stats;
  /** Jump links found, by id (every class's), the next id, the links' areas. */
  std::unordered_map<uint32_t, KX_NavLinkInfo> found_links;
  uint32_t next_found_id = kNavFoundLinkBase;
  uint8_t jump_area = 0, drop_area = 0;
};

/* -------------------------------------------------------------------- */
/** \name Making tiles
 * \{ */

namespace {

/** The game's space (Z up) -> Recast's (Y up). */
inline void to_nav(const MT_Vector3 &v, float *out)
{
  out[0] = float(v[0]);
  out[1] = float(v[2]);
  out[2] = float(v[1]);
}

/** A tile's triangles in Recast's space, with their areas. */
struct TileInput {
  TileKey key;
  std::vector<float> verts;
  std::vector<int> tris;
  std::vector<unsigned char> areas;
  /** Per triangle: its closed convex piece, -1 for none (KX_NavGeometry::pieces). */
  std::vector<int32_t> pieces;
  float ymin = 0.0f, ymax = 0.0f;
  std::vector<LayerData> layers;
};

/** The box (the game's space) a tile reads its triangles from: the tile and its border. */
void tile_box(const KX_NavBuildState &state,
              const SizeClass &size,
              const TileKey &key,
              MT_Vector3 &r_min,
              MT_Vector3 &r_max)
{
  const rcConfig cfg = tile_config(state.settings, size);
  const float pad = float(cfg.borderSize) * cfg.cs;
  r_min = MT_Vector3(key.first * state.tile_width - pad, key.second * state.tile_width - pad, -kLevelReach);
  r_max = MT_Vector3(
      (key.first + 1) * state.tile_width + pad, (key.second + 1) * state.tile_width + pad, kLevelReach);
}

void append_geometry(TileInput &input, const KX_NavGeometry &geometry, const std::vector<int> *only)
{
  /* Vertices are copied per triangle (a tile reads few): no index remapping. */
  auto add = [&](const int t) {
    const int base = int(input.verts.size() / 3);
    for (int k = 0; k < 3; k++) {
      const int v = geometry.tris[size_t(t) * 3 + k];
      const float *p = &geometry.verts[size_t(v) * 3];
      input.verts.push_back(p[0]);
      input.verts.push_back(p[2]);
      input.verts.push_back(p[1]);
      input.ymin = std::min(input.ymin, p[2]);
      input.ymax = std::max(input.ymax, p[2]);
      input.tris.push_back(base + k);
    }
    input.areas.push_back(geometry.areas.empty() ? 0 : geometry.areas[size_t(t)]);
    input.pieces.push_back(geometry.pieces.empty() ? -1 : geometry.pieces[size_t(t)]);
  };
  if (only != nullptr) {
    for (const int t : *only) {
      add(t);
    }
  }
  else {
    for (int t = 0; t < int(geometry.tris.size() / 3); t++) {
      add(t);
    }
  }
}

/** Closed convex pieces filled solid, from their bottom to their top in each cell column whose
 * middle they cover (2026-11-19): Recast rasterizes surfaces only, so the floor inside a box
 * taller than an NPC (a wall 3 m high) kept its headroom up to the box's top and stayed
 * walkable, an island inside the wall that NPCs pressed against it slid into where it met the
 * floor at a gate. The filled span ends where the top face's does, so the top keeps its area
 * (Recast merges the two, keeping the higher area). */
void fill_solid_pieces(const TileInput &input,
                       const rcConfig &cfg,
                       rcHeightfield &solid,
                       rcContext *ctx)
{
  if (input.pieces.empty()) {
    return;
  }
  std::unordered_map<int32_t, std::vector<int>> by_piece;
  for (int t = 0; t < int(input.pieces.size()); t++) {
    if (input.pieces[size_t(t)] >= 0) {
      by_piece[input.pieces[size_t(t)]].push_back(t);
    }
  }
  /* (The order of pieces does not matter: filling is a union.) */
  for (const auto &[piece, tris] : by_piece) {
    if (tris.size() < 4) {
      continue;
    }
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const int t : tris) {
      for (int k = 0; k < 3; k++) {
        const float *v = &input.verts[size_t(input.tris[size_t(t) * 3 + k]) * 3];
        for (int axis = 0; axis < 3; axis++) {
          lo[axis] = std::min(lo[axis], v[axis]);
          hi[axis] = std::max(hi[axis], v[axis]);
        }
      }
    }
    const int x0 = std::max(0, int(std::floor((lo[0] - cfg.bmin[0]) / cfg.cs)));
    const int x1 = std::min(solid.width - 1, int(std::floor((hi[0] - cfg.bmin[0]) / cfg.cs)));
    const int z0 = std::max(0, int(std::floor((lo[2] - cfg.bmin[2]) / cfg.cs)));
    const int z1 = std::min(solid.height - 1, int(std::floor((hi[2] - cfg.bmin[2]) / cfg.cs)));
    for (int z = z0; z <= z1; z++) {
      for (int x = x0; x <= x1; x++) {
        const float px = cfg.bmin[0] + (float(x) + 0.5f) * cfg.cs;
        const float pz = cfg.bmin[2] + (float(z) + 0.5f) * cfg.cs;
        /* The vertical line through the cell's middle: where it meets the piece's faces. */
        float bottom = FLT_MAX, top = -FLT_MAX;
        for (const int t : tris) {
          const float *a = &input.verts[size_t(input.tris[size_t(t) * 3 + 0]) * 3];
          const float *b = &input.verts[size_t(input.tris[size_t(t) * 3 + 1]) * 3];
          const float *c = &input.verts[size_t(input.tris[size_t(t) * 3 + 2]) * 3];
          const float d = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
          if (std::fabs(d) < 1e-9f) {
            continue; /* A wall seen edge on from above. */
          }
          const float u = ((px - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (pz - a[2])) / d;
          const float v = ((b[0] - a[0]) * (pz - a[2]) - (px - a[0]) * (b[2] - a[2])) / d;
          if (u < -1e-5f || v < -1e-5f || u + v > 1.0f + 1e-5f) {
            continue;
          }
          const float y = a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
          bottom = std::min(bottom, y);
          top = std::max(top, y);
        }
        if (!(top > bottom)) {
          continue;
        }
        const int smin = std::clamp(int(std::floor((bottom - cfg.bmin[1]) / cfg.ch)),
                                    0,
                                    RC_SPAN_MAX_HEIGHT);
        const int smax = std::clamp(int(std::ceil((top - cfg.bmin[1]) / cfg.ch)),
                                    smin + 1,
                                    RC_SPAN_MAX_HEIGHT);
        rcAddSpan(ctx,
                  solid,
                  x,
                  z,
                  (unsigned short)smin,
                  (unsigned short)smax,
                  RC_NULL_AREA,
                  cfg.walkableClimb);
      }
    }
  }
}

/** Voxelizes one tile into its layers (any thread). */
void rasterize_tile(const KX_NavBuildState &state, const SizeClass &size, TileInput &input)
{
  input.layers.clear();
  const int ntris = int(input.tris.size() / 3);
  if (ntris == 0) {
    return;
  }
  rcContext ctx(false);
  rcConfig cfg = tile_config(state.settings, size);
  const float tcs = float(cfg.tileSize) * cfg.cs;
  cfg.bmin[0] = input.key.first * tcs - cfg.borderSize * cfg.cs;
  cfg.bmin[2] = input.key.second * tcs - cfg.borderSize * cfg.cs;
  cfg.bmax[0] = (input.key.first + 1) * tcs + cfg.borderSize * cfg.cs;
  cfg.bmax[2] = (input.key.second + 1) * tcs + cfg.borderSize * cfg.cs;
  cfg.bmin[1] = input.ymin - 1.0f;
  cfg.bmax[1] = input.ymax + state.settings.agent_height + 1.0f;

  rcHeightfield *solid = rcAllocHeightfield();
  if (solid == nullptr ||
      !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
  {
    rcFreeHeightField(solid);
    return;
  }
  /* Walkable triangles (by slope) take their object's area, as 1 + its id: ground is 1, the
   * lowest walkable area, because where two surfaces merge into one span (a pond's slab lying on
   * the floor, a road on a field) Recast keeps the higher area: the named one wins. process()
   * turns them back into the ids (ground 0). */
  std::vector<unsigned char> areas(size_t(ntris), RC_NULL_AREA);
  const int nverts = int(input.verts.size() / 3);
  rcMarkWalkableTriangles(&ctx,
                          cfg.walkableSlopeAngle,
                          input.verts.data(),
                          nverts,
                          input.tris.data(),
                          ntris,
                          areas.data());
  for (int i = 0; i < ntris; i++) {
    if (areas[size_t(i)] == RC_WALKABLE_AREA) {
      areas[size_t(i)] = (unsigned char)(1 + std::min<int>(input.areas[size_t(i)], 62));
    }
  }
  rcRasterizeTriangles(&ctx,
                       input.verts.data(),
                       nverts,
                       input.tris.data(),
                       areas.data(),
                       ntris,
                       *solid,
                       cfg.walkableClimb);
  fill_solid_pieces(input, cfg, *solid, &ctx);
  rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
  rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
  rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

  rcCompactHeightfield *chf = rcAllocCompactHeightfield();
  if (chf == nullptr ||
      !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
  {
    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    return;
  }
  rcFreeHeightField(solid);
  rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
  rcHeightfieldLayerSet *lset = rcAllocHeightfieldLayerSet();
  if (lset == nullptr ||
      !rcBuildHeightfieldLayers(&ctx, *chf, cfg.borderSize, cfg.walkableHeight, *lset))
  {
    rcFreeCompactHeightfield(chf);
    rcFreeHeightfieldLayerSet(lset);
    return;
  }
  rcFreeCompactHeightfield(chf);
  RawCompressor compressor;
  for (int i = 0; i < std::min(lset->nlayers, kMaxLayers); i++) {
    const rcHeightfieldLayer *layer = &lset->layers[i];
    dtTileCacheLayerHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = DT_TILECACHE_MAGIC;
    header.version = DT_TILECACHE_VERSION;
    header.tx = input.key.first;
    header.ty = input.key.second;
    header.tlayer = i;
    dtVcopy(header.bmin, layer->bmin);
    dtVcopy(header.bmax, layer->bmax);
    header.width = (unsigned char)layer->width;
    header.height = (unsigned char)layer->height;
    header.minx = (unsigned char)layer->minx;
    header.maxx = (unsigned char)layer->maxx;
    header.miny = (unsigned char)layer->miny;
    header.maxy = (unsigned char)layer->maxy;
    header.hmin = (unsigned short)layer->hmin;
    header.hmax = (unsigned short)layer->hmax;
    LayerData out;
    if (dtStatusSucceed(dtBuildTileCacheLayer(&compressor,
                                              &header,
                                              layer->heights,
                                              layer->areas,
                                              layer->cons,
                                              &out.data,
                                              &out.size)))
    {
      input.layers.push_back(out);
    }
  }
  rcFreeHeightfieldLayerSet(lset);
}

void run_tiles(LN_JobPool *pool,
               std::vector<TileInput> &inputs,
               const std::function<void(TileInput &)> &rasterize)
{
  if (pool != nullptr && pool->GetHelperCount() > 0 && inputs.size() > 1) {
    pool->Run(inputs.size(),
              [&](const size_t i, uint32_t /*thread*/) { rasterize(inputs[i]); });
  }
  else {
    for (TileInput &input : inputs) {
      rasterize(input);
    }
  }
}

/** Removes a tile from the cache and the navmesh (its layers handed back when `r_layers`). */
void remove_tile(SizeClass &size,
                 const TileKey &key,
                 std::vector<std::vector<unsigned char>> *r_layers)
{
  dtCompressedTileRef refs[kMaxLayers];
  const int n = size.cache->getTilesAt(key.first, key.second, refs, kMaxLayers);
  for (int i = 0; i < n; i++) {
    if (r_layers != nullptr) {
      const dtCompressedTile *tile = size.cache->getTileByRef(refs[i]);
      if (tile != nullptr && tile->data != nullptr) {
        r_layers->emplace_back(tile->data, tile->data + tile->dataSize);
      }
    }
    size.cache->removeTile(refs[i], nullptr, nullptr);
  }
  dtNavMesh *mesh = size.navmesh->Mesh();
  const dtMeshTile *tiles[kMaxLayers];
  const int nt = mesh->getTilesAt(key.first, key.second, tiles, kMaxLayers);
  std::vector<dtTileRef> navrefs;
  for (int i = 0; i < nt; i++) {
    navrefs.push_back(mesh->getTileRef(tiles[i]));
  }
  for (const dtTileRef ref : navrefs) {
    mesh->removeTile(ref, nullptr, nullptr);
  }
  size.tiles.erase(key);
}

/** Puts layers into the cache and makes the tile's navmesh tiles (main thread). */
bool add_tile(KX_NavBuildState &state, SizeClass &size, const TileKey &key, std::vector<LayerData> &layers)
{
  bool any = false;
  for (LayerData &layer : layers) {
    dtCompressedTileRef ref = 0;
    if (dtStatusSucceed(
            size.cache->addTile(layer.data, layer.size, DT_COMPRESSEDTILE_FREE_DATA, &ref)))
    {
      any = true;
    }
    else {
      dtFree(layer.data);
    }
    layer.data = nullptr;
  }
  layers.clear();
  size.tiles.insert(key);
  if (any) {
    size.cache->buildNavMeshTilesAt(key.first, key.second, size.navmesh->Mesh());
  }
  (void)state;
  return any;
}

/** An obstacle into the tile cache: its box widened by the class's agent radius on the plane
 * (2026-11-19). Detour's tile cache marks only the box itself, after the walkable area was
 * shrunk by the radius for walls: unwidened, an NPC's middle could reach the box's side and its
 * body overlapped the obstacle (only the avoidance's circle kept it off, and not at corners).
 * A round one (2026-11-20, plan item 5) is Detour's cylinder, its radius widened the same way:
 * the corners of the box round it stay walkable. */
dtStatus add_box_obstacle(SizeClass &size,
                          const MT_Vector3 &center_game,
                          const MT_Vector3 &half_game,
                          const float yaw,
                          const bool round,
                          dtObstacleRef *r_ref)
{
  float center[3], half[3];
  to_nav(center_game, center);
  if (round) {
    /* (The cylinder stands on its position: its bottom.) */
    float bottom[3];
    to_nav(center_game - MT_Vector3(0.0f, 0.0f, half_game[2]), bottom);
    return size.cache->addObstacle(bottom,
                                   float(half_game[0]) + size.radius,
                                   std::max(float(half_game[2]) * 2.0f, 0.01f),
                                   r_ref);
  }
  half[0] = float(half_game[0]) + size.radius;
  half[1] = float(half_game[2]);
  half[2] = float(half_game[1]) + size.radius;
  /* The game turns counter-clockwise from x toward y; Recast's y axis turns x toward -z. */
  return size.cache->addBoxObstacle(center, half, -yaw, r_ref);
}

/** Obstacles touching `changed` tiles are cut out again (the tile cache cuts an obstacle only
 * out of the tiles that existed when it was added). */
void recarve_obstacles(KX_NavBuildState &state, SizeClass &size, const std::set<TileKey> &changed)
{
  if (changed.empty()) {
    return;
  }
  for (auto &[object, obstacle] : size.obstacles) {
    const float reach = float(std::max(obstacle.half[0], obstacle.half[1])) +
                        size.radius + state.tile_width * 0.05f;
    const int x0 = int(std::floor((obstacle.center[0] - reach) / state.tile_width));
    const int x1 = int(std::floor((obstacle.center[0] + reach) / state.tile_width));
    const int y0 = int(std::floor((obstacle.center[1] - reach) / state.tile_width));
    const int y1 = int(std::floor((obstacle.center[1] + reach) / state.tile_width));
    bool touches = false;
    for (int y = y0; y <= y1 && !touches; y++) {
      for (int x = x0; x <= x1 && !touches; x++) {
        touches = changed.count({x, y}) != 0;
      }
    }
    if (!touches || obstacle.ref == 0) {
      continue;
    }
    size.cache->removeObstacle(obstacle.ref);
    obstacle.ref = 0;
    add_box_obstacle(
        size, obstacle.center, obstacle.half, obstacle.yaw, obstacle.round, &obstacle.ref);
  }
}

/** Runs the tile cache until every obstacle change is in the navmesh. */
bool settle_obstacles(SizeClass &size)
{
  bool changed = false;
  for (int guard = 0; guard < 100000; guard++) {
    bool up_to_date = false;
    if (dtStatusFailed(size.cache->update(0.0f, size.navmesh->Mesh(), &up_to_date))) {
      break;
    }
    if (up_to_date) {
      break;
    }
    changed = true;
  }
  return changed;
}

SizeClass *make_class(KX_NavBuildState &state, const ClassKey &key)
{
  std::unique_ptr<SizeClass> size = std::make_unique<SizeClass>();
  size->key = key;
  size->radius_cells = key.radius_cells;
  size->radius = float(key.radius_cells) * state.settings.cell_size;
  /* The scene's climb and slope themselves when the class keeps them (no rounding). */
  const int scene_climb = int(std::floor(state.settings.agent_climb / state.settings.cell_height));
  size->climb = key.climb_cells == scene_climb ?
                    state.settings.agent_climb :
                    float(key.climb_cells) * state.settings.cell_height;
  const int scene_slope = int(std::lround(state.settings.agent_max_slope * 57.2957795f));
  size->max_slope = key.slope_degrees == scene_slope ? state.settings.agent_max_slope :
                                                       float(key.slope_degrees) / 57.2957795f;
  const bool around = state.settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS;

  dtTileCacheParams tcp;
  memset(&tcp, 0, sizeof(tcp));
  tcp.orig[0] = tcp.orig[1] = tcp.orig[2] = 0.0f;
  tcp.cs = state.settings.cell_size;
  tcp.ch = state.settings.cell_height;
  tcp.width = kTileCells;
  tcp.height = kTileCells;
  tcp.walkableHeight = state.settings.agent_height;
  tcp.walkableRadius = size->radius;
  tcp.walkableClimb = size->climb;
  tcp.maxSimplificationError = state.settings.edge_max_error;
  tcp.maxTiles = around ? (1 << 16) : (1 << 18);
  tcp.maxObstacles = kMaxObstacles;
  size->cache = dtAllocTileCache();
  if (size->cache == nullptr ||
      dtStatusFailed(size->cache->init(&tcp, &state.alloc, &state.compressor, &state.process)))
  {
    CM_Error("navigation: could not make the tile cache of the scene's navmesh");
    return nullptr;
  }
  dtNavMesh *mesh = dtAllocNavMesh();
  dtNavMeshParams params;
  memset(&params, 0, sizeof(params));
  params.tileWidth = state.tile_width;
  params.tileHeight = state.tile_width;
  params.maxTiles = around ? (1 << 16) : (1 << 18);
  params.maxPolys = 1 << 16;
  if (mesh == nullptr || dtStatusFailed(mesh->init(&params))) {
    dtFreeNavMesh(mesh);
    CM_Error("navigation: could not make the scene's navmesh");
    return nullptr;
  }
  size->navmesh = KX_NavMesh::FromDetour(mesh);
  SizeClass *raw = size.get();
  state.classes.push_back(std::move(size));
  state.stats.size_classes = state.classes.size();
  return raw;
}

/** Builds every tile of the level (Whole Level, at the class's first update). */
void build_level(KX_NavBuildState &state, SizeClass &size, LN_JobPool *pool)
{
  size.level_built = true;
  KX_NavGeometry geometry;
  state.source(MT_Vector3(-kLevelReach, -kLevelReach, -kLevelReach),
               MT_Vector3(kLevelReach, kLevelReach, kLevelReach),
               geometry);
  const int ntris = int(geometry.tris.size() / 3);
  if (ntris == 0) {
    return;
  }
  /* Each triangle goes to every tile its box (and the border) meets. */
  const rcConfig cfg = tile_config(state.settings, size);
  const float pad = float(cfg.borderSize) * cfg.cs;
  std::map<TileKey, std::vector<int>> buckets;
  for (int t = 0; t < ntris; t++) {
    float minx = FLT_MAX, maxx = -FLT_MAX, miny = FLT_MAX, maxy = -FLT_MAX;
    for (int k = 0; k < 3; k++) {
      const float *p = &geometry.verts[size_t(geometry.tris[size_t(t) * 3 + k]) * 3];
      minx = std::min(minx, p[0]);
      maxx = std::max(maxx, p[0]);
      miny = std::min(miny, p[1]);
      maxy = std::max(maxy, p[1]);
    }
    const int x0 = int(std::floor((minx - pad) / state.tile_width));
    const int x1 = int(std::floor((maxx + pad) / state.tile_width));
    const int y0 = int(std::floor((miny - pad) / state.tile_width));
    const int y1 = int(std::floor((maxy + pad) / state.tile_width));
    if (int64_t(x1 - x0 + 1) * int64_t(y1 - y0 + 1) > 1 << 20) {
      continue; /* A giant triangle: no level is made of one. */
    }
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        buckets[{x, y}].push_back(t);
      }
    }
  }
  std::vector<TileInput> inputs;
  inputs.reserve(buckets.size());
  for (auto &[key, tris] : buckets) {
    TileInput input;
    input.key = key;
    input.ymin = FLT_MAX;
    input.ymax = -FLT_MAX;
    append_geometry(input, geometry, &tris);
    inputs.push_back(std::move(input));
  }
  run_tiles(pool, inputs, [&](TileInput &input) { rasterize_tile(state, size, input); });
  std::set<TileKey> changed;
  for (TileInput &input : inputs) {
    if (add_tile(state, size, input.key, input.layers)) {
      state.stats.tiles_built++;
    }
    changed.insert(input.key);
  }
  recarve_obstacles(state, size, changed);
  size.navmesh->NoteTilesChanged();
}

/** Voxelizes `keys` from the physics' shapes as they are now and puts them in (replacing what
 * was there). */
void build_tiles(KX_NavBuildState &state,
                 SizeClass &size,
                 const std::vector<TileKey> &keys,
                 LN_JobPool *pool,
                 std::set<TileKey> &r_changed)
{
  if (keys.empty()) {
    return;
  }
  std::vector<TileInput> inputs(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    TileInput &input = inputs[i];
    input.key = keys[i];
    input.ymin = FLT_MAX;
    input.ymax = -FLT_MAX;
    MT_Vector3 bmin, bmax;
    tile_box(state, size, keys[i], bmin, bmax);
    KX_NavGeometry geometry;
    state.source(bmin, bmax, geometry);
    append_geometry(input, geometry, nullptr);
  }
  run_tiles(pool, inputs, [&](TileInput &input) { rasterize_tile(state, size, input); });
  for (TileInput &input : inputs) {
    if (size.tiles.count(input.key)) {
      remove_tile(size, input.key, nullptr);
    }
    size.parked.erase(input.key);
    add_tile(state, size, input.key, input.layers);
    state.stats.tiles_built++;
    r_changed.insert(input.key);
  }
}

/** The place (the game's plane) an obstacle's carving changes: its box turned any way, widened
 * by the agents' radius. */
void obstacle_place(const SizeClass &size,
                    const MT_Vector3 &center,
                    const MT_Vector3 &half,
                    std::vector<KX_NavChangedPlace> &r_places)
{
  const float reach = float(std::max(half[0], half[1])) * 1.4143f + size.radius;
  KX_NavChangedPlace place;
  place.min[0] = float(center[0]) - reach;
  place.min[1] = float(center[1]) - reach;
  place.max[0] = float(center[0]) + reach;
  place.max[1] = float(center[1]) + reach;
  r_places.push_back(place);
}

void sync_obstacles(KX_NavBuildState &state,
                    SizeClass &size,
                    const std::vector<KX_NavCarveObstacle> &obstacles,
                    std::vector<KX_NavChangedPlace> &r_places)
{
  std::unordered_map<const KX_GameObject *, const KX_NavCarveObstacle *> wanted;
  for (const KX_NavCarveObstacle &o : obstacles) {
    wanted[o.object] = &o;
  }
  /* Gone or changed: out. */
  for (auto it = size.obstacles.begin(); it != size.obstacles.end();) {
    auto found = wanted.find(it->first);
    const bool same = found != wanted.end() &&
                      (found->second->center - it->second.center).length2() < 1e-6f &&
                      (found->second->half_size - it->second.half).length2() < 1e-6f &&
                      std::fabs(found->second->yaw - it->second.yaw) < 1e-4f &&
                      found->second->round == it->second.round;
    if (!same) {
      if (it->second.ref != 0) {
        size.cache->removeObstacle(it->second.ref);
        obstacle_place(size, it->second.center, it->second.half, r_places);
      }
      it = size.obstacles.erase(it);
    }
    else {
      ++it;
    }
  }
  /* New: in, in the given order (the scene's object order). */
  for (const KX_NavCarveObstacle &o : obstacles) {
    if (size.obstacles.count(o.object)) {
      continue;
    }
    ObstacleState obstacle;
    obstacle.center = o.center;
    obstacle.half = o.half_size;
    obstacle.yaw = o.yaw;
    obstacle.round = o.round;
    if (dtStatusFailed(
            add_box_obstacle(size, o.center, o.half_size, o.yaw, o.round, &obstacle.ref)))
    {
      if (!size.warned_obstacles) {
        size.warned_obstacles = true;
        CM_Warning("navigation: more than " << kMaxObstacles
                                            << " obstacles to carve; the rest are only avoided");
      }
      obstacle.ref = 0;
    }
    else {
      obstacle_place(size, o.center, o.half_size, r_places);
    }
    size.obstacles[o.object] = obstacle;
  }
  state.stats.obstacles_carved = size.obstacles.size();
}

/** Jump Links Automatic (2026-11-19): the tiles in `changed` and their neighbours searched again
 * (KX_NavFindJumpLinks on the helpers, each tile's level read here first), each tile's links
 * renumbered in tile order when they changed (the same ids on any thread count) and the tile
 * made again from its layers with them; those tiles join `changed`. */
void find_jump_links(KX_NavBuildState &state,
                     SizeClass &size,
                     std::set<TileKey> &changed,
                     LN_JobPool *pool)
{
  std::set<TileKey> search;
  for (const TileKey &key : changed) {
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        search.insert({key.first + dx, key.second + dy});
      }
    }
  }
  KX_NavJumpLinkSpec spec;
  spec.radius = size.radius;
  spec.height = state.settings.agent_height;
  spec.climb = size.climb;
  spec.gap = state.settings.jump_gap;
  spec.up = state.settings.jump_up;
  spec.drop = state.settings.jump_drop;
  spec.spacing = state.settings.jump_spacing;
  spec.tile_width = state.tile_width;
  struct Job {
    TileKey key;
    /* The level's triangles (indices into `geometry`) within reach of the tile. */
    std::vector<int> triangles;
    std::vector<KX_NavFoundLink> links;
  };
  std::vector<Job> jobs;
  std::map<TileKey, size_t> job_of;
  std::vector<TileKey> gone;
  const float reach = KX_NavJumpLinkReach(spec);
  MT_Vector3 lo(FLT_MAX, FLT_MAX, -kLevelReach), hi(-FLT_MAX, -FLT_MAX, kLevelReach);
  for (const TileKey &key : search) {
    if (!size.tiles.count(key)) {
      if (size.found_by_tile.count(key)) {
        gone.push_back(key);
      }
      continue;
    }
    job_of[key] = jobs.size();
    Job job;
    job.key = key;
    jobs.push_back(std::move(job));
    lo[0] = std::min(float(lo[0]), key.first * state.tile_width - reach);
    lo[1] = std::min(float(lo[1]), key.second * state.tile_width - reach);
    hi[0] = std::max(float(hi[0]), (key.first + 1) * state.tile_width + reach);
    hi[1] = std::max(float(hi[1]), (key.second + 1) * state.tile_width + reach);
  }
  /* The level read once for every tile searched (on the main thread: the source reads the
   * scene), each triangle handed to the tiles it is within reach of (2026-11-19: read for each
   * tile, the level build's 196 tiles took 11 ms more on the main thread). */
  KX_NavGeometry geometry;
  if (!jobs.empty()) {
    state.source(lo, hi, geometry);
  }
  const int ntris = int(geometry.tris.size() / 3);
  for (int t = 0; t < ntris; t++) {
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
    for (int k = 0; k < 3; k++) {
      const float *p = &geometry.verts[size_t(geometry.tris[size_t(t) * 3 + k]) * 3];
      min_x = std::min(min_x, p[0]);
      max_x = std::max(max_x, p[0]);
      min_y = std::min(min_y, p[1]);
      max_y = std::max(max_y, p[1]);
    }
    const int x0 = int(std::floor((min_x - reach) / state.tile_width));
    const int x1 = int(std::floor((max_x + reach) / state.tile_width));
    const int y0 = int(std::floor((min_y - reach) / state.tile_width));
    const int y1 = int(std::floor((max_y + reach) / state.tile_width));
    if (int64_t(x1 - x0 + 1) * int64_t(y1 - y0 + 1) > int64_t(job_of.size())) {
      /* A huge triangle: every job looks (cheaper than every tile it spans). */
      for (Job &job : jobs) {
        if (job.key.first >= x0 && job.key.first <= x1 && job.key.second >= y0 &&
            job.key.second <= y1)
        {
          job.triangles.push_back(t);
        }
      }
      continue;
    }
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        auto it = job_of.find(TileKey(x, y));
        if (it != job_of.end()) {
          jobs[it->second].triangles.push_back(t);
        }
      }
    }
  }
  const dtNavMesh &mesh = *size.navmesh->Mesh();
  auto search_tile = [&](Job &job) {
    dtNavMeshQuery *query = dtAllocNavMeshQuery();
    if (query != nullptr && dtStatusSucceed(query->init(&mesh, 1024))) {
      KX_NavFindJumpLinks(mesh,
                          *query,
                          job.key.first,
                          job.key.second,
                          spec,
                          geometry,
                          &job.triangles,
                          job.links);
    }
    dtFreeNavMeshQuery(query);
  };
  if (pool != nullptr && pool->GetHelperCount() > 0 && jobs.size() > 1) {
    pool->Run(jobs.size(), [&](const size_t i, uint32_t /*thread*/) { search_tile(jobs[i]); });
  }
  else {
    for (Job &job : jobs) {
      search_tile(job);
    }
  }
  state.stats.tiles_jump_searched += jobs.size();
  bool any = false;
  auto forget = [&](const TileKey &key) {
    auto it = size.found_by_tile.find(key);
    if (it == size.found_by_tile.end()) {
      return;
    }
    for (const uint32_t id : it->second) {
      state.found_links.erase(id);
    }
    size.found_by_tile.erase(it);
    any = true;
  };
  for (const TileKey &key : gone) {
    forget(key);
  }
  std::vector<TileKey> remake;
  for (const Job &job : jobs) {
    /* The same links as before: nothing to do (their ids kept). */
    auto old = size.found_by_tile.find(job.key);
    bool same = old != size.found_by_tile.end() ? old->second.size() == job.links.size() :
                                                   job.links.empty();
    for (size_t i = 0; same && i < job.links.size(); i++) {
      const KX_NavLinkInfo &info = state.found_links.at(old->second[i]);
      same = (info.start - job.links[i].start).length2() < 1e-6f &&
             (info.end - job.links[i].end).length2() < 1e-6f && info.drop == job.links[i].drop;
    }
    if (same) {
      continue;
    }
    forget(job.key);
    std::vector<uint32_t> &ids = size.found_by_tile[job.key];
    for (const KX_NavFoundLink &link : job.links) {
      KX_NavLinkInfo info;
      info.start = link.start;
      info.end = link.end;
      info.drop = link.drop;
      info.gap = link.gap;
      const uint32_t id = state.next_found_id++;
      state.found_links.emplace(id, info);
      ids.push_back(id);
    }
    if (ids.empty()) {
      size.found_by_tile.erase(job.key);
    }
    any = true;
    remake.push_back(job.key);
  }
  if (!any) {
    return;
  }
  /* The remade tiles' links as Detour takes them, in id order. */
  const float link_radius = std::clamp(size.radius, 0.2f, 0.6f);
  for (const TileKey &key : remake) {
    size.found.erase(key);
    auto it = size.found_by_tile.find(key);
    if (it == size.found_by_tile.end()) {
      continue;
    }
    LinkArrays &arrays = size.found[key];
    for (const uint32_t id : it->second) {
      const KX_NavLinkInfo &info = state.found_links.at(id);
      float a[3], b[3];
      to_nav(info.start, a);
      to_nav(info.end, b);
      arrays.add(a, b, link_radius, 0, info.drop ? state.drop_area : state.jump_area, id);
    }
  }
  for (const TileKey &key : gone) {
    size.found.erase(key);
  }
  for (const TileKey &key : remake) {
    if (size.tiles.count(key)) {
      size.cache->buildNavMeshTilesAt(key.first, key.second, size.navmesh->Mesh());
      changed.insert(key);
    }
  }
  state.stats.found_links = state.found_links.size();
}

}  // namespace

/** \} */

/* -------------------------------------------------------------------- */
/** \name The interface
 * \{ */

KX_NavBuildState *KX_NavBuildCreate(const KX_Navigation::Settings &settings,
                                     KX_NavGeometrySource source)
{
  KX_NavBuildState *state = new KX_NavBuildState();
  state->settings = settings;
  state->source = std::move(source);
  state->tile_width = float(kTileCells) * settings.cell_size;
  return state;
}

void KX_NavBuildFree(KX_NavBuildState *state)
{
  delete state;
}

void KX_NavBuildForEachNavMesh(const KX_NavBuildState &state,
                               const std::function<void(const KX_NavMesh &)> &navmesh)
{
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    navmesh(*size->navmesh);
  }
}

static ClassKey key_of(const KX_NavBuildState &state, const KX_NavAgentSize &agent)
{
  const KX_Navigation::Settings &s = state.settings;
  ClassKey key;
  const float r = agent.radius > 0.0f ? agent.radius : s.agent_radius;
  key.radius_cells = std::max(1, int(std::ceil(r / s.cell_size - 1e-4f)));
  /* Never more than the scene allows: its settings are the level's own limits. */
  const float climb = agent.climb > 0.0f ? std::min(agent.climb, s.agent_climb) : s.agent_climb;
  key.climb_cells = std::max(0, int(std::floor(climb / s.cell_height + 1e-4f)));
  const float slope = agent.max_slope > 0.0f ? std::min(agent.max_slope, s.agent_max_slope) :
                                               s.agent_max_slope;
  key.slope_degrees = int(std::lround(slope * 57.2957795f));
  return key;
}

static SizeClass *class_of(KX_NavBuildState &state, const ClassKey &key, const bool make)
{
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->key == key) {
      return size.get();
    }
  }
  if (make && int(state.classes.size()) < kMaxSizeClasses) {
    return make_class(state, key);
  }
  /* Out of classes: the nearest one the agent surely walks (as large or larger, climbing and
   * sloping no more), else the nearest larger one, else the largest. */
  SizeClass *best = nullptr;
  auto cost = [&](const SizeClass &size) {
    return (size.key.radius_cells - key.radius_cells) * 1000000 +
           (key.climb_cells - size.key.climb_cells) * 1000 +
           (key.slope_degrees - size.key.slope_degrees);
  };
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->key.radius_cells >= key.radius_cells && size->key.climb_cells <= key.climb_cells &&
        size->key.slope_degrees <= key.slope_degrees && (!best || cost(*size) < cost(*best)))
    {
      best = size.get();
    }
  }
  if (!best) {
    for (const std::unique_ptr<SizeClass> &size : state.classes) {
      if (size->key.radius_cells >= key.radius_cells &&
          (!best || size->key.radius_cells < best->key.radius_cells))
      {
        best = size.get();
      }
    }
  }
  if (!best) {
    for (const std::unique_ptr<SizeClass> &size : state.classes) {
      if (!best || size->key.radius_cells > best->key.radius_cells) {
        best = size.get();
      }
    }
  }
  return best;
}

KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, const KX_NavAgentSize &agent)
{
  if (state.settings.build_mode == blender::NAVMESH_BUILD_OFF) {
    return nullptr;
  }
  SizeClass *size = class_of(state, key_of(state, agent), true);
  return size ? size->navmesh.get() : nullptr;
}

bool KX_NavBuildHasOwnClass(const KX_NavBuildState &state, const KX_NavAgentSize &agent)
{
  const ClassKey key = key_of(state, agent);
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->key == key) {
      return true;
    }
  }
  return false;
}

KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, const float agent_radius)
{
  KX_NavAgentSize agent;
  agent.radius = agent_radius;
  return KX_NavBuildNavMesh(state, agent);
}

const KX_NavMesh *KX_NavBuildFindNavMesh(const KX_NavBuildState &state, const float agent_radius)
{
  KX_NavAgentSize agent;
  agent.radius = agent_radius;
  const ClassKey key = key_of(state, agent);
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->key == key) {
      return size->navmesh.get();
    }
  }
  return nullptr;
}

void KX_NavBuildMarkDirty(KX_NavBuildState &state,
                          const MT_Vector3 &aabb_min,
                          const MT_Vector3 &aabb_max)
{
  /* The border: a change shrinks or grows the walkable area up to an agent's radius away. */
  const float pad = state.tile_width * 0.25f;
  const int x0 = int(std::floor((aabb_min[0] - pad) / state.tile_width));
  const int x1 = int(std::floor((aabb_max[0] + pad) / state.tile_width));
  const int y0 = int(std::floor((aabb_min[1] - pad) / state.tile_width));
  const int y1 = int(std::floor((aabb_max[1] + pad) / state.tile_width));
  if (int64_t(x1 - x0 + 1) * int64_t(y1 - y0 + 1) > 1 << 16) {
    return;
  }
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      state.dirty.insert({x, y});
    }
  }
}

void KX_NavBuildRelinkAt(KX_NavBuildState &state, const MT_Vector3 &point)
{
  if (!std::isfinite(float(point[0])) || !std::isfinite(float(point[1]))) {
    return;
  }
  state.relink.insert({int(std::floor(float(point[0]) / state.tile_width)),
                       int(std::floor(float(point[1]) / state.tile_width))});
}

bool KX_NavBuildLinkFits(const KX_NavBuildState &state,
                         const MT_Vector3 &start,
                         const MT_Vector3 &end)
{
  const float w = state.tile_width;
  const int sx = int(std::floor(float(start[0]) / w)), sy = int(std::floor(float(start[1]) / w));
  const int ex = int(std::floor(float(end[0]) / w)), ey = int(std::floor(float(end[1]) / w));
  return std::abs(sx - ex) <= 1 && std::abs(sy - ey) <= 1;
}

void KX_NavBuildNoteWaysChanged(KX_NavBuildState &state,
                                const std::vector<KX_NavChangedPlace> &places)
{
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->navmesh) {
      size->navmesh->NoteTilesChanged(&places);
    }
  }
}

KX_NavBuildStats KX_NavBuildGetStats(const KX_NavBuildState &state)
{
  KX_NavBuildStats stats = state.stats;
  stats.tiles_alive = 0;
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    stats.tiles_alive += size->tiles.size();
  }
  return stats;
}

void KX_NavBuildUpdate(KX_NavBuildState &state,
                       const std::vector<MT_Vector3> &agent_positions,
                       const std::vector<KX_NavAgentSize> &agent_sizes,
                       const std::vector<uint64_t> &agent_ids,
                       const std::vector<KX_NavCarveObstacle> &obstacles,
                       const std::vector<KX_NavLink> &links,
                       LN_JobPool *pool)
{
  if (state.settings.build_mode == blender::NAVMESH_BUILD_OFF) {
    state.dirty.clear();
    state.relink.clear();
    return;
  }
  /* Links in Recast's space (each tile takes those starting in it). A link's own number
   * (KX_NavLink::user_id): the same while it lives, whatever the list's order (links come and go
   * while playing). A jump both ways is two links (2026-11-19: each way's needs, judged by each
   * NPC's jump limits), its way back's number with kNavLinkReverse; jumps in the "jump" or
   * "drop" area (priced and closed per NPC). */
  MeshProcess &process = state.process;
  process.links.clear();
  for (size_t i = 0; i < links.size(); i++) {
    float a[3], b[3];
    to_nav(links[i].start, a);
    to_nav(links[i].end, b);
    const float radius = std::max(links[i].radius, 0.05f);
    const uint32_t id = links[i].user_id != 0 ? links[i].user_id : uint32_t(i + 1);
    if (!KX_NavLinkKindIsJump(links[i].kind)) {
      process.links.add(a, b, radius, links[i].both_ways ? DT_OFFMESH_CON_BIDIR : 0, 0, id);
      continue;
    }
    const bool down = links[i].end[2] < links[i].start[2];
    process.links.add(a, b, radius, 0, down ? state.drop_area : state.jump_area, id);
    if (links[i].both_ways) {
      process.links.add(
          b, a, radius, 0, down ? state.jump_area : state.drop_area, id | kNavLinkReverse);
    }
  }

  const bool around = state.settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS;
  for (const std::unique_ptr<SizeClass> &holder : state.classes) {
    SizeClass &size = *holder;
    /* Its tiles are made with its own found links. */
    process.found = &size.found;
    std::set<TileKey> changed;
    std::vector<KX_NavChangedPlace> places;
    if (!around && !size.level_built) {
      build_level(state, size, pool);
      for (const TileKey &key : size.tiles) {
        changed.insert(key);
      }
    }
    if (around) {
      /* The tiles wanted (within the build radius of an agent of this class) and kept (within
       * the remove radius), counted as agents move (2026-11-19). An agent's place is its cell
       * (a quarter tile): its tiles change only when it enters another cell, so a step costs a
       * lookup an agent plus the few that moved on. (Recounting every agent's tiles every step
       * cost 1,000 spread NPCs 3 ms a step, the tiles themselves nothing.) Agents in their order,
       * those gone in their numbers' order: the same on any thread count. */
      const float build = state.settings.build_radius, remove = state.settings.remove_radius;
      const float cell = state.tile_width * 0.25f;
      std::vector<TileKey> unkept;
      auto cover = [&](const int cx, const int cy, const int delta) {
        const float px = (float(cx) + 0.5f) * cell, py = (float(cy) + 0.5f) * cell;
        const int x0 = int(std::floor((px - remove) / state.tile_width));
        const int x1 = int(std::floor((px + remove) / state.tile_width));
        const int y0 = int(std::floor((py - remove) / state.tile_width));
        const int y1 = int(std::floor((py + remove) / state.tile_width));
        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            /* The tile's nearest point to the cell's middle. */
            const float nx = std::clamp(px, x * state.tile_width, (x + 1) * state.tile_width);
            const float ny = std::clamp(py, y * state.tile_width, (y + 1) * state.tile_width);
            const float d = std::hypot(nx - px, ny - py);
            if (d > remove) {
              continue;
            }
            const TileKey key{x, y};
            int &keep = size.keep_count[key];
            keep += delta;
            if (keep <= 0) {
              size.keep_count.erase(key);
              unkept.push_back(key);
            }
            if (d > build) {
              continue;
            }
            int &want = size.want_count[key];
            want += delta;
            if (want <= 0) {
              size.want_count.erase(key);
              size.pending.erase(key);
            }
            else if (delta > 0 && !size.tiles.count(key)) {
              auto it = size.pending.find(key);
              if (it == size.pending.end()) {
                size.pending.emplace(key, d);
              }
              else {
                it->second = std::min(it->second, d);
              }
            }
          }
        }
      };
      size.cover_serial++;
      for (size_t a = 0; a < agent_positions.size(); a++) {
        const KX_NavAgentSize agent = a < agent_sizes.size() ? agent_sizes[a] : KX_NavAgentSize();
        if (class_of(state, key_of(state, agent), false) != &size) {
          continue;
        }
        const uint64_t id = a < agent_ids.size() ? agent_ids[a] : uint64_t(a);
        const MT_Vector3 &p = agent_positions[a];
        if (!std::isfinite(float(p[0])) || !std::isfinite(float(p[1]))) {
          continue;
        }
        const int cx = int(std::floor(float(p[0]) / cell));
        const int cy = int(std::floor(float(p[1]) / cell));
        auto [it, inserted] = size.covers.try_emplace(id);
        SizeClass::Cover &c = it->second;
        if (inserted || c.cx != cx || c.cy != cy) {
          if (!inserted) {
            cover(c.cx, c.cy, -1);
          }
          cover(cx, cy, +1);
          c.cx = cx;
          c.cy = cy;
        }
        c.seen = size.cover_serial;
      }
      std::vector<uint64_t> gone;
      for (const auto &[id, c] : size.covers) {
        if (c.seen != size.cover_serial) {
          gone.push_back(id);
        }
      }
      std::sort(gone.begin(), gone.end());
      for (const uint64_t id : gone) {
        const SizeClass::Cover c = size.covers[id];
        cover(c.cx, c.cy, -1);
        size.covers.erase(id);
      }
      /* Gone too far from every agent: out, its layers kept. */
      std::sort(unkept.begin(), unkept.end());
      unkept.erase(std::unique(unkept.begin(), unkept.end()), unkept.end());
      std::vector<TileKey> drop;
      for (const TileKey &key : unkept) {
        if (!size.keep_count.count(key) && size.tiles.count(key)) {
          drop.push_back(key);
        }
      }
      for (const TileKey &key : drop) {
        std::vector<std::vector<unsigned char>> layers;
        remove_tile(size, key, &layers);
        state.stats.tiles_removed++;
        if (!layers.empty()) {
          size.parked[key] = std::move(layers);
          size.parked_order.push_back(key);
          while (size.parked.size() > kMaxParkedTiles && !size.parked_order.empty()) {
            size.parked.erase(size.parked_order.front());
            size.parked_order.pop_front();
          }
        }
        changed.insert(key);
      }
      /* Wanted and not there: the nearest first (where it was first wanted from). */
      std::vector<std::pair<float, TileKey>> missing;
      for (auto it = size.pending.begin(); it != size.pending.end();) {
        if (size.tiles.count(it->first)) {
          it = size.pending.erase(it);
          continue;
        }
        missing.emplace_back(it->second, it->first);
        ++it;
      }
      std::sort(missing.begin(), missing.end());
      std::vector<TileKey> to_build;
      /* The first step makes all it needs (the NPCs stand on it at once); then a few a step. */
      const size_t budget = size.started ? size_t(kTilesPerStep) : missing.size();
      for (const auto &[d, key] : missing) {
        auto parked = size.parked.find(key);
        if (parked != size.parked.end()) {
          /* Back without voxelizing. */
          std::vector<LayerData> layers;
          for (std::vector<unsigned char> &bytes : parked->second) {
            LayerData layer;
            layer.size = int(bytes.size());
            layer.data = (unsigned char *)dtAlloc(bytes.size(), DT_ALLOC_PERM);
            memcpy(layer.data, bytes.data(), bytes.size());
            layers.push_back(layer);
          }
          size.parked.erase(parked);
          add_tile(state, size, key, layers);
          state.stats.tiles_restored++;
          changed.insert(key);
          continue;
        }
        if (to_build.size() < budget) {
          to_build.push_back(key);
        }
      }
      build_tiles(state, size, to_build, pool, changed);
      size.started = true;
    }
    /* Level pieces changed: their tiles again. */
    if (!state.dirty.empty()) {
      std::vector<TileKey> again;
      for (const TileKey &key : state.dirty) {
        if (size.tiles.count(key) || !around) {
          again.push_back(key);
        }
        if (around) {
          size.parked.erase(key);
        }
      }
      build_tiles(state, size, again, pool, changed);
    }
    /* Jump Links Automatic: the tiles made or dropped and their neighbours are searched again
     * (a jump may land in a new tile); tiles whose links changed are made again from their
     * layers. */
    if (state.settings.jump_links && !changed.empty()) {
      find_jump_links(state, size, changed, pool);
    }
    /* Links came, went or moved: the tiles their starts lie in, from their layers (the tile
     * cache makes them with the links handed over above and its obstacles). */
    for (const TileKey &key : state.relink) {
      if (changed.count(key) || !size.tiles.count(key)) {
        continue;
      }
      if (dtStatusSucceed(
              size.cache->buildNavMeshTilesAt(key.first, key.second, size.navmesh->Mesh())))
      {
        state.stats.tiles_relinked++;
      }
      changed.insert(key);
    }
    sync_obstacles(state, size, obstacles, places);
    recarve_obstacles(state, size, changed);
    const bool carved = settle_obstacles(size);
    if (!changed.empty() || carved) {
      for (const TileKey &key : changed) {
        KX_NavChangedPlace place;
        place.min[0] = float(key.first) * state.tile_width;
        place.min[1] = float(key.second) * state.tile_width;
        place.max[0] = float(key.first + 1) * state.tile_width;
        place.max[1] = float(key.second + 1) * state.tile_width;
        places.push_back(place);
      }
      size.navmesh->NoteTilesChanged(&places);
    }
  }
  process.found = nullptr;
  state.dirty.clear();
  state.relink.clear();
}

void KX_NavBuildSetLinkAreas(KX_NavBuildState &state,
                             const uint8_t jump_area,
                             const uint8_t drop_area)
{
  state.jump_area = jump_area;
  state.drop_area = drop_area;
}

const KX_NavLinkInfo *KX_NavBuildFindLink(const KX_NavBuildState &state, const uint32_t id)
{
  auto it = state.found_links.find(id);
  return it == state.found_links.end() ? nullptr : &it->second;
}

void KX_NavBuildForEachFoundLink(
    const KX_NavBuildState &state,
    const std::function<void(uint32_t id, const KX_NavLinkInfo &link)> &link)
{
  std::vector<uint32_t> ids;
  ids.reserve(state.found_links.size());
  for (const auto &[id, info] : state.found_links) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  for (const uint32_t id : ids) {
    link(id, state.found_links.at(id));
  }
}

bool KX_NavLinkKindIsJump(const std::string &kind)
{
  return kind == "jump" || kind == "drop";
}

size_t KX_NavBuildCountUnjoinedLinks(const KX_NavBuildState &state)
{
  size_t unjoined = 0;
  for (const std::unique_ptr<SizeClass> &holder : state.classes) {
    if (!holder->navmesh) {
      continue;
    }
    const dtNavMesh &mesh = *holder->navmesh->Mesh();
    for (int t = 0; t < mesh.getMaxTiles(); t++) {
      const dtMeshTile *tile = mesh.getTile(t);
      if (tile == nullptr || tile->header == nullptr) {
        continue;
      }
      for (int i = 0; i < tile->header->offMeshConCount; i++) {
        const dtOffMeshConnection &con = tile->offMeshCons[i];
        const dtPoly &poly = tile->polys[con.poly];
        bool start = false, end = false;
        for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
          start = start || tile->links[l].edge == 0;
          end = end || tile->links[l].edge == 1;
        }
        /* (A start on no polygon joins nothing; an end over a tile not made yet waits.) */
        int tx = 0, ty = 0;
        mesh.calcTileLoc(&con.pos[3], &tx, &ty);
        const dtMeshTile *there[4];
        if (start && !end && mesh.getTilesAt(tx, ty, there, 4) > 0) {
          unjoined++;
        }
      }
    }
  }
  return unjoined;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The scene's level and links
 * \{ */

void KX_NavCollectSceneGeometry(KX_Scene *scene,
                                const MT_Vector3 &aabb_min,
                                const MT_Vector3 &aabb_max,
                                const std::function<int(const std::string &)> &area_id,
                                const std::function<bool(const KX_GameObject *)> &is_obstacle,
                                KX_NavGeometry &r_geometry)
{
  KX_NavCollectPhysicsGeometry(scene ? scene->GetPhysicsEnvironment() : nullptr,
                               aabb_min,
                               aabb_max,
                               area_id,
                               is_obstacle,
                               r_geometry);
}

void KX_NavCollectPhysicsGeometry(PHY_IPhysicsEnvironment *physics,
                                  const MT_Vector3 &aabb_min,
                                  const MT_Vector3 &aabb_max,
                                  const std::function<int(const std::string &)> &area_id,
                                  const std::function<bool(const KX_GameObject *)> &is_obstacle,
                                  KX_NavGeometry &r_geometry)
{
  if (physics == nullptr) {
    return;
  }
  std::unordered_map<const KX_GameObject *, int> areas;
  physics->CollectStaticTriangles(
      aabb_min,
      aabb_max,
      [&](KX_GameObject *object,
          const int solid_piece,
          const MT_Vector3 &a,
          const MT_Vector3 &b,
          const MT_Vector3 &c) {
        int area = 0;
        if (object != nullptr) {
          auto it = areas.find(object);
          if (it == areas.end()) {
            int id = 0;
            if (is_obstacle(object)) {
              id = -1;
            }
            else if (!object->GetNavSettings().area.empty()) {
              id = area_id(object->GetNavSettings().area);
            }
            it = areas.emplace(object, id).first;
          }
          area = it->second;
        }
        if (area < 0) {
          return;
        }
        const int base = int(r_geometry.verts.size() / 3);
        for (const MT_Vector3 *v : {&a, &b, &c}) {
          r_geometry.verts.push_back(float((*v)[0]));
          r_geometry.verts.push_back(float((*v)[1]));
          r_geometry.verts.push_back(float((*v)[2]));
        }
        /* Recast wants walkable faces wound clockwise seen from above (its up is +y, the turn
         * flips the handedness): the game's counter-clockwise faces, reversed. */
        r_geometry.tris.push_back(base);
        r_geometry.tris.push_back(base + 2);
        r_geometry.tris.push_back(base + 1);
        r_geometry.areas.push_back(uint8_t(area));
        r_geometry.pieces.push_back(solid_piece);
      });
}

std::vector<KX_NavLink> KX_NavCollectLinks(KX_Scene *scene)
{
  std::vector<KX_NavLink> links;
  if (scene == nullptr) {
    return links;
  }
  /* (2026-11-20: Physics > Navigation > Link; the end an object picked, emptied when that object
   * goes. Until then the game properties nav_link & co.) */
  for (KX_GameObject *object : *scene->GetObjectList()) {
    KX_GameObject *end = object->GetNavLinkEnd();
    if (end == nullptr || end == object) {
      continue;
    }
    const KX_NavObjectSettings &settings = object->GetNavSettings();
    KX_NavLink link;
    link.object = object;
    link.end_object = end;
    link.start = object->NodeGetWorldPosition();
    link.end = end->NodeGetWorldPosition();
    link.seen_start = link.start;
    link.seen_end = link.end;
    link.kind = settings.link_kind;
    link.both_ways = settings.link_both_ways;
    link.radius = std::max(settings.link_radius, 0.05f);
    link.reach = std::max(settings.link_reach, 0.0f);
    link.duration = std::max(settings.link_duration, 0.0f);
    link.enabled = settings.link_enabled;
    link.wait = settings.link_wait;
    link.wait_time = std::max(settings.link_wait_time, 0.0f);
    link.user_id = uint32_t(links.size() + 1);
    links.push_back(link);
  }
  return links;
}

/** \} */

