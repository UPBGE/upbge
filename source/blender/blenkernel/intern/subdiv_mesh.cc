/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2018 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "atomic_ops.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_math_vector.h"

#include "BKE_customdata.h"
#include "BKE_key.h"
#include "BKE_mesh.h"
#include "BKE_subdiv.h"
#include "BKE_subdiv_eval.h"
#include "BKE_subdiv_foreach.h"
#include "BKE_subdiv_mesh.h"

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Subdivision Context
 * \{ */

struct SubdivMeshContext {
  const SubdivToMeshSettings *settings;
  const Mesh *coarse_mesh;
  Subdiv *subdiv;
  Mesh *subdiv_mesh;
  /* Cached custom data arrays for faster access. */
  int *vert_origindex;
  int *edge_origindex;
  int *loop_origindex;
  int *poly_origindex;
  /* UV layers interpolation. */
  int num_uv_layers;
  MLoopUV *uv_layers[MAX_MTFACE];
  /* Original coordinates (ORCO) interpolation. */
  float (*orco)[3];
  float (*cloth_orco)[3];
  /* Per-subdivided vertex counter of averaged values. */
  int *accumulated_counters;
  bool have_displacement;
};

static void subdiv_mesh_ctx_cache_uv_layers(SubdivMeshContext *ctx)
{
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  ctx->num_uv_layers = CustomData_number_of_layers(&subdiv_mesh->ldata, CD_MLOOPUV);
  for (int layer_index = 0; layer_index < ctx->num_uv_layers; layer_index++) {
    ctx->uv_layers[layer_index] = static_cast<MLoopUV *>(
        CustomData_get_layer_n(&subdiv_mesh->ldata, CD_MLOOPUV, layer_index));
  }
}

static void subdiv_mesh_ctx_cache_custom_data_layers(SubdivMeshContext *ctx)
{
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  /* Pointers to original indices layers. */
  ctx->vert_origindex = static_cast<int *>(
      CustomData_get_layer(&subdiv_mesh->vdata, CD_ORIGINDEX));
  ctx->edge_origindex = static_cast<int *>(
      CustomData_get_layer(&subdiv_mesh->edata, CD_ORIGINDEX));
  ctx->loop_origindex = static_cast<int *>(
      CustomData_get_layer(&subdiv_mesh->ldata, CD_ORIGINDEX));
  ctx->poly_origindex = static_cast<int *>(
      CustomData_get_layer(&subdiv_mesh->pdata, CD_ORIGINDEX));
  /* UV layers interpolation. */
  subdiv_mesh_ctx_cache_uv_layers(ctx);
  /* Orco interpolation. */
  ctx->orco = static_cast<float(*)[3]>(CustomData_get_layer(&subdiv_mesh->vdata, CD_ORCO));
  ctx->cloth_orco = static_cast<float(*)[3]>(
      CustomData_get_layer(&subdiv_mesh->vdata, CD_CLOTH_ORCO));
}

static void subdiv_mesh_prepare_accumulator(SubdivMeshContext *ctx, int num_vertices)
{
  if (!ctx->have_displacement) {
    return;
  }
  ctx->accumulated_counters = static_cast<int *>(
      MEM_calloc_arrayN(num_vertices, sizeof(*ctx->accumulated_counters), __func__));
}

static void subdiv_mesh_context_free(SubdivMeshContext *ctx)
{
  MEM_SAFE_FREE(ctx->accumulated_counters);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loop custom data copy helpers
 * \{ */

struct LoopsOfPtex {
  /* First loop of the ptex, starts at ptex (0, 0) and goes in u direction. */
  const MLoop *first_loop;
  /* Last loop of the ptex, starts at ptex (0, 0) and goes in v direction. */
  const MLoop *last_loop;
  /* For quad coarse faces only. */
  const MLoop *second_loop;
  const MLoop *third_loop;
};

static void loops_of_ptex_get(const SubdivMeshContext *ctx,
                              LoopsOfPtex *loops_of_ptex,
                              const MPoly *coarse_poly,
                              const int ptex_of_poly_index)
{
  const MLoop *coarse_mloop = ctx->coarse_mesh->mloop;
  const int first_ptex_loop_index = coarse_poly->loopstart + ptex_of_poly_index;
  /* Loop which look in the (opposite) V direction of the current
   * ptex face.
   *
   * TODO(sergey): Get rid of using module on every iteration. */
  const int last_ptex_loop_index = coarse_poly->loopstart +
                                   (ptex_of_poly_index + coarse_poly->totloop - 1) %
                                       coarse_poly->totloop;
  loops_of_ptex->first_loop = &coarse_mloop[first_ptex_loop_index];
  loops_of_ptex->last_loop = &coarse_mloop[last_ptex_loop_index];
  if (coarse_poly->totloop == 4) {
    loops_of_ptex->second_loop = loops_of_ptex->first_loop + 1;
    loops_of_ptex->third_loop = loops_of_ptex->first_loop + 2;
  }
  else {
    loops_of_ptex->second_loop = nullptr;
    loops_of_ptex->third_loop = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex custom data interpolation helpers
 * \{ */

/* TODO(sergey): Somehow de-duplicate with loops storage, without too much
 * exception cases all over the code. */

struct VerticesForInterpolation {
  /* This field points to a vertex data which is to be used for interpolation.
   * The idea is to avoid unnecessary allocations for regular faces, where
   * we can simply use corner vertices. */
  const CustomData *vertex_data;
  /* Vertices data calculated for ptex corners. There are always 4 elements
   * in this custom data, aligned the following way:
   *
   *   index 0 -> uv (0, 0)
   *   index 1 -> uv (0, 1)
   *   index 2 -> uv (1, 1)
   *   index 3 -> uv (1, 0)
   *
   * Is allocated for non-regular faces (triangles and n-gons). */
  CustomData vertex_data_storage;
  bool vertex_data_storage_allocated;
  /* Indices within vertex_data to interpolate for. The indices are aligned
   * with uv coordinates in a similar way as indices in loop_data_storage. */
  int vertex_indices[4];
};

static void vertex_interpolation_init(const SubdivMeshContext *ctx,
                                      VerticesForInterpolation *vertex_interpolation,
                                      const MPoly *coarse_poly)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MLoop *coarse_mloop = coarse_mesh->mloop;
  if (coarse_poly->totloop == 4) {
    vertex_interpolation->vertex_data = &coarse_mesh->vdata;
    vertex_interpolation->vertex_indices[0] = coarse_mloop[coarse_poly->loopstart + 0].v;
    vertex_interpolation->vertex_indices[1] = coarse_mloop[coarse_poly->loopstart + 1].v;
    vertex_interpolation->vertex_indices[2] = coarse_mloop[coarse_poly->loopstart + 2].v;
    vertex_interpolation->vertex_indices[3] = coarse_mloop[coarse_poly->loopstart + 3].v;
    vertex_interpolation->vertex_data_storage_allocated = false;
  }
  else {
    vertex_interpolation->vertex_data = &vertex_interpolation->vertex_data_storage;
    /* Allocate storage for loops corresponding to ptex corners. */
    CustomData_copy(&ctx->coarse_mesh->vdata,
                    &vertex_interpolation->vertex_data_storage,
                    CD_MASK_EVERYTHING.vmask,
                    CD_CALLOC,
                    4);
    /* Initialize indices. */
    vertex_interpolation->vertex_indices[0] = 0;
    vertex_interpolation->vertex_indices[1] = 1;
    vertex_interpolation->vertex_indices[2] = 2;
    vertex_interpolation->vertex_indices[3] = 3;
    vertex_interpolation->vertex_data_storage_allocated = true;
    /* Interpolate center of poly right away, it stays unchanged for all
     * ptex faces. */
    const float weight = 1.0f / (float)coarse_poly->totloop;
    blender::Array<float, 32> weights(coarse_poly->totloop);
    blender::Array<int, 32> indices(coarse_poly->totloop);
    for (int i = 0; i < coarse_poly->totloop; i++) {
      weights[i] = weight;
      indices[i] = coarse_mloop[coarse_poly->loopstart + i].v;
    }
    CustomData_interp(&coarse_mesh->vdata,
                      &vertex_interpolation->vertex_data_storage,
                      indices.data(),
                      weights.data(),
                      nullptr,
                      coarse_poly->totloop,
                      2);
  }
}

static void vertex_interpolation_from_corner(const SubdivMeshContext *ctx,
                                             VerticesForInterpolation *vertex_interpolation,
                                             const MPoly *coarse_poly,
                                             const int corner)
{
  if (coarse_poly->totloop == 4) {
    /* Nothing to do, all indices and data is already assigned. */
  }
  else {
    const CustomData *vertex_data = &ctx->coarse_mesh->vdata;
    const Mesh *coarse_mesh = ctx->coarse_mesh;
    const MLoop *coarse_mloop = coarse_mesh->mloop;
    LoopsOfPtex loops_of_ptex;
    loops_of_ptex_get(ctx, &loops_of_ptex, coarse_poly, corner);
    /* Ptex face corner corresponds to a poly loop with same index. */
    CustomData_copy_data(vertex_data,
                         &vertex_interpolation->vertex_data_storage,
                         coarse_mloop[coarse_poly->loopstart + corner].v,
                         0,
                         1);
    /* Interpolate remaining ptex face corners, which hits loops
     * middle points.
     *
     * TODO(sergey): Re-use one of interpolation results from previous
     * iteration. */
    const float weights[2] = {0.5f, 0.5f};
    const int first_loop_index = loops_of_ptex.first_loop - coarse_mloop;
    const int last_loop_index = loops_of_ptex.last_loop - coarse_mloop;
    const int first_indices[2] = {
        static_cast<int>(coarse_mloop[first_loop_index].v),
        static_cast<int>(
            coarse_mloop[coarse_poly->loopstart +
                         (first_loop_index - coarse_poly->loopstart + 1) % coarse_poly->totloop]
                .v)};
    const int last_indices[2] = {
        static_cast<int>(coarse_mloop[first_loop_index].v),
        static_cast<int>(coarse_mloop[last_loop_index].v),
    };
    CustomData_interp(vertex_data,
                      &vertex_interpolation->vertex_data_storage,
                      first_indices,
                      weights,
                      nullptr,
                      2,
                      1);
    CustomData_interp(vertex_data,
                      &vertex_interpolation->vertex_data_storage,
                      last_indices,
                      weights,
                      nullptr,
                      2,
                      3);
  }
}

static void vertex_interpolation_end(VerticesForInterpolation *vertex_interpolation)
{
  if (vertex_interpolation->vertex_data_storage_allocated) {
    CustomData_free(&vertex_interpolation->vertex_data_storage, 4);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loop custom data interpolation helpers
 * \{ */

struct LoopsForInterpolation {
  /* This field points to a loop data which is to be used for interpolation.
   * The idea is to avoid unnecessary allocations for regular faces, where
   * we can simply interpolate corner vertices. */
  const CustomData *loop_data;
  /* Loops data calculated for ptex corners. There are always 4 elements
   * in this custom data, aligned the following way:
   *
   *   index 0 -> uv (0, 0)
   *   index 1 -> uv (0, 1)
   *   index 2 -> uv (1, 1)
   *   index 3 -> uv (1, 0)
   *
   * Is allocated for non-regular faces (triangles and n-gons). */
  CustomData loop_data_storage;
  bool loop_data_storage_allocated;
  /* Indices within loop_data to interpolate for. The indices are aligned with
   * uv coordinates in a similar way as indices in loop_data_storage. */
  int loop_indices[4];
};

static void loop_interpolation_init(const SubdivMeshContext *ctx,
                                    LoopsForInterpolation *loop_interpolation,
                                    const MPoly *coarse_poly)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  if (coarse_poly->totloop == 4) {
    loop_interpolation->loop_data = &coarse_mesh->ldata;
    loop_interpolation->loop_indices[0] = coarse_poly->loopstart + 0;
    loop_interpolation->loop_indices[1] = coarse_poly->loopstart + 1;
    loop_interpolation->loop_indices[2] = coarse_poly->loopstart + 2;
    loop_interpolation->loop_indices[3] = coarse_poly->loopstart + 3;
    loop_interpolation->loop_data_storage_allocated = false;
  }
  else {
    loop_interpolation->loop_data = &loop_interpolation->loop_data_storage;
    /* Allocate storage for loops corresponding to ptex corners. */
    CustomData_copy(&ctx->coarse_mesh->ldata,
                    &loop_interpolation->loop_data_storage,
                    CD_MASK_EVERYTHING.lmask,
                    CD_CALLOC,
                    4);
    /* Initialize indices. */
    loop_interpolation->loop_indices[0] = 0;
    loop_interpolation->loop_indices[1] = 1;
    loop_interpolation->loop_indices[2] = 2;
    loop_interpolation->loop_indices[3] = 3;
    loop_interpolation->loop_data_storage_allocated = true;
    /* Interpolate center of poly right away, it stays unchanged for all
     * ptex faces. */
    const float weight = 1.0f / (float)coarse_poly->totloop;
    blender::Array<float, 32> weights(coarse_poly->totloop);
    blender::Array<int, 32> indices(coarse_poly->totloop);
    for (int i = 0; i < coarse_poly->totloop; i++) {
      weights[i] = weight;
      indices[i] = coarse_poly->loopstart + i;
    }
    CustomData_interp(&coarse_mesh->ldata,
                      &loop_interpolation->loop_data_storage,
                      indices.data(),
                      weights.data(),
                      nullptr,
                      coarse_poly->totloop,
                      2);
  }
}

static void loop_interpolation_from_corner(const SubdivMeshContext *ctx,
                                           LoopsForInterpolation *loop_interpolation,
                                           const MPoly *coarse_poly,
                                           const int corner)
{
  if (coarse_poly->totloop == 4) {
    /* Nothing to do, all indices and data is already assigned. */
  }
  else {
    const CustomData *loop_data = &ctx->coarse_mesh->ldata;
    const Mesh *coarse_mesh = ctx->coarse_mesh;
    const MLoop *coarse_mloop = coarse_mesh->mloop;
    LoopsOfPtex loops_of_ptex;
    loops_of_ptex_get(ctx, &loops_of_ptex, coarse_poly, corner);
    /* Ptex face corner corresponds to a poly loop with same index. */
    CustomData_free_elem(&loop_interpolation->loop_data_storage, 0, 1);
    CustomData_copy_data(
        loop_data, &loop_interpolation->loop_data_storage, coarse_poly->loopstart + corner, 0, 1);
    /* Interpolate remaining ptex face corners, which hits loops
     * middle points.
     *
     * TODO(sergey): Re-use one of interpolation results from previous
     * iteration. */
    const float weights[2] = {0.5f, 0.5f};
    const int base_loop_index = coarse_poly->loopstart;
    const int first_loop_index = loops_of_ptex.first_loop - coarse_mloop;
    const int second_loop_index = base_loop_index +
                                  (first_loop_index - base_loop_index + 1) % coarse_poly->totloop;
    const int first_indices[2] = {first_loop_index, second_loop_index};
    const int last_indices[2] = {
        static_cast<int>(loops_of_ptex.last_loop - coarse_mloop),
        static_cast<int>(loops_of_ptex.first_loop - coarse_mloop),
    };
    CustomData_interp(
        loop_data, &loop_interpolation->loop_data_storage, first_indices, weights, nullptr, 2, 1);
    CustomData_interp(
        loop_data, &loop_interpolation->loop_data_storage, last_indices, weights, nullptr, 2, 3);
  }
}

static void loop_interpolation_end(LoopsForInterpolation *loop_interpolation)
{
  if (loop_interpolation->loop_data_storage_allocated) {
    CustomData_free(&loop_interpolation->loop_data_storage, 4);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name TLS
 * \{ */

struct SubdivMeshTLS {
  bool vertex_interpolation_initialized;
  VerticesForInterpolation vertex_interpolation;
  const MPoly *vertex_interpolation_coarse_poly;
  int vertex_interpolation_coarse_corner;

  bool loop_interpolation_initialized;
  LoopsForInterpolation loop_interpolation;
  const MPoly *loop_interpolation_coarse_poly;
  int loop_interpolation_coarse_corner;
};

static void subdiv_mesh_tls_free(void *tls_v)
{
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  if (tls->vertex_interpolation_initialized) {
    vertex_interpolation_end(&tls->vertex_interpolation);
  }
  if (tls->loop_interpolation_initialized) {
    loop_interpolation_end(&tls->loop_interpolation);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation helper functions
 * \{ */

static void subdiv_vertex_orco_evaluate(const SubdivMeshContext *ctx,
                                        const int ptex_face_index,
                                        const float u,
                                        const float v,
                                        const int subdiv_vertex_index)
{
  if (ctx->orco || ctx->cloth_orco) {
    float vertex_data[6];
    BKE_subdiv_eval_vertex_data(ctx->subdiv, ptex_face_index, u, v, vertex_data);

    if (ctx->orco) {
      copy_v3_v3(ctx->orco[subdiv_vertex_index], vertex_data);
      if (ctx->cloth_orco) {
        copy_v3_v3(ctx->orco[subdiv_vertex_index], vertex_data + 3);
      }
    }
    else if (ctx->cloth_orco) {
      copy_v3_v3(ctx->orco[subdiv_vertex_index], vertex_data);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Accumulation helpers
 * \{ */

static void subdiv_accumulate_vertex_displacement(SubdivMeshContext *ctx,
                                                  const int ptex_face_index,
                                                  const float u,
                                                  const float v,
                                                  MVert *subdiv_vert)
{
  /* Accumulate displacement. */
  Subdiv *subdiv = ctx->subdiv;
  const int subdiv_vertex_index = subdiv_vert - ctx->subdiv_mesh->mvert;
  float dummy_P[3], dPdu[3], dPdv[3], D[3];
  BKE_subdiv_eval_limit_point_and_derivatives(subdiv, ptex_face_index, u, v, dummy_P, dPdu, dPdv);

  /* NOTE: The subdivided mesh is allocated in this module, and its vertices are kept at zero
   * locations as a default calloc(). */
  BKE_subdiv_eval_displacement(subdiv, ptex_face_index, u, v, dPdu, dPdv, D);
  add_v3_v3(subdiv_vert->co, D);

  if (ctx->accumulated_counters) {
    ++ctx->accumulated_counters[subdiv_vertex_index];
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks
 * \{ */

static bool subdiv_mesh_topology_info(const SubdivForeachContext *foreach_context,
                                      const int num_vertices,
                                      const int num_edges,
                                      const int num_loops,
                                      const int num_polygons,
                                      const int *UNUSED(subdiv_polygon_offset))
{
  /* Multi-resolution grid data will be applied or become invalid after subdivision,
   * so don't try to preserve it and use memory. */
  CustomData_MeshMasks mask = CD_MASK_EVERYTHING;
  mask.lmask &= ~CD_MASK_MULTIRES_GRIDS;

  SubdivMeshContext *subdiv_context = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  subdiv_context->subdiv_mesh = BKE_mesh_new_nomain_from_template_ex(
      subdiv_context->coarse_mesh, num_vertices, num_edges, 0, num_loops, num_polygons, mask);
  subdiv_mesh_ctx_cache_custom_data_layers(subdiv_context);
  subdiv_mesh_prepare_accumulator(subdiv_context, num_vertices);
  MEM_SAFE_FREE(subdiv_context->subdiv_mesh->runtime.subsurf_face_dot_tags);
  subdiv_context->subdiv_mesh->runtime.subsurf_face_dot_tags = BLI_BITMAP_NEW(num_vertices,
                                                                              __func__);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex subdivision process
 * \{ */

static void subdiv_vertex_data_copy(const SubdivMeshContext *ctx,
                                    const MVert *coarse_vertex,
                                    MVert *subdiv_vertex)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  const int coarse_vertex_index = coarse_vertex - coarse_mesh->mvert;
  const int subdiv_vertex_index = subdiv_vertex - subdiv_mesh->mvert;
  CustomData_copy_data(
      &coarse_mesh->vdata, &ctx->subdiv_mesh->vdata, coarse_vertex_index, subdiv_vertex_index, 1);
}

static void subdiv_vertex_data_interpolate(const SubdivMeshContext *ctx,
                                           MVert *subdiv_vertex,
                                           const VerticesForInterpolation *vertex_interpolation,
                                           const float u,
                                           const float v)
{
  const int subdiv_vertex_index = subdiv_vertex - ctx->subdiv_mesh->mvert;
  const float weights[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
  CustomData_interp(vertex_interpolation->vertex_data,
                    &ctx->subdiv_mesh->vdata,
                    vertex_interpolation->vertex_indices,
                    weights,
                    nullptr,
                    4,
                    subdiv_vertex_index);
  if (ctx->vert_origindex != nullptr) {
    ctx->vert_origindex[subdiv_vertex_index] = ORIGINDEX_NONE;
  }
}

static void evaluate_vertex_and_apply_displacement_copy(const SubdivMeshContext *ctx,
                                                        const int ptex_face_index,
                                                        const float u,
                                                        const float v,
                                                        const MVert *coarse_vert,
                                                        MVert *subdiv_vert)
{
  const int subdiv_vertex_index = subdiv_vert - ctx->subdiv_mesh->mvert;
  /* Displacement is accumulated in subdiv vertex position.
   * Needs to be backed up before copying data from original vertex. */
  float D[3] = {0.0f, 0.0f, 0.0f};
  if (ctx->have_displacement) {
    const float inv_num_accumulated = 1.0f / ctx->accumulated_counters[subdiv_vertex_index];
    copy_v3_v3(D, subdiv_vert->co);
    mul_v3_fl(D, inv_num_accumulated);
  }
  /* Copy custom data and evaluate position. */
  subdiv_vertex_data_copy(ctx, coarse_vert, subdiv_vert);
  BKE_subdiv_eval_limit_point(ctx->subdiv, ptex_face_index, u, v, subdiv_vert->co);
  /* Apply displacement. */
  add_v3_v3(subdiv_vert->co, D);
  /* Evaluate undeformed texture coordinate. */
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
  /* Remove face-dot flag. This can happen if there is more than one subsurf modifier. */
  BLI_BITMAP_DISABLE(ctx->subdiv_mesh->runtime.subsurf_face_dot_tags, subdiv_vertex_index);
}

static void evaluate_vertex_and_apply_displacement_interpolate(
    const SubdivMeshContext *ctx,
    const int ptex_face_index,
    const float u,
    const float v,
    VerticesForInterpolation *vertex_interpolation,
    MVert *subdiv_vert)
{
  const int subdiv_vertex_index = subdiv_vert - ctx->subdiv_mesh->mvert;
  /* Displacement is accumulated in subdiv vertex position.
   * Needs to be backed up before copying data from original vertex. */
  float D[3] = {0.0f, 0.0f, 0.0f};
  if (ctx->have_displacement) {
    const float inv_num_accumulated = 1.0f / ctx->accumulated_counters[subdiv_vertex_index];
    copy_v3_v3(D, subdiv_vert->co);
    mul_v3_fl(D, inv_num_accumulated);
  }
  /* Interpolate custom data and evaluate position. */
  subdiv_vertex_data_interpolate(ctx, subdiv_vert, vertex_interpolation, u, v);
  BKE_subdiv_eval_limit_point(ctx->subdiv, ptex_face_index, u, v, subdiv_vert->co);
  /* Apply displacement. */
  add_v3_v3(subdiv_vert->co, D);
  /* Evaluate undeformed texture coordinate. */
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_displacement_every_corner_or_edge(
    const SubdivForeachContext *foreach_context,
    void *UNUSED(tls),
    const int ptex_face_index,
    const float u,
    const float v,
    const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  MVert *subdiv_vert = &subdiv_mvert[subdiv_vertex_index];
  subdiv_accumulate_vertex_displacement(ctx, ptex_face_index, u, v, subdiv_vert);
}

static void subdiv_mesh_vertex_displacement_every_corner(
    const SubdivForeachContext *foreach_context,
    void *tls,
    const int ptex_face_index,
    const float u,
    const float v,
    const int UNUSED(coarse_vertex_index),
    const int UNUSED(coarse_poly_index),
    const int UNUSED(coarse_corner),
    const int subdiv_vertex_index)
{
  subdiv_mesh_vertex_displacement_every_corner_or_edge(
      foreach_context, tls, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_displacement_every_edge(const SubdivForeachContext *foreach_context,
                                                       void *tls,
                                                       const int ptex_face_index,
                                                       const float u,
                                                       const float v,
                                                       const int UNUSED(coarse_edge_index),
                                                       const int UNUSED(coarse_poly_index),
                                                       const int UNUSED(coarse_corner),
                                                       const int subdiv_vertex_index)
{
  subdiv_mesh_vertex_displacement_every_corner_or_edge(
      foreach_context, tls, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_corner(const SubdivForeachContext *foreach_context,
                                      void *UNUSED(tls),
                                      const int ptex_face_index,
                                      const float u,
                                      const float v,
                                      const int coarse_vertex_index,
                                      const int UNUSED(coarse_poly_index),
                                      const int UNUSED(coarse_corner),
                                      const int subdiv_vertex_index)
{
  BLI_assert(coarse_vertex_index != ORIGINDEX_NONE);
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MVert *coarse_mvert = coarse_mesh->mvert;
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  const MVert *coarse_vert = &coarse_mvert[coarse_vertex_index];
  MVert *subdiv_vert = &subdiv_mvert[subdiv_vertex_index];
  evaluate_vertex_and_apply_displacement_copy(
      ctx, ptex_face_index, u, v, coarse_vert, subdiv_vert);
}

static void subdiv_mesh_ensure_vertex_interpolation(SubdivMeshContext *ctx,
                                                    SubdivMeshTLS *tls,
                                                    const MPoly *coarse_poly,
                                                    const int coarse_corner)
{
  /* Check whether we've moved to another corner or polygon. */
  if (tls->vertex_interpolation_initialized) {
    if (tls->vertex_interpolation_coarse_poly != coarse_poly ||
        tls->vertex_interpolation_coarse_corner != coarse_corner) {
      vertex_interpolation_end(&tls->vertex_interpolation);
      tls->vertex_interpolation_initialized = false;
    }
  }
  /* Initialize the interpolation. */
  if (!tls->vertex_interpolation_initialized) {
    vertex_interpolation_init(ctx, &tls->vertex_interpolation, coarse_poly);
  }
  /* Update it for a new corner if needed. */
  if (!tls->vertex_interpolation_initialized ||
      tls->vertex_interpolation_coarse_corner != coarse_corner) {
    vertex_interpolation_from_corner(ctx, &tls->vertex_interpolation, coarse_poly, coarse_corner);
  }
  /* Store settings used for the current state of interpolator. */
  tls->vertex_interpolation_initialized = true;
  tls->vertex_interpolation_coarse_poly = coarse_poly;
  tls->vertex_interpolation_coarse_corner = coarse_corner;
}

static void subdiv_mesh_vertex_edge(const SubdivForeachContext *foreach_context,
                                    void *tls_v,
                                    const int ptex_face_index,
                                    const float u,
                                    const float v,
                                    const int UNUSED(coarse_edge_index),
                                    const int coarse_poly_index,
                                    const int coarse_corner,
                                    const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MPoly *coarse_mpoly = coarse_mesh->mpoly;
  const MPoly *coarse_poly = &coarse_mpoly[coarse_poly_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  MVert *subdiv_vert = &subdiv_mvert[subdiv_vertex_index];
  subdiv_mesh_ensure_vertex_interpolation(ctx, tls, coarse_poly, coarse_corner);
  evaluate_vertex_and_apply_displacement_interpolate(
      ctx, ptex_face_index, u, v, &tls->vertex_interpolation, subdiv_vert);
}

static bool subdiv_mesh_is_center_vertex(const MPoly *coarse_poly, const float u, const float v)
{
  if (coarse_poly->totloop == 4) {
    if (u == 0.5f && v == 0.5f) {
      return true;
    }
  }
  else {
    if (u == 1.0f && v == 1.0f) {
      return true;
    }
  }
  return false;
}

static void subdiv_mesh_tag_center_vertex(const MPoly *coarse_poly,
                                          const int subdiv_vertex_index,
                                          const float u,
                                          const float v,
                                          Mesh *subdiv_mesh)
{
  if (subdiv_mesh_is_center_vertex(coarse_poly, u, v)) {
    BLI_BITMAP_ENABLE(subdiv_mesh->runtime.subsurf_face_dot_tags, subdiv_vertex_index);
  }
}

static void subdiv_mesh_vertex_inner(const SubdivForeachContext *foreach_context,
                                     void *tls_v,
                                     const int ptex_face_index,
                                     const float u,
                                     const float v,
                                     const int coarse_poly_index,
                                     const int coarse_corner,
                                     const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  Subdiv *subdiv = ctx->subdiv;
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MPoly *coarse_mpoly = coarse_mesh->mpoly;
  const MPoly *coarse_poly = &coarse_mpoly[coarse_poly_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  MVert *subdiv_vert = &subdiv_mvert[subdiv_vertex_index];
  subdiv_mesh_ensure_vertex_interpolation(ctx, tls, coarse_poly, coarse_corner);
  subdiv_vertex_data_interpolate(ctx, subdiv_vert, &tls->vertex_interpolation, u, v);
  BKE_subdiv_eval_final_point(subdiv, ptex_face_index, u, v, subdiv_vert->co);
  subdiv_mesh_tag_center_vertex(coarse_poly, subdiv_vertex_index, u, v, subdiv_mesh);
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge subdivision process
 * \{ */

static void subdiv_copy_edge_data(SubdivMeshContext *ctx,
                                  MEdge *subdiv_edge,
                                  const MEdge *coarse_edge)
{
  const int subdiv_edge_index = subdiv_edge - ctx->subdiv_mesh->medge;
  if (coarse_edge == nullptr) {
    subdiv_edge->crease = 0;
    subdiv_edge->bweight = 0;
    subdiv_edge->flag = 0;
    if (!ctx->settings->use_optimal_display) {
      subdiv_edge->flag |= ME_EDGERENDER;
    }
    if (ctx->edge_origindex != nullptr) {
      ctx->edge_origindex[subdiv_edge_index] = ORIGINDEX_NONE;
    }
    return;
  }
  const int coarse_edge_index = coarse_edge - ctx->coarse_mesh->medge;
  CustomData_copy_data(
      &ctx->coarse_mesh->edata, &ctx->subdiv_mesh->edata, coarse_edge_index, subdiv_edge_index, 1);
  subdiv_edge->flag |= ME_EDGERENDER;
}

static void subdiv_mesh_edge(const SubdivForeachContext *foreach_context,
                             void *UNUSED(tls),
                             const int coarse_edge_index,
                             const int subdiv_edge_index,
                             const bool UNUSED(is_loose),
                             const int subdiv_v1,
                             const int subdiv_v2)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MEdge *subdiv_medge = subdiv_mesh->medge;
  MEdge *subdiv_edge = &subdiv_medge[subdiv_edge_index];
  const MEdge *coarse_edge = nullptr;
  if (coarse_edge_index != ORIGINDEX_NONE) {
    const Mesh *coarse_mesh = ctx->coarse_mesh;
    const MEdge *coarse_medge = coarse_mesh->medge;
    coarse_edge = &coarse_medge[coarse_edge_index];
  }
  subdiv_copy_edge_data(ctx, subdiv_edge, coarse_edge);
  subdiv_edge->v1 = subdiv_v1;
  subdiv_edge->v2 = subdiv_v2;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loops creation/interpolation
 * \{ */

static void subdiv_interpolate_loop_data(const SubdivMeshContext *ctx,
                                         MLoop *subdiv_loop,
                                         const LoopsForInterpolation *loop_interpolation,
                                         const float u,
                                         const float v)
{
  const int subdiv_loop_index = subdiv_loop - ctx->subdiv_mesh->mloop;
  const float weights[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
  CustomData_interp(loop_interpolation->loop_data,
                    &ctx->subdiv_mesh->ldata,
                    loop_interpolation->loop_indices,
                    weights,
                    nullptr,
                    4,
                    subdiv_loop_index);
  /* TODO(sergey): Set ORIGINDEX. */
}

static void subdiv_eval_uv_layer(SubdivMeshContext *ctx,
                                 MLoop *subdiv_loop,
                                 const int ptex_face_index,
                                 const float u,
                                 const float v)
{
  if (ctx->num_uv_layers == 0) {
    return;
  }
  Subdiv *subdiv = ctx->subdiv;
  const int mloop_index = subdiv_loop - ctx->subdiv_mesh->mloop;
  for (int layer_index = 0; layer_index < ctx->num_uv_layers; layer_index++) {
    MLoopUV *subdiv_loopuv = &ctx->uv_layers[layer_index][mloop_index];
    BKE_subdiv_eval_face_varying(subdiv, layer_index, ptex_face_index, u, v, subdiv_loopuv->uv);
  }
}

static void subdiv_mesh_ensure_loop_interpolation(SubdivMeshContext *ctx,
                                                  SubdivMeshTLS *tls,
                                                  const MPoly *coarse_poly,
                                                  const int coarse_corner)
{
  /* Check whether we've moved to another corner or polygon. */
  if (tls->loop_interpolation_initialized) {
    if (tls->loop_interpolation_coarse_poly != coarse_poly ||
        tls->loop_interpolation_coarse_corner != coarse_corner) {
      loop_interpolation_end(&tls->loop_interpolation);
      tls->loop_interpolation_initialized = false;
    }
  }
  /* Initialize the interpolation. */
  if (!tls->loop_interpolation_initialized) {
    loop_interpolation_init(ctx, &tls->loop_interpolation, coarse_poly);
  }
  /* Update it for a new corner if needed. */
  if (!tls->loop_interpolation_initialized ||
      tls->loop_interpolation_coarse_corner != coarse_corner) {
    loop_interpolation_from_corner(ctx, &tls->loop_interpolation, coarse_poly, coarse_corner);
  }
  /* Store settings used for the current state of interpolator. */
  tls->loop_interpolation_initialized = true;
  tls->loop_interpolation_coarse_poly = coarse_poly;
  tls->loop_interpolation_coarse_corner = coarse_corner;
}

static void subdiv_mesh_loop(const SubdivForeachContext *foreach_context,
                             void *tls_v,
                             const int ptex_face_index,
                             const float u,
                             const float v,
                             const int UNUSED(coarse_loop_index),
                             const int coarse_poly_index,
                             const int coarse_corner,
                             const int subdiv_loop_index,
                             const int subdiv_vertex_index,
                             const int subdiv_edge_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MPoly *coarse_mpoly = coarse_mesh->mpoly;
  const MPoly *coarse_poly = &coarse_mpoly[coarse_poly_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MLoop *subdiv_mloop = subdiv_mesh->mloop;
  MLoop *subdiv_loop = &subdiv_mloop[subdiv_loop_index];
  subdiv_mesh_ensure_loop_interpolation(ctx, tls, coarse_poly, coarse_corner);
  subdiv_interpolate_loop_data(ctx, subdiv_loop, &tls->loop_interpolation, u, v);
  subdiv_eval_uv_layer(ctx, subdiv_loop, ptex_face_index, u, v);
  subdiv_loop->v = subdiv_vertex_index;
  subdiv_loop->e = subdiv_edge_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polygons subdivision process
 * \{ */

static void subdiv_copy_poly_data(const SubdivMeshContext *ctx,
                                  MPoly *subdiv_poly,
                                  const MPoly *coarse_poly)
{
  const int coarse_poly_index = coarse_poly - ctx->coarse_mesh->mpoly;
  const int subdiv_poly_index = subdiv_poly - ctx->subdiv_mesh->mpoly;
  CustomData_copy_data(
      &ctx->coarse_mesh->pdata, &ctx->subdiv_mesh->pdata, coarse_poly_index, subdiv_poly_index, 1);
}

static void subdiv_mesh_poly(const SubdivForeachContext *foreach_context,
                             void *UNUSED(tls),
                             const int coarse_poly_index,
                             const int subdiv_poly_index,
                             const int start_loop_index,
                             const int num_loops)
{
  BLI_assert(coarse_poly_index != ORIGINDEX_NONE);
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MPoly *coarse_mpoly = coarse_mesh->mpoly;
  const MPoly *coarse_poly = &coarse_mpoly[coarse_poly_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MPoly *subdiv_mpoly = subdiv_mesh->mpoly;
  MPoly *subdiv_poly = &subdiv_mpoly[subdiv_poly_index];
  subdiv_copy_poly_data(ctx, subdiv_poly, coarse_poly);
  subdiv_poly->loopstart = start_loop_index;
  subdiv_poly->totloop = num_loops;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loose elements subdivision process
 * \{ */

static void subdiv_mesh_vertex_loose(const SubdivForeachContext *foreach_context,
                                     void *UNUSED(tls),
                                     const int coarse_vertex_index,
                                     const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MVert *coarse_mvert = coarse_mesh->mvert;
  const MVert *coarse_vertex = &coarse_mvert[coarse_vertex_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  MVert *subdiv_vertex = &subdiv_mvert[subdiv_vertex_index];
  subdiv_vertex_data_copy(ctx, coarse_vertex, subdiv_vertex);
}

/* Get neighbor edges of the given one.
 * - neighbors[0] is an edge adjacent to edge->v1.
 * - neighbors[1] is an edge adjacent to edge->v2. */
static void find_edge_neighbors(const Mesh *coarse_mesh,
                                const MEdge *edge,
                                const MEdge *neighbors[2])
{
  const MEdge *coarse_medge = coarse_mesh->medge;
  neighbors[0] = nullptr;
  neighbors[1] = nullptr;
  int neighbor_counters[2] = {0, 0};
  for (int edge_index = 0; edge_index < coarse_mesh->totedge; edge_index++) {
    const MEdge *current_edge = &coarse_medge[edge_index];
    if (current_edge == edge) {
      continue;
    }
    if (ELEM(edge->v1, current_edge->v1, current_edge->v2)) {
      neighbors[0] = current_edge;
      ++neighbor_counters[0];
    }
    if (ELEM(edge->v2, current_edge->v1, current_edge->v2)) {
      neighbors[1] = current_edge;
      ++neighbor_counters[1];
    }
  }
  /* Vertices which has more than one neighbor are considered infinitely
   * sharp. This is also how topology factory treats vertices of a surface
   * which are adjacent to a loose edge. */
  if (neighbor_counters[0] > 1) {
    neighbors[0] = nullptr;
  }
  if (neighbor_counters[1] > 1) {
    neighbors[1] = nullptr;
  }
}

static void points_for_loose_edges_interpolation_get(const Mesh *coarse_mesh,
                                                     const MEdge *coarse_edge,
                                                     const MEdge *neighbors[2],
                                                     float points_r[4][3])
{
  const MVert *coarse_mvert = coarse_mesh->mvert;
  /* Middle points corresponds to the edge. */
  copy_v3_v3(points_r[1], coarse_mvert[coarse_edge->v1].co);
  copy_v3_v3(points_r[2], coarse_mvert[coarse_edge->v2].co);
  /* Start point, duplicate from edge start if no neighbor. */
  if (neighbors[0] != nullptr) {
    if (neighbors[0]->v1 == coarse_edge->v1) {
      copy_v3_v3(points_r[0], coarse_mvert[neighbors[0]->v2].co);
    }
    else {
      copy_v3_v3(points_r[0], coarse_mvert[neighbors[0]->v1].co);
    }
  }
  else {
    sub_v3_v3v3(points_r[0], points_r[1], points_r[2]);
    add_v3_v3(points_r[0], points_r[1]);
  }
  /* End point, duplicate from edge end if no neighbor. */
  if (neighbors[1] != nullptr) {
    if (neighbors[1]->v1 == coarse_edge->v2) {
      copy_v3_v3(points_r[3], coarse_mvert[neighbors[1]->v2].co);
    }
    else {
      copy_v3_v3(points_r[3], coarse_mvert[neighbors[1]->v1].co);
    }
  }
  else {
    sub_v3_v3v3(points_r[3], points_r[2], points_r[1]);
    add_v3_v3(points_r[3], points_r[2]);
  }
}

void BKE_subdiv_mesh_interpolate_position_on_edge(const Mesh *coarse_mesh,
                                                  const MEdge *coarse_edge,
                                                  const bool is_simple,
                                                  const float u,
                                                  float pos_r[3])
{
  if (is_simple) {
    const MVert *coarse_mvert = coarse_mesh->mvert;
    const MVert *vert_1 = &coarse_mvert[coarse_edge->v1];
    const MVert *vert_2 = &coarse_mvert[coarse_edge->v2];
    interp_v3_v3v3(pos_r, vert_1->co, vert_2->co, u);
  }
  else {
    /* Find neighbors of the coarse edge. */
    const MEdge *neighbors[2];
    find_edge_neighbors(coarse_mesh, coarse_edge, neighbors);
    float points[4][3];
    points_for_loose_edges_interpolation_get(coarse_mesh, coarse_edge, neighbors, points);
    float weights[4];
    key_curve_position_weights(u, weights, KEY_BSPLINE);
    interp_v3_v3v3v3v3(pos_r, points[0], points[1], points[2], points[3], weights);
  }
}

static void subdiv_mesh_vertex_of_loose_edge_interpolate(SubdivMeshContext *ctx,
                                                         const MEdge *coarse_edge,
                                                         const float u,
                                                         const int subdiv_vertex_index)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  /* This is never used for end-points (which are copied from the original). */
  BLI_assert(u > 0.0f);
  BLI_assert(u < 1.0f);
  const float interpolation_weights[2] = {1.0f - u, u};
  const int coarse_vertex_indices[2] = {static_cast<int>(coarse_edge->v1),
                                        static_cast<int>(coarse_edge->v2)};
  CustomData_interp(&coarse_mesh->vdata,
                    &subdiv_mesh->vdata,
                    coarse_vertex_indices,
                    interpolation_weights,
                    nullptr,
                    2,
                    subdiv_vertex_index);
  if (ctx->vert_origindex != nullptr) {
    ctx->vert_origindex[subdiv_vertex_index] = ORIGINDEX_NONE;
  }
}

static void subdiv_mesh_vertex_of_loose_edge(const SubdivForeachContext *foreach_context,
                                             void *UNUSED(tls),
                                             const int coarse_edge_index,
                                             const float u,
                                             const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MEdge *coarse_edge = &coarse_mesh->medge[coarse_edge_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  MVert *subdiv_mvert = subdiv_mesh->mvert;
  const bool is_simple = ctx->subdiv->settings.is_simple;
  /* Interpolate custom data when not an end point.
   * This data has already been copied from the original vertex by #subdiv_mesh_vertex_loose. */
  if (!ELEM(u, 0.0, 1.0)) {
    subdiv_mesh_vertex_of_loose_edge_interpolate(ctx, coarse_edge, u, subdiv_vertex_index);
  }
  /* Interpolate coordinate. */
  MVert *subdiv_vertex = &subdiv_mvert[subdiv_vertex_index];
  BKE_subdiv_mesh_interpolate_position_on_edge(
      coarse_mesh, coarse_edge, is_simple, u, subdiv_vertex->co);
  /* Reset flags and such. */
  subdiv_vertex->flag = 0;
  /* TODO(sergey): This matches old behavior, but we can as well interpolate
   * it. Maybe even using vertex varying attributes. */
  subdiv_vertex->bweight = 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Initialization
 * \{ */

static void setup_foreach_callbacks(const SubdivMeshContext *subdiv_context,
                                    SubdivForeachContext *foreach_context)
{
  memset(foreach_context, 0, sizeof(*foreach_context));
  /* General information. */
  foreach_context->topology_info = subdiv_mesh_topology_info;
  /* Every boundary geometry. Used for displacement averaging. */
  if (subdiv_context->have_displacement) {
    foreach_context->vertex_every_corner = subdiv_mesh_vertex_displacement_every_corner;
    foreach_context->vertex_every_edge = subdiv_mesh_vertex_displacement_every_edge;
  }
  foreach_context->vertex_corner = subdiv_mesh_vertex_corner;
  foreach_context->vertex_edge = subdiv_mesh_vertex_edge;
  foreach_context->vertex_inner = subdiv_mesh_vertex_inner;
  foreach_context->edge = subdiv_mesh_edge;
  foreach_context->loop = subdiv_mesh_loop;
  foreach_context->poly = subdiv_mesh_poly;
  foreach_context->vertex_loose = subdiv_mesh_vertex_loose;
  foreach_context->vertex_of_loose_edge = subdiv_mesh_vertex_of_loose_edge;
  foreach_context->user_data_tls_free = subdiv_mesh_tls_free;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public entry point
 * \{ */

Mesh *BKE_subdiv_to_mesh(Subdiv *subdiv,
                         const SubdivToMeshSettings *settings,
                         const Mesh *coarse_mesh)
{
  BKE_subdiv_stats_begin(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
  /* Make sure evaluator is up to date with possible new topology, and that
   * it is refined for the new positions of coarse vertices. */
  if (!BKE_subdiv_eval_begin_from_mesh(
          subdiv, coarse_mesh, nullptr, SUBDIV_EVALUATOR_TYPE_CPU, nullptr)) {
    /* This could happen in two situations:
     * - OpenSubdiv is disabled.
     * - Something totally bad happened, and OpenSubdiv rejected our
     *   topology.
     * In either way, we can't safely continue. */
    if (coarse_mesh->totpoly) {
      BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
      return nullptr;
    }
  }
  /* Initialize subdivision mesh creation context. */
  SubdivMeshContext subdiv_context = {0};
  subdiv_context.settings = settings;
  subdiv_context.coarse_mesh = coarse_mesh;
  subdiv_context.subdiv = subdiv;
  subdiv_context.have_displacement = (subdiv->displacement_evaluator != nullptr);
  /* Multi-threaded traversal/evaluation. */
  BKE_subdiv_stats_begin(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH_GEOMETRY);
  SubdivForeachContext foreach_context;
  setup_foreach_callbacks(&subdiv_context, &foreach_context);
  SubdivMeshTLS tls = {0};
  foreach_context.user_data = &subdiv_context;
  foreach_context.user_data_tls_size = sizeof(SubdivMeshTLS);
  foreach_context.user_data_tls = &tls;
  BKE_subdiv_foreach_subdiv_geometry(subdiv, &foreach_context, settings, coarse_mesh);
  BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH_GEOMETRY);
  Mesh *result = subdiv_context.subdiv_mesh;
  // BKE_mesh_validate(result, true, true);
  BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
  /* Using normals from the limit surface gives different results than Blender's vertex normal
   * calculation. Since vertex normals are supposed to be a consistent cache, don't bother
   * calculating them here. The work may have been pointless anyway if the mesh is deformed or
   * changed afterwards. */
  BLI_assert(BKE_mesh_vertex_normals_are_dirty(result) || BKE_mesh_poly_normals_are_dirty(result));
  /* Free used memory. */
  subdiv_mesh_context_free(&subdiv_context);
  return result;
}

/** \} */
