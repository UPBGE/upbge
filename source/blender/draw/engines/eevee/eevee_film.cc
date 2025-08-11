/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * A film is a buffer (usually at display extent)
 * that will be able to accumulate sample in any distorted camera_type
 * using a pixel filter.
 *
 * Input needs to be jittered so that the filter converges to the right result.
 */

#include "BLI_hash.h"
#include "BLI_rect.h"
#include "BLI_set.hh"

#include "BKE_compositor.hh"
#include "BKE_scene.hh"

#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "DRW_render.hh"
#include "RE_pipeline.h"

#include "draw_view_data.hh"

#include "eevee_film.hh"
#include "eevee_instance.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Arbitrary Output Variables
 * \{ */

void Film::init_aovs(const Set<std::string> &passes_used_by_viewport_compositor)
{
  Vector<ViewLayerAOV *> aovs;

  aovs_info.display_id = -1;
  aovs_info.display_is_value = false;
  aovs_info.value_len = aovs_info.color_len = 0;

  if (inst_.is_viewport()) {
    /* Viewport case. */
    if (inst_.v3d->shading.render_pass == EEVEE_RENDER_PASS_AOV) {
      /* AOV display, request only a single AOV. */
      ViewLayerAOV *aov = (ViewLayerAOV *)BLI_findstring(
          &inst_.view_layer->aovs, inst_.v3d->shading.aov_name, offsetof(ViewLayerAOV, name));

      /* AOV found in view layer. */
      if (aov) {
        aovs.append(aov);
        aovs_info.display_id = 0;
        aovs_info.display_is_value = (aov->type == AOV_TYPE_VALUE);
      }
    }

    if (inst_.is_viewport_compositor_enabled) {
      LISTBASE_FOREACH (ViewLayerAOV *, aov, &inst_.view_layer->aovs) {
        /* Already added as a display pass. No need to add again. */
        if (!aovs.is_empty() && aovs.last() == aov) {
          continue;
        }

        if (passes_used_by_viewport_compositor.contains(aov->name)) {
          aovs.append(aov);
        }
      }
    }
  }
  else {
    /* Render case. */
    LISTBASE_FOREACH (ViewLayerAOV *, aov, &inst_.view_layer->aovs) {
      aovs.append(aov);
    }
  }

  if (aovs.size() > AOV_MAX) {
    inst_.info_append_i18n("Error: Too many AOVs");
    return;
  }

  for (ViewLayerAOV *aov : aovs) {
    bool is_value = (aov->type == AOV_TYPE_VALUE);
    int &index = is_value ? aovs_info.value_len : aovs_info.color_len;
    uint &hash = is_value ? aovs_info.hash_value[index].x : aovs_info.hash_color[index].x;
    hash = BLI_hash_string(aov->name);
    index++;
  }

  if (!aovs.is_empty()) {
    enabled_categories_ |= PASS_CATEGORY_AOV;
  }
}

float *Film::read_aov(ViewLayerAOV *aov)
{
  gpu::Texture *pass_tx = this->get_aov_texture(aov);

  if (pass_tx == nullptr) {
    return nullptr;
  }

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  return (float *)GPU_texture_read(pass_tx, GPU_DATA_FLOAT, 0);
}

gpu::Texture *Film::get_aov_texture(ViewLayerAOV *aov)
{
  bool is_value = (aov->type == AOV_TYPE_VALUE);
  Texture &accum_tx = is_value ? value_accum_tx_ : color_accum_tx_;

  Span<uint4> aovs_hash(is_value ? aovs_info.hash_value : aovs_info.hash_color,
                        is_value ? aovs_info.value_len : aovs_info.color_len);
  /* Find AOV index. */
  uint hash = BLI_hash_string(aov->name);
  int aov_index = -1;
  int i = 0;
  for (uint4 candidate_hash : aovs_hash) {
    if (candidate_hash.x == hash) {
      aov_index = i;
      break;
    }
    i++;
  }

  if (aov_index == -1) {
    return nullptr;
  }

  accum_tx.ensure_layer_views();

  int index = aov_index + (is_value ? data_.aov_value_id : data_.aov_color_id);
  return accum_tx.layer_view(index);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mist Pass
 * \{ */

void Film::sync_mist()
{
  const CameraData &cam = inst_.camera.data_get();
  const ::World *world = inst_.scene->world;
  float mist_start = world ? world->miststa : cam.clip_near;
  float mist_distance = world ? world->mistdist : fabsf(cam.clip_far - cam.clip_near);
  int mist_type = world ? world->mistype : int(WO_MIST_LINEAR);

  switch (mist_type) {
    case WO_MIST_QUADRATIC:
      data_.mist_exponent = 2.0f;
      break;
    case WO_MIST_LINEAR:
      data_.mist_exponent = 1.0f;
      break;
    case WO_MIST_INVERSE_QUADRATIC:
      data_.mist_exponent = 0.5f;
      break;
  }

  data_.mist_scale = 1.0 / mist_distance;
  data_.mist_bias = -mist_start / mist_distance;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FilmData
 * \{ */

inline bool operator==(const FilmData &a, const FilmData &b)
{
  return (a.extent == b.extent) && (a.offset == b.offset) &&
         (a.render_extent == b.render_extent) && (a.overscan == b.overscan) &&
         (a.filter_radius == b.filter_radius) && (a.scaling_factor == b.scaling_factor) &&
         (a.background_opacity == b.background_opacity);
}

inline bool operator!=(const FilmData &a, const FilmData &b)
{
  return !(a == b);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Film
 * \{ */

static eViewLayerEEVEEPassType enabled_passes(const ViewLayer *view_layer)
{
  eViewLayerEEVEEPassType result = eViewLayerEEVEEPassType(view_layer->eevee.render_passes);

  /* We enforce the use of combined pass to be compliant with Cycles and EEVEE-Legacy (#122188). */
  result |= EEVEE_RENDER_PASS_COMBINED;

#define ENABLE_FROM_LEGACY(name_legacy, name_eevee) \
  SET_FLAG_FROM_TEST(result, \
                     (view_layer->passflag & SCE_PASS_##name_legacy) != 0, \
                     EEVEE_RENDER_PASS_##name_eevee);

  ENABLE_FROM_LEGACY(DEPTH, DEPTH)
  ENABLE_FROM_LEGACY(MIST, MIST)
  ENABLE_FROM_LEGACY(NORMAL, NORMAL)
  ENABLE_FROM_LEGACY(POSITION, POSITION)
  ENABLE_FROM_LEGACY(SHADOW, SHADOW)
  ENABLE_FROM_LEGACY(AO, AO)
  ENABLE_FROM_LEGACY(EMIT, EMIT)
  ENABLE_FROM_LEGACY(ENVIRONMENT, ENVIRONMENT)
  ENABLE_FROM_LEGACY(DIFFUSE_COLOR, DIFFUSE_COLOR)
  ENABLE_FROM_LEGACY(GLOSSY_COLOR, SPECULAR_COLOR)
  ENABLE_FROM_LEGACY(DIFFUSE_DIRECT, DIFFUSE_LIGHT)
  ENABLE_FROM_LEGACY(GLOSSY_DIRECT, SPECULAR_LIGHT)
  ENABLE_FROM_LEGACY(ENVIRONMENT, ENVIRONMENT)
  ENABLE_FROM_LEGACY(VECTOR, VECTOR)

#undef ENABLE_FROM_LEGACY

  SET_FLAG_FROM_TEST(result,
                     view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_OBJECT,
                     EEVEE_RENDER_PASS_CRYPTOMATTE_OBJECT);
  SET_FLAG_FROM_TEST(result,
                     view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_ASSET,
                     EEVEE_RENDER_PASS_CRYPTOMATTE_ASSET);
  SET_FLAG_FROM_TEST(result,
                     view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_MATERIAL,
                     EEVEE_RENDER_PASS_CRYPTOMATTE_MATERIAL);

  return result;
}

/* Get all pass types used by the viewport compositor from the set of all needed passes. */
static eViewLayerEEVEEPassType get_viewport_compositor_enabled_passes(
    const Set<std::string> &viewport_compositor_needed_passes, const ViewLayer *view_layer)
{
  const eViewLayerEEVEEPassType scene_enabled_passes = enabled_passes(view_layer);

  /* Go over all possible pass types, check if their possible pass names exist in the viewport
   * compositor needed passes, and if true, mark them as needed. */
  eViewLayerEEVEEPassType viewport_compositor_enabled_passes = eViewLayerEEVEEPassType(0);
  for (const int i : IndexRange(EEVEE_RENDER_PASS_MAX_BIT + 1)) {
    /* Mask by the scene enabled passes, because some pass types like EEVEE_RENDER_PASS_UNUSED_8
     * have no corresponding pass names, so they will assert later. */
    eViewLayerEEVEEPassType pass_type = eViewLayerEEVEEPassType(scene_enabled_passes & (1 << i));
    if (pass_type == 0) {
      continue;
    }

    for (const std::string &pass_name : Film::pass_to_render_pass_names(pass_type, view_layer)) {
      if (viewport_compositor_needed_passes.contains(pass_name)) {
        viewport_compositor_enabled_passes |= pass_type;
      }
    }
  }

  return viewport_compositor_enabled_passes;
}

void Film::init(const int2 &extent, const rcti *output_rect)
{
  using namespace math;

  Sampling &sampling = inst_.sampling;
  Scene &scene = *inst_.scene;

  if (inst_.is_viewport()) {
    /* Update detection of viewport setting. */
    const View3DShading &shading = inst_.v3d->shading;
    int update = 0;
    update += assign_if_different(ui_render_pass_, eViewLayerEEVEEPassType(shading.render_pass));
    update += assign_if_different(ui_aov_name_, std::string(shading.aov_name));
    if (update) {
      inst_.sampling.reset();
    }
  }

  this->depth = GPU_clip_control_support() ? DepthState{0.0f, DRW_STATE_DEPTH_GREATER_EQUAL} :
                                             DepthState{1.0f, DRW_STATE_DEPTH_LESS_EQUAL};

  /* Compute the passes needed by the viewport compositor. */
  Set<std::string> passes_used_by_viewport_compositor;
  if (inst_.is_viewport_compositor_enabled) {
    passes_used_by_viewport_compositor = bke::compositor::get_used_passes(scene, inst_.view_layer);
    viewport_compositor_enabled_passes_ = get_viewport_compositor_enabled_passes(
        passes_used_by_viewport_compositor, inst_.view_layer);
  }

  enabled_categories_ = PassCategory(0);
  init_aovs(passes_used_by_viewport_compositor);

  {
    /* Enable passes that need to be rendered. */
    if (inst_.is_viewport()) {
      /* Viewport Case. */
      eViewLayerEEVEEPassType enabled_passes = eViewLayerEEVEEPassType(
                                                   inst_.v3d->shading.render_pass) |
                                               viewport_compositor_enabled_passes_;

      if (inst_.overlays_enabled() || inst_.gpencil_engine_enabled()) {
        /* Overlays and Grease Pencil needs the depth for correct compositing.
         * Using the render pass ensure we store the center depth. */
        enabled_passes |= EEVEE_RENDER_PASS_DEPTH;
      }

      if (assign_if_different(enabled_passes_, enabled_passes)) {
        inst_.sampling.reset();
      }
    }
    else {
      /* Render Case. */
      enabled_passes_ = enabled_passes(inst_.view_layer);
    }

    /* Filter obsolete passes. */
    enabled_passes_ &= ~(EEVEE_RENDER_PASS_UNUSED_8 | EEVEE_RENDER_PASS_UNUSED_14);

    if (scene.r.mode & R_MBLUR) {
      /* Disable motion vector pass if motion blur is enabled. */
      enabled_passes_ &= ~EEVEE_RENDER_PASS_VECTOR;
    }
  }
  {
    data_.scaling_factor = 1;
    if (inst_.is_viewport()) {
      data_.scaling_factor = BKE_render_preview_pixel_size(&inst_.scene->r);
    }
    /* Sharpen the LODs (1.5x) to avoid TAA filtering causing over-blur (see #122941). */
    data_.texture_lod_bias = 1.0f / (data_.scaling_factor * 1.5f);
  }
  {
    rcti fallback_rect;
    if (BLI_rcti_is_empty(output_rect)) {
      BLI_rcti_init(&fallback_rect, 0, extent[0], 0, extent[1]);
      output_rect = &fallback_rect;
    }

    display_extent = extent;

    data_.extent = int2(BLI_rcti_size_x(output_rect), BLI_rcti_size_y(output_rect));
    data_.offset = int2(output_rect->xmin, output_rect->ymin);
    data_.extent_inv = 1.0f / float2(data_.extent);
    data_.render_extent = divide_ceil(data_.extent, int2(data_.scaling_factor));
    data_.overscan = overscan_pixels_get(inst_.camera.overscan(), data_.render_extent);
    data_.render_extent += data_.overscan * 2;

    is_valid_render_extent_ = data_.render_extent.x <= GPU_max_texture_size() &&
                              data_.render_extent.y <= GPU_max_texture_size();
    if (!is_valid_render_extent_) {
      inst_.info_append_i18n(
          "Required render size ({}px) is larger than reported texture size limit ({}px).",
          max_ii(data_.render_extent.x, data_.render_extent.y),
          GPU_max_texture_size());

      data_.extent = int2(4, 4);
      data_.render_extent = int2(4, 4);
      data_.extent_inv = 1.0f / float2(data_.extent);
      data_.offset = int2(0, 0);
      data_.overscan = 0;
    }

    data_.filter_radius = clamp_f(scene.r.gauss, 0.0f, 100.0f);
    if (sampling.sample_count() == 1) {
      /* Disable filtering if sample count is 1. */
      data_.filter_radius = 0.0f;
    }
    if (data_.scaling_factor > 1) {
      /* Fixes issue when using scaling factor and no filtering.
       * Without this, the filter becomes a dirac and samples gets only the fallback weight.
       * This results in a box blur instead of no filtering. */
      data_.filter_radius = math::max(data_.filter_radius, 0.0001f);
    }

    data_.cryptomatte_samples_len = inst_.view_layer->cryptomatte_levels;

    data_.background_opacity = (scene.r.alphamode == R_ALPHAPREMUL) ? 0.0f : 1.0f;
    if (inst_.is_viewport() && false /* TODO(fclem): StudioLight */) {
      data_.background_opacity = inst_.v3d->shading.studiolight_background;
    }

    const eViewLayerEEVEEPassType data_passes = EEVEE_RENDER_PASS_DEPTH |
                                                EEVEE_RENDER_PASS_NORMAL |
                                                EEVEE_RENDER_PASS_POSITION |
                                                EEVEE_RENDER_PASS_VECTOR;
    const eViewLayerEEVEEPassType color_passes_1 = EEVEE_RENDER_PASS_DIFFUSE_LIGHT |
                                                   EEVEE_RENDER_PASS_SPECULAR_LIGHT |
                                                   EEVEE_RENDER_PASS_VOLUME_LIGHT |
                                                   EEVEE_RENDER_PASS_EMIT;
    const eViewLayerEEVEEPassType color_passes_2 = EEVEE_RENDER_PASS_DIFFUSE_COLOR |
                                                   EEVEE_RENDER_PASS_SPECULAR_COLOR |
                                                   EEVEE_RENDER_PASS_ENVIRONMENT |
                                                   EEVEE_RENDER_PASS_MIST |
                                                   EEVEE_RENDER_PASS_SHADOW | EEVEE_RENDER_PASS_AO;
    const eViewLayerEEVEEPassType color_passes_3 = EEVEE_RENDER_PASS_TRANSPARENT;

    data_.exposure_scale = pow2f(scene.view_settings.exposure);
    if (enabled_passes_ & data_passes) {
      enabled_categories_ |= PASS_CATEGORY_DATA;
    }
    if (enabled_passes_ & color_passes_1) {
      enabled_categories_ |= PASS_CATEGORY_COLOR_1;
    }
    if (enabled_passes_ & color_passes_2) {
      enabled_categories_ |= PASS_CATEGORY_COLOR_2;
    }
    if (enabled_passes_ & color_passes_3) {
      enabled_categories_ |= PASS_CATEGORY_COLOR_3;
    }
  }
  {
    /* Set pass offsets. */

    data_.display_id = aovs_info.display_id;
    data_.display_storage_type = aovs_info.display_is_value ? PASS_STORAGE_VALUE :
                                                              PASS_STORAGE_COLOR;

    /* Combined is in a separate buffer. */
    data_.combined_id = (enabled_passes_ & EEVEE_RENDER_PASS_COMBINED) ? 0 : -1;
    /* Depth is in a separate buffer. */
    data_.depth_id = (enabled_passes_ & EEVEE_RENDER_PASS_DEPTH) ? 0 : -1;

    data_.color_len = 0;
    data_.value_len = 0;

    auto pass_index_get = [&](eViewLayerEEVEEPassType pass_type) {
      ePassStorageType storage_type = pass_storage_type(pass_type);
      int index = (enabled_passes_ & pass_type) ?
                      (storage_type == PASS_STORAGE_VALUE ? data_.value_len : data_.color_len)++ :
                      -1;
      if (inst_.is_viewport() && inst_.v3d->shading.render_pass == pass_type) {
        data_.display_id = index;
        data_.display_storage_type = storage_type;
      }
      return index;
    };

    data_.mist_id = pass_index_get(EEVEE_RENDER_PASS_MIST);
    data_.normal_id = pass_index_get(EEVEE_RENDER_PASS_NORMAL);
    data_.position_id = pass_index_get(EEVEE_RENDER_PASS_POSITION);
    data_.vector_id = pass_index_get(EEVEE_RENDER_PASS_VECTOR);
    data_.diffuse_light_id = pass_index_get(EEVEE_RENDER_PASS_DIFFUSE_LIGHT);
    data_.diffuse_color_id = pass_index_get(EEVEE_RENDER_PASS_DIFFUSE_COLOR);
    data_.specular_light_id = pass_index_get(EEVEE_RENDER_PASS_SPECULAR_LIGHT);
    data_.specular_color_id = pass_index_get(EEVEE_RENDER_PASS_SPECULAR_COLOR);
    data_.volume_light_id = pass_index_get(EEVEE_RENDER_PASS_VOLUME_LIGHT);
    data_.emission_id = pass_index_get(EEVEE_RENDER_PASS_EMIT);
    data_.environment_id = pass_index_get(EEVEE_RENDER_PASS_ENVIRONMENT);
    data_.shadow_id = pass_index_get(EEVEE_RENDER_PASS_SHADOW);
    data_.ambient_occlusion_id = pass_index_get(EEVEE_RENDER_PASS_AO);
    data_.transparent_id = pass_index_get(EEVEE_RENDER_PASS_TRANSPARENT);

    data_.aov_color_id = data_.color_len;
    data_.aov_value_id = data_.value_len;

    data_.aov_color_len = aovs_info.color_len;
    data_.aov_value_len = aovs_info.value_len;

    data_.color_len += data_.aov_color_len;
    data_.value_len += data_.aov_value_len;

    int cryptomatte_id = 0;
    auto cryptomatte_index_get = [&](eViewLayerEEVEEPassType pass_type) {
      int index = -1;
      if (enabled_passes_ & pass_type) {
        index = cryptomatte_id;
        cryptomatte_id += divide_ceil_u(data_.cryptomatte_samples_len, 2u);

        if (inst_.is_viewport() && inst_.v3d->shading.render_pass == pass_type) {
          data_.display_id = index;
          data_.display_storage_type = PASS_STORAGE_CRYPTOMATTE;
        }
      }
      return index;
    };
    data_.cryptomatte_object_id = cryptomatte_index_get(EEVEE_RENDER_PASS_CRYPTOMATTE_OBJECT);
    data_.cryptomatte_asset_id = cryptomatte_index_get(EEVEE_RENDER_PASS_CRYPTOMATTE_ASSET);
    data_.cryptomatte_material_id = cryptomatte_index_get(EEVEE_RENDER_PASS_CRYPTOMATTE_MATERIAL);

    if ((enabled_passes_ &
         (EEVEE_RENDER_PASS_CRYPTOMATTE_ASSET | EEVEE_RENDER_PASS_CRYPTOMATTE_MATERIAL |
          EEVEE_RENDER_PASS_CRYPTOMATTE_OBJECT)) != 0)
    {
      enabled_categories_ |= PASS_CATEGORY_CRYPTOMATTE;
    }
  }
  {
    int2 weight_extent = (inst_.camera.is_panoramic() || (data_.scaling_factor > 1)) ?
                             data_.extent :
                             int2(1);

    gpu::TextureFormat color_format = gpu::TextureFormat::SFLOAT_16_16_16_16;
    gpu::TextureFormat float_format = gpu::TextureFormat::SFLOAT_16;
    gpu::TextureFormat weight_format = gpu::TextureFormat::SFLOAT_32;
    gpu::TextureFormat depth_format = gpu::TextureFormat::SFLOAT_32;
    gpu::TextureFormat cryptomatte_format = gpu::TextureFormat::SFLOAT_32_32_32_32;

    int reset = 0;
    reset += depth_tx_.ensure_2d(depth_format, data_.extent);
    reset += combined_tx_.current().ensure_2d(color_format, data_.extent);
    reset += combined_tx_.next().ensure_2d(color_format, data_.extent);
    /* Two layers, one for nearest sample weight and one for weight accumulation. */
    reset += weight_tx_.current().ensure_2d_array(weight_format, weight_extent, 2);
    reset += weight_tx_.next().ensure_2d_array(weight_format, weight_extent, 2);
    reset += color_accum_tx_.ensure_2d_array(color_format,
                                             (data_.color_len > 0) ? data_.extent : int2(1),
                                             (data_.color_len > 0) ? data_.color_len : 1);
    reset += value_accum_tx_.ensure_2d_array(float_format,
                                             (data_.value_len > 0) ? data_.extent : int2(1),
                                             (data_.value_len > 0) ? data_.value_len : 1);
    /* Divided by two as two cryptomatte samples fit in pixel (RG, BA). */
    int cryptomatte_array_len = cryptomatte_layer_len_get() *
                                divide_ceil_u(data_.cryptomatte_samples_len, 2u);
    reset += cryptomatte_tx_.ensure_2d_array(cryptomatte_format,
                                             (cryptomatte_array_len > 0) ? data_.extent : int2(1),
                                             (cryptomatte_array_len > 0) ? cryptomatte_array_len :
                                                                           1);

    if (reset > 0) {
      data_.use_history = 0;
      use_reprojection_ = false;

      /* Avoid NaN in uninitialized texture memory making history blending dangerous. */
      color_accum_tx_.clear(float4(0.0f));
      value_accum_tx_.clear(float4(0.0f));
      combined_tx_.current().clear(float4(0.0f));
      weight_tx_.current().clear(float4(0.0f));
      depth_tx_.clear(float4(0.0f));
      cryptomatte_tx_.clear(float4(0.0f));
    }
  }
}

void Film::sync()
{
  /* We use a fragment shader for viewport because we need to output the depth.
   *
   * Compute shader is also used to work around Metal/Intel iGPU issues concerning
   * read write support for array textures. In this case the copy_ps_ is used to
   * copy the right color/value to the framebuffer. */
  use_compute_ = !inst_.is_viewport() ||
                 GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_MAC, GPU_DRIVER_ANY);

  eShaderType shader = use_compute_ ? FILM_COMP : FILM_FRAG;

  /* TODO(fclem): Shader variation for panoramic & scaled resolution. */

  gpu::Shader *sh = inst_.shaders.static_shader_get(shader);
  accumulate_ps_.init();
  init_pass(accumulate_ps_, sh);
  /* Sync with rendering passes. */
  accumulate_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  if (use_compute_) {
    accumulate_ps_.dispatch(int3(math::divide_ceil(data_.extent, int2(FILM_GROUP_SIZE)), 1));
  }
  else {
    accumulate_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  copy_ps_.init();
  if (use_compute_ && inst_.is_viewport()) {
    init_pass(copy_ps_, inst_.shaders.static_shader_get(FILM_COPY));
    copy_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  const int cryptomatte_layer_count = cryptomatte_layer_len_get();
  const bool is_cryptomatte_pass_enabled = cryptomatte_layer_count > 0;
  const bool do_cryptomatte_sorting = !inst_.is_viewport() || inst_.is_viewport_compositor_enabled;
  cryptomatte_post_ps_.init();
  if (is_cryptomatte_pass_enabled && do_cryptomatte_sorting) {
    cryptomatte_post_ps_.state_set(DRW_STATE_NO_DRAW);
    cryptomatte_post_ps_.shader_set(inst_.shaders.static_shader_get(FILM_CRYPTOMATTE_POST));
    cryptomatte_post_ps_.bind_image("cryptomatte_img", &cryptomatte_tx_);
    cryptomatte_post_ps_.bind_resources(inst_.uniform_data);
    cryptomatte_post_ps_.push_constant("cryptomatte_layer_len", cryptomatte_layer_count);
    cryptomatte_post_ps_.push_constant("cryptomatte_samples_per_layer",
                                       inst_.view_layer->cryptomatte_levels);
    int2 dispatch_size = math::divide_ceil(int2(cryptomatte_tx_.size()), int2(FILM_GROUP_SIZE));
    cryptomatte_post_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    cryptomatte_post_ps_.dispatch(int3(UNPACK2(dispatch_size), 1));
  }
}

void Film::init_pass(PassSimple &pass, gpu::Shader *sh)
{
  GPUSamplerState filter = {GPU_SAMPLER_FILTERING_LINEAR};
  RenderBuffers &rbuffers = inst_.render_buffers;
  VelocityModule &velocity = inst_.velocity;

  pass.specialize_constant(sh, "enabled_categories", uint(enabled_categories_));
  pass.specialize_constant(sh, "samples_len", &data_.samples_len);
  pass.specialize_constant(sh, "use_reprojection", &use_reprojection_);
  pass.specialize_constant(sh, "scaling_factor", data_.scaling_factor);
  pass.specialize_constant(sh, "combined_id", &data_.combined_id);
  pass.specialize_constant(sh, "display_id", &data_.display_id);
  pass.specialize_constant(sh, "normal_id", &data_.normal_id);
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS);
  pass.shader_set(sh);
  /* For viewport, only previous motion is supported.
   * Still bind previous step to avoid undefined behavior. */
  eVelocityStep step_next = inst_.is_viewport() ? STEP_PREVIOUS : STEP_NEXT;

  pass.bind_resources(inst_.uniform_data);
  pass.bind_ubo("camera_prev", &(*velocity.camera_steps[STEP_PREVIOUS]));
  pass.bind_ubo("camera_curr", &(*velocity.camera_steps[STEP_CURRENT]));
  pass.bind_ubo("camera_next", &(*velocity.camera_steps[step_next]));
  pass.bind_texture("depth_tx", &rbuffers.depth_tx);
  pass.bind_texture("combined_tx", &combined_final_tx_);
  pass.bind_texture("vector_tx", &rbuffers.vector_tx);
  pass.bind_texture("rp_color_tx", &rbuffers.rp_color_tx);
  pass.bind_texture("rp_value_tx", &rbuffers.rp_value_tx);
  pass.bind_texture("cryptomatte_tx", &rbuffers.cryptomatte_tx);
  /* NOTE(@fclem): 16 is the max number of sampled texture in many implementations.
   * If we need more, we need to pack more of the similar passes in the same textures as arrays or
   * use image binding instead. */
  pass.bind_image("in_weight_img", &weight_tx_.current());
  pass.bind_image("out_weight_img", &weight_tx_.next());
  pass.bind_texture("in_combined_tx", &combined_tx_.current(), filter);
  pass.bind_image("out_combined_img", &combined_tx_.next());
  pass.bind_image("depth_img", &depth_tx_);
  pass.bind_image("color_accum_img", &color_accum_tx_);
  pass.bind_image("value_accum_img", &value_accum_tx_);
  pass.bind_image("cryptomatte_img", &cryptomatte_tx_);
  pass.bind_resources(inst_.uniform_data);
}

void Film::end_sync()
{
  use_reprojection_ = inst_.sampling.interactive_mode();

  /* Just bypass the reprojection and reset the accumulation. */
  if (inst_.is_viewport() && !use_reprojection_ && inst_.sampling.is_reset()) {
    use_reprojection_ = false;
    data_.use_history = false;
  }

  aovs_info.push_update();

  sync_mist();

  /* Update sample table length for specialization warm up.
   * Otherwise, we will warm a specialization that is not actually used.
   * We still need to update it once per sample afterward. */
  update_sample_table();

  inst_.manager->warm_shader_specialization(accumulate_ps_);
  inst_.manager->warm_shader_specialization(copy_ps_);
  inst_.manager->warm_shader_specialization(cryptomatte_post_ps_);
}

float2 Film::pixel_jitter_get() const
{
  float2 jitter = inst_.sampling.rng_2d_get(SAMPLING_FILTER_U);

  if (!use_box_filter && data_.filter_radius < M_SQRT1_2 && !inst_.camera.is_panoramic()) {
    /* For filter size less than a pixel, change sampling strategy and use a uniform disk
     * distribution covering the filter shape. This avoids putting samples in areas without any
     * weights. */
    /* TODO(fclem): Importance sampling could be a better option here. */
    /* NOTE: We bias the disk to encompass most of the energy of the filter to avoid energy issues
     * with motion blur at low sample. */
    const float bias = 0.5f;
    jitter = Sampling::sample_disk(jitter) * bias * data_.filter_radius;
  }
  else {
    /* Jitter the size of a whole pixel. [-0.5..0.5] */
    jitter -= 0.5f;
  }

  if (data_.scaling_factor > 1) {
    /* In this case, the jitter sequence is the same for the number of film pixel a render pixel
     * covers. This allows to add a manual offset to the different film pixels to ensure they get
     * appropriate coverage instead of waiting that random sampling covers all the area. This
     * ensures a much faster convergence. */
    const int scale = data_.scaling_factor;
    const int render_pixel_per_final_pixel = square_i(scale);
    /* TODO(fclem): Random in Z-order curve. */
    /* Works great for the scaling factor we have. */
    int prime = (render_pixel_per_final_pixel / 2) - 1;
    /* For now just randomize in scan-lines using a prime number. */
    uint64_t index = (inst_.sampling.sample_index() * prime) % render_pixel_per_final_pixel;
    int2 pixel_co = int2(index % scale, index / scale);
    /* The jitter is applied on render target pixels. Make it proportional to film pixel. */
    jitter /= float(scale);
    /* Offset from the render pixel center to the center of film pixel. */
    jitter += ((float2(pixel_co) + 0.5f) / scale) - 0.5f;
  }
  return jitter;
}

eViewLayerEEVEEPassType Film::enabled_passes_get() const
{
  if (inst_.is_viewport() && use_reprojection_) {
    /* Enable motion vector rendering but not the accumulation buffer. */
    return enabled_passes_ | EEVEE_RENDER_PASS_VECTOR;
  }
  return enabled_passes_;
}

int Film::cryptomatte_layer_len_get() const
{
  int result = 0;
  result += data_.cryptomatte_object_id == -1 ? 0 : 1;
  result += data_.cryptomatte_asset_id == -1 ? 0 : 1;
  result += data_.cryptomatte_material_id == -1 ? 0 : 1;
  return result;
}

void Film::update_sample_table()
{
  /* Offset in render target pixels. */
  data_.subpixel_offset = pixel_jitter_get();

  int filter_radius_ceil = ceilf(data_.filter_radius);
  float filter_radius_sqr = square_f(data_.filter_radius);

  /* Reset */
  for (FilmSample &sample : data_.samples) {
    sample.texel = int2(0, 0);
    sample.weight = 0.0f;
  }

  data_.samples_len = 0;
  if (data_.scaling_factor > 1) {
    /* For this case there might be no valid samples for some pixels.
     * Still visit all four neighbors to have the best weight available.
     * Note that weight is computed on the GPU as it is different for each sample. */
    /* TODO(fclem): Make it work for filters larger than then scaling_factor. */
    for (int y = 0; y <= 1; y++) {
      for (int x = 0; x <= 1; x++) {
        FilmSample &sample = data_.samples[data_.samples_len];
        sample.texel = int2(x, y);
        sample.weight = -1.0f; /* Computed on GPU. */
        data_.samples_len++;
      }
    }
    data_.samples_weight_total = -1.0f; /* Computed on GPU. */
  }
  else if (use_box_filter || data_.filter_radius < 0.01f) {
    /* Disable gather filtering. */
    data_.samples[0].texel = int2(0, 0);
    data_.samples[0].weight = 1.0f;
    data_.samples_weight_total = 1.0f;
    data_.samples_len = 1;
  }
  /* NOTE: Threshold determined by hand until we don't hit the assert below. */
  else if (data_.filter_radius < 2.20f) {
    /* Small filter Size. */
    int closest_index = 0;
    float closest_distance = FLT_MAX;
    data_.samples_weight_total = 0.0f;
    /* TODO(fclem): For optimization, could try Z-tile ordering. */
    for (int y = -filter_radius_ceil; y <= filter_radius_ceil; y++) {
      for (int x = -filter_radius_ceil; x <= filter_radius_ceil; x++) {
        float2 pixel_offset = float2(x, y) - data_.subpixel_offset;
        float distance_sqr = math::length_squared(pixel_offset);
        if (distance_sqr < filter_radius_sqr) {
          if (data_.samples_len >= FILM_PRECOMP_SAMPLE_MAX) {
            BLI_assert_msg(0, "Precomputed sample table is too small.");
            break;
          }
          FilmSample &sample = data_.samples[data_.samples_len];
          sample.texel = int2(x, y);
          sample.weight = film_filter_weight(data_.filter_radius, distance_sqr);
          data_.samples_weight_total += sample.weight;

          if (distance_sqr < closest_distance) {
            closest_distance = distance_sqr;
            closest_index = data_.samples_len;
          }
          data_.samples_len++;
        }
      }
    }
    /* Put the closest one in first position. */
    if (closest_index != 0) {
      std::swap(data_.samples[closest_index], data_.samples[0]);
    }
    /* Avoid querying a different shader specialization for this case.
     * This can happen with the default settings. */
    if (data_.samples_len <= 9) {
      data_.samples_len = 9;
    }
  }
  else {
    /* Large Filter Size. */
    MutableSpan<FilmSample> sample_table(data_.samples, FILM_PRECOMP_SAMPLE_MAX);
    /* To avoid hitting driver TDR and slowing rendering too much we use random sampling. */
    /* TODO(fclem): This case needs more work. We could distribute the samples better to avoid
     * loading the same pixel twice. */
    data_.samples_len = sample_table.size();
    data_.samples_weight_total = 0.0f;

    int i = 0;
    for (FilmSample &sample : sample_table) {
      /* TODO(fclem): Own RNG. */
      float2 random_2d = inst_.sampling.rng_2d_get(SAMPLING_SSS_U);
      /* This randomization makes sure we converge to the right result but also makes nearest
       * neighbor filtering not converging rapidly. */
      random_2d.x = (random_2d.x + i) / float(FILM_PRECOMP_SAMPLE_MAX);

      float2 pixel_offset = math::floor(Sampling::sample_spiral(random_2d) * data_.filter_radius);
      sample.texel = int2(pixel_offset);

      float distance_sqr = math::length_squared(pixel_offset - data_.subpixel_offset);
      sample.weight = film_filter_weight(data_.filter_radius, distance_sqr);
      data_.samples_weight_total += sample.weight;
      i++;
    }
  }

  /* Round to specific amount of sample to avoid variation in sample count to cause stutter on
   * startup because of shader specialization. */
  if (data_.samples_len == 1) {
    data_.samples_len = 1;
  }
  else if (data_.samples_len <= 4) {
    data_.samples_len = 4;
  }
  else if (data_.samples_len <= 9) {
    data_.samples_len = 9;
  }
  else if (data_.samples_len <= 16) {
    data_.samples_len = 16;
  }
  else {
    BLI_assert_unreachable();
  }
}

void Film::accumulate(View &view, gpu::Texture *combined_final_tx)
{
  if (inst_.is_viewport()) {
    DefaultFramebufferList *dfbl = inst_.draw_ctx->viewport_framebuffer_list_get();
    DefaultTextureList *dtxl = inst_.draw_ctx->viewport_texture_list_get();
    GPU_framebuffer_bind(dfbl->default_fb);
    /* Clear when using render borders. */
    if (data_.extent != int2(GPU_texture_width(dtxl->color), GPU_texture_height(dtxl->color))) {
      float4 clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
      GPU_framebuffer_clear_color(dfbl->default_fb, clear_color);
    }
    GPU_framebuffer_viewport_set(dfbl->default_fb, UNPACK2(data_.offset), UNPACK2(data_.extent));
  }

  update_sample_table();

  combined_final_tx_ = combined_final_tx;

  data_.display_only = false;
  inst_.uniform_data.push_update();

  inst_.manager->submit(accumulate_ps_, view);
  inst_.manager->submit(copy_ps_, view);

  combined_tx_.swap();
  weight_tx_.swap();

  /* Use history after first sample. */
  if (data_.use_history == 0) {
    data_.use_history = 1;
  }
}

void Film::display()
{
  BLI_assert(inst_.is_viewport());

  /* Acquire dummy render buffers for correct binding. They will not be used. */
  inst_.render_buffers.acquire(int2(1));

  DefaultFramebufferList *dfbl = inst_.draw_ctx->viewport_framebuffer_list_get();
  GPU_framebuffer_bind(dfbl->default_fb);
  GPU_framebuffer_viewport_set(dfbl->default_fb, UNPACK2(data_.offset), UNPACK2(data_.extent));

  combined_final_tx_ = inst_.render_buffers.combined_tx;

  data_.display_only = true;
  inst_.uniform_data.push_update();

  draw::View &drw_view = draw::View::default_get();

  DRW_manager_get()->submit(accumulate_ps_, drw_view);

  inst_.render_buffers.release();

  /* IMPORTANT: Do not swap! No accumulation has happened. */
}

void Film::cryptomatte_sort()
{
  DRW_manager_get()->submit(cryptomatte_post_ps_);
}

float *Film::read_pass(eViewLayerEEVEEPassType pass_type, int layer_offset)
{
  gpu::Texture *pass_tx = this->get_pass_texture(pass_type, layer_offset);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  float *result = (float *)GPU_texture_read(pass_tx, GPU_DATA_FLOAT, 0);

  if (pass_is_float3(pass_type)) {
    /* Convert result in place as we cannot do this conversion on GPU. */
    for (const int px : IndexRange(GPU_texture_width(pass_tx) * GPU_texture_height(pass_tx))) {
      float3 tmp = *(reinterpret_cast<float3 *>(result + px * 4));
      *(reinterpret_cast<float3 *>(result) + px) = tmp;
    }
  }

  return result;
}

gpu::Texture *Film::get_pass_texture(eViewLayerEEVEEPassType pass_type, int layer_offset)
{
  ePassStorageType storage_type = pass_storage_type(pass_type);
  const bool is_value = storage_type == PASS_STORAGE_VALUE;
  const bool is_cryptomatte = storage_type == PASS_STORAGE_CRYPTOMATTE;

  Texture &accum_tx = (pass_type == EEVEE_RENDER_PASS_COMBINED) ?
                          combined_tx_.current() :
                      (pass_type == EEVEE_RENDER_PASS_DEPTH) ?
                          depth_tx_ :
                          (is_cryptomatte ? cryptomatte_tx_ :
                                            (is_value ? value_accum_tx_ : color_accum_tx_));

  int index = pass_id_get(pass_type);
  if (index == -1) {
    return nullptr;
  }

  accum_tx.ensure_layer_views();
  return accum_tx.layer_view(index + layer_offset);
}

/* Gets the appropriate shader to write the given pass type. This is because passes of different
 * types are stored in different textures types and formats. */
static eShaderType get_write_pass_shader_type(eViewLayerEEVEEPassType pass_type)
{
  switch (pass_type) {
    case EEVEE_RENDER_PASS_COMBINED:
      return FILM_PASS_CONVERT_COMBINED;
    case EEVEE_RENDER_PASS_DEPTH:
      return FILM_PASS_CONVERT_DEPTH;
    default:
      break;
  }

  switch (Film::pass_storage_type(pass_type)) {
    case PASS_STORAGE_VALUE:
      return FILM_PASS_CONVERT_VALUE;
    case PASS_STORAGE_COLOR:
      return FILM_PASS_CONVERT_COLOR;
    case PASS_STORAGE_CRYPTOMATTE:
      return FILM_PASS_CONVERT_CRYPTOMATTE;
  }

  return FILM_PASS_CONVERT_VALUE;
}

/* Gets the appropriate shader to write the given AOV pass. */
static eShaderType get_aov_write_pass_shader_type(const ViewLayerAOV *aov)
{
  switch (aov->type) {
    case AOV_TYPE_VALUE:
      return FILM_PASS_CONVERT_VALUE;
    case AOV_TYPE_COLOR:
      return FILM_PASS_CONVERT_COLOR;
  }

  return FILM_PASS_CONVERT_VALUE;
}

void Film::write_viewport_compositor_passes()
{
  this->cryptomatte_sort();

  /* Write standard passes. */
  for (const int i : IndexRange(EEVEE_RENDER_PASS_MAX_BIT + 1)) {
    const eViewLayerEEVEEPassType pass_type = eViewLayerEEVEEPassType(
        viewport_compositor_enabled_passes_ & (1 << i));
    if (pass_type == 0) {
      continue;
    }

    /* The compositor will use the viewport color texture as the combined pass because the viewport
     * texture will include Grease Pencil, so no need to write the combined pass from the engine
     * side. */
    if (pass_type == EEVEE_RENDER_PASS_COMBINED) {
      continue;
    }

    Vector<std::string> pass_names = Film::pass_to_render_pass_names(pass_type, inst_.view_layer);
    for (const int64_t pass_offset : IndexRange(pass_names.size())) {
      gpu::Texture *pass_texture = this->get_pass_texture(pass_type, pass_offset);
      if (!pass_texture) {
        continue;
      }

      /* Allocate passes that spans the entire display extent, even when border rendering, then
       * copy the border region while zeroing the rest. That's because the compositor doesn't have
       * a distinction between display and data windows at the moment, so it expects passes to have
       * the extent of the viewport. Furthermore, we still do not support passes from Cycles and
       * external engines, so the viewport size assumption holds at the compositor side to support
       * all cases for now. */
      const char *pass_name = pass_names[pass_offset].c_str();
      draw::TextureFromPool &output_pass_texture = DRW_viewport_pass_texture_get(pass_name);
      output_pass_texture.acquire(this->display_extent, GPU_texture_format(pass_texture));

      PassSimple write_pass_ps = {"Film.WriteViewportCompositorPass"};
      const eShaderType write_shader_type = get_write_pass_shader_type(pass_type);
      write_pass_ps.shader_set(inst_.shaders.static_shader_get(write_shader_type));
      write_pass_ps.push_constant("offset", data_.offset);
      write_pass_ps.bind_texture("input_tx", pass_texture);
      write_pass_ps.bind_image("output_img", output_pass_texture);
      write_pass_ps.barrier(GPU_BARRIER_TEXTURE_FETCH);
      write_pass_ps.dispatch(math::divide_ceil(this->display_extent, int2(FILM_GROUP_SIZE)));
      inst_.manager->submit(write_pass_ps);
    }
  }

  /* Write AOV passes. */
  LISTBASE_FOREACH (ViewLayerAOV *, aov, &inst_.view_layer->aovs) {
    if ((aov->flag & AOV_CONFLICT) != 0) {
      continue;
    }
    gpu::Texture *pass_texture = this->get_aov_texture(aov);
    if (!pass_texture) {
      continue;
    }

    /* See above comment regarding the allocation extent. */
    draw::TextureFromPool &output_pass_texture = DRW_viewport_pass_texture_get(aov->name);
    output_pass_texture.acquire(this->display_extent, GPU_texture_format(pass_texture));

    PassSimple write_pass_ps = {"Film.WriteViewportCompositorPass"};
    const eShaderType write_shader_type = get_aov_write_pass_shader_type(aov);
    write_pass_ps.shader_set(inst_.shaders.static_shader_get(write_shader_type));
    write_pass_ps.push_constant("offset", data_.offset);
    write_pass_ps.bind_texture("input_tx", pass_texture);
    write_pass_ps.bind_image("output_img", output_pass_texture);
    write_pass_ps.barrier(GPU_BARRIER_TEXTURE_FETCH);
    write_pass_ps.dispatch(math::divide_ceil(this->display_extent, int2(FILM_GROUP_SIZE)));
    inst_.manager->submit(write_pass_ps);
  }
}

/** \} */

}  // namespace blender::eevee
