/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_shadow.hh"
#include "eevee_game_instance.hh"

#include "BKE_object.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h" /* DEG2RADF */
#include "DNA_light_types.h"
#include "DNA_object_types.h"

#include <cfloat>
#include <cstring> /* memcpy */
#include <cmath> /* sqrtf */

namespace blender::eevee_game {

ShadowModule::ShadowModule(GameInstance &inst, ShadowData &data) : inst_(&inst), data_(data) {}

/* =========================================================================
 * init()
 * ========================================================================= */

void ShadowModule::init(DynamicShadowSlots dynamic_slots)
{
  dynamic_slots_ = dynamic_slots;

  allocate_static_atlas();
  allocate_dynamic_atlas(); /* no-op when OFF */

  auto views_ensure = [](std::vector<std::unique_ptr<View>> &views, int len, const char *name) {
    views.clear();
    for (int i = 0; i < len; i++) {
      views.push_back(std::make_unique<View>(name));
    }
  };
  views_ensure(cascade_views_, MAX_SHADOW_CASCADES, "Shadow.Cascade");
  /* Punctual face views are created on first use (up to 6 per slot). */
  static_views_.clear();
  dynamic_views_.clear();

  /* Fresh atlases hold nothing worth keeping. */
  for (StaticTileCache &cache : static_cache_) {
    cache.valid = false;
  }
  cascade_cache_.valid = false;
}

void ShadowModule::allocate_static_atlas()
{
  /* Fixed 4096x4096 atlas. Tiles 1024x1024.
   *   Row 0        : 4 CSM cascade tiles (directional light).
   *   Rows 1..3    : 4 columns x 3 rows = 12 punctual slots.
   *
   * D32F avoids shadow acne at large world scales. */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;

  static_atlas_tx_.ensure_2d(
      gpu::TextureFormat::SFLOAT_32_DEPTH, int2(STATIC_ATLAS_RES), usage);

  /* Nearest + compare mode: PCF taps use sampler2DShadow hardware comparison. */
  GPU_texture_filter_mode(static_atlas_tx_, false);
  GPU_texture_compare_mode(static_atlas_tx_, true);

  static_fb_.ensure(GPU_ATTACHMENT_TEXTURE(static_atlas_tx_));
}

void ShadowModule::allocate_dynamic_atlas()
{
  const int n = int(dynamic_slots_);
  if (n == 0) {
    /* DynamicShadowSlots::OFF: nothing to allocate. */
    dynamic_atlas_tx_.free();
    return;
  }

  /* tiles_per_side = sqrt(n) is an integer for the allowed values
   * (4 -> 2, 16 -> 4, 64 -> 8). A square atlas keeps the shader UV math
   * symmetric: tile_uv = vec2(col, row) / tiles_per_side. */
  const int tiles_per_side = int(sqrtf(float(n)));
  const int atlas_res = tiles_per_side * DYNAMIC_TILE_SIZE;
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;

  dynamic_atlas_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH, int2(atlas_res), usage);

  GPU_texture_filter_mode(dynamic_atlas_tx_, false);
  GPU_texture_compare_mode(dynamic_atlas_tx_, true);

  dynamic_fb_.ensure(GPU_ATTACHMENT_TEXTURE(dynamic_atlas_tx_));
}

/* =========================================================================
 * Sync
 * ========================================================================= */

void ShadowModule::begin_sync()
{
  cascades_.clear();
  static_punctual_.clear();
  dynamic_punctual_.clear();
  caster_min_ = float3(FLT_MAX);
  caster_max_ = float3(-FLT_MAX);
  casters_.clear();
  casters_signature_ = 0;
}

void ShadowModule::update_cascades(Camera & /*camera*/, LightEntry &light)
{
  /* Split distribution between linear and logarithmic, as EEVEE Legacy: the logarithmic one
   * puts the resolution near the camera where the angular density is highest.
   *   split_i = mix(near + (far - near) * i / N, near * (far / near)^(i / N), exponent)
   * The cascades end at the light's maximum distance, not at the camera far plane, which can
   * be kilometers away. */
  cascades_.resize(MAX_SHADOW_CASCADES);

  const float near_clip = inst_->uniform_data.z_near;
  const float far_clip = math::clamp(
      light.cascade_max_dist, near_clip * 2.0f, inst_->uniform_data.z_far);
  /* The frustum corners below are on the camera clip planes. */
  const float camera_far = inst_->uniform_data.z_far;
  const float exponent = math::clamp(light.cascade_exponent, 0.0f, 1.0f);
  auto split_get = [&](float i) {
    const float x = i / float(MAX_SHADOW_CASCADES);
    return math::interpolate(near_clip + (far_clip - near_clip) * x,
                             near_clip * powf(far_clip / near_clip, x),
                             exponent);
  };

  /* Light space from the rotation and position of the sun only: a scaled sun
   * object would scale it, and the texel snapping below works in world
   * units. */
  float4x4 light_to_world = light.object_to_world;
  light_to_world.x_axis() = math::normalize(light_to_world.x_axis());
  light_to_world.y_axis() = math::normalize(light_to_world.y_axis());
  light_to_world.z_axis() = math::normalize(light_to_world.z_axis());
  const float4x4 light_view = math::invert(light_to_world);
  const float4x4 view_to_light = light_view * inst_->uniform_data.viewinv;

  /* Casters between the light and a slice are outside the slice bounds but
   * must not be clipped by the ortho near plane. Without depth clamp the
   * near plane is pulled toward the light up to the farthest synced caster
   * (the CPU gate already restricted the synced set to the casters that can
   * shadow the view). */
  float caster_ls_zmax = -FLT_MAX;
  if (caster_min_.x <= caster_max_.x) {
    for (int c = 0; c < 8; c++) {
      const float3 corner((c & 1) ? caster_max_.x : caster_min_.x,
                          (c & 2) ? caster_max_.y : caster_min_.y,
                          (c & 4) ? caster_max_.z : caster_min_.z);
      caster_ls_zmax = math::max(caster_ls_zmax, math::transform_point(light_view, corner).z);
    }
  }

  /* Frustum corners in view space at the near and far clip planes. Along each
   * corner ray the position is linear in view depth, for both projections. */
  const float4x4 wininv = math::invert(inst_->uniform_data.projectionmat);
  float3 corner_near[4], corner_far[4];
  for (int c = 0; c < 4; c++) {
    const float2 ndc((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f);
    corner_near[c] = math::project_point(wininv, float3(ndc, -1.0f));
    corner_far[c] = math::project_point(wininv, float3(ndc, 1.0f));
  }

  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    /* Each cascade also covers the band where the previous one fades into
     * it. */
    const float slice_near = (i == 0) ? near_clip :
                                        split_get(float(i)) * (1.0f - SHADOW_CASCADE_BLEND);
    const float slice_far = split_get(float(i + 1));

    cascades_[i].split_depth = slice_far;

    /* Stable cascades: the slice is bounded by a sphere computed in view
     * space, so its radius, and the texel size with it, does not change when
     * the camera turns or moves. A light space box fitted to the slice
     * changes size with every rotation, and the shadow edges crawl. */
    float3 corners[8];
    const float t_near = (slice_near - near_clip) / (camera_far - near_clip);
    const float t_far = (slice_far - near_clip) / (camera_far - near_clip);
    float2 axis(0.0f);
    for (int c = 0; c < 4; c++) {
      corners[c] = math::interpolate(corner_near[c], corner_far[c], t_near);
      corners[c + 4] = math::interpolate(corner_near[c], corner_far[c], t_far);
      axis += corners[c].xy() + corners[c + 4].xy();
    }
    axis /= 8.0f;
    /* Center on the slice axis where it is as far from the near corners as
     * from the far ones: (z - dn)^2 + a^2 = (df - z)^2 + b^2. Past the far
     * plane (wide slices) the far plane center is the tighter choice. */
    float a2 = 0.0f, b2 = 0.0f;
    for (int c = 0; c < 4; c++) {
      a2 = math::max(a2, math::length_squared(corners[c].xy() - axis));
      b2 = math::max(b2, math::length_squared(corners[c + 4].xy() - axis));
    }
    const float center_depth = math::clamp(
        (slice_far * slice_far - slice_near * slice_near + b2 - a2) /
            (2.0f * (slice_far - slice_near)),
        slice_near,
        slice_far);
    const float3 center_vs(axis, -center_depth);
    /* Exact enclosing radius, whatever the frustum symmetry. */
    float radius = 0.0f;
    for (int c = 0; c < 8; c++) {
      radius = math::max(radius, math::distance_squared(center_vs, corners[c]));
    }
    radius = math::sqrt(radius);

    /* Two texels of border each side: the snapped center moves the box by
     * up to one texel, and the 3x3 PCF reads one more. */
    const float texel = 2.0f * radius / float(STATIC_TILE_SIZE - 4);
    const float half_size = 0.5f * texel * float(STATIC_TILE_SIZE);
    /* Snapped to whole texels in light space: the shadow map moves by whole
     * texels, so its texels land on the same world positions frame after
     * frame. Depth too, so a sub texel camera motion keeps the matrices
     * bit identical and the cascade cache valid. */
    float3 center_ls = math::transform_point(view_to_light, center_vs);
    center_ls.x = math::floor(center_ls.x / texel) * texel;
    center_ls.y = math::floor(center_ls.y / texel) * texel;
    /* +Z points toward the light in light space; the range is snapped
     * outward. */
    const float z_min = math::floor((center_ls.z - radius) / texel) * texel;
    const float z_max = math::ceil(math::max(center_ls.z + radius, caster_ls_zmax) / texel) *
                        texel;

    /* The light looks down its local -Z, so depth range is [-max.z, -min.z]. */
    cascades_[i].view = light_view;
    cascades_[i].proj = math::projection::orthographic(center_ls.x - half_size,
                                                       center_ls.x + half_size,
                                                       center_ls.y - half_size,
                                                       center_ls.y + half_size,
                                                       -z_max,
                                                       -z_min);
    cascades_[i].view_proj = cascades_[i].proj * cascades_[i].view;

    data_.cascade_viewproj[i] = cascades_[i].view_proj;
    data_.cascade_splits[i] = slice_far;
  }
}

/* Mixes 64 bits so that sums of keys behave like a set hash (splitmix64). */
static uint64_t mix64(uint64_t x)
{
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

static uint64_t float_bits(float f)
{
  uint32_t u;
  memcpy(&u, &f, sizeof(u));
  return u;
}

/* Rigid view of a light: its axes without the object scale, which would
 * otherwise scale the light space the projection is built in. */
static float4x4 light_view_get(const float4x4 &object_to_world)
{
  const float4x4 world_from_light(float4(math::normalize(object_to_world.x_axis()), 0.0f),
                                  float4(math::normalize(object_to_world.y_axis()), 0.0f),
                                  float4(math::normalize(object_to_world.z_axis()), 0.0f),
                                  float4(object_to_world.location(), 1.0f));
  return math::invert(world_from_light);
}

/* Cube face f of a point light at pos, ordered +X -X +Y -Y +Z -Z. The view
 * looks down -Z, so -Z of the face is the face direction. */
static float4x4 cube_face_view_get(float3 pos, int face)
{
  static const float3 dirs[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const float3 f = dirs[face];
  const float3 up = (fabsf(f.z) > 0.5f) ? float3(0, 1, 0) : float3(0, 0, 1);
  const float3 z = -f;
  const float3 x = math::normalize(math::cross(up, z));
  const float3 y = math::cross(z, x);
  const float4x4 world_from_face(
      float4(x, 0.0f), float4(y, 0.0f), float4(z, 0.0f), float4(pos, 1.0f));
  return math::invert(world_from_face);
}

/* Texels of guard band around a face: the 3x3 PCF footprint plus the normal
 * offset must find rendered depth past the face border, or samples near a
 * cube seam would read the neighbor face. */
static constexpr float SHADOW_FACE_GUARD_TEXELS = 4.0f;

static int face_size_get(int tile_size, int face_count)
{
  return (face_count == 6) ? tile_size / 3 : tile_size;
}

/* Origin of face f inside its tile: a point light packs 3x2 faces. */
static int2 face_origin_get(int2 tile_origin, int face_size, int face_count, int face)
{
  if (face_count != 6) {
    return tile_origin;
  }
  return tile_origin + int2(face % 3, face / 3) * face_size;
}

static int2 static_tile_origin(uint32_t atlas_index)
{
  /* Rows 1..3 below the cascade row, 4 columns. */
  return int2(int(atlas_index) % 4, 1 + int(atlas_index) / 4) * STATIC_TILE_SIZE;
}

void ShadowModule::punctual_faces_build(ShadowPunctual &slot,
                                        const LightEntry &light,
                                        int tile_size)
{
  /* Point and area lights see all around their center: a cube. */
  const bool is_cube = light.data.type != uint32_t(LightType::PUNCTUAL_SPOT);
  slot.face_count = is_cube ? 6 : 1;
  slot.face_mask = (1u << slot.face_count) - 1u;
  slot.position = light.object_to_world.location();
  slot.radius = math::max(light.data.attenuation, 0.02f);
  /* Legacy EEVEE: 0.05 world units per unit of bias, subtracted from the receiver distance. */
  slot.bias = 0.05f * light.shadow_bias;

  const float face_res = float(face_size_get(tile_size, slot.face_count));
  const float guard = 2.0f * SHADOW_FACE_GUARD_TEXELS / face_res;
  /* Cube: 90 degree faces widened by the guard band. Spot: its cone. */
  const float base_tan = is_cube ? 1.0f :
                                   tanf(math::min(light.spot_angle, DEG2RADF(170.0f)) *
                                        0.5f);
  const float tan_half = base_tan * (1.0f + guard);
  const float half_angle = atanf(tan_half);
  slot.proj = math::projection::perspective_fov(
      -half_angle,
      half_angle,
      -half_angle,
      half_angle,
      math::clamp(light.shadow_clip_start, 0.01f, slot.radius * 0.5f),
      slot.radius);

  if (is_cube) {
    for (int f = 0; f < 6; f++) {
      slot.face_view[f] = cube_face_view_get(slot.position, f);
    }
  }
  if (light.data.type == uint32_t(LightType::AREA_RECT) ||
      light.data.type == uint32_t(LightType::AREA_ELLIPSE))
  {
    /* One sided emitter: a face whose whole (widened) frustum lies behind
     * the panel never sees a lit point, so it is not rendered. Its tile entry
     * stays, the lookup picks faces by index. The frustum directions of face
     * (axis a, sign s) are s e_a + t (+-e_b +-e_c): the largest dot with the
     * emission direction n is s n_a + t (|n_b| + |n_c|). Typically one face. */
    const float3 n = light.data.direction;
    for (int f = 0; f < 6; f++) {
      const int a = f / 2;
      const float s = (f & 1) ? -1.0f : 1.0f;
      const float side = fabsf(n[(a + 1) % 3]) + fabsf(n[(a + 2) % 3]);
      if (s * n[a] + tan_half * side <= 0.0f) {
        slot.face_mask &= ~(1u << f);
      }
    }
  }
  else if (!is_cube) {
    slot.face_view[0] = light_view_get(light.object_to_world);
  }
}

void ShadowModule::punctual_tiles_write(
    const ShadowPunctual &slot, int2 tile_origin, int tile_size, int atlas_res, int atlas)
{
  const int face_size = face_size_get(tile_size, slot.face_count);
  /* tan(half fov) is the (0,0) entry inverse of the projection. */
  const float texel_scale = 2.0f / (slot.proj[0][0] * float(face_size));
  for (int f = 0; f < slot.face_count; f++) {
    if (tile_count_ >= MAX_SHADOW_TILES) {
      return;
    }
    const int2 origin = face_origin_get(tile_origin, face_size, slot.face_count, f);
    GameShadowTileData &tile = tile_buf_[tile_count_++];
    tile.view_proj = slot.proj * slot.face_view[f];
    tile.atlas_rect = float4(float2(origin) / float(atlas_res),
                             float2(float(face_size) / float(atlas_res)));
    tile.atlas = atlas;
    tile.texel_scale = texel_scale;
    tile.split_depth = 0.0f;
    tile.bias = slot.bias;
  }
}

uint64_t ShadowModule::caster_signature(float3 center, float radius) const
{
  /* Sum of the keys of every caster box touching the sphere: changes when
   * one of them moves, deforms, appears or disappears. */
  uint64_t sum = 0;
  const float r2 = radius * radius;
  for (const CasterRecord &c : casters_) {
    const float3 d = math::max(math::abs(center - c.center) - c.half_size, float3(0.0f));
    if (math::dot(d, d) <= r2) {
      sum += c.key;
    }
  }
  return sum;
}

void ShadowModule::end_sync()
{
  /* Route each shadow-casting light to an atlas and write the faces it reads
   * into tile_buf_. GameLightData::shadow_index becomes the index of the first
   * face (punctual), 0 for the shadowed sun, -1 for no shadow.
   * Priority: LIGHT_FLAG_SHADOW_DYNAMIC -> dynamic (if not full).
   * Fallback: static (if not full). If both full: no shadow. */
  const size_t dynamic_cap = size_t(dynamic_slots_);
  bool has_sun = false;
  tile_count_ = 0;

  /* Objects whose material was still compiling were drawn with a fallback (an opaque
   * default surface, also for a volume only material), and a cached map keeps that until
   * its casters move. Redraw everything once their shaders are ready. */
  if (inst_->materials.shaders_compiled_since_last_sync()) {
    for (StaticTileCache &cache : static_cache_) {
      cache.valid = false;
    }
    cascade_cache_.valid = false;
  }

  for (StaticTileCache &cache : static_cache_) {
    cache.used = false;
  }
  /* Static lights that did not find their tile of last frame. */
  Vector<int, STATIC_PUNCTUAL_SLOTS> unplaced;

  Vector<LightEntry> &entries = inst_->lights.entries_;
  for (int l = 0; l < int(entries.size()); l++) {
    LightEntry &light = entries[l];
    if (light.data.shadow_index < 0) {
      continue; /* Shadow casting disabled on this light. */
    }
    light.data.shadow_index = -1;

    if (light.data.type == uint32_t(LightType::SUN)) {
      /* One set of cascades: the first sun wins, the others stay unshadowed. */
      if (!has_sun) {
        update_cascades(inst_->camera, light);
        has_sun = true;
        /* The cascades go first in tile_buf_: the forward shaders look them
         * up there, the deferred pass reads the filtered sun mask instead. */
        light.data.shadow_index = tile_count_;
        for (int i = 0; i < int(cascades_.size()) && tile_count_ < MAX_SHADOW_TILES; i++) {
          GameShadowTileData &tile = tile_buf_[tile_count_++];
          tile.view_proj = cascades_[i].view_proj;
          tile.atlas_rect = float4(float(i) * 0.25f, 0.0f, 0.25f, 0.25f);
          tile.atlas = 0;
          /* Orthographic: one texel is the cascade width over the tile. */
          tile.texel_scale = 2.0f / (cascades_[i].proj[0][0] * float(STATIC_TILE_SIZE));
          tile.split_depth = cascades_[i].split_depth;
          tile.bias = 0.0f;
        }
      }
      continue;
    }

    ShadowPunctual slot = {};
    slot.uid = light.uid;
    slot.light_index = l;
    slot.active = true;
    slot.needs_update = true;

    const bool want_dynamic = (light.flags & LIGHT_FLAG_SHADOW_DYNAMIC) != 0;
    if (want_dynamic && dynamic_punctual_.size() < dynamic_cap) {
      slot.is_dynamic = true;
      slot.atlas_index = uint32_t(dynamic_punctual_.size());
      punctual_faces_build(slot, light, DYNAMIC_TILE_SIZE);
      dynamic_punctual_.push_back(slot);
      continue;
    }
    if (static_punctual_.size() >= STATIC_PUNCTUAL_SLOTS) {
      continue; /* Both atlases full, the light casts no shadow this frame. */
    }
    slot.is_dynamic = false;
    punctual_faces_build(slot, light, STATIC_TILE_SIZE);
    /* Keep the tile this light had: its content may still be valid. */
    int tile = -1;
    for (int t = 0; t < STATIC_PUNCTUAL_SLOTS; t++) {
      if (static_cache_[t].valid && !static_cache_[t].used &&
          static_cache_[t].light_uid == slot.uid)
      {
        tile = t;
        break;
      }
    }
    if (tile < 0) {
      unplaced.append(int(static_punctual_.size()));
    }
    else {
      static_cache_[tile].used = true;
      slot.atlas_index = uint32_t(tile);
    }
    static_punctual_.push_back(slot);
  }

  /* New static lights take the free tiles, invalid ones first. */
  for (int idx : unplaced) {
    int tile = -1;
    for (int pass = 0; pass < 2 && tile < 0; pass++) {
      for (int t = 0; t < STATIC_PUNCTUAL_SLOTS; t++) {
        if (!static_cache_[t].used && (pass == 1 || !static_cache_[t].valid)) {
          tile = t;
          break;
        }
      }
    }
    static_cache_[tile].used = true;
    static_cache_[tile].valid = false;
    static_punctual_[idx].atlas_index = uint32_t(tile);
  }

  /* Static tiles are redrawn only when what they hold would change. */
  for (ShadowPunctual &slot : static_punctual_) {
    StaticTileCache &cache = static_cache_[slot.atlas_index];
    const uint64_t signature = caster_signature(slot.position, slot.radius);
    slot.needs_update = !cache.valid || cache.light_uid != slot.uid ||
                        cache.signature != signature || cache.proj != slot.proj ||
                        cache.face_view0 != slot.face_view[0] ||
                        cache.face_mask != slot.face_mask;
    cache.light_uid = slot.uid;
    cache.face_mask = slot.face_mask;
    cache.face_view0 = slot.face_view[0];
    cache.proj = slot.proj;
    cache.signature = signature;
    cache.valid = true;

    entries[slot.light_index].data.shadow_index = tile_count_;
    punctual_tiles_write(slot,
                         static_tile_origin(slot.atlas_index),
                         STATIC_TILE_SIZE,
                         STATIC_ATLAS_RES,
                         0);
  }
  /* Tiles nobody claimed this frame keep their depth but no longer match a
   * light: drop them so a new light never trusts them. */
  for (StaticTileCache &cache : static_cache_) {
    if (!cache.used) {
      cache.valid = false;
    }
  }

  const int tiles_per_side = int(sqrtf(float(int(dynamic_slots_))));
  for (ShadowPunctual &slot : dynamic_punctual_) {
    entries[slot.light_index].data.shadow_index = tile_count_;
    const int2 origin = int2(int(slot.atlas_index) % tiles_per_side,
                             int(slot.atlas_index) / tiles_per_side) *
                        DYNAMIC_TILE_SIZE;
    punctual_tiles_write(
        slot, origin, DYNAMIC_TILE_SIZE, tiles_per_side * DYNAMIC_TILE_SIZE, 1);
  }
  tile_buf_.push_update();

  /* Cascades follow the camera: redrawn when their matrices move or any
   * caster changed. */
  CascadeCache &cc = cascade_cache_;
  bool cascades_same = cc.valid && cc.count == int(cascades_.size()) &&
                       cc.signature == casters_signature_;
  for (int i = 0; i < int(cascades_.size()) && cascades_same; i++) {
    cascades_same = cc.view_proj[i] == cascades_[i].view_proj;
  }
  cascades_need_update_ = !cascades_same && !cascades_.empty();
  cc.count = int(cascades_.size());
  cc.signature = casters_signature_;
  cc.valid = true;
  for (int i = 0; i < int(cascades_.size()); i++) {
    cc.view_proj[i] = cascades_[i].view_proj;
  }
}

void ShadowModule::sync_object(Object *ob, uint64_t uid)
{
  if (ob->visibility_flag & OB_HIDE_SHADOW) {
    return;
  }
  const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_eval_cached_get(ob);
  if (!bounds) {
    return;
  }
  /* World AABB of the local box in center / half size form: the half size
   * along a world axis is |row| . local half size (Arvo), exact for a box. */
  const float4x4 &m = ob->object_to_world();
  const float3 lc = (bounds->min + bounds->max) * 0.5f;
  const float3 le = (bounds->max - bounds->min) * 0.5f;
  const float3 wc = math::transform_point(m, lc);
  const float3 we = math::abs(m.x_axis()) * le.x + math::abs(m.y_axis()) * le.y +
                    math::abs(m.z_axis()) * le.z;
  caster_min_ = math::min(caster_min_, wc - we);
  caster_max_ = math::max(caster_max_, wc + we);

  /* The key changes with the identity, the world box and the geometry
   * evaluation stamp (deformation inside an unchanged box, GPU skinning). */
  uint64_t key = mix64(uid);
  for (int k = 0; k < 3; k++) {
    key = mix64(key ^ float_bits(wc[k]) ^ (float_bits(we[k]) << 32));
  }
  key = mix64(key ^ ob->runtime->last_update_geometry);
  casters_.push_back({wc, we, key});
  casters_signature_ += key;
}

/* =========================================================================
 * set_view(): main entry point called from pipeline.cc
 * ========================================================================= */

void ShadowModule::set_view(View & /*view*/, int2 extent)
{
  GPU_debug_group_begin("Shadow");
  render_static_atlas();
  render_dynamic_atlas();
  GPU_debug_group_end();

  dispatch_pcf(extent);
}

void ShadowModule::render_tile(Framebuffer &fb, View &view, int2 tile_origin, int tile_size)
{
  GPU_framebuffer_viewport_set(fb, tile_origin.x, tile_origin.y, tile_size, tile_size);

  /* Shadow casters are the opaque depth prepass drawn from the light. Same
   * draws, same material depth shaders (alpha clip included), and the
   * Manager culls them against this view's own frustum. No second geometry
   * pass has to be built or kept in sync with the first.
   *
   * Consequence: the atlas stores what those shaders write, reversed depth
   * (1 = near the light, 0 = far), tested with GREATER_EQUAL. */
  inst_->manager->submit(inst_->pipelines.prepass_opaque_get(), view);
  /* Blended surfaces, which have no opaque prepass. */
  PassMain &casters = inst_->pipelines.shadow_casters_get();
  if (!casters.is_empty()) {
    inst_->manager->submit(casters, view);
  }
}

static View &view_ensure(std::vector<std::unique_ptr<View>> &views, int index, const char *name)
{
  /* Views own GPU buffers: created on first use, then kept. */
  if (int(views.size()) <= index) {
    views.resize(index + 1);
  }
  if (!views[index]) {
    views[index] = std::make_unique<View>(name);
  }
  return *views[index];
}

void ShadowModule::render_punctual(Framebuffer &fb,
                                   std::vector<std::unique_ptr<View>> &views,
                                   const ShadowPunctual &slot,
                                   int2 tile_origin,
                                   int tile_size)
{
  const int face_size = face_size_get(tile_size, slot.face_count);
  for (int f = 0; f < slot.face_count; f++) {
    if ((slot.face_mask & (1u << f)) == 0) {
      continue;
    }
    View &view = view_ensure(views, int(slot.atlas_index) * 6 + f, "Shadow.Punctual");
    view.sync(slot.face_view[f], slot.proj);
    render_tile(fb,
                view,
                face_origin_get(tile_origin, face_size, slot.face_count, f),
                face_size);
  }
}

/* Depth clear of one rectangle: a clear ignores the viewport but honors the
 * scissor. Reversed depth: 0 is the far plane. */
static void clear_rect(Framebuffer &fb, int2 origin, int2 size)
{
  GPU_scissor_test(true);
  GPU_scissor(origin.x, origin.y, size.x, size.y);
  GPU_framebuffer_clear_depth(fb, 0.0f);
  GPU_scissor_test(false);
}

/* =========================================================================
 * render_static_atlas()
 * ========================================================================= */

void ShadowModule::render_static_atlas()
{
  bool any_punctual = false;
  for (const ShadowPunctual &p : static_punctual_) {
    any_punctual |= p.active && p.needs_update;
  }
  if (!cascades_need_update_ && !any_punctual) {
    /* Nothing changed: the atlas still holds this frame's shadows. */
    return;
  }

  GPU_debug_group_begin("Shadow.Static");
  static_fb_.bind();

  /* CSM cascades: row 0. */
  if (cascades_need_update_) {
    clear_rect(static_fb_, int2(0), int2(STATIC_ATLAS_RES, STATIC_TILE_SIZE));
    for (int i = 0; i < int(cascades_.size()); ++i) {
      View &view = *cascade_views_[i];
      view.sync(cascades_[i].view, cascades_[i].proj);
      render_tile(static_fb_, view, int2(i * STATIC_TILE_SIZE, 0), STATIC_TILE_SIZE);
    }
  }

  /* Static punctual lights: rows 1..3, only the tiles whose content changed. */
  for (ShadowPunctual &p : static_punctual_) {
    if (!p.active || !p.needs_update) {
      continue;
    }
    const int2 origin = static_tile_origin(p.atlas_index);
    clear_rect(static_fb_, origin, int2(STATIC_TILE_SIZE));
    render_punctual(static_fb_, static_views_, p, origin, STATIC_TILE_SIZE);
    p.needs_update = false;
  }

  GPU_framebuffer_viewport_reset(static_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * render_dynamic_atlas()
 * ========================================================================= */

void ShadowModule::render_dynamic_atlas()
{
  if (!dynamic_atlas_tx_.is_valid() || dynamic_punctual_.empty()) {
    return; /* OFF or no dynamic lights this frame */
  }

  GPU_debug_group_begin("Shadow.Dynamic");

  /* tiles_per_side = sqrt(N): 4 -> 2, 16 -> 4, 64 -> 8.
   * atlas_index is flat [0..N-1]; col = idx % tps, row = idx / tps. */
  const int tiles_per_side = int(sqrtf(float(int(dynamic_slots_))));

  dynamic_fb_.bind();
  GPU_framebuffer_clear_depth(dynamic_fb_, 0.0f);

  for (const ShadowPunctual &p : dynamic_punctual_) {
    if (!p.active) {
      continue;
    }
    const int2 origin = int2(int(p.atlas_index) % tiles_per_side,
                             int(p.atlas_index) / tiles_per_side) *
                        DYNAMIC_TILE_SIZE;
    render_punctual(dynamic_fb_, dynamic_views_, p, origin, DYNAMIC_TILE_SIZE);
  }

  GPU_framebuffer_viewport_reset(dynamic_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * dispatch_pcf(): PCF 3x3 compute pass
 * ========================================================================= */

void ShadowModule::dispatch_pcf(int2 extent)
{
  RenderBuffers &rbufs = inst_->render_buffers;

  pcf_ps_.init();
  pcf_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_PCF_FILTER));

  /* Depth, and shading normals decoded from the G-Buffer. */
  pcf_ps_.bind_texture("depth_tx", &rbufs.depth_tx);
  rbufs.gbuffer.bind_resources(pcf_ps_);

  /* PCF reads from the static atlas (CSM cascades), the primary shadow source.
   * Dynamic atlas is not sampled here; dynamic punctual shadows are a future pass. */
  pcf_ps_.bind_texture("shadow_atlas", static_atlas_tx_);

  /* Output: one shadow factor per pixel */
  pcf_ps_.bind_image("shadow_mask_img", &rbufs.shadow_mask_tx);

  /* Inverse VP for world-space position reconstruction. */
  pcf_ps_.push_constant("viewprojinv", math::invert(inst_->uniform_data.viewprojmat));

  /* One named constant per cascade: the pass API has no matrix array
   * overload. Unused cascades are identity with a zero split, so the shader
   * never selects them. */
  const char *cascade_names[MAX_SHADOW_CASCADES] = {
      "cascade_viewproj_0", "cascade_viewproj_1", "cascade_viewproj_2", "cascade_viewproj_3"};
  float4 splits = float4(0.0f);
  float4 texel_size = float4(0.0f);
  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const bool used = i < int(cascades_.size());
    pcf_ps_.push_constant(cascade_names[i],
                          used ? cascades_[i].view_proj : float4x4::identity());
    splits[i] = used ? cascades_[i].split_depth : 0.0f;
    /* Orthographic: the cascade width over the tile resolution. */
    texel_size[i] = used ? 2.0f / (cascades_[i].proj[0][0] * float(STATIC_TILE_SIZE)) : 0.0f;
  }
  pcf_ps_.push_constant("cascade_splits", splits);
  pcf_ps_.push_constant("cascade_texel_size", texel_size);
  pcf_ps_.push_constant("shadow_bias", 0.005f);
  pcf_ps_.push_constant("pcf_offset_scale", data_.pcf_offset_scale);
  pcf_ps_.push_constant("shadow_map_res", int(STATIC_ATLAS_RES));

  /* One thread per pixel, 8x8 groups. */
  pcf_ps_.dispatch(math::divide_ceil(extent, int2(8)));

  /* The deferred lighting pass samples the mask right after. */
  pcf_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_->manager->submit(pcf_ps_);
}

/* =========================================================================
 * bind_resources(): punctual shadow lookup (deferred lighting)
 * ========================================================================= */

void ShadowModule::bind_resources(PassSimple &ps)
{
  ps.bind_ssbo("shadow_tile_buf", &tile_buf_);
  ps.bind_texture("shadow_static_atlas", static_atlas_tx_);
  /* No face points into the dynamic atlas when it is off: any depth texture
   * satisfies the binding. */
  gpu::Texture *dynamic_atlas = dynamic_atlas_tx_.is_valid() ? (gpu::Texture *)dynamic_atlas_tx_ :
                                                               (gpu::Texture *)static_atlas_tx_;
  ps.bind_texture("shadow_dynamic_atlas", dynamic_atlas);
}

}  // namespace blender::eevee_game
