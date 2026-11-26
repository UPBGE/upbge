/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_volume.hh"
#include "eevee_game_instance.hh"

#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "DNA_modifier_types.h"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_world_types.h"
#include "NOD_shader.h"

#include "BLI_function_ref.hh"
#include "BLI_math_geom.h"

#include "GPU_debug.hh"
#include "draw_common.hh" /* volume_sub_pass() */

#include "../eevee/eevee_shader.hh" /* World volume shader */

namespace blender::eevee_game {

/* -------------------------------------------------------------------- */
/** \name Constant media
 *
 * A volume node tree made of one volume node whose inputs are all left at
 * their values is the same medium everywhere: its coefficients can be read
 * here, on the CPU, with the formulas of the node GLSL. Anything else (links
 * into the node, textures, attributes, several nodes) needs the shader.
 * \{ */

static bNodeSocket *input_find(bNode *node, const char *identifier)
{
  for (bNodeSocket &sock : node->inputs) {
    if (STREQ(sock.identifier, identifier)) {
      return &sock;
    }
  }
  return nullptr;
}

/* Value of an unlinked input, false when linked or missing. */
static bool input_float(bNode *node, const char *identifier, float &r_value)
{
  bNodeSocket *sock = input_find(node, identifier);
  if (sock == nullptr || sock->link != nullptr || sock->default_value == nullptr) {
    return false;
  }
  r_value = static_cast<const bNodeSocketValueFloat *>(sock->default_value)->value;
  return true;
}

static bool input_color(bNode *node, const char *identifier, float3 &r_value)
{
  bNodeSocket *sock = input_find(node, identifier);
  if (sock == nullptr || sock->link != nullptr || sock->default_value == nullptr) {
    return false;
  }
  r_value = float3(static_cast<const bNodeSocketValueRGBA *>(sock->default_value)->value);
  return true;
}

/* Node feeding the volume output, through reroutes. */
static bNode *volume_source_node(bNodeTree *ntree)
{
  bNode *output = ntreeShaderOutputNode(ntree, SHD_OUTPUT_EEVEE);
  if (output == nullptr) {
    return nullptr;
  }
  bNodeSocket *sock = input_find(output, "Volume");
  while (sock != nullptr && sock->link != nullptr) {
    const bNodeLink *link = sock->link;
    if (link->flag & NODE_LINK_MUTED) {
      return nullptr;
    }
    bNode *node = link->fromnode;
    if (node->type_legacy != NODE_REROUTE) {
      return (node->flag & NODE_MUTED) ? nullptr : node;
    }
    sock = static_cast<bNodeSocket *>(node->inputs.first);
  }
  return nullptr;
}

/**
 * Coefficients of a constant volume node tree, written into r_box (the
 * matrix is left to the caller). `has_density_attribute` tells whether the
 * geometry provides the attribute named by the Principled Volume: when it
 * does, the density varies and the medium is not constant.
 */
static bool volume_constant_medium(bNodeTree *ntree,
                                   FunctionRef<bool(const char *)> has_attribute,
                                   VolumeBoxData &r_box)
{
  if (ntree == nullptr) {
    return false;
  }
  bNode *node = volume_source_node(ntree);
  if (node == nullptr) {
    return false;
  }
  /* A linked weight comes from a mix: not a single node any more. */
  if (const bNodeSocket *weight = input_find(node, "Weight"); weight && weight->link) {
    return false;
  }
  float3 scattering(0.0f), absorption(0.0f), emission(0.0f);
  float anisotropy = 0.0f;

  switch (node->type_legacy) {
    case SH_NODE_VOLUME_PRINCIPLED: {
      float3 color, absorption_color, emission_color;
      float density, emission_strength, blackbody_intensity;
      if (!input_color(node, "Color", color) || !input_float(node, "Density", density) ||
          !input_float(node, "Anisotropy", anisotropy) ||
          !input_color(node, "Absorption Color", absorption_color) ||
          !input_float(node, "Emission Strength", emission_strength) ||
          !input_color(node, "Emission Color", emission_color) ||
          !input_float(node, "Blackbody Intensity", blackbody_intensity))
      {
        return false;
      }
      /* Blackbody needs a temperature, which comes from an attribute. */
      if (blackbody_intensity > 1e-3f) {
        return false;
      }
      /* Missing attributes read 1: only a present one makes it vary. */
      for (const char *identifier : {"Density Attribute", "Color Attribute"}) {
        const bNodeSocket *attr = input_find(node, identifier);
        if (attr != nullptr && attr->default_value != nullptr) {
          const char *name =
              static_cast<const bNodeSocketValueString *>(attr->default_value)->value;
          if (name[0] != '\0' && has_attribute(name)) {
            return false;
          }
        }
      }
      /* node_volume_principled(), with the attributes at their default of 1. */
      color = math::max(color, float3(0.0f));
      density = math::max(density, 0.0f);
      if (density > 1e-5f) {
        scattering = color * density;
        const float3 absorption_sqrt = math::sqrt(math::max(absorption_color, float3(0.0f)));
        absorption = math::max(1.0f - color, float3(0.0f)) *
                     math::max(1.0f - absorption_sqrt, float3(0.0f)) * density;
      }
      if (emission_strength > 1e-5f) {
        emission = emission_strength * math::max(emission_color, float3(0.0f));
      }
      break;
    }
    case SH_NODE_VOLUME_SCATTER: {
      float3 color;
      float density;
      if (!input_color(node, "Color", color) || !input_float(node, "Density", density) ||
          !input_float(node, "Anisotropy", anisotropy))
      {
        return false;
      }
      /* node_volume_scatter(): EEVEE keeps the Henyey-Greenstein anisotropy
       * whatever the phase function. */
      scattering = math::max(color, float3(0.0f)) * math::max(density, 0.0f);
      break;
    }
    case SH_NODE_VOLUME_ABSORPTION: {
      float3 color;
      float density;
      if (!input_color(node, "Color", color) || !input_float(node, "Density", density)) {
        return false;
      }
      /* node_volume_absorption(). */
      absorption = (1.0f - math::max(color, float3(0.0f))) * math::max(density, 0.0f);
      break;
    }
    default:
      return false;
  }

  r_box.scattering = float4(scattering, anisotropy);
  r_box.absorption = float4(absorption, 0.0f);
  r_box.emission = float4(emission, 0.0f);
  return true;
}

/* Box a constant medium fills: the bounding box of the mesh, as in EEVEE Legacy (UPBGE 0.36),
 * whatever its faces. Exact for a box-shaped mesh; for any other shape the medium also fills
 * the space between the mesh and its box, which is what game scenes made for 0.36 expect (a
 * fog block is often a scaled cube, but also any closed or open shell). The voxelized path
 * follows the faces instead, and a mesh that is not watertight leaks there. */
static std::optional<Bounds<float3>> mesh_volume_bounds(const Mesh &mesh)
{
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  if (!bounds || math::reduce_min(bounds->max - bounds->min) <= 0.0f) {
    return std::nullopt;
  }
  return bounds;
}

bool VolumeModule::box_add(const Object *ob, const Mesh &mesh, bNodeTree *ntree)
{
  if (box_count_ >= VOLUME_BOX_MAX) {
    return false;
  }
  /* A smoke domain is a box too, but its density comes from the simulation
   * grids, which the volume shader reads as attributes. */
  if (BKE_modifiers_findby_type(ob, eModifierType_Fluid) != nullptr) {
    return false;
  }
  const std::optional<Bounds<float3>> bounds = mesh_volume_bounds(mesh);
  if (!bounds) {
    return false;
  }
  VolumeBoxData box;
  auto has_attribute = [&](const char *name) { return mesh.attributes().contains(name); };
  if (!volume_constant_medium(ntree, has_attribute, box)) {
    return false;
  }
  /* World to unit box: undo the object transform, center, then scale the
   * half extents to 1. */
  const float3 center = bounds->center();
  const float3 half_inv = 2.0f / (bounds->max - bounds->min);
  const float4x4 world_to_unit = math::from_scale<float4x4>(half_inv) *
                                 math::from_location<float4x4>(-center) *
                                 ob->world_to_object();
  /* Rows of the affine part: the matrix is stored by columns. */
  const float4x4 rows = math::transpose(world_to_unit);
  box.world_to_unit_x = rows[0];
  box.world_to_unit_y = rows[1];
  box.world_to_unit_z = rows[2];
  box_buf_[box_count_++] = box;
  return true;
}

/** \} */

VolumeModule::VolumeModule(GameInstance &inst) : inst_(&inst) {}

VolumeModule::~VolumeModule()
{
  GPU_BATCH_DISCARD_SAFE(cube_batch_);
}

void VolumeModule::init() {}

void VolumeModule::begin_sync()
{
  box_count_ = 0;
  layer_count_ = 0;
}

/* -------------------------------------------------------------------- */
/** \name Voxelized media
 * \{ */

template<typename PassT> void VolumeModule::properties_bind(PassT &pass)
{
  pass.bind_image(VOLUME_PROP_SCATTERING_IMG_SLOT, &prop_scattering_tx_);
  pass.bind_image(VOLUME_PROP_EXTINCTION_IMG_SLOT, &prop_extinction_tx_);
  pass.bind_image(VOLUME_PROP_EMISSION_IMG_SLOT, &prop_emission_tx_);
  pass.bind_image(VOLUME_PROP_PHASE_IMG_SLOT, &prop_phase_tx_);
  pass.bind_image(VOLUME_PROP_PHASE_WEIGHT_IMG_SLOT, &prop_phase_weight_tx_);
  pass.bind_image(VOLUME_OCCUPANCY_SLOT, &occupancy_tx_);
}

bool VolumeModule::object_add(Object *ob,
                              const blender::Material *blender_mat,
                              GPUMaterial *occupancy_mat,
                              GPUMaterial *volume_mat,
                              gpu::Batch *geom,
                              ResourceHandleRange res_handle)
{
  if (layer_count_ == layers_.size()) {
    layers_.append(std::make_unique<VoxelLayer>());
  }
  PassMain &ps = layers_[layer_count_]->ps;
  ps.init();
  /* The material pass lets one fragment per froxel column through. */
  ps.clear_stencil(0x0u);

  /* Volume objects and the fast method flip the occupancy bits at every
   * surface the ray crosses; the accurate one lists the hits and sorts them
   * front and back facing, which also handles non-manifold meshes. */
  const bool use_fast_method = (ob->type == OB_VOLUME) || (blender_mat == nullptr) ||
                               (blender_mat->volume_intersection_method ==
                                MA_VOLUME_ISECT_FAST);
  {
    /* Double sided, no depth test: every surface has to be counted. */
    PassMain::Sub &occupancy = ps.sub("Occupancy");
    occupancy.state_set(DRW_STATE_WRITE_DEPTH);
    inst_->pipelines.eevee_globals_bind(occupancy);
    occupancy.bind_image(VOLUME_OCCUPANCY_SLOT, &occupancy_tx_);
    occupancy.bind_image(VOLUME_HIT_DEPTH_SLOT, &hit_depth_tx_);
    occupancy.bind_image(VOLUME_HIT_COUNT_SLOT, &hit_count_tx_);
    PassMain::Sub &sub = occupancy.sub(GPU_material_get_name(occupancy_mat));
    sub.material_set(*inst_->manager, occupancy_mat, true, inst_->anisotropic_filtering);
    sub.push_constant("use_fast_method", use_fast_method);
    PassMain::Sub *draw = volume_sub_pass(sub, inst_->scene, ob, occupancy_mat);
    if (draw == nullptr) {
      return false;
    }
    draw->draw(geom, res_handle);
    if (!use_fast_method) {
      /* Sorted hits to bits, after the object drew its hits. */
      occupancy.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_OCCUPANCY_CONVERT));
      occupancy.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      occupancy.draw_procedural(GPU_PRIM_TRIS, 1, 3);
    }
  }
  {
    PassMain::Sub &material = ps.sub("Material");
    material.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_NEQUAL);
    material.state_stencil(0x1u, 0x1u, 0x1u);
    material.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    inst_->pipelines.eevee_globals_bind(material);
    properties_bind(material);
    PassMain::Sub &sub = material.sub(GPU_material_get_name(volume_mat));
    sub.material_set(*inst_->manager, volume_mat, true, inst_->anisotropic_filtering);
    PassMain::Sub *draw = volume_sub_pass(sub, inst_->scene, ob, volume_mat);
    if (draw == nullptr) {
      return false;
    }
    draw->draw(geom, res_handle);
  }
  /* Next object reads what this one added. */
  ps.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  layer_count_++;
  return true;
}

void VolumeModule::eevee_data_fill(eevee::VolumesInfoData &data) const
{
  if (!use_voxels_) {
    return;
  }
  const UniformData &ud = inst_->uniform_data;
  const int2 extent = inst_->film.render_extent_get();
  data.tex_size = grid_res_;
  data.inv_tex_size = 1.0f / float3(grid_res_);
  /* The grid spans exactly the render extent here (EEVEE pads it to whole
   * tiles): no XY rescale between screen and grid UVs. */
  data.coord_scale = float2(1.0f);
  data.main_view_extent = float2(extent);
  data.main_view_extent_inv = 1.0f / float2(extent);
  data.tile_size = settings_.tile_size;
  data.tile_size_lod = int(log2f(float(settings_.tile_size)));
  /* EEVEE maps a slice w in [0, 1] to view z = (2^(w / d) - near) / far.
   * near = 0, far = -1 / z_near and d = 1 / log2(z_far / z_near) give
   * |z| = z_near (z_far / z_near)^w: the slicing of the scatter, integration
   * and resolve passes. */
  data.depth_near = 0.0f;
  data.depth_far = -1.0f / ud.z_near;
  data.depth_distribution = 1.0f / log2f(ud.z_far / ud.z_near);
  /* The voxelization samples this frame's jittered positions. */
  data.winmat_finite = winmat_jittered_;
  data.wininv_finite = math::invert(winmat_jittered_);
  data.winmat_stable = ud.projectionmat;
  data.wininv_stable = math::invert(ud.projectionmat);
  data.history_winmat_stable = data.winmat_stable;
  data.curr_view_to_past_view = float4x4::identity();
  data.history_depth_near = data.depth_near;
  data.history_depth_far = data.depth_far;
  data.history_depth_distribution = data.depth_distribution;
  data.history_opacity = 0.0f;
  data.light_clamp = 1e20f;
  data.shadow_steps = 0.0f;
}

/** \} */

void VolumeModule::sync()
{
  /* A constant world medium is a box without bounds: the zero matrix maps
   * every point to the center. Attribute lookups have nothing to find. */
  World *world = inst_->scene->world;
  bool world_has_volume = false;
  if (world != nullptr && world->nodetree != nullptr) {
    world_has_volume = volume_source_node(world->nodetree) != nullptr;
    VolumeBoxData box = {};
    if (world_has_volume && box_count_ < VOLUME_BOX_MAX &&
        volume_constant_medium(world->nodetree, [](const char *) { return false; }, box))
    {
      box_buf_[box_count_++] = box;
      world_has_volume = false; /* Done analytically. */
    }
  }

  /* Any other world volume is voxelized: its shader writes every froxel,
   * which also stands for the clear of the property grids. */
  use_world_voxels_ = false;
  if (world_has_volume) {
    GPUMaterial *gpumat = eevee::ShaderModule::module_get()->world_shader_get(
        world, world->nodetree, eevee::MAT_PIPE_VOLUME_MATERIAL, true);
    if (GPU_material_status(gpumat) == GPU_MAT_SUCCESS &&
        GPU_material_has_volume_output(gpumat))
    {
      PassSimple &pass = world_ps_;
      pass.init();
      pass.state_set(DRW_STATE_WRITE_COLOR);
      inst_->pipelines.eevee_globals_bind(pass);
      properties_bind(pass);
      pass.material_set(*inst_->manager, gpumat, false, inst_->anisotropic_filtering);
      /* Default values of the attributes the world cannot have. */
      if (volume_sub_pass(pass, nullptr, nullptr, gpumat) != nullptr) {
        pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
        pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
        use_world_voxels_ = true;
      }
    }
  }

  use_voxels_ = use_world_voxels_ || (layer_count_ > 0);
  enabled_ = (box_count_ > 0) || use_voxels_;
  if (!enabled_) {
    /* The grids stop following the camera. */
    history_valid_ = false;
    return;
  }
  box_buf_.push_update();

  const int2 render_res = inst_->film.render_extent_get();

  /* Froxel grid dimensions derived from render resolution and tile size.
   * XY: screen tiles of tile_size pixels each.
   * Z: exponentially-spaced depth slices (more near, fewer far). */
  grid_res_ = int3(math::divide_ceil(render_res, int2(settings_.tile_size)), settings_.samples_z);

  /* RGBA16F stores: RGB = scattering, A = extinction.
   * 16F is sufficient for fog density; avoids the 2x bandwidth cost of RGBA32F. */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ;
  bool created = false;
  for (int i = 0; i < 2; i++) {
    created |= volume_grid_tx_.current().ensure_3d(
        gpu::TextureFormat::SFLOAT_16_16_16_16, grid_res_, usage);
    /* Extinction and transmittance per channel. R11G11B10 like EEVEE: both
     * are positive, and transmittance lives in [0, 1]. */
    created |= volume_extinction_tx_.current().ensure_3d(
        gpu::TextureFormat::UFLOAT_11_11_10, grid_res_, usage);
    volume_grid_tx_.swap();
    volume_extinction_tx_.swap();
  }
  volume_integrated_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16_16_16_16, grid_res_, usage);
  volume_transmittance_tx_.ensure_3d(gpu::TextureFormat::UFLOAT_11_11_10, grid_res_, usage);
  history_valid_ &= !created && inst_->frame_state.history_valid;

  /* Sub froxel jitter of this frame: Halton (2, 3) in xy, within half a
   * froxel each way in NDC, and the golden ratio sequence offsetting the per
   * froxel depth noise. Low discrepancy, so the 8 frames of history spread
   * over the froxel instead of clumping. Wrapped at 1024 frames, far more
   * than the history keeps: the fraction of frame * 0.618 in float loses
   * bits as the frame count grows. */
  const uint32_t frame = inst_->frame_state.frame_index % 1024u;
  auto radical_inverse = [](uint32_t i, uint32_t base) {
    float inv = 1.0f / float(base), f = inv, r = 0.0f;
    for (; i > 0; i /= base, f *= inv) {
      r += f * float(i % base);
    }
    return r;
  };
  jitter_ = float3((radical_inverse(frame, 2) * 2.0f - 1.0f) / float(grid_res_.x),
                   (radical_inverse(frame, 3) * 2.0f - 1.0f) / float(grid_res_.y),
                   math::fract(float(frame) * 0.618034f));
  /* Pre-multiplied clip space translation: moves NDC by jitter for any
   * projection. */
  winmat_jittered_ = inst_->uniform_data.projectionmat;
  for (int c = 0; c < 4; c++) {
    winmat_jittered_[c][0] += jitter_.x * winmat_jittered_[c][3];
    winmat_jittered_[c][1] += jitter_.y * winmat_jittered_[c][3];
  }

  if (dummy_prop_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16_16_16_16, int3(1), usage)) {
    dummy_prop_tx_.clear(float4(0.0f));
  }
  scatter_scattering_tx_ = dummy_prop_tx_;
  scatter_extinction_tx_ = dummy_prop_tx_;
  scatter_emission_tx_ = dummy_prop_tx_;
  scatter_phase_tx_ = dummy_prop_tx_;
  scatter_phase_weight_tx_ = dummy_prop_tx_;
  if (!use_voxels_) {
    /* Kept once allocated: objects entering and leaving the view would
     * otherwise reallocate them back and forth. */
    return;
  }

  /* EEVEE's formats: the volume shaders declare them. */
  const eGPUTextureUsage prop_usage = usage | GPU_TEXTURE_USAGE_ATTACHMENT;
  prop_scattering_tx_.ensure_3d(gpu::TextureFormat::UFLOAT_11_11_10, grid_res_, prop_usage);
  prop_extinction_tx_.ensure_3d(gpu::TextureFormat::UFLOAT_11_11_10, grid_res_, prop_usage);
  prop_emission_tx_.ensure_3d(gpu::TextureFormat::UFLOAT_11_11_10, grid_res_, prop_usage);
  prop_phase_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16, grid_res_, prop_usage);
  prop_phase_weight_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16, grid_res_, prop_usage);
  scatter_scattering_tx_ = prop_scattering_tx_;
  scatter_extinction_tx_ = prop_extinction_tx_;
  scatter_emission_tx_ = prop_emission_tx_;
  scatter_phase_tx_ = prop_phase_tx_;
  scatter_phase_weight_tx_ = prop_phase_weight_tx_;

  /* 32 slices per texel layer. */
  const int occupancy_layers = (grid_res_.z + 31) / 32;
  occupancy_tx_.ensure_3d(gpu::TextureFormat::UINT_32,
                          int3(grid_res_.xy(), occupancy_layers),
                          usage | GPU_TEXTURE_USAGE_ATOMIC);
  /* Hit lists: EEVEE's ray depth setting caps the surfaces kept per column. */
  const int hit_layers = clamp_i(inst_->scene->eevee.volumetric_ray_depth, 1, 16);
  hit_depth_tx_.ensure_3d(
      gpu::TextureFormat::SFLOAT_32, int3(grid_res_.xy(), hit_layers), usage);
  if (hit_count_tx_.ensure_2d(
          gpu::TextureFormat::UINT_32, grid_res_.xy(), usage | GPU_TEXTURE_USAGE_ATOMIC))
  {
    /* The convert pass resets the counts it reads. */
    hit_count_tx_.clear(uint4(0u));
  }
  front_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                            grid_res_.xy(),
                            GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT);
  occupancy_fb_.ensure(GPU_ATTACHMENT_TEXTURE(front_depth_tx_));
}

void VolumeModule::render(View & /*view*/)
{
  if (!enabled_) {
    return;
  }

  const UniformData &ud = inst_->uniform_data;
  /* Last frame's result becomes the history. */
  volume_grid_tx_.swap();
  volume_extinction_tx_.swap();
  const int3 grid_size = volume_grid_tx_.current().size();

  if (use_voxels_) {
    /* Properties of the voxelized media, rasterized at froxel resolution
     * through an infinite projection of the same frustum, jittered as the
     * samples of the scatter pass. */
    float left, right, bottom, top, near, far;
    projmat_dimensions(ud.projectionmat.ptr(), &left, &right, &bottom, &top, &near, &far);
    const bool is_persp = ud.projectionmat[3][3] == 0.0f;
    float4x4 winmat = is_persp ? math::projection::perspective_infinite(
                                     left, right, bottom, top, near) :
                                 math::projection::orthographic_infinite(
                                     left, right, bottom, top, near);
    /* Same jitter as winmat_jittered_. */
    for (int c = 0; c < 4; c++) {
      winmat[c][0] += jitter_.x * winmat[c][3];
      winmat[c][1] += jitter_.y * winmat[c][3];
    }
    volume_view_.sync(ud.viewmat, winmat);
    /* Culling against an infinite frustum is unreliable; EEVEE skips it too. */
    volume_view_.visibility_test(false);

    GPU_debug_group_begin("Volume.Properties");
    occupancy_fb_.bind();
    if (use_world_voxels_) {
      inst_->manager->submit(world_ps_, volume_view_);
    }
    else {
      prop_scattering_tx_.clear(float4(0.0f));
      prop_extinction_tx_.clear(float4(0.0f));
      prop_emission_tx_.clear(float4(0.0f));
      prop_phase_tx_.clear(float4(0.0f));
      prop_phase_weight_tx_.clear(float4(0.0f));
    }
    for (int i = 0; i < layer_count_; i++) {
      occupancy_tx_.clear(uint4(0u));
      inst_->manager->submit(layers_[i]->ps, volume_view_);
    }
    GPU_debug_group_end();
  }

  /* Pass 1: Light Injection (Scatter)
   * Each froxel samples the static shadow atlas (sun cascades) to determine
   * how much directional light reaches that point. The Henyey-Greenstein
   * phase function biases scattering toward the light direction. */
  PassSimple &scatter = volume_scatter_ps_;
  scatter.init();
  scatter.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_SCATTER));
  /* Lights of the tiles and shadows: culled by the lighting passes earlier
   * in the frame. */
  inst_->pipelines.light_data_bind(scatter);
  scatter.bind_image("out_grid_img", volume_grid_tx_.current());
  scatter.bind_image("out_extinction_img", volume_extinction_tx_.current());
  /* History, reprojected with the previous unjittered camera. Linear filter,
   * clamped: reprojections out of the grid are rejected in the shader. */
  const GPUSamplerState history_filter = {GPU_SAMPLER_FILTERING_LINEAR};
  scatter.bind_texture("scatter_history_tx", volume_grid_tx_.previous(), history_filter);
  scatter.bind_texture("extinction_history_tx", volume_extinction_tx_.previous(), history_filter);
  scatter.push_constant("history_opacity", history_valid_ ? history_opacity_ : 0.0f);
  scatter.push_constant("history_viewmat", history_viewmat_);
  scatter.push_constant("history_viewproj", history_viewproj_);
  scatter.push_constant("history_z_near", history_z_near_);
  scatter.push_constant("history_z_far", history_z_far_);
  scatter.push_constant("jitter", jitter_);
  scatter.bind_ssbo("volume_box_buf", &box_buf_);
  scatter.push_constant("volume_box_count", box_count_);
  scatter.push_constant("use_voxels", int(use_voxels_));
  /* The grid splits [-1, 1] NDC in grid_size cells: a half cell is
   * 1 / grid in NDC, depth / (grid P00) in view space (perspective). */
  scatter.push_constant("froxel_half_tan",
                        float2(1.0f / (float(grid_size.x) * ud.projectionmat[0][0]),
                               1.0f / (float(grid_size.y) * ud.projectionmat[1][1])));
  scatter.bind_texture("prop_scattering_tx", &scatter_scattering_tx_);
  scatter.bind_texture("prop_extinction_tx", &scatter_extinction_tx_);
  scatter.bind_texture("prop_emission_tx", &scatter_emission_tx_);
  scatter.bind_texture("prop_phase_tx", &scatter_phase_tx_);
  scatter.bind_texture("prop_phase_weight_tx", &scatter_phase_weight_tx_);
  scatter.push_constant("tile_size", settings_.tile_size);
  scatter.push_constant("samples_z", settings_.samples_z);
  scatter.push_constant("viewmat", ud.viewmat);
  scatter.push_constant("viewprojinv", math::invert(ud.viewprojmat));
  scatter.push_constant("z_near", ud.z_near);
  scatter.push_constant("z_far", ud.z_far);
  scatter.push_constant("screen_res", ud.screen_res);
  /* One thread per column: Z is the shader's loop, not a grid dimension. */
  scatter.dispatch(int3(math::divide_ceil(grid_size.xy(), int2(8)), 1));
  scatter.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(scatter);

  /* Pass 2: Ray Integration along Z
   * One thread per XY froxel column, accumulating front to back. Only XY is
   * dispatched: the shader walks Z itself so each thread reads its column
   * sequentially. */
  PassSimple &integrate = volume_integration_ps_;
  integrate.init();
  integrate.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_INTEGRATE));
  integrate.bind_texture("in_grid_tx", volume_grid_tx_.current());
  integrate.bind_texture("in_extinction_tx", volume_extinction_tx_.current());
  integrate.bind_image("out_integrated_img", volume_integrated_tx_);
  integrate.bind_image("out_transmittance_img", volume_transmittance_tx_);
  integrate.push_constant("z_near", ud.z_near);
  integrate.push_constant("z_far", ud.z_far);
  integrate.push_constant("samples_z", settings_.samples_z);
  integrate.dispatch(int3(math::divide_ceil(grid_size.xy(), int2(8)), 1));
  integrate.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(integrate);

  /* Unjittered: the history is looked up at froxel centers. */
  history_viewmat_ = ud.viewmat;
  history_viewproj_ = ud.viewprojmat;
  history_z_near_ = ud.z_near;
  history_z_far_ = ud.z_far;
  history_valid_ = true;
}

void VolumeModule::resolve(gpu::Texture *target_color_tx, gpu::Texture *depth_tx)
{
  if (!enabled_) {
    return;
  }

  /* Full-screen composite of the integrated froxel grid:
   *   1. Depth gives the view-space ray length per pixel.
   *   2. Pixel + depth map to froxel UVW in [0,1]^3.
   *   3. Sample: RGB = in-scattered radiance, A = transmittance.
   *   4. out_color = in_color * transmittance + in_scatter. */
  const UniformData &ud = inst_->uniform_data;
  const int2 render_res = inst_->film.render_extent_get();

  PassSimple &pass = volume_resolve_ps_;
  pass.init();
  pass.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_RESOLVE));
  pass.bind_texture("volume_integrated_tx", volume_integrated_tx_);
  pass.bind_texture("volume_transmittance_tx", volume_transmittance_tx_);
  pass.bind_texture("depth_tx", depth_tx);
  pass.bind_image("out_color_img", target_color_tx);
  pass.push_constant("z_near", ud.z_near);
  pass.push_constant("z_far", ud.z_far);
  pass.push_constant("tile_size", settings_.tile_size);
  pass.push_constant("samples_z", settings_.samples_z);
  pass.push_constant("screen_res", float2(render_res));
  pass.dispatch(math::divide_ceil(render_res, int2(8)));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(pass);
}

void VolumeModule::object_sync(const ObjectHandle & /*ob_handle*/)
{
  /* The froxel grid covers the whole camera frustum with a homogeneous
   * medium, so there is nothing to register per object yet. Kept as the hook
   * for per-object voxelisation of volume materials. */
}

} // namespace blender::eevee_game
