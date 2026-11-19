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
/** Agent sizes that get their own navmesh; more sizes share the next larger one. */
constexpr int kMaxSizeClasses = 4;
constexpr int kMaxObstacles = 4096;
/** Around NPCs: dropped tiles kept (oldest forgotten first). */
constexpr size_t kMaxParkedTiles = 4096;
/** The whole level is found inside this box. */
constexpr float kLevelReach = 1.0e6f;

using TileKey = std::pair<int, int>;

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

/** Areas and flags of each new navmesh polygon, and the links. */
struct MeshProcess : public dtTileCacheMeshProcess {
  std::vector<float> link_verts;
  std::vector<float> link_radii;
  std::vector<unsigned char> link_dirs;
  std::vector<unsigned char> link_areas;
  std::vector<unsigned short> link_flags;
  std::vector<unsigned int> link_ids;

  void process(dtNavMeshCreateParams *params,
               unsigned char *areas,
               unsigned short *flags) override
  {
    for (int i = 0; i < params->polyCount; i++) {
      /* The build's 1 + id back to the id (ground 0). */
      areas[i] = areas[i] > 0 ? (unsigned char)(areas[i] - 1) : 0;
      flags[i] = 1;
    }
    if (!link_ids.empty()) {
      params->offMeshConVerts = link_verts.data();
      params->offMeshConRad = link_radii.data();
      params->offMeshConDir = link_dirs.data();
      params->offMeshConAreas = link_areas.data();
      params->offMeshConFlags = link_flags.data();
      params->offMeshConUserID = link_ids.data();
      params->offMeshConCount = int(link_ids.size());
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
};

struct SizeClass {
  int radius_cells = 1;
  float radius = 0.5f;
  dtTileCache *cache = nullptr;
  std::unique_ptr<KX_NavMesh> navmesh;
  /** Tiles whose layers are in the cache. */
  std::set<TileKey> tiles;
  /** Around NPCs: dropped tiles' layers, for a return without voxelizing. */
  std::map<TileKey, std::vector<std::vector<unsigned char>>> parked;
  std::deque<TileKey> parked_order;
  std::unordered_map<const KX_GameObject *, ObstacleState> obstacles;
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
  cfg.walkableSlopeAngle = settings.agent_max_slope * 57.2957795f;
  cfg.walkableHeight = std::max(int(std::ceil(settings.agent_height / cfg.ch)), 3);
  cfg.walkableClimb = int(std::floor(settings.agent_climb / cfg.ch));
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
  float tile_width = 19.2f;
  KX_NavBuildStats stats;
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
    float center[3], half[3];
    to_nav(obstacle.center, center);
    half[0] = float(obstacle.half[0]);
    half[1] = float(obstacle.half[2]);
    half[2] = float(obstacle.half[1]);
    obstacle.ref = 0;
    size.cache->addBoxObstacle(center, half, -obstacle.yaw, &obstacle.ref);
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

SizeClass *make_class(KX_NavBuildState &state, const int radius_cells)
{
  std::unique_ptr<SizeClass> size = std::make_unique<SizeClass>();
  size->radius_cells = radius_cells;
  size->radius = float(radius_cells) * state.settings.cell_size;
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
  tcp.walkableClimb = state.settings.agent_climb;
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

void sync_obstacles(KX_NavBuildState &state,
                    SizeClass &size,
                    const std::vector<KX_NavCarveObstacle> &obstacles)
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
                      std::fabs(found->second->yaw - it->second.yaw) < 1e-4f;
    if (!same) {
      if (it->second.ref != 0) {
        size.cache->removeObstacle(it->second.ref);
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
    float center[3], half[3];
    to_nav(o.center, center);
    half[0] = float(o.half_size[0]);
    half[1] = float(o.half_size[2]);
    half[2] = float(o.half_size[1]);
    /* The game turns counter-clockwise from x toward y; Recast's y axis turns x toward -z. */
    if (dtStatusFailed(size.cache->addBoxObstacle(center, half, -o.yaw, &obstacle.ref))) {
      if (!size.warned_obstacles) {
        size.warned_obstacles = true;
        CM_Warning("navigation: more than " << kMaxObstacles
                                            << " obstacles to carve; the rest are only avoided");
      }
      obstacle.ref = 0;
    }
    size.obstacles[o.object] = obstacle;
  }
  state.stats.obstacles_carved = size.obstacles.size();
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

static int radius_cells_of(const KX_NavBuildState &state, const float agent_radius)
{
  const float r = agent_radius > 0.0f ? agent_radius : state.settings.agent_radius;
  return std::max(1, int(std::ceil(r / state.settings.cell_size - 1e-4f)));
}

static SizeClass *class_of(KX_NavBuildState &state, const float agent_radius, const bool make)
{
  const int cells = radius_cells_of(state, agent_radius);
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->radius_cells == cells) {
      return size.get();
    }
  }
  if (make && int(state.classes.size()) < kMaxSizeClasses) {
    return make_class(state, cells);
  }
  /* The smallest larger size (the agent fits where it fits), else the largest. */
  SizeClass *best = nullptr;
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->radius_cells >= cells && (!best || size->radius_cells < best->radius_cells)) {
      best = size.get();
    }
  }
  if (!best) {
    for (const std::unique_ptr<SizeClass> &size : state.classes) {
      if (!best || size->radius_cells > best->radius_cells) {
        best = size.get();
      }
    }
  }
  return best;
}

KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, const float agent_radius)
{
  if (state.settings.build_mode == blender::NAVMESH_BUILD_OFF) {
    return nullptr;
  }
  SizeClass *size = class_of(state, agent_radius, true);
  return size ? size->navmesh.get() : nullptr;
}

const KX_NavMesh *KX_NavBuildFindNavMesh(const KX_NavBuildState &state, const float agent_radius)
{
  const int cells = radius_cells_of(state, agent_radius);
  for (const std::unique_ptr<SizeClass> &size : state.classes) {
    if (size->radius_cells == cells) {
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
                       const std::vector<float> &agent_radii,
                       const std::vector<KX_NavCarveObstacle> &obstacles,
                       const std::vector<KX_NavLink> &links,
                       LN_JobPool *pool)
{
  if (state.settings.build_mode == blender::NAVMESH_BUILD_OFF) {
    return;
  }
  /* Links in Recast's space (each tile takes those starting in it). */
  MeshProcess &process = state.process;
  process.link_verts.clear();
  process.link_radii.clear();
  process.link_dirs.clear();
  process.link_areas.clear();
  process.link_flags.clear();
  process.link_ids.clear();
  for (size_t i = 0; i < links.size(); i++) {
    float a[3], b[3];
    to_nav(links[i].start, a);
    to_nav(links[i].end, b);
    process.link_verts.insert(process.link_verts.end(), a, a + 3);
    process.link_verts.insert(process.link_verts.end(), b, b + 3);
    process.link_radii.push_back(std::max(links[i].radius, 0.05f));
    process.link_dirs.push_back(links[i].both_ways ? DT_OFFMESH_CON_BIDIR : 0);
    process.link_areas.push_back(0);
    process.link_flags.push_back(1);
    process.link_ids.push_back(uint32_t(i + 1));
  }

  const bool around = state.settings.build_mode == blender::NAVMESH_BUILD_AROUND_NPCS;
  for (const std::unique_ptr<SizeClass> &holder : state.classes) {
    SizeClass &size = *holder;
    std::set<TileKey> changed;
    if (!around && !size.level_built) {
      build_level(state, size, pool);
      for (const TileKey &key : size.tiles) {
        changed.insert(key);
      }
    }
    if (around) {
      /* The tiles wanted: within the build radius of an agent of this size, nearest first. */
      std::map<TileKey, float> wanted;
      std::set<TileKey> keep;
      const float build = state.settings.build_radius, remove = state.settings.remove_radius;
      for (size_t a = 0; a < agent_positions.size(); a++) {
        const float r = a < agent_radii.size() ? agent_radii[a] : 0.0f;
        if (class_of(state, r, false) != &size) {
          continue;
        }
        const MT_Vector3 &p = agent_positions[a];
        auto cover = [&](const float reach, auto &&fn) {
          const int x0 = int(std::floor((p[0] - reach) / state.tile_width));
          const int x1 = int(std::floor((p[0] + reach) / state.tile_width));
          const int y0 = int(std::floor((p[1] - reach) / state.tile_width));
          const int y1 = int(std::floor((p[1] + reach) / state.tile_width));
          for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
              /* The tile's nearest point to the agent. */
              const float nx = std::clamp(float(p[0]), x * state.tile_width, (x + 1) * state.tile_width);
              const float ny = std::clamp(float(p[1]), y * state.tile_width, (y + 1) * state.tile_width);
              const float d = std::hypot(nx - float(p[0]), ny - float(p[1]));
              if (d <= reach) {
                fn(TileKey{x, y}, d);
              }
            }
          }
        };
        cover(build, [&](const TileKey &key, const float d) {
          auto it = wanted.find(key);
          if (it == wanted.end() || d < it->second) {
            wanted[key] = d;
          }
        });
        cover(remove, [&](const TileKey &key, float) { keep.insert(key); });
      }
      /* Gone too far from every agent: out, its layers kept. */
      std::vector<TileKey> drop;
      for (const TileKey &key : size.tiles) {
        if (!keep.count(key)) {
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
      std::vector<std::pair<float, TileKey>> missing;
      for (const auto &[key, d] : wanted) {
        if (!size.tiles.count(key)) {
          missing.emplace_back(d, key);
        }
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
    sync_obstacles(state, size, obstacles);
    recarve_obstacles(state, size, changed);
    const bool carved = settle_obstacles(size);
    if (!changed.empty() || carved) {
      size.navmesh->NoteTilesChanged();
    }
  }
  state.dirty.clear();
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
      [&](KX_GameObject *object, const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector3 &c) {
        int area = 0;
        if (object != nullptr) {
          auto it = areas.find(object);
          if (it == areas.end()) {
            int id = 0;
            if (is_obstacle(object)) {
              id = -1;
            }
            else if (EXP_Value *property = object->GetProperty("nav_area")) {
              id = area_id(property->GetText());
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
      });
}

std::vector<KX_NavLink> KX_NavCollectLinks(KX_Scene *scene)
{
  std::vector<KX_NavLink> links;
  if (scene == nullptr) {
    return links;
  }
  EXP_ListValue<KX_GameObject> *objects = scene->GetObjectList();
  for (KX_GameObject *object : *objects) {
    EXP_Value *end_name = object->GetProperty("nav_link");
    if (end_name == nullptr) {
      continue;
    }
    KX_GameObject *end = objects->FindValue(end_name->GetText());
    if (end == nullptr || end == object) {
      CM_Warning("navigation: the link of object \"" << object->GetName()
                                                     << "\" names no other object (\""
                                                     << end_name->GetText() << "\")");
      continue;
    }
    KX_NavLink link;
    link.object = object;
    link.start = object->NodeGetWorldPosition();
    link.end = end->NodeGetWorldPosition();
    if (EXP_Value *kind = object->GetProperty("nav_link_kind")) {
      link.kind = kind->GetText();
    }
    if (EXP_Value *both = object->GetProperty("nav_link_both_ways")) {
      link.both_ways = both->GetNumber() != 0.0;
    }
    if (EXP_Value *radius = object->GetProperty("nav_link_radius")) {
      link.radius = std::max(float(radius->GetNumber()), 0.05f);
    }
    if (EXP_Value *duration = object->GetProperty("nav_link_duration")) {
      link.duration = std::max(float(duration->GetNumber()), 0.0f);
    }
    link.user_id = uint32_t(links.size() + 1);
    links.push_back(link);
  }
  return links;
}

/** \} */
