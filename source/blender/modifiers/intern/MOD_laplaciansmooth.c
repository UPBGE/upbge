/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "eigen_capi.h"

struct BLaplacianSystem {
  float *eweights;      /* Length weights per Edge */
  float (*fweights)[3]; /* Cotangent weights per face */
  float *ring_areas;    /* Total area per ring. */
  float *vlengths;      /* Total sum of lengths(edges) per vertex. */
  float *vweights;      /* Total sum of weights per vertex. */
  int edges_num;        /* Number of edges. */
  int loops_num;        /* Number of edges. */
  int polys_num;        /* Number of faces. */
  int verts_num;        /* Number of verts. */
  short *ne_fa_num;     /* Number of neighbors faces around vertex. */
  short *ne_ed_num;     /* Number of neighbors Edges around vertex. */
  bool *zerola;         /* Is zero area or length. */

  /* Pointers to data. */
  float (*vertexCos)[3];
  const MPoly *mpoly;
  const MLoop *mloop;
  const MEdge *medges;
  LinearSolver *context;

  /* Data. */
  float min_area;
  float vert_centroid[3];
};
typedef struct BLaplacianSystem LaplacianSystem;

static void required_data_mask(Object *ob, ModifierData *md, CustomData_MeshMasks *r_cddata_masks);
static bool is_disabled(const struct Scene *scene, ModifierData *md, bool useRenderParams);
static float compute_volume(const float center[3],
                            float (*vertexCos)[3],
                            const MPoly *mpoly,
                            int polys_num,
                            const MLoop *mloop);
static LaplacianSystem *init_laplacian_system(int a_numEdges,
                                              int a_numPolys,
                                              int a_numLoops,
                                              int a_numVerts);
static void delete_laplacian_system(LaplacianSystem *sys);
static void fill_laplacian_matrix(LaplacianSystem *sys);
static void init_data(ModifierData *md);
static void init_laplacian_matrix(LaplacianSystem *sys);
static void memset_laplacian_system(LaplacianSystem *sys, int val);
static void volume_preservation(LaplacianSystem *sys, float vini, float vend, short flag);
static void validate_solution(LaplacianSystem *sys, short flag, float lambda, float lambda_border);

static void delete_laplacian_system(LaplacianSystem *sys)
{
  MEM_SAFE_FREE(sys->eweights);
  MEM_SAFE_FREE(sys->fweights);
  MEM_SAFE_FREE(sys->ne_ed_num);
  MEM_SAFE_FREE(sys->ne_fa_num);
  MEM_SAFE_FREE(sys->ring_areas);
  MEM_SAFE_FREE(sys->vlengths);
  MEM_SAFE_FREE(sys->vweights);
  MEM_SAFE_FREE(sys->zerola);

  if (sys->context) {
    EIG_linear_solver_delete(sys->context);
  }
  sys->vertexCos = NULL;
  sys->mpoly = NULL;
  sys->mloop = NULL;
  sys->medges = NULL;
  MEM_freeN(sys);
}

static void memset_laplacian_system(LaplacianSystem *sys, int val)
{
  memset(sys->eweights, val, sizeof(float) * sys->edges_num);
  memset(sys->fweights, val, sizeof(float[3]) * sys->loops_num);
  memset(sys->ne_ed_num, val, sizeof(short) * sys->verts_num);
  memset(sys->ne_fa_num, val, sizeof(short) * sys->verts_num);
  memset(sys->ring_areas, val, sizeof(float) * sys->verts_num);
  memset(sys->vlengths, val, sizeof(float) * sys->verts_num);
  memset(sys->vweights, val, sizeof(float) * sys->verts_num);
  memset(sys->zerola, val, sizeof(bool) * sys->verts_num);
}

static LaplacianSystem *init_laplacian_system(int a_numEdges,
                                              int a_numPolys,
                                              int a_numLoops,
                                              int a_numVerts)
{
  LaplacianSystem *sys;
  sys = MEM_callocN(sizeof(LaplacianSystem), "ModLaplSmoothSystem");
  sys->edges_num = a_numEdges;
  sys->polys_num = a_numPolys;
  sys->loops_num = a_numLoops;
  sys->verts_num = a_numVerts;

  sys->eweights = MEM_calloc_arrayN(sys->edges_num, sizeof(float), __func__);
  sys->fweights = MEM_calloc_arrayN(sys->loops_num, sizeof(float[3]), __func__);
  sys->ne_ed_num = MEM_calloc_arrayN(sys->verts_num, sizeof(short), __func__);
  sys->ne_fa_num = MEM_calloc_arrayN(sys->verts_num, sizeof(short), __func__);
  sys->ring_areas = MEM_calloc_arrayN(sys->verts_num, sizeof(float), __func__);
  sys->vlengths = MEM_calloc_arrayN(sys->verts_num, sizeof(float), __func__);
  sys->vweights = MEM_calloc_arrayN(sys->verts_num, sizeof(float), __func__);
  sys->zerola = MEM_calloc_arrayN(sys->verts_num, sizeof(bool), __func__);

  return sys;
}

static float compute_volume(const float center[3],
                            float (*vertexCos)[3],
                            const MPoly *mpoly,
                            int polys_num,
                            const MLoop *mloop)
{
  int i;
  float vol = 0.0f;

  for (i = 0; i < polys_num; i++) {
    const MPoly *mp = &mpoly[i];
    const MLoop *l_first = &mloop[mp->loopstart];
    const MLoop *l_prev = l_first + 1;
    const MLoop *l_curr = l_first + 2;
    const MLoop *l_term = l_first + mp->totloop;

    for (; l_curr != l_term; l_prev = l_curr, l_curr++) {
      vol += volume_tetrahedron_signed_v3(
          center, vertexCos[l_first->v], vertexCos[l_prev->v], vertexCos[l_curr->v]);
    }
  }

  return fabsf(vol);
}

static void volume_preservation(LaplacianSystem *sys, float vini, float vend, short flag)
{
  float beta;
  int i;

  if (vend != 0.0f) {
    beta = pow(vini / vend, 1.0f / 3.0f);
    for (i = 0; i < sys->verts_num; i++) {
      if (flag & MOD_LAPLACIANSMOOTH_X) {
        sys->vertexCos[i][0] = (sys->vertexCos[i][0] - sys->vert_centroid[0]) * beta +
                               sys->vert_centroid[0];
      }
      if (flag & MOD_LAPLACIANSMOOTH_Y) {
        sys->vertexCos[i][1] = (sys->vertexCos[i][1] - sys->vert_centroid[1]) * beta +
                               sys->vert_centroid[1];
      }
      if (flag & MOD_LAPLACIANSMOOTH_Z) {
        sys->vertexCos[i][2] = (sys->vertexCos[i][2] - sys->vert_centroid[2]) * beta +
                               sys->vert_centroid[2];
      }
    }
  }
}

static void init_laplacian_matrix(LaplacianSystem *sys)
{
  float *v1, *v2;
  float w1, w2, w3;
  float areaf;
  int i;
  uint idv1, idv2;

  for (i = 0; i < sys->edges_num; i++) {
    idv1 = sys->medges[i].v1;
    idv2 = sys->medges[i].v2;

    v1 = sys->vertexCos[idv1];
    v2 = sys->vertexCos[idv2];

    sys->ne_ed_num[idv1] = sys->ne_ed_num[idv1] + 1;
    sys->ne_ed_num[idv2] = sys->ne_ed_num[idv2] + 1;
    w1 = len_v3v3(v1, v2);
    if (w1 < sys->min_area) {
      sys->zerola[idv1] = true;
      sys->zerola[idv2] = true;
    }
    else {
      w1 = 1.0f / w1;
    }

    sys->eweights[i] = w1;
  }

  for (i = 0; i < sys->polys_num; i++) {
    const MPoly *mp = &sys->mpoly[i];
    const MLoop *l_next = &sys->mloop[mp->loopstart];
    const MLoop *l_term = l_next + mp->totloop;
    const MLoop *l_prev = l_term - 2;
    const MLoop *l_curr = l_term - 1;

    for (; l_next != l_term; l_prev = l_curr, l_curr = l_next, l_next++) {
      const float *v_prev = sys->vertexCos[l_prev->v];
      const float *v_curr = sys->vertexCos[l_curr->v];
      const float *v_next = sys->vertexCos[l_next->v];
      const uint l_curr_index = l_curr - sys->mloop;

      sys->ne_fa_num[l_curr->v] += 1;

      areaf = area_tri_v3(v_prev, v_curr, v_next);

      if (areaf < sys->min_area) {
        sys->zerola[l_curr->v] = true;
      }

      sys->ring_areas[l_prev->v] += areaf;
      sys->ring_areas[l_curr->v] += areaf;
      sys->ring_areas[l_next->v] += areaf;

      w1 = cotangent_tri_weight_v3(v_curr, v_next, v_prev) / 2.0f;
      w2 = cotangent_tri_weight_v3(v_next, v_prev, v_curr) / 2.0f;
      w3 = cotangent_tri_weight_v3(v_prev, v_curr, v_next) / 2.0f;

      sys->fweights[l_curr_index][0] += w1;
      sys->fweights[l_curr_index][1] += w2;
      sys->fweights[l_curr_index][2] += w3;

      sys->vweights[l_curr->v] += w2 + w3;
      sys->vweights[l_next->v] += w1 + w3;
      sys->vweights[l_prev->v] += w1 + w2;
    }
  }
  for (i = 0; i < sys->edges_num; i++) {
    idv1 = sys->medges[i].v1;
    idv2 = sys->medges[i].v2;
    /* if is boundary, apply scale-dependent umbrella operator only with neighbors in boundary */
    if (sys->ne_ed_num[idv1] != sys->ne_fa_num[idv1] &&
        sys->ne_ed_num[idv2] != sys->ne_fa_num[idv2]) {
      sys->vlengths[idv1] += sys->eweights[i];
      sys->vlengths[idv2] += sys->eweights[i];
    }
  }
}

static void fill_laplacian_matrix(LaplacianSystem *sys)
{
  int i;
  uint idv1, idv2;

  for (i = 0; i < sys->polys_num; i++) {
    const MPoly *mp = &sys->mpoly[i];
    const MLoop *l_next = &sys->mloop[mp->loopstart];
    const MLoop *l_term = l_next + mp->totloop;
    const MLoop *l_prev = l_term - 2;
    const MLoop *l_curr = l_term - 1;

    for (; l_next != l_term; l_prev = l_curr, l_curr = l_next, l_next++) {
      const uint l_curr_index = l_curr - sys->mloop;

      /* Is ring if number of faces == number of edges around vertex. */
      if (sys->ne_ed_num[l_curr->v] == sys->ne_fa_num[l_curr->v] &&
          sys->zerola[l_curr->v] == false) {
        EIG_linear_solver_matrix_add(sys->context,
                                     l_curr->v,
                                     l_next->v,
                                     sys->fweights[l_curr_index][2] * sys->vweights[l_curr->v]);
        EIG_linear_solver_matrix_add(sys->context,
                                     l_curr->v,
                                     l_prev->v,
                                     sys->fweights[l_curr_index][1] * sys->vweights[l_curr->v]);
      }
      if (sys->ne_ed_num[l_next->v] == sys->ne_fa_num[l_next->v] &&
          sys->zerola[l_next->v] == false) {
        EIG_linear_solver_matrix_add(sys->context,
                                     l_next->v,
                                     l_curr->v,
                                     sys->fweights[l_curr_index][2] * sys->vweights[l_next->v]);
        EIG_linear_solver_matrix_add(sys->context,
                                     l_next->v,
                                     l_prev->v,
                                     sys->fweights[l_curr_index][0] * sys->vweights[l_next->v]);
      }
      if (sys->ne_ed_num[l_prev->v] == sys->ne_fa_num[l_prev->v] &&
          sys->zerola[l_prev->v] == false) {
        EIG_linear_solver_matrix_add(sys->context,
                                     l_prev->v,
                                     l_curr->v,
                                     sys->fweights[l_curr_index][1] * sys->vweights[l_prev->v]);
        EIG_linear_solver_matrix_add(sys->context,
                                     l_prev->v,
                                     l_next->v,
                                     sys->fweights[l_curr_index][0] * sys->vweights[l_prev->v]);
      }
    }
  }

  for (i = 0; i < sys->edges_num; i++) {
    idv1 = sys->medges[i].v1;
    idv2 = sys->medges[i].v2;
    /* Is boundary */
    if (sys->ne_ed_num[idv1] != sys->ne_fa_num[idv1] &&
        sys->ne_ed_num[idv2] != sys->ne_fa_num[idv2] && sys->zerola[idv1] == false &&
        sys->zerola[idv2] == false) {
      EIG_linear_solver_matrix_add(
          sys->context, idv1, idv2, sys->eweights[i] * sys->vlengths[idv1]);
      EIG_linear_solver_matrix_add(
          sys->context, idv2, idv1, sys->eweights[i] * sys->vlengths[idv2]);
    }
  }
}

static void validate_solution(LaplacianSystem *sys, short flag, float lambda, float lambda_border)
{
  int i;
  float lam;
  float vini = 0.0f, vend = 0.0f;

  if (flag & MOD_LAPLACIANSMOOTH_PRESERVE_VOLUME) {
    vini = compute_volume(
        sys->vert_centroid, sys->vertexCos, sys->mpoly, sys->polys_num, sys->mloop);
  }
  for (i = 0; i < sys->verts_num; i++) {
    if (sys->zerola[i] == false) {
      lam = sys->ne_ed_num[i] == sys->ne_fa_num[i] ? (lambda >= 0.0f ? 1.0f : -1.0f) :
                                                     (lambda_border >= 0.0f ? 1.0f : -1.0f);
      if (flag & MOD_LAPLACIANSMOOTH_X) {
        sys->vertexCos[i][0] += lam * ((float)EIG_linear_solver_variable_get(sys->context, 0, i) -
                                       sys->vertexCos[i][0]);
      }
      if (flag & MOD_LAPLACIANSMOOTH_Y) {
        sys->vertexCos[i][1] += lam * ((float)EIG_linear_solver_variable_get(sys->context, 1, i) -
                                       sys->vertexCos[i][1]);
      }
      if (flag & MOD_LAPLACIANSMOOTH_Z) {
        sys->vertexCos[i][2] += lam * ((float)EIG_linear_solver_variable_get(sys->context, 2, i) -
                                       sys->vertexCos[i][2]);
      }
    }
  }
  if (flag & MOD_LAPLACIANSMOOTH_PRESERVE_VOLUME) {
    vend = compute_volume(
        sys->vert_centroid, sys->vertexCos, sys->mpoly, sys->polys_num, sys->mloop);
    volume_preservation(sys, vini, vend, flag);
  }
}

static void laplaciansmoothModifier_do(
    LaplacianSmoothModifierData *smd, Object *ob, Mesh *mesh, float (*vertexCos)[3], int verts_num)
{
  LaplacianSystem *sys;
  MDeformVert *dvert = NULL;
  MDeformVert *dv = NULL;
  float w, wpaint;
  int i, iter;
  int defgrp_index;
  const bool invert_vgroup = (smd->flag & MOD_LAPLACIANSMOOTH_INVERT_VGROUP) != 0;

  sys = init_laplacian_system(mesh->totedge, mesh->totpoly, mesh->totloop, verts_num);
  if (!sys) {
    return;
  }

  sys->mpoly = mesh->mpoly;
  sys->mloop = mesh->mloop;
  sys->medges = mesh->medge;
  sys->vertexCos = vertexCos;
  sys->min_area = 0.00001f;
  MOD_get_vgroup(ob, mesh, smd->defgrp_name, &dvert, &defgrp_index);

  sys->vert_centroid[0] = 0.0f;
  sys->vert_centroid[1] = 0.0f;
  sys->vert_centroid[2] = 0.0f;
  memset_laplacian_system(sys, 0);

  sys->context = EIG_linear_least_squares_solver_new(verts_num, verts_num, 3);

  init_laplacian_matrix(sys);

  for (iter = 0; iter < smd->repeat; iter++) {
    for (i = 0; i < verts_num; i++) {
      EIG_linear_solver_variable_set(sys->context, 0, i, vertexCos[i][0]);
      EIG_linear_solver_variable_set(sys->context, 1, i, vertexCos[i][1]);
      EIG_linear_solver_variable_set(sys->context, 2, i, vertexCos[i][2]);
      if (iter == 0) {
        add_v3_v3(sys->vert_centroid, vertexCos[i]);
      }
    }
    if (iter == 0 && verts_num > 0) {
      mul_v3_fl(sys->vert_centroid, 1.0f / (float)verts_num);
    }

    dv = dvert;
    for (i = 0; i < verts_num; i++) {
      EIG_linear_solver_right_hand_side_add(sys->context, 0, i, vertexCos[i][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, i, vertexCos[i][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, i, vertexCos[i][2]);
      if (iter == 0) {
        if (dv) {
          wpaint = invert_vgroup ? 1.0f - BKE_defvert_find_weight(dv, defgrp_index) :
                                   BKE_defvert_find_weight(dv, defgrp_index);
          dv++;
        }
        else {
          wpaint = 1.0f;
        }

        if (sys->zerola[i] == false) {
          if (smd->flag & MOD_LAPLACIANSMOOTH_NORMALIZED) {
            w = sys->vweights[i];
            sys->vweights[i] = (w == 0.0f) ? 0.0f : -fabsf(smd->lambda) * wpaint / w;
            w = sys->vlengths[i];
            sys->vlengths[i] = (w == 0.0f) ? 0.0f : -fabsf(smd->lambda_border) * wpaint * 2.0f / w;
            if (sys->ne_ed_num[i] == sys->ne_fa_num[i]) {
              EIG_linear_solver_matrix_add(sys->context, i, i, 1.0f + fabsf(smd->lambda) * wpaint);
            }
            else {
              EIG_linear_solver_matrix_add(
                  sys->context, i, i, 1.0f + fabsf(smd->lambda_border) * wpaint * 2.0f);
            }
          }
          else {
            w = sys->vweights[i] * sys->ring_areas[i];
            sys->vweights[i] = (w == 0.0f) ? 0.0f : -fabsf(smd->lambda) * wpaint / (4.0f * w);
            w = sys->vlengths[i];
            sys->vlengths[i] = (w == 0.0f) ? 0.0f : -fabsf(smd->lambda_border) * wpaint * 2.0f / w;

            if (sys->ne_ed_num[i] == sys->ne_fa_num[i]) {
              EIG_linear_solver_matrix_add(sys->context,
                                           i,
                                           i,
                                           1.0f + fabsf(smd->lambda) * wpaint /
                                                      (4.0f * sys->ring_areas[i]));
            }
            else {
              EIG_linear_solver_matrix_add(
                  sys->context, i, i, 1.0f + fabsf(smd->lambda_border) * wpaint * 2.0f);
            }
          }
        }
        else {
          EIG_linear_solver_matrix_add(sys->context, i, i, 1.0f);
        }
      }
    }

    if (iter == 0) {
      fill_laplacian_matrix(sys);
    }

    if (EIG_linear_solver_solve(sys->context)) {
      validate_solution(sys, smd->flag, smd->lambda, smd->lambda_border);
    }
  }
  EIG_linear_solver_delete(sys->context);
  sys->context = NULL;

  delete_laplacian_system(sys);
}

static void init_data(ModifierData *md)
{
  LaplacianSmoothModifierData *smd = (LaplacianSmoothModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(smd, modifier));

  MEMCPY_STRUCT_AFTER(smd, DNA_struct_default_get(LaplacianSmoothModifierData), modifier);
}

static bool is_disabled(const struct Scene *UNUSED(scene),
                        ModifierData *md,
                        bool UNUSED(useRenderParams))
{
  LaplacianSmoothModifierData *smd = (LaplacianSmoothModifierData *)md;
  short flag;

  flag = smd->flag & (MOD_LAPLACIANSMOOTH_X | MOD_LAPLACIANSMOOTH_Y | MOD_LAPLACIANSMOOTH_Z);

  /* disable if modifier is off for X, Y and Z or if factor is 0 */
  if (flag == 0) {
    return 1;
  }

  return 0;
}

static void required_data_mask(Object *UNUSED(ob),
                               ModifierData *md,
                               CustomData_MeshMasks *r_cddata_masks)
{
  LaplacianSmoothModifierData *smd = (LaplacianSmoothModifierData *)md;

  /* ask for vertexgroups if we need them */
  if (smd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int verts_num)
{
  Mesh *mesh_src;

  if (verts_num == 0) {
    return;
  }

  mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, verts_num, false);

  laplaciansmoothModifier_do(
      (LaplacianSmoothModifierData *)md, ctx->object, mesh_src, vertexCos, verts_num);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void deformVertsEM(ModifierData *md,
                          const ModifierEvalContext *ctx,
                          struct BMEditMesh *editData,
                          Mesh *mesh,
                          float (*vertexCos)[3],
                          int verts_num)
{
  Mesh *mesh_src;

  if (verts_num == 0) {
    return;
  }

  mesh_src = MOD_deform_mesh_eval_get(ctx->object, editData, mesh, NULL, verts_num, false);

  /* TODO(@campbellbarton): use edit-mode data only (remove this line). */
  if (mesh_src != NULL) {
    BKE_mesh_wrapper_ensure_mdata(mesh_src);
  }

  laplaciansmoothModifier_do(
      (LaplacianSmoothModifierData *)md, ctx->object, mesh_src, vertexCos, verts_num);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *row;
  uiLayout *layout = panel->layout;
  int toggles_flag = UI_ITEM_R_TOGGLE | UI_ITEM_R_FORCE_BLANK_DECORATE;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "iterations", 0, NULL, ICON_NONE);

  row = uiLayoutRowWithHeading(layout, true, IFACE_("Axis"));
  uiItemR(row, ptr, "use_x", toggles_flag, NULL, ICON_NONE);
  uiItemR(row, ptr, "use_y", toggles_flag, NULL, ICON_NONE);
  uiItemR(row, ptr, "use_z", toggles_flag, NULL, ICON_NONE);

  uiItemR(layout, ptr, "lambda_factor", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "lambda_border", 0, NULL, ICON_NONE);

  uiItemR(layout, ptr, "use_volume_preserve", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "use_normalized", 0, NULL, ICON_NONE);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", NULL);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_LaplacianSmooth, panel_draw);
}

ModifierTypeInfo modifierType_LaplacianSmooth = {
    /* name */ N_("LaplacianSmooth"),
    /* structName */ "LaplacianSmoothModifierData",
    /* structSize */ sizeof(LaplacianSmoothModifierData),
    /* srna */ &RNA_LaplacianSmoothModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /* icon */ ICON_MOD_SMOOTH,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ NULL,
    /* modifyGeometrySet */ NULL,

    /* initData */ init_data,
    /* requiredDataMask */ required_data_mask,
    /* freeData */ NULL,
    /* isDisabled */ is_disabled,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ NULL,
};
