/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_volume_types.h"

#include "UI_resources.hh"

#include "BLI_ghash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_object.hh"

#include "GPU_batch.hh"
#include "GPU_batch_utils.hh"
#include "GPU_capabilities.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"
#include "draw_manager_c.hh"

using blender::Span;

/* -------------------------------------------------------------------- */
/** \name Internal Defines
 * \{ */

#define VCLASS_LIGHT_AREA_SHAPE (1 << 0)
#define VCLASS_LIGHT_SPOT_SHAPE (1 << 1)
#define VCLASS_LIGHT_SPOT_BLEND (1 << 2)
#define VCLASS_LIGHT_SPOT_CONE (1 << 3)
#define VCLASS_LIGHT_DIST (1 << 4)

#define VCLASS_CAMERA_FRAME (1 << 5)
#define VCLASS_CAMERA_DIST (1 << 6)
#define VCLASS_CAMERA_VOLUME (1 << 7)

#define VCLASS_SCREENSPACE (1 << 8)
#define VCLASS_SCREENALIGNED (1 << 9)

#define VCLASS_EMPTY_SCALED (1 << 10)
#define VCLASS_EMPTY_AXES (1 << 11)
#define VCLASS_EMPTY_AXES_NAME (1 << 12)
#define VCLASS_EMPTY_AXES_SHADOW (1 << 13)
#define VCLASS_EMPTY_SIZE (1 << 14)

/* Sphere shape resolution */
/* Low */
#define DRW_SPHERE_SHAPE_LATITUDE_LOW 32
#define DRW_SPHERE_SHAPE_LONGITUDE_LOW 24
/* Medium */
#define DRW_SPHERE_SHAPE_LATITUDE_MEDIUM 64
#define DRW_SPHERE_SHAPE_LONGITUDE_MEDIUM 48
/* High */
#define DRW_SPHERE_SHAPE_LATITUDE_HIGH 80
#define DRW_SPHERE_SHAPE_LONGITUDE_HIGH 60

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Types
 * \{ */

struct Vert {
  float pos[3];
  int v_class;

  /** Allows creating a pointer to `Vert` in a single expression. */
  operator const void *() const
  {
    return this;
  }
};

struct VertShaded {
  float pos[3];
  int v_class;
  float nor[3];

  operator const void *() const
  {
    return this;
  }
};

/* Batch's only (freed as an array). */
static struct DRWShapeCache {
  blender::gpu::Batch *drw_procedural_verts;
  blender::gpu::Batch *drw_procedural_lines;
  blender::gpu::Batch *drw_procedural_tris;
  blender::gpu::Batch *drw_procedural_tri_strips;
  blender::gpu::Batch *drw_cursor;
  blender::gpu::Batch *drw_cursor_only_circle;
  blender::gpu::Batch *drw_fullscreen_quad;
  blender::gpu::Batch *drw_quad;
  blender::gpu::Batch *drw_quad_wires;
  blender::gpu::Batch *drw_grid;
  blender::gpu::Batch *drw_plain_axes;
  blender::gpu::Batch *drw_single_arrow;
  blender::gpu::Batch *drw_cube;
  blender::gpu::Batch *drw_circle;
  blender::gpu::Batch *drw_normal_arrow;
  blender::gpu::Batch *drw_empty_cube;
  blender::gpu::Batch *drw_empty_sphere;
  blender::gpu::Batch *drw_empty_cylinder;
  blender::gpu::Batch *drw_empty_capsule_body;
  blender::gpu::Batch *drw_empty_capsule_cap;
  blender::gpu::Batch *drw_empty_cone;
  blender::gpu::Batch *drw_field_wind;
  blender::gpu::Batch *drw_field_force;
  blender::gpu::Batch *drw_field_vortex;
  blender::gpu::Batch *drw_field_curve;
  blender::gpu::Batch *drw_field_tube_limit;
  blender::gpu::Batch *drw_field_cone_limit;
  blender::gpu::Batch *drw_field_sphere_limit;
  blender::gpu::Batch *drw_ground_line;
  blender::gpu::Batch *drw_light_icon_inner_lines;
  blender::gpu::Batch *drw_light_icon_outer_lines;
  blender::gpu::Batch *drw_light_icon_sun_rays;
  blender::gpu::Batch *drw_light_point_lines;
  blender::gpu::Batch *drw_light_sun_lines;
  blender::gpu::Batch *drw_light_spot_lines;
  blender::gpu::Batch *drw_light_spot_volume;
  blender::gpu::Batch *drw_light_area_disk_lines;
  blender::gpu::Batch *drw_light_area_square_lines;
  blender::gpu::Batch *drw_speaker;
  blender::gpu::Batch *drw_lightprobe_cube;
  blender::gpu::Batch *drw_lightprobe_planar;
  blender::gpu::Batch *drw_lightprobe_grid;
  blender::gpu::Batch *drw_bone_octahedral;
  blender::gpu::Batch *drw_bone_octahedral_wire;
  blender::gpu::Batch *drw_bone_box;
  blender::gpu::Batch *drw_bone_box_wire;
  blender::gpu::Batch *drw_bone_envelope;
  blender::gpu::Batch *drw_bone_envelope_outline;
  blender::gpu::Batch *drw_bone_point;
  blender::gpu::Batch *drw_bone_point_wire;
  blender::gpu::Batch *drw_bone_stick;
  blender::gpu::Batch *drw_bone_arrows;
  blender::gpu::Batch *drw_bone_dof_sphere;
  blender::gpu::Batch *drw_bone_dof_lines;
  blender::gpu::Batch *drw_camera_frame;
  blender::gpu::Batch *drw_camera_tria;
  blender::gpu::Batch *drw_camera_tria_wire;
  blender::gpu::Batch *drw_camera_distances;
  blender::gpu::Batch *drw_camera_volume;
  blender::gpu::Batch *drw_camera_volume_wire;
  blender::gpu::Batch *drw_particle_cross;
  blender::gpu::Batch *drw_particle_circle;
  blender::gpu::Batch *drw_particle_axis;
  blender::gpu::Batch *drw_gpencil_dummy_quad;
  blender::gpu::Batch *drw_sphere_lod[DRW_LOD_MAX];
} SHC = {nullptr};

void DRW_shape_cache_free()
{
  uint i = sizeof(SHC) / sizeof(blender::gpu::Batch *);
  blender::gpu::Batch **batch = (blender::gpu::Batch **)&SHC;
  while (i--) {
    GPU_BATCH_DISCARD_SAFE(*batch);
    batch++;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Procedural Batches
 * \{ */

blender::gpu::Batch *drw_cache_procedural_points_get()
{
  if (!SHC.drw_procedural_verts) {
    /* TODO(fclem): get rid of this dummy VBO. */
    GPUVertFormat format = {0};
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 1);

    SHC.drw_procedural_verts = GPU_batch_create_ex(
        GPU_PRIM_POINTS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_procedural_verts;
}

blender::gpu::Batch *drw_cache_procedural_lines_get()
{
  if (!SHC.drw_procedural_lines) {
    /* TODO(fclem): get rid of this dummy VBO. */
    GPUVertFormat format = {0};
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 1);

    SHC.drw_procedural_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_procedural_lines;
}

blender::gpu::Batch *drw_cache_procedural_triangles_get()
{
  if (!SHC.drw_procedural_tris) {
    /* TODO(fclem): get rid of this dummy VBO. */
    GPUVertFormat format = {0};
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 1);

    SHC.drw_procedural_tris = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_procedural_tris;
}

blender::gpu::Batch *drw_cache_procedural_triangle_strips_get()
{
  if (!SHC.drw_procedural_tri_strips) {
    /* TODO(fclem): get rid of this dummy VBO. */
    GPUVertFormat format = {0};
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 1);

    SHC.drw_procedural_tri_strips = GPU_batch_create_ex(
        GPU_PRIM_TRI_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_procedural_tri_strips;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper functions
 * \{ */

static GPUVertFormat extra_vert_format()
{
  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  GPU_vertformat_attr_add(&format, "vclass", GPU_COMP_I32, 1, GPU_FETCH_INT);
  return format;
}

static void UNUSED_FUNCTION(add_fancy_edge)(blender::gpu::VertBuf *vbo,
                                            uint pos_id,
                                            uint n1_id,
                                            uint n2_id,
                                            uint *v_idx,
                                            const float co1[3],
                                            const float co2[3],
                                            const float n1[3],
                                            const float n2[3])
{
  GPU_vertbuf_attr_set(vbo, n1_id, *v_idx, n1);
  GPU_vertbuf_attr_set(vbo, n2_id, *v_idx, n2);
  GPU_vertbuf_attr_set(vbo, pos_id, (*v_idx)++, co1);

  GPU_vertbuf_attr_set(vbo, n1_id, *v_idx, n1);
  GPU_vertbuf_attr_set(vbo, n2_id, *v_idx, n2);
  GPU_vertbuf_attr_set(vbo, pos_id, (*v_idx)++, co2);
}

#if 0  /* UNUSED */
static void add_lat_lon_vert(blender::gpu::VertBuf *vbo,
                             uint pos_id,
                             uint nor_id,
                             uint *v_idx,
                             const float rad,
                             const float lat,
                             const float lon)
{
  float pos[3], nor[3];
  nor[0] = sinf(lat) * cosf(lon);
  nor[1] = cosf(lat);
  nor[2] = sinf(lat) * sinf(lon);
  mul_v3_v3fl(pos, nor, rad);

  GPU_vertbuf_attr_set(vbo, nor_id, *v_idx, nor);
  GPU_vertbuf_attr_set(vbo, pos_id, (*v_idx)++, pos);
}

static blender::gpu::VertBuf *fill_arrows_vbo(const float scale)
{
  /* Position Only 3D format */
  static GPUVertFormat format = {0};
  static struct {
    uint pos;
  } attr_id;
  if (format.attr_len == 0) {
    attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  }

  /* Line */
  blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, 6 * 3);

  float v1[3] = {0.0, 0.0, 0.0};
  float v2[3] = {0.0, 0.0, 0.0};
  float vtmp1[3], vtmp2[3];

  for (int axis = 0; axis < 3; axis++) {
    const int arrow_axis = (axis == 0) ? 1 : 0;

    v2[axis] = 1.0f;
    mul_v3_v3fl(vtmp1, v1, scale);
    mul_v3_v3fl(vtmp2, v2, scale);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 0, vtmp1);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 1, vtmp2);

    v1[axis] = 0.85f;
    v1[arrow_axis] = -0.08f;
    mul_v3_v3fl(vtmp1, v1, scale);
    mul_v3_v3fl(vtmp2, v2, scale);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 2, vtmp1);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 3, vtmp2);

    v1[arrow_axis] = 0.08f;
    mul_v3_v3fl(vtmp1, v1, scale);
    mul_v3_v3fl(vtmp2, v2, scale);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 4, vtmp1);
    GPU_vertbuf_attr_set(vbo, attr_id.pos, axis * 6 + 5, vtmp2);

    /* reset v1 & v2 to zero */
    v1[arrow_axis] = v1[axis] = v2[axis] = 0.0f;
  }

  return vbo;
}
#endif /* UNUSED */

static blender::gpu::VertBuf *sphere_wire_vbo(const float rad, int flag)
{
#define NSEGMENTS 32
  /* Position Only 3D format */
  GPUVertFormat format = extra_vert_format();

  blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, NSEGMENTS * 2 * 3);

  int v = 0;
  /* a single ring of vertices */
  float p[NSEGMENTS][2];
  for (int i = 0; i < NSEGMENTS; i++) {
    float angle = 2 * M_PI * (float(i) / float(NSEGMENTS));
    p[i][0] = rad * cosf(angle);
    p[i][1] = rad * sinf(angle);
  }

  for (int axis = 0; axis < 3; axis++) {
    for (int i = 0; i < NSEGMENTS; i++) {
      for (int j = 0; j < 2; j++) {
        float cv[2];

        cv[0] = p[(i + j) % NSEGMENTS][0];
        cv[1] = p[(i + j) % NSEGMENTS][1];

        if (axis == 0) {
          GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], cv[1], 0.0f}, flag});
        }
        else if (axis == 1) {
          GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], 0.0f, cv[1]}, flag});
        }
        else {
          GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, cv[0], cv[1]}, flag});
        }
      }
    }
  }

  return vbo;
#undef NSEGMENTS
}

/* Quads */

blender::gpu::Batch *DRW_cache_fullscreen_quad_get()
{
  if (!SHC.drw_fullscreen_quad) {
    /* Use a triangle instead of a real quad */
    /* https://www.slideshare.net/DevCentralAMD/vertex-shader-tricks-bill-bilodeau - slide 14 */
    const float pos[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
    const float uvs[3][2] = {{0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 2.0f}};

    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos, uvs;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      attr_id.uvs = GPU_vertformat_attr_add(&format, "uvs", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      GPU_vertformat_alias_add(&format, "texCoord");
      GPU_vertformat_alias_add(&format, "orco"); /* Fix driver bug (see #70004) */
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 3);

    for (int i = 0; i < 3; i++) {
      GPU_vertbuf_attr_set(vbo, attr_id.pos, i, pos[i]);
      GPU_vertbuf_attr_set(vbo, attr_id.uvs, i, uvs[i]);
    }

    SHC.drw_fullscreen_quad = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_fullscreen_quad;
}

blender::gpu::Batch *DRW_cache_quad_get()
{
  if (!SHC.drw_quad) {
    GPUVertFormat format = extra_vert_format();

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 4);

    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    const float p[4][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}};
    for (int a = 0; a < 4; a++) {
      GPU_vertbuf_vert_set(vbo, v++, Vert{{p[a][0], p[a][1], 0.0f}, flag});
    }

    SHC.drw_quad = GPU_batch_create_ex(GPU_PRIM_TRI_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_quad;
}

blender::gpu::Batch *DRW_cache_quad_wires_get()
{
  if (!SHC.drw_quad_wires) {
    GPUVertFormat format = extra_vert_format();

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 5);

    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    const float p[4][2] = {{-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
    for (int a = 0; a < 5; a++) {
      GPU_vertbuf_vert_set(vbo, v++, Vert{{p[a % 4][0], p[a % 4][1], 0.0f}, flag});
    }

    SHC.drw_quad_wires = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_quad_wires;
}

blender::gpu::Batch *DRW_cache_grid_get()
{
  if (!SHC.drw_grid) {
    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 8 * 8 * 2 * 3);

    uint v_idx = 0;
    for (int i = 0; i < 8; i++) {
      for (int j = 0; j < 8; j++) {
        float pos0[2] = {float(i) / 8.0f, float(j) / 8.0f};
        float pos1[2] = {float(i + 1) / 8.0f, float(j) / 8.0f};
        float pos2[2] = {float(i) / 8.0f, float(j + 1) / 8.0f};
        float pos3[2] = {float(i + 1) / 8.0f, float(j + 1) / 8.0f};

        madd_v2_v2v2fl(pos0, blender::float2{-1.0f, -1.0f}, pos0, 2.0f);
        madd_v2_v2v2fl(pos1, blender::float2{-1.0f, -1.0f}, pos1, 2.0f);
        madd_v2_v2v2fl(pos2, blender::float2{-1.0f, -1.0f}, pos2, 2.0f);
        madd_v2_v2v2fl(pos3, blender::float2{-1.0f, -1.0f}, pos3, 2.0f);

        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos0);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos1);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos2);

        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos2);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos1);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, pos3);
      }
    }

    SHC.drw_grid = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_grid;
}

/* Sphere */
static void sphere_lat_lon_vert(blender::gpu::VertBuf *vbo, int *v_ofs, float lat, float lon)
{
  float x = sinf(lat) * cosf(lon);
  float y = cosf(lat);
  float z = sinf(lat) * sinf(lon);
  GPU_vertbuf_vert_set(vbo, *v_ofs, VertShaded{{x, y, z}, VCLASS_EMPTY_SCALED, {x, y, z}});
  (*v_ofs)++;
}

blender::gpu::Batch *DRW_cache_sphere_get(const eDRWLevelOfDetail level_of_detail)
{
  BLI_assert(level_of_detail >= DRW_LOD_LOW && level_of_detail < DRW_LOD_MAX);

  if (!SHC.drw_sphere_lod[level_of_detail]) {
    int lat_res;
    int lon_res;

    switch (level_of_detail) {
      case DRW_LOD_LOW:
        lat_res = DRW_SPHERE_SHAPE_LATITUDE_LOW;
        lon_res = DRW_SPHERE_SHAPE_LONGITUDE_LOW;
        break;
      case DRW_LOD_MEDIUM:
        lat_res = DRW_SPHERE_SHAPE_LATITUDE_MEDIUM;
        lon_res = DRW_SPHERE_SHAPE_LONGITUDE_MEDIUM;
        break;
      case DRW_LOD_HIGH:
        lat_res = DRW_SPHERE_SHAPE_LATITUDE_HIGH;
        lon_res = DRW_SPHERE_SHAPE_LONGITUDE_HIGH;
        break;
      default:
        return nullptr;
    }

    GPUVertFormat format = extra_vert_format();
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    int v_len = (lat_res - 1) * lon_res * 6;
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float lon_inc = 2 * M_PI / lon_res;
    const float lat_inc = M_PI / lat_res;
    float lon, lat;

    int v = 0;
    lon = 0.0f;
    for (int i = 0; i < lon_res; i++, lon += lon_inc) {
      lat = 0.0f;
      for (int j = 0; j < lat_res; j++, lat += lat_inc) {
        if (j != lat_res - 1) { /* Pole */
          sphere_lat_lon_vert(vbo, &v, lat + lat_inc, lon + lon_inc);
          sphere_lat_lon_vert(vbo, &v, lat + lat_inc, lon);
          sphere_lat_lon_vert(vbo, &v, lat, lon);
        }
        if (j != 0) { /* Pole */
          sphere_lat_lon_vert(vbo, &v, lat, lon + lon_inc);
          sphere_lat_lon_vert(vbo, &v, lat + lat_inc, lon + lon_inc);
          sphere_lat_lon_vert(vbo, &v, lat, lon);
        }
      }
    }

    SHC.drw_sphere_lod[level_of_detail] = GPU_batch_create_ex(
        GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_sphere_lod[level_of_detail];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

static void circle_verts(
    blender::gpu::VertBuf *vbo, int *vert_idx, int segments, float radius, float z, int flag)
{
  for (int a = 0; a < segments; a++) {
    for (int b = 0; b < 2; b++) {
      float angle = (2.0f * M_PI * (a + b)) / segments;
      float s = sinf(angle) * radius;
      float c = cosf(angle) * radius;
      int v = *vert_idx;
      *vert_idx = v + 1;
      GPU_vertbuf_vert_set(vbo, v, Vert{{s, c, z}, flag});
    }
  }
}

static void circle_dashed_verts(
    blender::gpu::VertBuf *vbo, int *vert_idx, int segments, float radius, float z, int flag)
{
  for (int a = 0; a < segments * 2; a += 2) {
    for (int b = 0; b < 2; b++) {
      float angle = (2.0f * M_PI * (a + b)) / (segments * 2);
      float s = sinf(angle) * radius;
      float c = cosf(angle) * radius;
      int v = *vert_idx;
      *vert_idx = v + 1;
      GPU_vertbuf_vert_set(vbo, v, Vert{{s, c, z}, flag});
    }
  }
}

/* XXX TODO: move that 1 unit cube to more common/generic place? */
static const float bone_box_verts[8][3] = {
    {1.0f, 0.0f, 1.0f},
    {1.0f, 0.0f, -1.0f},
    {-1.0f, 0.0f, -1.0f},
    {-1.0f, 0.0f, 1.0f},
    {1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, -1.0f},
    {-1.0f, 1.0f, -1.0f},
    {-1.0f, 1.0f, 1.0f},
};

static const uint bone_box_wire[24] = {
    0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7,
};

#if 0 /* UNUSED */
/* aligned with bone_octahedral_wire
 * Contains adjacent normal index */
static const uint bone_box_wire_adjacent_face[24] = {
    0, 2, 0, 4, 1, 6, 1, 8, 3, 10, 5, 10, 7, 11, 9, 11, 3, 8, 2, 5, 4, 7, 6, 9,
};
#endif

static const uint bone_box_solid_tris[12][3] = {
    {0, 2, 1}, /* bottom */
    {0, 3, 2},

    {0, 1, 5}, /* sides */
    {0, 5, 4},

    {1, 2, 6},
    {1, 6, 5},

    {2, 3, 7},
    {2, 7, 6},

    {3, 0, 4},
    {3, 4, 7},

    {4, 5, 6}, /* top */
    {4, 6, 7},
};

/**
 * Store indices of generated verts from bone_box_solid_tris to define adjacency infos.
 * See bone_octahedral_solid_tris for more infos.
 */
static const uint bone_box_wire_lines_adjacency[12][4] = {
    {4, 2, 0, 11},
    {0, 1, 2, 8},
    {2, 4, 1, 14},
    {1, 0, 4, 20}, /* bottom */
    {0, 8, 11, 14},
    {2, 14, 8, 20},
    {1, 20, 14, 11},
    {4, 11, 20, 8}, /* top */
    {20, 0, 11, 2},
    {11, 2, 8, 1},
    {8, 1, 14, 4},
    {14, 4, 20, 0}, /* sides */
};

#if 0 /* UNUSED */
static const uint bone_box_solid_tris_adjacency[12][6] = {
    {0, 5, 1, 14, 2, 8},
    {3, 26, 4, 20, 5, 1},

    {6, 2, 7, 16, 8, 11},
    {9, 7, 10, 32, 11, 24},

    {12, 0, 13, 22, 14, 17},
    {15, 13, 16, 30, 17, 6},

    {18, 3, 19, 28, 20, 23},
    {21, 19, 22, 33, 23, 12},

    {24, 4, 25, 10, 26, 29},
    {27, 25, 28, 34, 29, 18},

    {30, 9, 31, 15, 32, 35},
    {33, 31, 34, 21, 35, 27},
};
#endif

/* aligned with bone_box_solid_tris */
static const float bone_box_solid_normals[12][3] = {
    {0.0f, -1.0f, 0.0f},
    {0.0f, -1.0f, 0.0f},

    {1.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f},

    {0.0f, 0.0f, -1.0f},
    {0.0f, 0.0f, -1.0f},

    {-1.0f, 0.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f},

    {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, 1.0f},

    {0.0f, 1.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
};

blender::gpu::Batch *DRW_cache_cube_get()
{
  if (!SHC.drw_cube) {
    GPUVertFormat format = extra_vert_format();

    const int tri_len = ARRAY_SIZE(bone_box_solid_tris);
    const int vert_len = ARRAY_SIZE(bone_box_verts);

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, vert_len);

    GPUIndexBufBuilder elb;
    GPU_indexbuf_init(&elb, GPU_PRIM_TRIS, tri_len, vert_len);

    int v = 0;
    for (int i = 0; i < vert_len; i++) {
      float x = bone_box_verts[i][0];
      float y = bone_box_verts[i][1] * 2.0f - 1.0f;
      float z = bone_box_verts[i][2];
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, z}, VCLASS_EMPTY_SCALED});
    }

    for (int i = 0; i < tri_len; i++) {
      const uint *tri_indices = bone_box_solid_tris[i];
      GPU_indexbuf_add_tri_verts(&elb, tri_indices[0], tri_indices[1], tri_indices[2]);
    }

    SHC.drw_cube = GPU_batch_create_ex(
        GPU_PRIM_TRIS, vbo, GPU_indexbuf_build(&elb), GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
  }
  return SHC.drw_cube;
}

blender::gpu::Batch *DRW_cache_circle_get()
{
#define CIRCLE_RESOL 64
  if (!SHC.drw_circle) {
    GPUVertFormat format = extra_vert_format();

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, CIRCLE_RESOL + 1);

    int v = 0;
    for (int a = 0; a < CIRCLE_RESOL + 1; a++) {
      float x = sinf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      float z = cosf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      float y = 0.0f;
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, z}, VCLASS_EMPTY_SCALED});
    }

    SHC.drw_circle = GPU_batch_create_ex(GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_circle;
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_normal_arrow_get()
{
  if (!SHC.drw_normal_arrow) {
    GPUVertFormat format = {0};
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 2);

    /* TODO: real arrow. For now, it's a line positioned in the vertex shader. */

    SHC.drw_normal_arrow = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_normal_arrow;
}

namespace blender::draw {

void DRW_vertbuf_create_wiredata(blender::gpu::VertBuf *vbo, const int vert_len)
{
  static GPUVertFormat format = {0};
  static struct {
    uint wd;
  } attr_id;
  if (format.attr_len == 0) {
    /* initialize vertex format */
    if (!GPU_crappy_amd_driver()) {
      /* Some AMD drivers strangely crash with a vbo with this format. */
      attr_id.wd = GPU_vertformat_attr_add(
          &format, "wd", GPU_COMP_U8, 1, GPU_FETCH_INT_TO_FLOAT_UNIT);
    }
    else {
      attr_id.wd = GPU_vertformat_attr_add(&format, "wd", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
    }
  }

  GPU_vertbuf_init_with_format(*vbo, format);
  GPU_vertbuf_data_alloc(*vbo, vert_len);

  if (GPU_vertbuf_get_format(vbo)->stride == 1) {
    memset(vbo->data<uint8_t>().data(), 0xFF, size_t(vert_len));
  }
  else {
    GPUVertBufRaw wd_step;
    GPU_vertbuf_attr_get_raw_data(vbo, attr_id.wd, &wd_step);
    for (int i = 0; i < vert_len; i++) {
      *((float *)GPU_vertbuf_raw_step(&wd_step)) = 1.0f;
    }
  }
}

}  // namespace blender::draw

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dummy VBO's
 *
 * We need a dummy VBO containing the vertex count to draw instances ranges.
 *
 * \{ */

blender::gpu::Batch *DRW_gpencil_dummy_buffer_get()
{
  if (SHC.drw_gpencil_dummy_quad == nullptr) {
    GPUVertFormat format = {0};
    /* NOTE: Use GPU_COMP_U32 to satisfy minimum 4-byte vertex stride for Metal backend. */
    GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_U32, 1, GPU_FETCH_INT);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 4);

    SHC.drw_gpencil_dummy_quad = GPU_batch_create_ex(
        GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_gpencil_dummy_quad;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common Object API
 *
 * \note Curve and text objects evaluate to the evaluated geometry set's mesh component if
 * they have a surface, so curve objects themselves do not have a surface (the mesh component
 * is presented to render engines as a separate object).
 * \{ */

blender::gpu::Batch *DRW_cache_object_all_edges_get(Object *ob)
{
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_all_edges_get(ob);
    /* TODO: should match #DRW_cache_object_surface_get. */
    default:
      return nullptr;
  }
}

blender::gpu::Batch *DRW_cache_object_edge_detection_get(Object *ob, bool *r_is_manifold)
{
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_edge_detection_get(ob, r_is_manifold);
    default:
      return nullptr;
  }
}

blender::gpu::Batch *DRW_cache_object_face_wireframe_get(const Scene *scene, Object *ob)
{
  using namespace blender::draw;
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_face_wireframe_get(ob);
    case OB_POINTCLOUD:
      return DRW_pointcloud_batch_cache_get_dots(ob);
    case OB_VOLUME:
      return DRW_cache_volume_face_wireframe_get(ob);
    case OB_GREASE_PENCIL:
      return DRW_cache_grease_pencil_face_wireframe_get(scene, ob);
    default:
      return nullptr;
  }
}

blender::gpu::Batch *DRW_cache_object_loose_edges_get(Object *ob)
{
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_loose_edges_get(ob);
    default:
      return nullptr;
  }
}

blender::gpu::Batch *DRW_cache_object_surface_get(Object *ob)
{
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_surface_get(ob);
    default:
      return nullptr;
  }
}

blender::gpu::VertBuf *DRW_cache_object_pos_vertbuf_get(Object *ob)
{
  using namespace blender::draw;
  Mesh *mesh = BKE_object_get_evaluated_mesh_no_subsurf_unchecked(ob);
  short type = (mesh != nullptr) ? short(OB_MESH) : ob->type;

  switch (type) {
    case OB_MESH:
      return DRW_mesh_batch_cache_pos_vertbuf_get(
          *static_cast<Mesh *>((mesh != nullptr) ? mesh : ob->data));
    default:
      return nullptr;
  }
}

Span<blender::gpu::Batch *> DRW_cache_object_surface_material_get(
    Object *ob, const Span<const GPUMaterial *> materials)
{
  switch (ob->type) {
    case OB_MESH:
      return DRW_cache_mesh_surface_shaded_get(ob, materials);
    default:
      return {};
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Empties
 * \{ */

blender::gpu::Batch *DRW_cache_plain_axes_get()
{
  if (!SHC.drw_plain_axes) {
    GPUVertFormat format = extra_vert_format();

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 6);

    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, -1.0f, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 1.0f, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{-1.0f, 0.0f, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{1.0f, 0.0f, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, -1.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 1.0f}, flag});

    SHC.drw_plain_axes = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_plain_axes;
}

blender::gpu::Batch *DRW_cache_empty_cube_get()
{
  if (!SHC.drw_empty_cube) {
    GPUVertFormat format = extra_vert_format();
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, ARRAY_SIZE(bone_box_wire));

    int v = 0;
    for (int i = 0; i < ARRAY_SIZE(bone_box_wire); i++) {
      float x = bone_box_verts[bone_box_wire[i]][0];
      float y = bone_box_verts[bone_box_wire[i]][1] * 2.0 - 1.0f;
      float z = bone_box_verts[bone_box_wire[i]][2];
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, z}, VCLASS_EMPTY_SCALED});
    }

    SHC.drw_empty_cube = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_cube;
}

blender::gpu::Batch *DRW_cache_single_arrow_get()
{
  if (!SHC.drw_single_arrow) {
    GPUVertFormat format = extra_vert_format();
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 4 * 2 * 2 + 2);

    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    float p[3][3] = {{0}};
    p[0][2] = 1.0f;
    p[1][0] = 0.035f;
    p[1][1] = 0.035f;
    p[2][0] = -0.035f;
    p[2][1] = 0.035f;
    p[1][2] = p[2][2] = 0.75f;
    for (int sides = 0; sides < 4; sides++) {
      if (sides % 2 == 1) {
        p[1][0] = -p[1][0];
        p[2][1] = -p[2][1];
      }
      else {
        p[1][1] = -p[1][1];
        p[2][0] = -p[2][0];
      }
      for (int i = 0, a = 1; i < 2; i++, a++) {
        GPU_vertbuf_vert_set(vbo, v++, Vert{{p[i][0], p[i][1], p[i][2]}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{p[a][0], p[a][1], p[a][2]}, flag});
      }
    }
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.0}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.75f}, flag});

    SHC.drw_single_arrow = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_single_arrow;
}

blender::gpu::Batch *DRW_cache_empty_sphere_get()
{
  if (!SHC.drw_empty_sphere) {
    blender::gpu::VertBuf *vbo = sphere_wire_vbo(1.0f, VCLASS_EMPTY_SCALED);
    SHC.drw_empty_sphere = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_sphere;
}

blender::gpu::Batch *DRW_cache_empty_cone_get()
{
#define NSEGMENTS 8
  if (!SHC.drw_empty_cone) {
    GPUVertFormat format = extra_vert_format();
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, NSEGMENTS * 4);

    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    /* a single ring of vertices */
    float p[NSEGMENTS][2];
    for (int i = 0; i < NSEGMENTS; i++) {
      float angle = 2 * M_PI * (float(i) / float(NSEGMENTS));
      p[i][0] = cosf(angle);
      p[i][1] = sinf(angle);
    }
    for (int i = 0; i < NSEGMENTS; i++) {
      float cv[2];
      cv[0] = p[(i) % NSEGMENTS][0];
      cv[1] = p[(i) % NSEGMENTS][1];

      /* cone sides */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], 0.0f, cv[1]}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 2.0f, 0.0f}, flag});

      /* end ring */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], 0.0f, cv[1]}, flag});
      cv[0] = p[(i + 1) % NSEGMENTS][0];
      cv[1] = p[(i + 1) % NSEGMENTS][1];
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], 0.0f, cv[1]}, flag});
    }

    SHC.drw_empty_cone = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_cone;
#undef NSEGMENTS
}

blender::gpu::Batch *DRW_cache_empty_cylinder_get()
{
#define NSEGMENTS 12
  if (!SHC.drw_empty_cylinder) {
    GPUVertFormat format = extra_vert_format();
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, NSEGMENTS * 6);

    /* a single ring of vertices */
    int v = 0;
    int flag = VCLASS_EMPTY_SCALED;
    float p[NSEGMENTS][2];
    for (int i = 0; i < NSEGMENTS; i++) {
      float angle = 2 * M_PI * (float(i) / float(NSEGMENTS));
      p[i][0] = cosf(angle);
      p[i][1] = sinf(angle);
    }
    for (int i = 0; i < NSEGMENTS; i++) {
      float cv[2], pv[2];
      cv[0] = p[(i) % NSEGMENTS][0];
      cv[1] = p[(i) % NSEGMENTS][1];
      pv[0] = p[(i + 1) % NSEGMENTS][0];
      pv[1] = p[(i + 1) % NSEGMENTS][1];

      /* cylinder sides */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], cv[1], -1.0f}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], cv[1], 1.0f}, flag});
      /* top ring */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], cv[1], 1.0f}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{pv[0], pv[1], 1.0f}, flag});
      /* bottom ring */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{cv[0], cv[1], -1.0f}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{pv[0], pv[1], -1.0f}, flag});
    }

    SHC.drw_empty_cylinder = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_cylinder;
#undef NSEGMENTS
}

blender::gpu::Batch *DRW_cache_empty_capsule_body_get()
{
  if (!SHC.drw_empty_capsule_body) {
    const float pos[8][3] = {
        {1.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 0.0f},
        {-1.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, -1.0f, 1.0f},
        {0.0f, -1.0f, 0.0f},
    };

    /* Position Only 3D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 8);
    GPU_vertbuf_attr_fill(vbo, attr_id.pos, pos);

    SHC.drw_empty_capsule_body = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_capsule_body;
}

blender::gpu::Batch *DRW_cache_empty_capsule_cap_get()
{
#define NSEGMENTS 24 /* Must be multiple of 2. */
  if (!SHC.drw_empty_capsule_cap) {
    /* a single ring of vertices */
    float p[NSEGMENTS][2];
    for (int i = 0; i < NSEGMENTS; i++) {
      float angle = 2 * M_PI * (float(i) / float(NSEGMENTS));
      p[i][0] = cosf(angle);
      p[i][1] = sinf(angle);
    }

    /* Position Only 3D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, (NSEGMENTS * 2) * 2);

    /* Base circle */
    int vidx = 0;
    for (int i = 0; i < NSEGMENTS; i++) {
      float v[3] = {0.0f, 0.0f, 0.0f};
      copy_v2_v2(v, p[(i) % NSEGMENTS]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      copy_v2_v2(v, p[(i + 1) % NSEGMENTS]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
    }

    for (int i = 0; i < NSEGMENTS / 2; i++) {
      float v[3] = {0.0f, 0.0f, 0.0f};
      int ci = i % NSEGMENTS;
      int pi = (i + 1) % NSEGMENTS;
      /* Y half circle */
      copy_v3_fl3(v, p[ci][0], 0.0f, p[ci][1]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      copy_v3_fl3(v, p[pi][0], 0.0f, p[pi][1]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      /* X half circle */
      copy_v3_fl3(v, 0.0f, p[ci][0], p[ci][1]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      copy_v3_fl3(v, 0.0f, p[pi][0], p[pi][1]);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
    }

    SHC.drw_empty_capsule_cap = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_empty_capsule_cap;
#undef NSEGMENTS
}

blender::gpu::Batch *DRW_cache_field_wind_get()
{
#define CIRCLE_RESOL 32
  if (!SHC.drw_field_wind) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (CIRCLE_RESOL * 4);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE;
    for (int i = 0; i < 4; i++) {
      float z = 0.05f * float(i);
      circle_verts(vbo, &v, CIRCLE_RESOL, 1.0f, z, flag);
    }

    SHC.drw_field_wind = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_wind;
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_field_force_get()
{
#define CIRCLE_RESOL 32
  if (!SHC.drw_field_force) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (CIRCLE_RESOL * 3);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE | VCLASS_SCREENALIGNED;
    for (int i = 0; i < 3; i++) {
      float radius = 1.0f + 0.5f * i;
      circle_verts(vbo, &v, CIRCLE_RESOL, radius, 0.0f, flag);
    }

    SHC.drw_field_force = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_force;
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_field_vortex_get()
{
#define SPIRAL_RESOL 32
  if (!SHC.drw_field_vortex) {
    GPUVertFormat format = extra_vert_format();

    int v_len = SPIRAL_RESOL * 2 + 1;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE;
    for (int a = SPIRAL_RESOL; a > -1; a--) {
      float r = a / float(SPIRAL_RESOL);
      float angle = (2.0f * M_PI * a) / SPIRAL_RESOL;
      GPU_vertbuf_vert_set(vbo, v++, Vert{{sinf(angle) * r, cosf(angle) * r, 0.0f}, flag});
    }
    for (int a = 1; a <= SPIRAL_RESOL; a++) {
      float r = a / float(SPIRAL_RESOL);
      float angle = (2.0f * M_PI * a) / SPIRAL_RESOL;
      GPU_vertbuf_vert_set(vbo, v++, Vert{{sinf(angle) * -r, cosf(angle) * -r, 0.0f}, flag});
    }

    SHC.drw_field_vortex = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_vortex;
#undef SPIRAL_RESOL
}

blender::gpu::Batch *DRW_cache_field_curve_get()
{
#define CIRCLE_RESOL 32
  if (!SHC.drw_field_curve) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (CIRCLE_RESOL);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE | VCLASS_SCREENALIGNED;
    circle_verts(vbo, &v, CIRCLE_RESOL, 1.0f, 0.0f, flag);

    SHC.drw_field_curve = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_curve;
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_field_tube_limit_get()
{
#define CIRCLE_RESOL 32
#define SIDE_STIPPLE 32
  if (!SHC.drw_field_tube_limit) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (CIRCLE_RESOL * 2 + 4 * SIDE_STIPPLE / 2);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE;
    /* Caps */
    for (int i = 0; i < 2; i++) {
      float z = i * 2.0f - 1.0f;
      circle_dashed_verts(vbo, &v, CIRCLE_RESOL, 1.0f, z, flag);
    }
    /* Side Edges */
    for (int a = 0; a < 4; a++) {
      float angle = (2.0f * M_PI * a) / 4.0f;
      for (int i = 0; i < SIDE_STIPPLE; i++) {
        float z = (i / float(SIDE_STIPPLE)) * 2.0f - 1.0f;
        GPU_vertbuf_vert_set(vbo, v++, Vert{{sinf(angle), cosf(angle), z}, flag});
      }
    }

    SHC.drw_field_tube_limit = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_tube_limit;
#undef SIDE_STIPPLE
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_field_cone_limit_get()
{
#define CIRCLE_RESOL 32
#define SIDE_STIPPLE 32
  if (!SHC.drw_field_cone_limit) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (CIRCLE_RESOL * 2 + 4 * SIDE_STIPPLE / 2);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE;
    /* Caps */
    for (int i = 0; i < 2; i++) {
      float z = i * 2.0f - 1.0f;
      circle_dashed_verts(vbo, &v, CIRCLE_RESOL, 1.0f, z, flag);
    }
    /* Side Edges */
    for (int a = 0; a < 4; a++) {
      float angle = (2.0f * M_PI * a) / 4.0f;
      for (int i = 0; i < SIDE_STIPPLE; i++) {
        float z = (i / float(SIDE_STIPPLE)) * 2.0f - 1.0f;
        GPU_vertbuf_vert_set(vbo, v++, Vert{{sinf(angle) * z, cosf(angle) * z, z}, flag});
      }
    }

    SHC.drw_field_cone_limit = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_cone_limit;
#undef SIDE_STIPPLE
#undef CIRCLE_RESOL
}

blender::gpu::Batch *DRW_cache_field_sphere_limit_get()
{
#define CIRCLE_RESOL 32
  if (!SHC.drw_field_sphere_limit) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * CIRCLE_RESOL;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_EMPTY_SIZE | VCLASS_SCREENALIGNED;
    circle_dashed_verts(vbo, &v, CIRCLE_RESOL, 1.0f, 0.0f, flag);

    SHC.drw_field_sphere_limit = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_field_sphere_limit;
#undef CIRCLE_RESOL
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lights
 * \{ */

#define DIAMOND_NSEGMENTS 4
#define INNER_NSEGMENTS 8
#define OUTER_NSEGMENTS 10
#define CIRCLE_NSEGMENTS 32

static float light_distance_z_get(char axis, const bool start)
{
  switch (axis) {
    case 'x': /* - X */
      return start ? 0.4f : 0.3f;
    case 'X': /* + X */
      return start ? 0.6f : 0.7f;
    case 'y': /* - Y */
      return start ? 1.4f : 1.3f;
    case 'Y': /* + Y */
      return start ? 1.6f : 1.7f;
    case 'z': /* - Z */
      return start ? 2.4f : 2.3f;
    case 'Z': /* + Z */
      return start ? 2.6f : 2.7f;
  }
  return 0.0;
}

blender::gpu::Batch *DRW_cache_groundline_get()
{
  if (!SHC.drw_ground_line) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (1 + DIAMOND_NSEGMENTS);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    /* Ground Point */
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.35f, 0.0f, 0);
    /* Ground Line */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, 1.0}, 0});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, 0.0}, 0});

    SHC.drw_ground_line = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_ground_line;
}

blender::gpu::Batch *DRW_cache_light_icon_inner_lines_get()
{
  if (!SHC.drw_light_icon_inner_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (DIAMOND_NSEGMENTS + INNER_NSEGMENTS);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 9.0f;
    int v = 0;

    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, r * 0.3f, 0.0f, VCLASS_SCREENSPACE);
    circle_dashed_verts(vbo, &v, INNER_NSEGMENTS, r * 1.0f, 0.0f, VCLASS_SCREENSPACE);

    SHC.drw_light_icon_inner_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_icon_inner_lines;
}

blender::gpu::Batch *DRW_cache_light_icon_outer_lines_get()
{
  if (!SHC.drw_light_icon_outer_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * OUTER_NSEGMENTS;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 9.0f;
    int v = 0;

    circle_dashed_verts(vbo, &v, OUTER_NSEGMENTS, r * 1.33f, 0.0f, VCLASS_SCREENSPACE);

    SHC.drw_light_icon_outer_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_icon_outer_lines;
}

blender::gpu::Batch *DRW_cache_light_icon_sun_rays_get()
{
  if (!SHC.drw_light_icon_sun_rays) {
    GPUVertFormat format = extra_vert_format();

    const int num_rays = 8;
    int v_len = 4 * num_rays;

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 9.0f;

    int v = 0;

    /* Sun Rays */
    for (int a = 0; a < num_rays; a++) {
      float angle = (2.0f * M_PI * a) / float(num_rays);
      float s = sinf(angle) * r;
      float c = cosf(angle) * r;
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s * 1.6f, c * 1.6f, 0.0f}, VCLASS_SCREENSPACE});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s * 1.9f, c * 1.9f, 0.0f}, VCLASS_SCREENSPACE});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s * 2.2f, c * 2.2f, 0.0f}, VCLASS_SCREENSPACE});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s * 2.5f, c * 2.5f, 0.0f}, VCLASS_SCREENSPACE});
    }

    SHC.drw_light_icon_sun_rays = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_icon_sun_rays;
}

blender::gpu::Batch *DRW_cache_light_point_lines_get()
{
  if (!SHC.drw_light_point_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * CIRCLE_NSEGMENTS;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;

    /* Light area */
    int flag = VCLASS_SCREENALIGNED | VCLASS_LIGHT_AREA_SHAPE;
    circle_verts(vbo, &v, CIRCLE_NSEGMENTS, 1.0f, 0.0f, flag);

    SHC.drw_light_point_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_point_lines;
}

blender::gpu::Batch *DRW_cache_light_sun_lines_get()
{
  if (!SHC.drw_light_sun_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;

    /* Direction Line */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, 0.0}, 0});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, -20.0}, 0}); /* Good default. */

    SHC.drw_light_sun_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_sun_lines;
}

blender::gpu::Batch *DRW_cache_light_spot_lines_get()
{
  if (!SHC.drw_light_spot_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (DIAMOND_NSEGMENTS * 2 + CIRCLE_NSEGMENTS * 4 + 1);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;

    /* Light area */
    int flag = VCLASS_SCREENALIGNED | VCLASS_LIGHT_AREA_SHAPE;
    circle_verts(vbo, &v, CIRCLE_NSEGMENTS, 1.0f, 0.0f, flag);
    /* Cone cap */
    flag = VCLASS_LIGHT_SPOT_SHAPE;
    circle_verts(vbo, &v, CIRCLE_NSEGMENTS, 1.0f, 0.0f, flag);
    flag = VCLASS_LIGHT_SPOT_SHAPE | VCLASS_LIGHT_SPOT_BLEND;
    circle_verts(vbo, &v, CIRCLE_NSEGMENTS, 1.0f, 0.0f, flag);
    /* Cone silhouette */
    flag = VCLASS_LIGHT_SPOT_SHAPE | VCLASS_LIGHT_SPOT_CONE;
    for (int a = 0; a < CIRCLE_NSEGMENTS; a++) {
      float angle = (2.0f * M_PI * a) / CIRCLE_NSEGMENTS;
      float s = sinf(angle);
      float c = cosf(angle);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.0f}, 0});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s, c, -1.0f}, flag});
    }
    /* Direction Line */
    float zsta = light_distance_z_get('z', true);
    float zend = light_distance_z_get('z', false);
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zsta}, VCLASS_LIGHT_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zend}, VCLASS_LIGHT_DIST});
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zsta, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zend, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);

    SHC.drw_light_spot_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_spot_lines;
}

blender::gpu::Batch *DRW_cache_light_spot_volume_get()
{
  if (!SHC.drw_light_spot_volume) {
    GPUVertFormat format = extra_vert_format();

    int v_len = CIRCLE_NSEGMENTS + 1 + 1;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    /* Cone apex */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.0f}, 0});
    /* Cone silhouette */
    int flag = VCLASS_LIGHT_SPOT_SHAPE;
    for (int a = 0; a < CIRCLE_NSEGMENTS + 1; a++) {
      float angle = (2.0f * M_PI * a) / CIRCLE_NSEGMENTS;
      float s = sinf(-angle);
      float c = cosf(-angle);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{s, c, -1.0f}, flag});
    }

    SHC.drw_light_spot_volume = GPU_batch_create_ex(
        GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_spot_volume;
}

blender::gpu::Batch *DRW_cache_light_area_disk_lines_get()
{
  if (!SHC.drw_light_area_disk_lines) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * (DIAMOND_NSEGMENTS * 2 + CIRCLE_NSEGMENTS + 1);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;

    /* Light area */
    circle_verts(vbo, &v, CIRCLE_NSEGMENTS, 0.5f, 0.0f, VCLASS_LIGHT_AREA_SHAPE);
    /* Direction Line */
    float zsta = light_distance_z_get('z', true);
    float zend = light_distance_z_get('z', false);
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zsta}, VCLASS_LIGHT_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zend}, VCLASS_LIGHT_DIST});
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zsta, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zend, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);

    SHC.drw_light_area_disk_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_area_disk_lines;
}

blender::gpu::Batch *DRW_cache_light_area_square_lines_get()
{
  if (!SHC.drw_light_area_square_lines) {
    GPUVertFormat format = extra_vert_format();

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    int v_len = 2 * (DIAMOND_NSEGMENTS * 2 + 4 + 1);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;

    /* Light area */
    int flag = VCLASS_LIGHT_AREA_SHAPE;
    for (int a = 0; a < 4; a++) {
      for (int b = 0; b < 2; b++) {
        const float p[4][2] = {{-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
        float x = p[(a + b) % 4][0];
        float y = p[(a + b) % 4][1];
        GPU_vertbuf_vert_set(vbo, v++, Vert{{x * 0.5f, y * 0.5f, 0.0f}, flag});
      }
    }
    /* Direction Line */
    float zsta = light_distance_z_get('z', true);
    float zend = light_distance_z_get('z', false);
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zsta}, VCLASS_LIGHT_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, zend}, VCLASS_LIGHT_DIST});
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zsta, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zend, VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE);

    SHC.drw_light_area_square_lines = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_light_area_square_lines;
}

#undef CIRCLE_NSEGMENTS
#undef OUTER_NSEGMENTS
#undef INNER_NSEGMENTS

/** \} */

/* -------------------------------------------------------------------- */
/** \name Speaker
 * \{ */

blender::gpu::Batch *DRW_cache_speaker_get()
{
  if (!SHC.drw_speaker) {
    float v[3];
    const int segments = 16;
    int vidx = 0;

    /* Position Only 3D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 3 * segments * 2 + 4 * 4);

    for (int j = 0; j < 3; j++) {
      float z = 0.25f * j - 0.125f;
      float r = (j == 0 ? 0.5f : 0.25f);

      copy_v3_fl3(v, r, 0.0f, z);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      for (int i = 1; i < segments; i++) {
        float x = cosf(2.0f * float(M_PI) * i / segments) * r;
        float y = sinf(2.0f * float(M_PI) * i / segments) * r;
        copy_v3_fl3(v, x, y, z);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
      }
      copy_v3_fl3(v, r, 0.0f, z);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
    }

    for (int j = 0; j < 4; j++) {
      float x = (((j + 1) % 2) * (j - 1)) * 0.5f;
      float y = ((j % 2) * (j - 2)) * 0.5f;
      for (int i = 0; i < 3; i++) {
        if (i == 1) {
          x *= 0.5f;
          y *= 0.5f;
        }

        float z = 0.25f * i - 0.125f;
        copy_v3_fl3(v, x, y, z);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
        if (i == 1) {
          GPU_vertbuf_attr_set(vbo, attr_id.pos, vidx++, v);
        }
      }
    }

    SHC.drw_speaker = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_speaker;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Probe
 * \{ */

blender::gpu::Batch *DRW_cache_lightprobe_cube_get()
{
  if (!SHC.drw_lightprobe_cube) {
    GPUVertFormat format = extra_vert_format();

    int v_len = (6 + 3 + (1 + 2 * DIAMOND_NSEGMENTS) * 6) * 2;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 14.0f;
    int v = 0;
    int flag = VCLASS_SCREENSPACE;
    /* Icon */
    const float sin_pi_3 = 0.86602540378f;
    const float cos_pi_3 = 0.5f;
    const float p[7][2] = {
        {0.0f, 1.0f},
        {sin_pi_3, cos_pi_3},
        {sin_pi_3, -cos_pi_3},
        {0.0f, -1.0f},
        {-sin_pi_3, -cos_pi_3},
        {-sin_pi_3, cos_pi_3},
        {0.0f, 0.0f},
    };
    for (int i = 0; i < 6; i++) {
      float t1[2], t2[2];
      copy_v2_v2(t1, p[i]);
      copy_v2_v2(t2, p[(i + 1) % 6]);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{t1[0] * r, t1[1] * r, 0.0f}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{t2[0] * r, t2[1] * r, 0.0f}, flag});
    }
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[1][0] * r, p[1][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[5][0] * r, p[5][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[3][0] * r, p[3][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    /* Direction Lines */
    flag = VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE;
    for (int i = 0; i < 6; i++) {
      const char axes[] = "zZyYxX";
      const float zsta = light_distance_z_get(axes[i], true);
      const float zend = light_distance_z_get(axes[i], false);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, zsta}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, zend}, flag});
      circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zsta, flag);
      circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zend, flag);
    }

    SHC.drw_lightprobe_cube = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_lightprobe_cube;
}

blender::gpu::Batch *DRW_cache_lightprobe_grid_get()
{
  if (!SHC.drw_lightprobe_grid) {
    GPUVertFormat format = extra_vert_format();

    int v_len = (6 * 2 + 3 + (1 + 2 * DIAMOND_NSEGMENTS) * 6) * 2;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 14.0f;
    int v = 0;
    int flag = VCLASS_SCREENSPACE;
    /* Icon */
    const float sin_pi_3 = 0.86602540378f;
    const float cos_pi_3 = 0.5f;
    const float p[7][2] = {
        {0.0f, 1.0f},
        {sin_pi_3, cos_pi_3},
        {sin_pi_3, -cos_pi_3},
        {0.0f, -1.0f},
        {-sin_pi_3, -cos_pi_3},
        {-sin_pi_3, cos_pi_3},
        {0.0f, 0.0f},
    };
    for (int i = 0; i < 6; i++) {
      float t1[2], t2[2], tr[2];
      copy_v2_v2(t1, p[i]);
      copy_v2_v2(t2, p[(i + 1) % 6]);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{t1[0] * r, t1[1] * r, 0.0f}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{t2[0] * r, t2[1] * r, 0.0f}, flag});
      /* Internal wires. */
      for (int j = 1; j < 2; j++) {
        mul_v2_v2fl(tr, p[(i / 2) * 2 + 1], -0.5f * j);
        add_v2_v2v2(t1, p[i], tr);
        add_v2_v2v2(t2, p[(i + 1) % 6], tr);
        GPU_vertbuf_vert_set(vbo, v++, Vert{{t1[0] * r, t1[1] * r, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{t2[0] * r, t2[1] * r, 0.0f}, flag});
      }
    }
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[1][0] * r, p[1][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[5][0] * r, p[5][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[3][0] * r, p[3][1] * r, 0.0f}, flag});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{p[6][0] * r, p[6][1] * r, 0.0f}, flag});
    /* Direction Lines */
    flag = VCLASS_LIGHT_DIST | VCLASS_SCREENSPACE;
    for (int i = 0; i < 6; i++) {
      const char axes[] = "zZyYxX";
      const float zsta = light_distance_z_get(axes[i], true);
      const float zend = light_distance_z_get(axes[i], false);
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, zsta}, flag});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, zend}, flag});
      circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zsta, flag);
      circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.2f, zend, flag);
    }

    SHC.drw_lightprobe_grid = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_lightprobe_grid;
}

blender::gpu::Batch *DRW_cache_lightprobe_planar_get()
{
  if (!SHC.drw_lightprobe_planar) {
    GPUVertFormat format = extra_vert_format();

    int v_len = 2 * 4;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    const float r = 20.0f;
    int v = 0;
    /* Icon */
    const float sin_pi_3 = 0.86602540378f;
    const float p[4][2] = {
        {0.0f, 0.5f},
        {sin_pi_3, 0.0f},
        {0.0f, -0.5f},
        {-sin_pi_3, 0.0f},
    };
    for (int i = 0; i < 4; i++) {
      for (int a = 0; a < 2; a++) {
        float x = p[(i + a) % 4][0] * r;
        float y = p[(i + a) % 4][1] * r;
        GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 0.0}, VCLASS_SCREENSPACE});
      }
    }

    SHC.drw_lightprobe_planar = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_lightprobe_planar;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Bones
 * \{ */

static const float bone_octahedral_verts[6][3] = {
    {0.0f, 0.0f, 0.0f},
    {0.1f, 0.1f, 0.1f},
    {0.1f, 0.1f, -0.1f},
    {-0.1f, 0.1f, -0.1f},
    {-0.1f, 0.1f, 0.1f},
    {0.0f, 1.0f, 0.0f},
};

#if 0 /* UNUSED */

static const uint bone_octahedral_wire[24] = {
    0, 1, 1, 5, 5, 3, 3, 0, 0, 4, 4, 5, 5, 2, 2, 0, 1, 2, 2, 3, 3, 4, 4, 1,
};

/* aligned with bone_octahedral_wire
 * Contains adjacent normal index */
static const uint bone_octahedral_wire_adjacent_face[24] = {
    0, 3, 4, 7, 5, 6, 1, 2, 2, 3, 6, 7, 4, 5, 0, 1, 0, 4, 1, 5, 2, 6, 3, 7,
};
#endif

static const uint bone_octahedral_solid_tris[8][3] = {
    {2, 1, 0}, /* bottom */
    {3, 2, 0},
    {4, 3, 0},
    {1, 4, 0},

    {5, 1, 2}, /* top */
    {5, 2, 3},
    {5, 3, 4},
    {5, 4, 1},
};

/**
 * Store indices of generated verts from bone_octahedral_solid_tris to define adjacency infos.
 * Example: triangle {2, 1, 0} is adjacent to {3, 2, 0}, {1, 4, 0} and {5, 1, 2}.
 * {2, 1, 0} becomes {0, 1, 2}
 * {3, 2, 0} becomes {3, 4, 5}
 * {1, 4, 0} becomes {9, 10, 11}
 * {5, 1, 2} becomes {12, 13, 14}
 * According to opengl specification it becomes (starting from
 * the first vertex of the first face aka. vertex 2):
 * {0, 12, 1, 10, 2, 3}
 */
static const uint bone_octahedral_wire_lines_adjacency[12][4] = {
    {0, 1, 2, 6},
    {0, 12, 1, 6},
    {0, 3, 12, 6},
    {0, 2, 3, 6},
    {1, 6, 2, 3},
    {1, 12, 6, 3},
    {1, 0, 12, 3},
    {1, 2, 0, 3},
    {2, 0, 1, 12},
    {2, 3, 0, 12},
    {2, 6, 3, 12},
    {2, 1, 6, 12},
};

#if 0 /* UNUSED */
static const uint bone_octahedral_solid_tris_adjacency[8][6] = {
    {0, 12, 1, 10, 2, 3},
    {3, 15, 4, 1, 5, 6},
    {6, 18, 7, 4, 8, 9},
    {9, 21, 10, 7, 11, 0},

    {12, 22, 13, 2, 14, 17},
    {15, 13, 16, 5, 17, 20},
    {18, 16, 19, 8, 20, 23},
    {21, 19, 22, 11, 23, 14},
};
#endif

/* aligned with bone_octahedral_solid_tris */
static const float bone_octahedral_solid_normals[8][3] = {
    {M_SQRT1_2, -M_SQRT1_2, 0.00000000f},
    {-0.00000000f, -M_SQRT1_2, -M_SQRT1_2},
    {-M_SQRT1_2, -M_SQRT1_2, 0.00000000f},
    {0.00000000f, -M_SQRT1_2, M_SQRT1_2},
    {0.99388373f, 0.11043154f, -0.00000000f},
    {0.00000000f, 0.11043154f, -0.99388373f},
    {-0.99388373f, 0.11043154f, 0.00000000f},
    {0.00000000f, 0.11043154f, 0.99388373f},
};

blender::gpu::Batch *DRW_cache_bone_octahedral_get()
{
  if (!SHC.drw_bone_octahedral) {
    uint v_idx = 0;

    static GPUVertFormat format = {0};
    static struct {
      uint pos, nor;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
      attr_id.nor = GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    /* Vertices */
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 24);

    for (int i = 0; i < 8; i++) {
      for (int j = 0; j < 3; j++) {
        GPU_vertbuf_attr_set(vbo, attr_id.nor, v_idx, bone_octahedral_solid_normals[i]);
        GPU_vertbuf_attr_set(
            vbo, attr_id.pos, v_idx++, bone_octahedral_verts[bone_octahedral_solid_tris[i][j]]);
      }
    }

    SHC.drw_bone_octahedral = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_octahedral;
}

blender::gpu::Batch *DRW_cache_bone_octahedral_wire_get()
{
  if (!SHC.drw_bone_octahedral_wire) {
    GPUIndexBufBuilder elb;
    GPU_indexbuf_init(&elb, GPU_PRIM_LINES_ADJ, 12, 24);

    for (int i = 0; i < 12; i++) {
      GPU_indexbuf_add_line_adj_verts(&elb,
                                      bone_octahedral_wire_lines_adjacency[i][0],
                                      bone_octahedral_wire_lines_adjacency[i][1],
                                      bone_octahedral_wire_lines_adjacency[i][2],
                                      bone_octahedral_wire_lines_adjacency[i][3]);
    }

    /* HACK Reuse vertex buffer. */
    blender::gpu::Batch *pos_nor_batch = DRW_cache_bone_octahedral_get();

    SHC.drw_bone_octahedral_wire = GPU_batch_create_ex(GPU_PRIM_LINES_ADJ,
                                                       pos_nor_batch->verts[0],
                                                       GPU_indexbuf_build(&elb),
                                                       GPU_BATCH_OWNS_INDEX);
  }
  return SHC.drw_bone_octahedral_wire;
}

blender::gpu::Batch *DRW_cache_bone_box_get()
{
  if (!SHC.drw_bone_box) {
    uint v_idx = 0;

    static GPUVertFormat format = {0};
    static struct {
      uint pos, nor;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
      attr_id.nor = GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    /* Vertices */
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 36);

    for (int i = 0; i < 12; i++) {
      for (int j = 0; j < 3; j++) {
        GPU_vertbuf_attr_set(vbo, attr_id.nor, v_idx, bone_box_solid_normals[i]);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, bone_box_verts[bone_box_solid_tris[i][j]]);
      }
    }

    SHC.drw_bone_box = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_box;
}

blender::gpu::Batch *DRW_cache_bone_box_wire_get()
{
  if (!SHC.drw_bone_box_wire) {
    GPUIndexBufBuilder elb;
    GPU_indexbuf_init(&elb, GPU_PRIM_LINES_ADJ, 12, 36);

    for (int i = 0; i < 12; i++) {
      GPU_indexbuf_add_line_adj_verts(&elb,
                                      bone_box_wire_lines_adjacency[i][0],
                                      bone_box_wire_lines_adjacency[i][1],
                                      bone_box_wire_lines_adjacency[i][2],
                                      bone_box_wire_lines_adjacency[i][3]);
    }

    /* HACK Reuse vertex buffer. */
    blender::gpu::Batch *pos_nor_batch = DRW_cache_bone_box_get();

    SHC.drw_bone_box_wire = GPU_batch_create_ex(GPU_PRIM_LINES_ADJ,
                                                pos_nor_batch->verts[0],
                                                GPU_indexbuf_build(&elb),
                                                GPU_BATCH_OWNS_INDEX);
  }
  return SHC.drw_bone_box_wire;
}

/* Helpers for envelope bone's solid sphere-with-hidden-equatorial-cylinder.
 * Note that here we only encode head/tail in forth component of the vector. */
static void benv_lat_lon_to_co(const float lat, const float lon, float r_nor[3])
{
  r_nor[0] = sinf(lat) * cosf(lon);
  r_nor[1] = sinf(lat) * sinf(lon);
  r_nor[2] = cosf(lat);
}

blender::gpu::Batch *DRW_cache_bone_envelope_solid_get()
{
  if (!SHC.drw_bone_envelope) {
    const int lon_res = 24;
    const int lat_res = 24;
    const float lon_inc = 2.0f * M_PI / lon_res;
    const float lat_inc = M_PI / lat_res;
    uint v_idx = 0;

    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    /* Vertices */
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, ((lat_res + 1) * 2) * lon_res * 1);

    float lon = 0.0f;
    for (int i = 0; i < lon_res; i++, lon += lon_inc) {
      float lat = 0.0f;
      float co1[3], co2[3];

      /* NOTE: the poles are duplicated on purpose, to restart the strip. */

      /* 1st sphere */
      for (int j = 0; j < lat_res; j++, lat += lat_inc) {
        benv_lat_lon_to_co(lat, lon, co1);
        benv_lat_lon_to_co(lat, lon + lon_inc, co2);

        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, co1);
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, co2);
      }

      /* Closing the loop */
      benv_lat_lon_to_co(M_PI, lon, co1);
      benv_lat_lon_to_co(M_PI, lon + lon_inc, co2);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, co1);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v_idx++, co2);
    }

    SHC.drw_bone_envelope = GPU_batch_create_ex(
        GPU_PRIM_TRI_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_envelope;
}

blender::gpu::Batch *DRW_cache_bone_envelope_outline_get()
{
  if (!SHC.drw_bone_envelope_outline) {
#define CIRCLE_RESOL 64
    float v0[2], v1[2], v2[2];
    const float radius = 1.0f;

    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos0, pos1, pos2;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos0 = GPU_vertformat_attr_add(&format, "pos0", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      attr_id.pos1 = GPU_vertformat_attr_add(&format, "pos1", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      attr_id.pos2 = GPU_vertformat_attr_add(&format, "pos2", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, CIRCLE_RESOL + 1);

    v0[0] = radius * sinf((2.0f * M_PI * -2) / float(CIRCLE_RESOL));
    v0[1] = radius * cosf((2.0f * M_PI * -2) / float(CIRCLE_RESOL));
    v1[0] = radius * sinf((2.0f * M_PI * -1) / float(CIRCLE_RESOL));
    v1[1] = radius * cosf((2.0f * M_PI * -1) / float(CIRCLE_RESOL));

    /* Output 4 verts for each position. See shader for explanation. */
    uint v = 0;
    for (int a = 0; a <= CIRCLE_RESOL; a++) {
      v2[0] = radius * sinf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      v2[1] = radius * cosf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      GPU_vertbuf_attr_set(vbo, attr_id.pos0, v, v0);
      GPU_vertbuf_attr_set(vbo, attr_id.pos1, v, v1);
      GPU_vertbuf_attr_set(vbo, attr_id.pos2, v++, v2);
      copy_v2_v2(v0, v1);
      copy_v2_v2(v1, v2);
    }

    SHC.drw_bone_envelope_outline = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
#undef CIRCLE_RESOL
  }
  return SHC.drw_bone_envelope_outline;
}

blender::gpu::Batch *DRW_cache_bone_point_get()
{
  if (!SHC.drw_bone_point) {
#if 0 /* old style geometry sphere */
    const int lon_res = 16;
    const int lat_res = 8;
    const float rad = 0.05f;
    const float lon_inc = 2 * M_PI / lon_res;
    const float lat_inc = M_PI / lat_res;
    uint v_idx = 0;

    static GPUVertFormat format = {0};
    static struct {
      uint pos, nor;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
      attr_id.nor = GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    /* Vertices */
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, (lat_res - 1) * lon_res * 6);

    float lon = 0.0f;
    for (int i = 0; i < lon_res; i++, lon += lon_inc) {
      float lat = 0.0f;
      for (int j = 0; j < lat_res; j++, lat += lat_inc) {
        if (j != lat_res - 1) { /* Pole */
          add_lat_lon_vert(
              vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat + lat_inc, lon + lon_inc);
          add_lat_lon_vert(vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat + lat_inc, lon);
          add_lat_lon_vert(vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat, lon);
        }

        if (j != 0) { /* Pole */
          add_lat_lon_vert(vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat, lon + lon_inc);
          add_lat_lon_vert(
              vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat + lat_inc, lon + lon_inc);
          add_lat_lon_vert(vbo, attr_id.pos, attr_id.nor, &v_idx, rad, lat, lon);
        }
      }
    }

    SHC.drw_bone_point = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
#else
#  define CIRCLE_RESOL 64
    float v[2];
    const float radius = 0.05f;

    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, CIRCLE_RESOL);

    for (int a = 0; a < CIRCLE_RESOL; a++) {
      v[0] = radius * sinf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      v[1] = radius * cosf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
      GPU_vertbuf_attr_set(vbo, attr_id.pos, a, v);
    }

    SHC.drw_bone_point = GPU_batch_create_ex(GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
#  undef CIRCLE_RESOL
#endif
  }
  return SHC.drw_bone_point;
}

blender::gpu::Batch *DRW_cache_bone_point_wire_outline_get()
{
  if (!SHC.drw_bone_point_wire) {
#if 0 /* old style geometry sphere */
    blender::gpu::VertBuf *vbo = sphere_wire_vbo(0.05f);
    SHC.drw_bone_point_wire = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
#else
#  define CIRCLE_RESOL 64
    const float radius = 0.05f;

    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, CIRCLE_RESOL + 1);

    uint v = 0;
    for (int a = 0; a <= CIRCLE_RESOL; a++) {
      float pos[2];
      pos[0] = radius * sinf((2.0f * M_PI * a) / CIRCLE_RESOL);
      pos[1] = radius * cosf((2.0f * M_PI * a) / CIRCLE_RESOL);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v++, pos);
    }

    SHC.drw_bone_point_wire = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
#  undef CIRCLE_RESOL
#endif
  }
  return SHC.drw_bone_point_wire;
}

/* keep in sync with armature_stick_vert.glsl */
#define COL_WIRE (1 << 0)
#define COL_HEAD (1 << 1)
#define COL_TAIL (1 << 2)
#define COL_BONE (1 << 3)

#define POS_HEAD (1 << 4)
#define POS_TAIL (1 << 5)
#define POS_BONE (1 << 6)

blender::gpu::Batch *DRW_cache_bone_stick_get()
{
  if (!SHC.drw_bone_stick) {
#define CIRCLE_RESOL 12
    uint v = 0;
    uint flag;
    const float radius = 2.0f; /* head/tail radius */
    float pos[2];

    /* Position Only 2D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos, flag;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      attr_id.flag = GPU_vertformat_attr_add(&format, "flag", GPU_COMP_U32, 1, GPU_FETCH_INT);
    }

    const uint vcount = (CIRCLE_RESOL + 1) * 2 + 6;

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, vcount);

    GPUIndexBufBuilder elb;
    GPU_indexbuf_init_ex(&elb, GPU_PRIM_TRI_FAN, (CIRCLE_RESOL + 2) * 2 + 6 + 2, vcount);

    /* head/tail points */
    for (int i = 0; i < 2; i++) {
      /* center vertex */
      copy_v2_fl(pos, 0.0f);
      flag = (i == 0) ? POS_HEAD : POS_TAIL;
      flag |= (i == 0) ? COL_HEAD : COL_TAIL;
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, pos);
      GPU_vertbuf_attr_set(vbo, attr_id.flag, v, &flag);
      GPU_indexbuf_add_generic_vert(&elb, v++);
      /* circle vertices */
      flag |= COL_WIRE;
      for (int a = 0; a < CIRCLE_RESOL; a++) {
        pos[0] = radius * sinf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
        pos[1] = radius * cosf((2.0f * M_PI * a) / float(CIRCLE_RESOL));
        GPU_vertbuf_attr_set(vbo, attr_id.pos, v, pos);
        GPU_vertbuf_attr_set(vbo, attr_id.flag, v, &flag);
        GPU_indexbuf_add_generic_vert(&elb, v++);
      }
      /* Close the circle */
      GPU_indexbuf_add_generic_vert(&elb, v - CIRCLE_RESOL);

      GPU_indexbuf_add_primitive_restart(&elb);
    }

    /* Bone rectangle */
    pos[0] = 0.0f;
    for (int i = 0; i < 6; i++) {
      pos[1] = ELEM(i, 0, 3) ? 0.0f : ((i < 3) ? 1.0f : -1.0f);
      flag = ((i < 2 || i > 4) ? POS_HEAD : POS_TAIL) | (ELEM(i, 0, 3) ? 0 : COL_WIRE) | COL_BONE |
             POS_BONE;
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, pos);
      GPU_vertbuf_attr_set(vbo, attr_id.flag, v, &flag);
      GPU_indexbuf_add_generic_vert(&elb, v++);
    }

    SHC.drw_bone_stick = GPU_batch_create_ex(GPU_PRIM_TRI_FAN,
                                             vbo,
                                             GPU_indexbuf_build(&elb),
                                             GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
#undef CIRCLE_RESOL
  }
  return SHC.drw_bone_stick;
}

#define S_X 0.0215f
#define S_Y 0.025f
static float x_axis_name[4][2] = {
    {0.9f * S_X, 1.0f * S_Y},
    {-1.0f * S_X, -1.0f * S_Y},
    {-0.9f * S_X, 1.0f * S_Y},
    {1.0f * S_X, -1.0f * S_Y},
};
#define X_LEN ARRAY_SIZE(x_axis_name)
#undef S_X
#undef S_Y

#define S_X 0.0175f
#define S_Y 0.025f
static float y_axis_name[6][2] = {
    {-1.0f * S_X, 1.0f * S_Y},
    {0.0f * S_X, -0.1f * S_Y},
    {1.0f * S_X, 1.0f * S_Y},
    {0.0f * S_X, -0.1f * S_Y},
    {0.0f * S_X, -0.1f * S_Y},
    {0.0f * S_X, -1.0f * S_Y},
};
#define Y_LEN ARRAY_SIZE(y_axis_name)
#undef S_X
#undef S_Y

#define S_X 0.02f
#define S_Y 0.025f
static float z_axis_name[10][2] = {
    {-0.95f * S_X, 1.00f * S_Y},
    {0.95f * S_X, 1.00f * S_Y},
    {0.95f * S_X, 1.00f * S_Y},
    {0.95f * S_X, 0.90f * S_Y},
    {0.95f * S_X, 0.90f * S_Y},
    {-1.00f * S_X, -0.90f * S_Y},
    {-1.00f * S_X, -0.90f * S_Y},
    {-1.00f * S_X, -1.00f * S_Y},
    {-1.00f * S_X, -1.00f * S_Y},
    {1.00f * S_X, -1.00f * S_Y},
};
#define Z_LEN ARRAY_SIZE(z_axis_name)
#undef S_X
#undef S_Y

#define S_X 0.007f
#define S_Y 0.007f
static float axis_marker[8][2] = {
#if 0 /* square */
    {-1.0f * S_X, 1.0f * S_Y},
    {1.0f * S_X, 1.0f * S_Y},
    {1.0f * S_X, 1.0f * S_Y},
    {1.0f * S_X, -1.0f * S_Y},
    {1.0f * S_X, -1.0f * S_Y},
    {-1.0f * S_X, -1.0f * S_Y},
    {-1.0f * S_X, -1.0f * S_Y},
    {-1.0f * S_X, 1.0f * S_Y}
#else /* diamond */
    {-S_X, 0.0f},
    {0.0f, S_Y},
    {0.0f, S_Y},
    {S_X, 0.0f},
    {S_X, 0.0f},
    {0.0f, -S_Y},
    {0.0f, -S_Y},
    {-S_X, 0.0f}
#endif
};
#define MARKER_LEN ARRAY_SIZE(axis_marker)
#define MARKER_FILL_LAYER 6
#undef S_X
#undef S_Y

blender::gpu::Batch *DRW_cache_bone_arrows_get()
{
  if (!SHC.drw_bone_arrows) {
    GPUVertFormat format = extra_vert_format();
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    int v_len = (2 + MARKER_LEN * MARKER_FILL_LAYER) * 3 + (X_LEN + Y_LEN + Z_LEN);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    for (int axis = 0; axis < 3; axis++) {
      int flag = VCLASS_EMPTY_AXES | VCLASS_SCREENALIGNED;
      /* Vertex layout is XY screen position and axis in Z.
       * Fractional part of Z is a positive offset at axis unit position. */
      float p[3] = {0.0f, 0.0f, float(axis)};
      /* center to axis line */
      GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.0f}, 0});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{p[0], p[1], p[2]}, flag});
      /* Axis end marker */
      for (int j = 1; j < MARKER_FILL_LAYER + 1; j++) {
        for (int i = 0; i < MARKER_LEN; i++) {
          mul_v2_v2fl(p, axis_marker[i], 4.0f * j / float(MARKER_FILL_LAYER));
          GPU_vertbuf_vert_set(vbo, v++, Vert{{p[0], p[1], p[2]}, flag});
        }
      }
      /* Axis name */
      flag = VCLASS_EMPTY_AXES | VCLASS_EMPTY_AXES_NAME | VCLASS_SCREENALIGNED;
      const int axis_v_len[] = {X_LEN, Y_LEN, Z_LEN};
      float(*axis_v)[2] = (axis == 0) ? x_axis_name : ((axis == 1) ? y_axis_name : z_axis_name);
      p[2] = axis + 0.25f;
      for (int i = 0; i < axis_v_len[axis]; i++) {
        mul_v2_v2fl(p, axis_v[i], 4.0f);
        GPU_vertbuf_vert_set(vbo, v++, Vert{{p[0], p[1], p[2]}, flag});
      }
    }

    SHC.drw_bone_arrows = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_arrows;
}

static const float staticSine[16] = {
    0.0f,
    0.104528463268f,
    0.207911690818f,
    0.309016994375f,
    0.406736643076f,
    0.5f,
    0.587785252292f,
    0.669130606359f,
    0.743144825477f,
    0.809016994375f,
    0.866025403784f,
    0.913545457643f,
    0.951056516295f,
    0.978147600734f,
    0.994521895368f,
    1.0f,
};

#define set_vert(a, b, quarter) \
  { \
    copy_v2_fl2(pos, (quarter % 2 == 0) ? -(a) : (a), (quarter < 2) ? -(b) : (b)); \
    GPU_vertbuf_attr_set(vbo, attr_id.pos, v++, pos); \
  } \
  ((void)0)

blender::gpu::Batch *DRW_cache_bone_dof_sphere_get()
{
  if (!SHC.drw_bone_dof_sphere) {
    int i, j, q, n = ARRAY_SIZE(staticSine);
    float x, z, px, pz, pos[2];

    /* Position Only 3D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, n * n * 6 * 4);

    uint v = 0;
    for (q = 0; q < 4; q++) {
      pz = 0.0f;
      for (i = 1; i < n; i++) {
        z = staticSine[i];
        px = 0.0f;
        for (j = 1; j <= (n - i); j++) {
          x = staticSine[j];
          if (j == n - i) {
            set_vert(px, z, q);
            set_vert(px, pz, q);
            set_vert(x, pz, q);
          }
          else {
            set_vert(x, z, q);
            set_vert(x, pz, q);
            set_vert(px, z, q);

            set_vert(x, pz, q);
            set_vert(px, pz, q);
            set_vert(px, z, q);
          }
          px = x;
        }
        pz = z;
      }
    }
    /* TODO: allocate right count from the beginning. */
    GPU_vertbuf_data_resize(*vbo, v);

    SHC.drw_bone_dof_sphere = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_dof_sphere;
}

blender::gpu::Batch *DRW_cache_bone_dof_lines_get()
{
  if (!SHC.drw_bone_dof_lines) {
    int i, n = ARRAY_SIZE(staticSine);
    float pos[2];

    /* Position Only 3D format */
    static GPUVertFormat format = {0};
    static struct {
      uint pos;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    }

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, n * 4);

    uint v = 0;
    for (i = 0; i < n * 4; i++) {
      float a = (1.0f - (i / float(n * 4))) * 2.0f * M_PI;
      float x = cosf(a);
      float y = sinf(a);
      set_vert(x, y, 0);
    }

    SHC.drw_bone_dof_lines = GPU_batch_create_ex(
        GPU_PRIM_LINE_LOOP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_bone_dof_lines;
}

#undef set_vert

/** \} */

/* -------------------------------------------------------------------- */
/** \name Camera
 * \{ */

blender::gpu::Batch *DRW_cache_camera_frame_get()
{
  if (!SHC.drw_camera_frame) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = 2 * (4 + 4);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    const float p[4][2] = {{-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
    /* Frame */
    for (int a = 0; a < 4; a++) {
      for (int b = 0; b < 2; b++) {
        float x = p[(a + b) % 4][0];
        float y = p[(a + b) % 4][1];
        GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 1.0f}, VCLASS_CAMERA_FRAME});
      }
    }
    /* Wires to origin. */
    for (int a = 0; a < 4; a++) {
      float x = p[a][0];
      float y = p[a][1];
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 1.0f}, VCLASS_CAMERA_FRAME});
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 0.0f}, VCLASS_CAMERA_FRAME});
    }

    SHC.drw_camera_frame = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_frame;
}

blender::gpu::Batch *DRW_cache_camera_volume_get()
{
  if (!SHC.drw_camera_volume) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = ARRAY_SIZE(bone_box_solid_tris) * 3;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_CAMERA_FRAME | VCLASS_CAMERA_VOLUME;
    for (int i = 0; i < ARRAY_SIZE(bone_box_solid_tris); i++) {
      for (int a = 0; a < 3; a++) {
        float x = bone_box_verts[bone_box_solid_tris[i][a]][2];
        float y = bone_box_verts[bone_box_solid_tris[i][a]][0];
        float z = bone_box_verts[bone_box_solid_tris[i][a]][1];
        GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, z}, flag});
      }
    }

    SHC.drw_camera_volume = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_volume;
}

blender::gpu::Batch *DRW_cache_camera_volume_wire_get()
{
  if (!SHC.drw_camera_volume_wire) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = ARRAY_SIZE(bone_box_wire);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    int flag = VCLASS_CAMERA_FRAME | VCLASS_CAMERA_VOLUME;
    for (int i = 0; i < ARRAY_SIZE(bone_box_wire); i++) {
      float x = bone_box_verts[bone_box_wire[i]][2];
      float y = bone_box_verts[bone_box_wire[i]][0];
      float z = bone_box_verts[bone_box_wire[i]][1];
      GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, z}, flag});
    }

    SHC.drw_camera_volume_wire = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_volume_wire;
}

blender::gpu::Batch *DRW_cache_camera_tria_wire_get()
{
  if (!SHC.drw_camera_tria_wire) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = 2 * 3;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    const float p[3][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}};
    for (int a = 0; a < 3; a++) {
      for (int b = 0; b < 2; b++) {
        float x = p[(a + b) % 3][0];
        float y = p[(a + b) % 3][1];
        GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 1.0f}, VCLASS_CAMERA_FRAME});
      }
    }

    SHC.drw_camera_tria_wire = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_tria_wire;
}

blender::gpu::Batch *DRW_cache_camera_tria_get()
{
  if (!SHC.drw_camera_tria) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = 3;
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    /* Use camera frame position */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{-1.0f, 1.0f, 1.0f}, VCLASS_CAMERA_FRAME});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{1.0f, 1.0f, 1.0f}, VCLASS_CAMERA_FRAME});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 1.0f}, VCLASS_CAMERA_FRAME});

    SHC.drw_camera_tria = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_tria;
}

blender::gpu::Batch *DRW_cache_camera_distances_get()
{
  if (!SHC.drw_camera_distances) {
    GPUVertFormat format = extra_vert_format();

    const int v_len = 2 * (1 + DIAMOND_NSEGMENTS * 2 + 2);
    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, v_len);

    int v = 0;
    /* Direction Line */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, 0.0}, VCLASS_CAMERA_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 0.0, 1.0}, VCLASS_CAMERA_DIST});
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.5f, 0.0f, VCLASS_CAMERA_DIST | VCLASS_SCREENSPACE);
    circle_verts(vbo, &v, DIAMOND_NSEGMENTS, 1.5f, 1.0f, VCLASS_CAMERA_DIST | VCLASS_SCREENSPACE);
    /* Focus cross */
    GPU_vertbuf_vert_set(vbo, v++, Vert{{1.0, 0.0, 2.0}, VCLASS_CAMERA_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{-1.0, 0.0, 2.0}, VCLASS_CAMERA_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, 1.0, 2.0}, VCLASS_CAMERA_DIST});
    GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0, -1.0, 2.0}, VCLASS_CAMERA_DIST});

    SHC.drw_camera_distances = GPU_batch_create_ex(
        GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  }
  return SHC.drw_camera_distances;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Meshes
 * \{ */

blender::gpu::Batch *DRW_cache_mesh_all_verts_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_all_verts(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_all_edges_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_all_edges(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_loose_edges_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_loose_edges(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_edge_detection_get(Object *ob, bool *r_is_manifold)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_edge_detection(*static_cast<Mesh *>(ob->data), r_is_manifold);
}

blender::gpu::Batch *DRW_cache_mesh_surface_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_edges_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_edges(*static_cast<Mesh *>(ob->data));
}

Span<blender::gpu::Batch *> DRW_cache_mesh_surface_shaded_get(
    Object *ob, const blender::Span<const GPUMaterial *> materials)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_shaded(*ob, *static_cast<Mesh *>(ob->data), materials);
}

Span<blender::gpu::Batch *> DRW_cache_mesh_surface_texpaint_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_texpaint(*ob, *static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_texpaint_single_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_texpaint_single(*ob, *static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_vertpaint_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_vertpaint(*ob, *static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_sculptcolors_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_sculpt(*ob, *static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_weights_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_weights(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_face_wireframe_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_wireframes_face(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_mesh_analysis_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_edit_mesh_analysis(*static_cast<Mesh *>(ob->data));
}

blender::gpu::Batch *DRW_cache_mesh_surface_viewer_attribute_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_MESH);
  return DRW_mesh_batch_cache_get_surface_viewer_attribute(*static_cast<Mesh *>(ob->data));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve
 * \{ */

blender::gpu::Batch *DRW_cache_curve_edge_wire_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_CURVES_LEGACY);
  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_wire_edge(cu);
}

blender::gpu::Batch *DRW_cache_curve_edge_wire_viewer_attribute_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_CURVES_LEGACY);
  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_wire_edge_viewer_attribute(cu);
}

blender::gpu::Batch *DRW_cache_curve_edge_normal_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_CURVES_LEGACY);
  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_normal_edge(cu);
}

blender::gpu::Batch *DRW_cache_curve_edge_overlay_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF));

  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_edit_edges(cu);
}

blender::gpu::Batch *DRW_cache_curve_vert_overlay_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF));

  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_edit_verts(cu);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font
 * \{ */

blender::gpu::Batch *DRW_cache_text_edge_wire_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_FONT);
  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_wire_edge(cu);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface
 * \{ */

blender::gpu::Batch *DRW_cache_surf_edge_wire_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_SURF);
  Curve *cu = static_cast<Curve *>(ob->data);
  return DRW_curve_batch_cache_get_wire_edge(cu);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lattice
 * \{ */

blender::gpu::Batch *DRW_cache_lattice_verts_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_LATTICE);

  Lattice *lt = static_cast<Lattice *>(ob->data);
  return DRW_lattice_batch_cache_get_all_verts(lt);
}

blender::gpu::Batch *DRW_cache_lattice_wire_get(Object *ob, bool use_weight)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_LATTICE);

  Lattice *lt = static_cast<Lattice *>(ob->data);
  int actdef = -1;

  if (use_weight && !BLI_listbase_is_empty(&lt->vertex_group_names) && lt->editlatt &&
      lt->editlatt->latt->dvert)
  {
    actdef = lt->vertex_group_active_index - 1;
  }

  return DRW_lattice_batch_cache_get_all_edges(lt, use_weight, actdef);
}

blender::gpu::Batch *DRW_cache_lattice_vert_overlay_get(Object *ob)
{
  using namespace blender::draw;
  BLI_assert(ob->type == OB_LATTICE);

  Lattice *lt = static_cast<Lattice *>(ob->data);
  return DRW_lattice_batch_cache_get_edit_verts(lt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PointCloud
 * \{ */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume
 * \{ */

namespace blender::draw {

blender::gpu::Batch *DRW_cache_volume_face_wireframe_get(Object *ob)
{
  BLI_assert(ob->type == OB_VOLUME);
  return DRW_volume_batch_cache_get_wireframes_face(static_cast<Volume *>(ob->data));
}

blender::gpu::Batch *DRW_cache_volume_selection_surface_get(Object *ob)
{
  BLI_assert(ob->type == OB_VOLUME);
  return DRW_volume_batch_cache_get_selection_surface(static_cast<Volume *>(ob->data));
}

}  // namespace blender::draw

/** \} */

/* -------------------------------------------------------------------- */
/** \name Particles
 * \{ */

blender::gpu::Batch *DRW_cache_particles_get_hair(Object *object,
                                                  ParticleSystem *psys,
                                                  ModifierData *md)
{
  using namespace blender::draw;
  return DRW_particles_batch_cache_get_hair(object, psys, md);
}

blender::gpu::Batch *DRW_cache_particles_get_dots(Object *object, ParticleSystem *psys)
{
  using namespace blender::draw;
  return DRW_particles_batch_cache_get_dots(object, psys);
}

blender::gpu::Batch *DRW_cache_particles_get_edit_strands(Object *object,
                                                          ParticleSystem *psys,
                                                          PTCacheEdit *edit,
                                                          bool use_weight)
{
  using namespace blender::draw;
  return DRW_particles_batch_cache_get_edit_strands(object, psys, edit, use_weight);
}

blender::gpu::Batch *DRW_cache_particles_get_edit_inner_points(Object *object,
                                                               ParticleSystem *psys,
                                                               PTCacheEdit *edit)
{
  using namespace blender::draw;
  return DRW_particles_batch_cache_get_edit_inner_points(object, psys, edit);
}

blender::gpu::Batch *DRW_cache_particles_get_edit_tip_points(Object *object,
                                                             ParticleSystem *psys,
                                                             PTCacheEdit *edit)
{
  using namespace blender::draw;
  return DRW_particles_batch_cache_get_edit_tip_points(object, psys, edit);
}

blender::gpu::Batch *DRW_cache_particles_get_prim(int type)
{
  switch (type) {
    case PART_DRAW_CROSS:
      if (!SHC.drw_particle_cross) {
        GPUVertFormat format = extra_vert_format();
        blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
        GPU_vertbuf_data_alloc(*vbo, 6);

        int v = 0;
        int flag = 0;
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, -1.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 1.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{-1.0f, 0.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{1.0f, 0.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, -1.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 1.0f}, flag});

        SHC.drw_particle_cross = GPU_batch_create_ex(
            GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
      }

      return SHC.drw_particle_cross;
    case PART_DRAW_AXIS:
      if (!SHC.drw_particle_axis) {
        GPUVertFormat format = extra_vert_format();
        blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
        GPU_vertbuf_data_alloc(*vbo, 6);

        int v = 0;
        int flag = VCLASS_EMPTY_AXES;
        /* Set minimum to 0.001f so we can easily normalize to get the color. */
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0001f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 2.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0001f, 0.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{2.0f, 0.0f, 0.0f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 0.0001f}, flag});
        GPU_vertbuf_vert_set(vbo, v++, Vert{{0.0f, 0.0f, 2.0f}, flag});

        SHC.drw_particle_axis = GPU_batch_create_ex(
            GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
      }

      return SHC.drw_particle_axis;
    case PART_DRAW_CIRC:
#define CIRCLE_RESOL 32
      if (!SHC.drw_particle_circle) {
        GPUVertFormat format = extra_vert_format();
        blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
        GPU_vertbuf_data_alloc(*vbo, CIRCLE_RESOL + 1);

        int v = 0;
        int flag = VCLASS_SCREENALIGNED;
        for (int a = 0; a <= CIRCLE_RESOL; a++) {
          float angle = (2.0f * M_PI * a) / CIRCLE_RESOL;
          float x = sinf(angle);
          float y = cosf(angle);
          GPU_vertbuf_vert_set(vbo, v++, Vert{{x, y, 0.0f}, flag});
        }

        SHC.drw_particle_circle = GPU_batch_create_ex(
            GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
      }

      return SHC.drw_particle_circle;
#undef CIRCLE_RESOL
    default:
      BLI_assert(false);
      break;
  }

  return nullptr;
}

blender::gpu::Batch *DRW_cache_cursor_get(bool crosshair_lines)
{
  blender::gpu::Batch **drw_cursor = crosshair_lines ? &SHC.drw_cursor :
                                                       &SHC.drw_cursor_only_circle;

  if (*drw_cursor == nullptr) {
    const float f5 = 0.25f;
    const float f10 = 0.5f;
    const float f20 = 1.0f;

    const int segments = 16;
    const int vert_len = segments + 8;
    const int index_len = vert_len + 5;

    const float red[3] = {1.0f, 0.0f, 0.0f};
    const float white[3] = {1.0f, 1.0f, 1.0f};

    static GPUVertFormat format = {0};
    static struct {
      uint pos, color;
    } attr_id;
    if (format.attr_len == 0) {
      attr_id.pos = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
      attr_id.color = GPU_vertformat_attr_add(&format, "color", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    }

    GPUIndexBufBuilder elb;
    GPU_indexbuf_init_ex(&elb, GPU_PRIM_LINE_STRIP, index_len, vert_len);

    blender::gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, vert_len);

    int v = 0;
    for (int i = 0; i < segments; i++) {
      float angle = float(2 * M_PI) * (float(i) / float(segments));
      float x = f10 * cosf(angle);
      float y = f10 * sinf(angle);

      GPU_vertbuf_attr_set(vbo, attr_id.color, v, (i % 2 == 0) ? red : white);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{x, y});
      GPU_indexbuf_add_generic_vert(&elb, v++);
    }
    GPU_indexbuf_add_generic_vert(&elb, 0);

    if (crosshair_lines) {
      float crosshair_color[3];
      UI_GetThemeColor3fv(TH_VIEW_OVERLAY, crosshair_color);

      /* TODO(fclem): Remove primitive restart. Incompatible with wide lines. */
      GPU_indexbuf_add_primitive_restart(&elb);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{-f20, 0});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{-f5, 0});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);

      GPU_indexbuf_add_primitive_restart(&elb);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{+f5, 0});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{+f20, 0});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);

      GPU_indexbuf_add_primitive_restart(&elb);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{0, -f20});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{0, -f5});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);

      GPU_indexbuf_add_primitive_restart(&elb);

      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{0, +f5});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);
      GPU_vertbuf_attr_set(vbo, attr_id.pos, v, blender::float2{0, +f20});
      GPU_vertbuf_attr_set(vbo, attr_id.color, v, crosshair_color);
      GPU_indexbuf_add_generic_vert(&elb, v++);
    }

    blender::gpu::IndexBuf *ibo = GPU_indexbuf_build(&elb);

    *drw_cursor = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
  }
  return *drw_cursor;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Batch Cache Implementation (common)
 * \{ */

void drw_batch_cache_validate(Object *ob)
{
  using namespace blender::draw;
  switch (ob->type) {
    case OB_MESH:
      DRW_mesh_batch_cache_validate(*(Mesh *)ob->data);
      break;
    case OB_CURVES_LEGACY:
    case OB_FONT:
    case OB_SURF:
      DRW_curve_batch_cache_validate((Curve *)ob->data);
      break;
    case OB_LATTICE:
      DRW_lattice_batch_cache_validate((Lattice *)ob->data);
      break;
    case OB_CURVES:
      DRW_curves_batch_cache_validate((Curves *)ob->data);
      break;
    case OB_POINTCLOUD:
      DRW_pointcloud_batch_cache_validate((PointCloud *)ob->data);
      break;
    case OB_VOLUME:
      DRW_volume_batch_cache_validate((Volume *)ob->data);
      break;
    case OB_GREASE_PENCIL:
      DRW_grease_pencil_batch_cache_validate((GreasePencil *)ob->data);
    default:
      break;
  }
}

void drw_batch_cache_generate_requested(Object *ob)
{
  using namespace blender::draw;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  const Scene *scene = draw_ctx->scene;
  const enum eContextObjectMode mode = CTX_data_mode_enum_ex(
      draw_ctx->object_edit, draw_ctx->obact, draw_ctx->object_mode);
  const bool is_paint_mode = ELEM(
      mode, CTX_MODE_SCULPT, CTX_MODE_PAINT_TEXTURE, CTX_MODE_PAINT_VERTEX, CTX_MODE_PAINT_WEIGHT);

  const bool use_hide = ((ob->type == OB_MESH) &&
                         ((is_paint_mode && (ob == draw_ctx->obact) &&
                           DRW_object_use_hide_faces(ob)) ||
                          ((mode == CTX_MODE_EDIT_MESH) && (ob->mode == OB_MODE_EDIT))));

  switch (ob->type) {
    case OB_MESH:
      DRW_mesh_batch_cache_create_requested(
          *DST.task_graph, *ob, *(Mesh *)ob->data, *scene, is_paint_mode, use_hide);
      break;
    case OB_CURVES_LEGACY:
    case OB_FONT:
    case OB_SURF:
      DRW_curve_batch_cache_create_requested(ob, scene);
      break;
    case OB_CURVES:
      DRW_curves_batch_cache_create_requested(ob);
      break;
    case OB_POINTCLOUD:
      DRW_pointcloud_batch_cache_create_requested(ob);
      break;
    /* TODO: all cases. */
    default:
      break;
  }
}

void drw_batch_cache_generate_requested_evaluated_mesh_or_curve(Object *ob)
{
  using namespace blender::draw;
  /* NOTE: Logic here is duplicated from #drw_batch_cache_generate_requested. */

  const DRWContextState *draw_ctx = DRW_context_state_get();
  const Scene *scene = draw_ctx->scene;
  const enum eContextObjectMode mode = CTX_data_mode_enum_ex(
      draw_ctx->object_edit, draw_ctx->obact, draw_ctx->object_mode);
  const bool is_paint_mode = ELEM(
      mode, CTX_MODE_SCULPT, CTX_MODE_PAINT_TEXTURE, CTX_MODE_PAINT_VERTEX, CTX_MODE_PAINT_WEIGHT);

  const bool use_hide = ((ob->type == OB_MESH) &&
                         ((is_paint_mode && (ob == draw_ctx->obact) &&
                           DRW_object_use_hide_faces(ob)) ||
                          ((mode == CTX_MODE_EDIT_MESH) && (ob->mode == OB_MODE_EDIT))));

  Mesh *mesh = BKE_object_get_evaluated_mesh_no_subsurf_unchecked(ob);
  /* Try getting the mesh first and if that fails, try getting the curve data.
   * If the curves are surfaces or have certain modifiers applied to them, the will have mesh data
   * of the final result.
   */
  if (mesh != nullptr) {
    DRW_mesh_batch_cache_create_requested(
        *DST.task_graph, *ob, *mesh, *scene, is_paint_mode, use_hide);
  }
  else if (ELEM(ob->type, OB_CURVES_LEGACY, OB_FONT, OB_SURF)) {
    DRW_curve_batch_cache_create_requested(ob, scene);
  }
}

void drw_batch_cache_generate_requested_delayed(Object *ob)
{
  BLI_gset_add(DST.delayed_extraction, ob);
}

namespace blender::draw {
void DRW_batch_cache_free_old(Object *ob, int ctime)
{
  switch (ob->type) {
    case OB_MESH:
      DRW_mesh_batch_cache_free_old((Mesh *)ob->data, ctime);
      break;
    case OB_CURVES:
      DRW_curves_batch_cache_free_old((Curves *)ob->data, ctime);
      break;
    case OB_POINTCLOUD:
      DRW_pointcloud_batch_cache_free_old((PointCloud *)ob->data, ctime);
      break;
    default:
      break;
  }
}
}  // namespace blender::draw

/** \} */

void DRW_cdlayer_attr_aliases_add(GPUVertFormat *format,
                                  const char *base_name,
                                  const int data_type,
                                  const char *layer_name,
                                  bool is_active_render,
                                  bool is_active_layer)
{
  char attr_name[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
  GPU_vertformat_safe_attr_name(layer_name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);

  /* Attribute layer name. */
  SNPRINTF(attr_name, "%s%s", base_name, attr_safe_name);
  GPU_vertformat_alias_add(format, attr_name);

  /* Auto layer name. */
  SNPRINTF(attr_name, "a%s", attr_safe_name);
  GPU_vertformat_alias_add(format, attr_name);

  /* Active render layer name. */
  if (is_active_render) {
    GPU_vertformat_alias_add(format, data_type == CD_PROP_FLOAT2 ? "a" : base_name);
  }

  /* Active display layer name. */
  if (is_active_layer) {
    SNPRINTF(attr_name, "a%s", base_name);
    GPU_vertformat_alias_add(format, attr_name);
  }
}
