/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines_stack.h"

#include "MEM_guardedalloc.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_particle.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "BLO_read_write.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "eigen_capi.h"

enum {
  LAPDEFORM_SYSTEM_NOT_CHANGE = 0,
  LAPDEFORM_SYSTEM_IS_DIFFERENT,
  LAPDEFORM_SYSTEM_ONLY_CHANGE_ANCHORS,
  LAPDEFORM_SYSTEM_ONLY_CHANGE_GROUP,
  LAPDEFORM_SYSTEM_ONLY_CHANGE_MESH,
  LAPDEFORM_SYSTEM_CHANGE_VERTEXES,
  LAPDEFORM_SYSTEM_CHANGE_EDGES,
  LAPDEFORM_SYSTEM_CHANGE_NOT_VALID_GROUP,
};

typedef struct LaplacianSystem {
  bool is_matrix_computed;
  bool has_solution;
  int verts_num;
  int edges_num;
  int tris_num;
  int anchors_num;
  int repeat;
  char anchor_grp_name[64]; /* Vertex Group name */
  float (*co)[3];           /* Original vertex coordinates */
  float (*no)[3];           /* Original vertex normal */
  float (*delta)[3];        /* Differential Coordinates */
  uint (*tris)[3];          /* Copy of MLoopTri (tessellation triangle) v1-v3 */
  int *index_anchors;       /* Static vertex index list */
  int *unit_verts;          /* Unit vectors of projected edges onto the plane orthogonal to n */
  int *ringf_indices;       /* Indices of faces per vertex */
  int *ringv_indices;       /* Indices of neighbors(vertex) per vertex */
  LinearSolver *context;    /* System for solve general implicit rotations */
  MeshElemMap *ringf_map;   /* Map of faces per vertex */
  MeshElemMap *ringv_map;   /* Map of vertex per vertex */
} LaplacianSystem;

static LaplacianSystem *newLaplacianSystem(void)
{
  LaplacianSystem *sys;
  sys = MEM_callocN(sizeof(LaplacianSystem), "DeformCache");

  sys->is_matrix_computed = false;
  sys->has_solution = false;
  sys->verts_num = 0;
  sys->edges_num = 0;
  sys->anchors_num = 0;
  sys->tris_num = 0;
  sys->repeat = 1;
  sys->anchor_grp_name[0] = '\0';

  return sys;
}

static LaplacianSystem *initLaplacianSystem(int verts_num,
                                            int edges_num,
                                            int tris_num,
                                            int anchors_num,
                                            const char defgrpName[64],
                                            int iterations)
{
  LaplacianSystem *sys = newLaplacianSystem();

  sys->is_matrix_computed = false;
  sys->has_solution = false;
  sys->verts_num = verts_num;
  sys->edges_num = edges_num;
  sys->tris_num = tris_num;
  sys->anchors_num = anchors_num;
  sys->repeat = iterations;
  BLI_strncpy(sys->anchor_grp_name, defgrpName, sizeof(sys->anchor_grp_name));
  sys->co = MEM_malloc_arrayN(verts_num, sizeof(float[3]), "DeformCoordinates");
  sys->no = MEM_calloc_arrayN(verts_num, sizeof(float[3]), "DeformNormals");
  sys->delta = MEM_calloc_arrayN(verts_num, sizeof(float[3]), "DeformDeltas");
  sys->tris = MEM_malloc_arrayN(tris_num, sizeof(int[3]), "DeformFaces");
  sys->index_anchors = MEM_malloc_arrayN((anchors_num), sizeof(int), "DeformAnchors");
  sys->unit_verts = MEM_calloc_arrayN(verts_num, sizeof(int), "DeformUnitVerts");
  return sys;
}

static void deleteLaplacianSystem(LaplacianSystem *sys)
{
  MEM_SAFE_FREE(sys->co);
  MEM_SAFE_FREE(sys->no);
  MEM_SAFE_FREE(sys->delta);
  MEM_SAFE_FREE(sys->tris);
  MEM_SAFE_FREE(sys->index_anchors);
  MEM_SAFE_FREE(sys->unit_verts);
  MEM_SAFE_FREE(sys->ringf_indices);
  MEM_SAFE_FREE(sys->ringv_indices);
  MEM_SAFE_FREE(sys->ringf_map);
  MEM_SAFE_FREE(sys->ringv_map);

  if (sys->context) {
    EIG_linear_solver_delete(sys->context);
  }
  MEM_SAFE_FREE(sys);
}

static void createFaceRingMap(const int mvert_tot,
                              const MLoopTri *mlooptri,
                              const int mtri_tot,
                              const MLoop *mloop,
                              MeshElemMap **r_map,
                              int **r_indices)
{
  int i, j, indices_num = 0;
  int *indices, *index_iter;
  MeshElemMap *map = MEM_calloc_arrayN(mvert_tot, sizeof(MeshElemMap), "DeformRingMap");
  const MLoopTri *mlt;

  for (i = 0, mlt = mlooptri; i < mtri_tot; i++, mlt++) {

    for (j = 0; j < 3; j++) {
      const uint v_index = mloop[mlt->tri[j]].v;
      map[v_index].count++;
      indices_num++;
    }
  }
  indices = MEM_calloc_arrayN(indices_num, sizeof(int), "DeformRingIndex");
  index_iter = indices;
  for (i = 0; i < mvert_tot; i++) {
    map[i].indices = index_iter;
    index_iter += map[i].count;
    map[i].count = 0;
  }
  for (i = 0, mlt = mlooptri; i < mtri_tot; i++, mlt++) {
    for (j = 0; j < 3; j++) {
      const uint v_index = mloop[mlt->tri[j]].v;
      map[v_index].indices[map[v_index].count] = i;
      map[v_index].count++;
    }
  }
  *r_map = map;
  *r_indices = indices;
}

static void createVertRingMap(const int mvert_tot,
                              const MEdge *medge,
                              const int medge_tot,
                              MeshElemMap **r_map,
                              int **r_indices)
{
  MeshElemMap *map = MEM_calloc_arrayN(mvert_tot, sizeof(MeshElemMap), "DeformNeighborsMap");
  int i, vid[2], indices_num = 0;
  int *indices, *index_iter;
  const MEdge *me;

  for (i = 0, me = medge; i < medge_tot; i++, me++) {
    vid[0] = me->v1;
    vid[1] = me->v2;
    map[vid[0]].count++;
    map[vid[1]].count++;
    indices_num += 2;
  }
  indices = MEM_calloc_arrayN(indices_num, sizeof(int), "DeformNeighborsIndex");
  index_iter = indices;
  for (i = 0; i < mvert_tot; i++) {
    map[i].indices = index_iter;
    index_iter += map[i].count;
    map[i].count = 0;
  }
  for (i = 0, me = medge; i < medge_tot; i++, me++) {
    vid[0] = me->v1;
    vid[1] = me->v2;
    map[vid[0]].indices[map[vid[0]].count] = vid[1];
    map[vid[0]].count++;
    map[vid[1]].indices[map[vid[1]].count] = vid[0];
    map[vid[1]].count++;
  }
  *r_map = map;
  *r_indices = indices;
}

/**
 * This method computes the Laplacian Matrix and Differential Coordinates
 * for all vertex in the mesh..
 * The Linear system is LV = d
 * Where L is Laplacian Matrix, V as the vertices in Mesh, d is the differential coordinates
 * The Laplacian Matrix is computes as a
 * Lij = sum(Wij) (if i == j)
 * Lij = Wij (if i != j)
 * Wij is weight between vertex Vi and vertex Vj, we use cotangent weight
 *
 * The Differential Coordinate is computes as a
 * di = Vi * sum(Wij) - sum(Wij * Vj)
 * Where :
 * di is the Differential Coordinate i
 * sum (Wij) is the sum of all weights between vertex Vi and its vertices neighbors (Vj)
 * sum (Wij * Vj) is the sum of the product between vertex neighbor Vj and weight Wij
 *                for all neighborhood.
 *
 * This Laplacian Matrix is described in the paper:
 * Desbrun M. et.al, Implicit fairing of irregular meshes using diffusion and curvature flow,
 * SIGGRAPH '99, page 317-324, New York, USA
 *
 * The computation of Laplace Beltrami operator on Hybrid Triangle/Quad Meshes is described in the
 * paper: Pinzon A., Romero E., Shape Inflation With an Adapted Laplacian Operator For
 * Hybrid Quad/Triangle Meshes,
 * Conference on Graphics Patterns and Images, SIBGRAPI, 2013
 *
 * The computation of Differential Coordinates is described in the paper:
 * Sorkine, O. Laplacian Surface Editing.
 * Proceedings of the EUROGRAPHICS/ACM SIGGRAPH Symposium on Geometry Processing,
 * 2004. p. 179-188.
 */
static void initLaplacianMatrix(LaplacianSystem *sys)
{
  float no[3];
  float w2, w3;
  int i = 3, j, ti;
  int idv[3];

  for (ti = 0; ti < sys->tris_num; ti++) {
    const uint *vidt = sys->tris[ti];
    const float *co[3];

    co[0] = sys->co[vidt[0]];
    co[1] = sys->co[vidt[1]];
    co[2] = sys->co[vidt[2]];

    normal_tri_v3(no, UNPACK3(co));
    add_v3_v3(sys->no[vidt[0]], no);
    add_v3_v3(sys->no[vidt[1]], no);
    add_v3_v3(sys->no[vidt[2]], no);

    for (j = 0; j < 3; j++) {
      const float *v1, *v2, *v3;

      idv[0] = vidt[j];
      idv[1] = vidt[(j + 1) % i];
      idv[2] = vidt[(j + 2) % i];

      v1 = sys->co[idv[0]];
      v2 = sys->co[idv[1]];
      v3 = sys->co[idv[2]];

      w2 = cotangent_tri_weight_v3(v3, v1, v2);
      w3 = cotangent_tri_weight_v3(v2, v3, v1);

      sys->delta[idv[0]][0] += v1[0] * (w2 + w3);
      sys->delta[idv[0]][1] += v1[1] * (w2 + w3);
      sys->delta[idv[0]][2] += v1[2] * (w2 + w3);

      sys->delta[idv[0]][0] -= v2[0] * w2;
      sys->delta[idv[0]][1] -= v2[1] * w2;
      sys->delta[idv[0]][2] -= v2[2] * w2;

      sys->delta[idv[0]][0] -= v3[0] * w3;
      sys->delta[idv[0]][1] -= v3[1] * w3;
      sys->delta[idv[0]][2] -= v3[2] * w3;

      EIG_linear_solver_matrix_add(sys->context, idv[0], idv[1], -w2);
      EIG_linear_solver_matrix_add(sys->context, idv[0], idv[2], -w3);
      EIG_linear_solver_matrix_add(sys->context, idv[0], idv[0], w2 + w3);
    }
  }
}

static void computeImplictRotations(LaplacianSystem *sys)
{
  int vid, *vidn = NULL;
  float minj, mjt, qj[3], vj[3];
  int i, j, ln;

  for (i = 0; i < sys->verts_num; i++) {
    normalize_v3(sys->no[i]);
    vidn = sys->ringv_map[i].indices;
    ln = sys->ringv_map[i].count;
    minj = 1000000.0f;
    for (j = 0; j < ln; j++) {
      vid = vidn[j];
      copy_v3_v3(qj, sys->co[vid]);
      sub_v3_v3v3(vj, qj, sys->co[i]);
      normalize_v3(vj);
      mjt = fabsf(dot_v3v3(vj, sys->no[i]));
      if (mjt < minj) {
        minj = mjt;
        sys->unit_verts[i] = vidn[j];
      }
    }
  }
}

static void rotateDifferentialCoordinates(LaplacianSystem *sys)
{
  float alpha, beta, gamma;
  float pj[3], ni[3], di[3];
  float uij[3], dun[3], e2[3], pi[3], fni[3], vn[3][3];
  int i, j, fidn_num, k, fi;
  int *fidn;

  for (i = 0; i < sys->verts_num; i++) {
    copy_v3_v3(pi, sys->co[i]);
    copy_v3_v3(ni, sys->no[i]);
    k = sys->unit_verts[i];
    copy_v3_v3(pj, sys->co[k]);
    sub_v3_v3v3(uij, pj, pi);
    mul_v3_v3fl(dun, ni, dot_v3v3(uij, ni));
    sub_v3_v3(uij, dun);
    normalize_v3(uij);
    cross_v3_v3v3(e2, ni, uij);
    copy_v3_v3(di, sys->delta[i]);
    alpha = dot_v3v3(ni, di);
    beta = dot_v3v3(uij, di);
    gamma = dot_v3v3(e2, di);

    pi[0] = EIG_linear_solver_variable_get(sys->context, 0, i);
    pi[1] = EIG_linear_solver_variable_get(sys->context, 1, i);
    pi[2] = EIG_linear_solver_variable_get(sys->context, 2, i);
    zero_v3(ni);
    fidn_num = sys->ringf_map[i].count;
    for (fi = 0; fi < fidn_num; fi++) {
      const uint *vin;
      fidn = sys->ringf_map[i].indices;
      vin = sys->tris[fidn[fi]];
      for (j = 0; j < 3; j++) {
        vn[j][0] = EIG_linear_solver_variable_get(sys->context, 0, vin[j]);
        vn[j][1] = EIG_linear_solver_variable_get(sys->context, 1, vin[j]);
        vn[j][2] = EIG_linear_solver_variable_get(sys->context, 2, vin[j]);
        if (vin[j] == sys->unit_verts[i]) {
          copy_v3_v3(pj, vn[j]);
        }
      }

      normal_tri_v3(fni, UNPACK3(vn));
      add_v3_v3(ni, fni);
    }

    normalize_v3(ni);
    sub_v3_v3v3(uij, pj, pi);
    mul_v3_v3fl(dun, ni, dot_v3v3(uij, ni));
    sub_v3_v3(uij, dun);
    normalize_v3(uij);
    cross_v3_v3v3(e2, ni, uij);
    fni[0] = alpha * ni[0] + beta * uij[0] + gamma * e2[0];
    fni[1] = alpha * ni[1] + beta * uij[1] + gamma * e2[1];
    fni[2] = alpha * ni[2] + beta * uij[2] + gamma * e2[2];

    if (len_squared_v3(fni) > FLT_EPSILON) {
      EIG_linear_solver_right_hand_side_add(sys->context, 0, i, fni[0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, i, fni[1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, i, fni[2]);
    }
    else {
      EIG_linear_solver_right_hand_side_add(sys->context, 0, i, sys->delta[i][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, i, sys->delta[i][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, i, sys->delta[i][2]);
    }
  }
}

static void laplacianDeformPreview(LaplacianSystem *sys, float (*vertexCos)[3])
{
  int vid, i, j, n, na;
  n = sys->verts_num;
  na = sys->anchors_num;

  if (!sys->is_matrix_computed) {
    sys->context = EIG_linear_least_squares_solver_new(n + na, n, 3);

    for (i = 0; i < n; i++) {
      EIG_linear_solver_variable_set(sys->context, 0, i, sys->co[i][0]);
      EIG_linear_solver_variable_set(sys->context, 1, i, sys->co[i][1]);
      EIG_linear_solver_variable_set(sys->context, 2, i, sys->co[i][2]);
    }
    for (i = 0; i < na; i++) {
      vid = sys->index_anchors[i];
      EIG_linear_solver_variable_set(sys->context, 0, vid, vertexCos[vid][0]);
      EIG_linear_solver_variable_set(sys->context, 1, vid, vertexCos[vid][1]);
      EIG_linear_solver_variable_set(sys->context, 2, vid, vertexCos[vid][2]);
    }

    initLaplacianMatrix(sys);
    computeImplictRotations(sys);

    for (i = 0; i < n; i++) {
      EIG_linear_solver_right_hand_side_add(sys->context, 0, i, sys->delta[i][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, i, sys->delta[i][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, i, sys->delta[i][2]);
    }
    for (i = 0; i < na; i++) {
      vid = sys->index_anchors[i];
      EIG_linear_solver_right_hand_side_add(sys->context, 0, n + i, vertexCos[vid][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, n + i, vertexCos[vid][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, n + i, vertexCos[vid][2]);
      EIG_linear_solver_matrix_add(sys->context, n + i, vid, 1.0f);
    }
    if (EIG_linear_solver_solve(sys->context)) {
      sys->has_solution = true;

      for (j = 1; j <= sys->repeat; j++) {
        rotateDifferentialCoordinates(sys);

        for (i = 0; i < na; i++) {
          vid = sys->index_anchors[i];
          EIG_linear_solver_right_hand_side_add(sys->context, 0, n + i, vertexCos[vid][0]);
          EIG_linear_solver_right_hand_side_add(sys->context, 1, n + i, vertexCos[vid][1]);
          EIG_linear_solver_right_hand_side_add(sys->context, 2, n + i, vertexCos[vid][2]);
        }

        if (!EIG_linear_solver_solve(sys->context)) {
          sys->has_solution = false;
          break;
        }
      }
      if (sys->has_solution) {
        for (vid = 0; vid < sys->verts_num; vid++) {
          vertexCos[vid][0] = EIG_linear_solver_variable_get(sys->context, 0, vid);
          vertexCos[vid][1] = EIG_linear_solver_variable_get(sys->context, 1, vid);
          vertexCos[vid][2] = EIG_linear_solver_variable_get(sys->context, 2, vid);
        }
      }
      else {
        sys->has_solution = false;
      }
    }
    else {
      sys->has_solution = false;
    }
    sys->is_matrix_computed = true;
  }
  else if (sys->has_solution) {
    for (i = 0; i < n; i++) {
      EIG_linear_solver_right_hand_side_add(sys->context, 0, i, sys->delta[i][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, i, sys->delta[i][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, i, sys->delta[i][2]);
    }
    for (i = 0; i < na; i++) {
      vid = sys->index_anchors[i];
      EIG_linear_solver_right_hand_side_add(sys->context, 0, n + i, vertexCos[vid][0]);
      EIG_linear_solver_right_hand_side_add(sys->context, 1, n + i, vertexCos[vid][1]);
      EIG_linear_solver_right_hand_side_add(sys->context, 2, n + i, vertexCos[vid][2]);
      EIG_linear_solver_matrix_add(sys->context, n + i, vid, 1.0f);
    }

    if (EIG_linear_solver_solve(sys->context)) {
      sys->has_solution = true;
      for (j = 1; j <= sys->repeat; j++) {
        rotateDifferentialCoordinates(sys);

        for (i = 0; i < na; i++) {
          vid = sys->index_anchors[i];
          EIG_linear_solver_right_hand_side_add(sys->context, 0, n + i, vertexCos[vid][0]);
          EIG_linear_solver_right_hand_side_add(sys->context, 1, n + i, vertexCos[vid][1]);
          EIG_linear_solver_right_hand_side_add(sys->context, 2, n + i, vertexCos[vid][2]);
        }
        if (!EIG_linear_solver_solve(sys->context)) {
          sys->has_solution = false;
          break;
        }
      }
      if (sys->has_solution) {
        for (vid = 0; vid < sys->verts_num; vid++) {
          vertexCos[vid][0] = EIG_linear_solver_variable_get(sys->context, 0, vid);
          vertexCos[vid][1] = EIG_linear_solver_variable_get(sys->context, 1, vid);
          vertexCos[vid][2] = EIG_linear_solver_variable_get(sys->context, 2, vid);
        }
      }
      else {
        sys->has_solution = false;
      }
    }
    else {
      sys->has_solution = false;
    }
  }
}

static bool isValidVertexGroup(LaplacianDeformModifierData *lmd, Object *ob, Mesh *mesh)
{
  int defgrp_index;
  MDeformVert *dvert = NULL;

  MOD_get_vgroup(ob, mesh, lmd->anchor_grp_name, &dvert, &defgrp_index);

  return (dvert != NULL);
}

static void initSystem(
    LaplacianDeformModifierData *lmd, Object *ob, Mesh *mesh, float (*vertexCos)[3], int verts_num)
{
  int i;
  int defgrp_index;
  int anchors_num;
  float wpaint;
  MDeformVert *dvert = NULL;
  MDeformVert *dv = NULL;
  LaplacianSystem *sys;
  const bool invert_vgroup = (lmd->flag & MOD_LAPLACIANDEFORM_INVERT_VGROUP) != 0;

  if (isValidVertexGroup(lmd, ob, mesh)) {
    int *index_anchors = MEM_malloc_arrayN(verts_num, sizeof(int), __func__); /* over-alloc */
    const MLoopTri *mlooptri;
    const MLoop *mloop;

    STACK_DECLARE(index_anchors);

    STACK_INIT(index_anchors, verts_num);

    MOD_get_vgroup(ob, mesh, lmd->anchor_grp_name, &dvert, &defgrp_index);
    BLI_assert(dvert != NULL);
    dv = dvert;
    for (i = 0; i < verts_num; i++) {
      wpaint = invert_vgroup ? 1.0f - BKE_defvert_find_weight(dv, defgrp_index) :
                               BKE_defvert_find_weight(dv, defgrp_index);
      dv++;
      if (wpaint > 0.0f) {
        STACK_PUSH(index_anchors, i);
      }
    }

    anchors_num = STACK_SIZE(index_anchors);
    lmd->cache_system = initLaplacianSystem(verts_num,
                                            mesh->totedge,
                                            BKE_mesh_runtime_looptri_len(mesh),
                                            anchors_num,
                                            lmd->anchor_grp_name,
                                            lmd->repeat);
    sys = (LaplacianSystem *)lmd->cache_system;
    memcpy(sys->index_anchors, index_anchors, sizeof(int) * anchors_num);
    memcpy(sys->co, vertexCos, sizeof(float[3]) * verts_num);
    MEM_freeN(index_anchors);
    lmd->vertexco = MEM_malloc_arrayN(verts_num, sizeof(float[3]), "ModDeformCoordinates");
    memcpy(lmd->vertexco, vertexCos, sizeof(float[3]) * verts_num);
    lmd->verts_num = verts_num;

    createFaceRingMap(mesh->totvert,
                      BKE_mesh_runtime_looptri_ensure(mesh),
                      BKE_mesh_runtime_looptri_len(mesh),
                      mesh->mloop,
                      &sys->ringf_map,
                      &sys->ringf_indices);
    createVertRingMap(
        mesh->totvert, mesh->medge, mesh->totedge, &sys->ringv_map, &sys->ringv_indices);

    mlooptri = BKE_mesh_runtime_looptri_ensure(mesh);
    mloop = mesh->mloop;

    for (i = 0; i < sys->tris_num; i++) {
      sys->tris[i][0] = mloop[mlooptri[i].tri[0]].v;
      sys->tris[i][1] = mloop[mlooptri[i].tri[1]].v;
      sys->tris[i][2] = mloop[mlooptri[i].tri[2]].v;
    }
  }
}

static int isSystemDifferent(LaplacianDeformModifierData *lmd,
                             Object *ob,
                             Mesh *mesh,
                             int verts_num)
{
  int i;
  int defgrp_index;
  int anchors_num = 0;
  float wpaint;
  MDeformVert *dvert = NULL;
  MDeformVert *dv = NULL;
  LaplacianSystem *sys = (LaplacianSystem *)lmd->cache_system;
  const bool invert_vgroup = (lmd->flag & MOD_LAPLACIANDEFORM_INVERT_VGROUP) != 0;

  if (sys->verts_num != verts_num) {
    return LAPDEFORM_SYSTEM_CHANGE_VERTEXES;
  }
  if (sys->edges_num != mesh->totedge) {
    return LAPDEFORM_SYSTEM_CHANGE_EDGES;
  }
  if (!STREQ(lmd->anchor_grp_name, sys->anchor_grp_name)) {
    return LAPDEFORM_SYSTEM_ONLY_CHANGE_GROUP;
  }
  MOD_get_vgroup(ob, mesh, lmd->anchor_grp_name, &dvert, &defgrp_index);
  if (!dvert) {
    return LAPDEFORM_SYSTEM_CHANGE_NOT_VALID_GROUP;
  }
  dv = dvert;
  for (i = 0; i < verts_num; i++) {
    wpaint = invert_vgroup ? 1.0f - BKE_defvert_find_weight(dv, defgrp_index) :
                             BKE_defvert_find_weight(dv, defgrp_index);
    dv++;
    if (wpaint > 0.0f) {
      anchors_num++;
    }
  }
  if (sys->anchors_num != anchors_num) {
    return LAPDEFORM_SYSTEM_ONLY_CHANGE_ANCHORS;
  }

  return LAPDEFORM_SYSTEM_NOT_CHANGE;
}

static void LaplacianDeformModifier_do(
    LaplacianDeformModifierData *lmd, Object *ob, Mesh *mesh, float (*vertexCos)[3], int verts_num)
{
  float(*filevertexCos)[3];
  int sysdif;
  LaplacianSystem *sys = NULL;
  filevertexCos = NULL;
  if (!(lmd->flag & MOD_LAPLACIANDEFORM_BIND)) {
    if (lmd->cache_system) {
      sys = lmd->cache_system;
      deleteLaplacianSystem(sys);
      lmd->cache_system = NULL;
    }
    lmd->verts_num = 0;
    MEM_SAFE_FREE(lmd->vertexco);
    return;
  }
  if (lmd->cache_system) {
    sysdif = isSystemDifferent(lmd, ob, mesh, verts_num);
    sys = lmd->cache_system;
    if (sysdif) {
      if (ELEM(sysdif, LAPDEFORM_SYSTEM_ONLY_CHANGE_ANCHORS, LAPDEFORM_SYSTEM_ONLY_CHANGE_GROUP)) {
        filevertexCos = MEM_malloc_arrayN(verts_num, sizeof(float[3]), "TempModDeformCoordinates");
        memcpy(filevertexCos, lmd->vertexco, sizeof(float[3]) * verts_num);
        MEM_SAFE_FREE(lmd->vertexco);
        lmd->verts_num = 0;
        deleteLaplacianSystem(sys);
        lmd->cache_system = NULL;
        initSystem(lmd, ob, mesh, filevertexCos, verts_num);
        sys = lmd->cache_system; /* may have been reallocated */
        MEM_SAFE_FREE(filevertexCos);
        if (sys) {
          laplacianDeformPreview(sys, vertexCos);
        }
      }
      else {
        if (sysdif == LAPDEFORM_SYSTEM_CHANGE_VERTEXES) {
          BKE_modifier_set_error(
              ob, &lmd->modifier, "Vertices changed from %d to %d", lmd->verts_num, verts_num);
        }
        else if (sysdif == LAPDEFORM_SYSTEM_CHANGE_EDGES) {
          BKE_modifier_set_error(
              ob, &lmd->modifier, "Edges changed from %d to %d", sys->edges_num, mesh->totedge);
        }
        else if (sysdif == LAPDEFORM_SYSTEM_CHANGE_NOT_VALID_GROUP) {
          BKE_modifier_set_error(ob,
                                 &lmd->modifier,
                                 "Vertex group '%s' is not valid, or maybe empty",
                                 sys->anchor_grp_name);
        }
      }
    }
    else {
      sys->repeat = lmd->repeat;
      laplacianDeformPreview(sys, vertexCos);
    }
  }
  else {
    if (!isValidVertexGroup(lmd, ob, mesh)) {
      BKE_modifier_set_error(ob,
                             &lmd->modifier,
                             "Vertex group '%s' is not valid, or maybe empty",
                             lmd->anchor_grp_name);
      lmd->flag &= ~MOD_LAPLACIANDEFORM_BIND;
    }
    else if (lmd->verts_num > 0 && lmd->verts_num == verts_num) {
      filevertexCos = MEM_malloc_arrayN(verts_num, sizeof(float[3]), "TempDeformCoordinates");
      memcpy(filevertexCos, lmd->vertexco, sizeof(float[3]) * verts_num);
      MEM_SAFE_FREE(lmd->vertexco);
      lmd->verts_num = 0;
      initSystem(lmd, ob, mesh, filevertexCos, verts_num);
      sys = lmd->cache_system;
      MEM_SAFE_FREE(filevertexCos);
      laplacianDeformPreview(sys, vertexCos);
    }
    else {
      initSystem(lmd, ob, mesh, vertexCos, verts_num);
      sys = lmd->cache_system;
      laplacianDeformPreview(sys, vertexCos);
    }
  }
  if (sys && sys->is_matrix_computed && !sys->has_solution) {
    BKE_modifier_set_error(ob, &lmd->modifier, "The system did not find a solution");
  }
}

static void initData(ModifierData *md)
{
  LaplacianDeformModifierData *lmd = (LaplacianDeformModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(lmd, modifier));

  MEMCPY_STRUCT_AFTER(lmd, DNA_struct_default_get(LaplacianDeformModifierData), modifier);
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
  const LaplacianDeformModifierData *lmd = (const LaplacianDeformModifierData *)md;
  LaplacianDeformModifierData *tlmd = (LaplacianDeformModifierData *)target;

  BKE_modifier_copydata_generic(md, target, flag);

  tlmd->vertexco = MEM_dupallocN(lmd->vertexco);
  tlmd->cache_system = NULL;
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(useRenderParams))
{
  LaplacianDeformModifierData *lmd = (LaplacianDeformModifierData *)md;
  if (lmd->anchor_grp_name[0]) {
    return 0;
  }
  return 1;
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  LaplacianDeformModifierData *lmd = (LaplacianDeformModifierData *)md;

  if (lmd->anchor_grp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int verts_num)
{
  Mesh *mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, verts_num, false);

  LaplacianDeformModifier_do(
      (LaplacianDeformModifierData *)md, ctx->object, mesh_src, vertexCos, verts_num);

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
  Mesh *mesh_src = MOD_deform_mesh_eval_get(ctx->object, editData, mesh, NULL, verts_num, false);

  /* TODO(@campbellbarton): use edit-mode data only (remove this line). */
  if (mesh_src != NULL) {
    BKE_mesh_wrapper_ensure_mdata(mesh_src);
  }

  LaplacianDeformModifier_do(
      (LaplacianDeformModifierData *)md, ctx->object, mesh_src, vertexCos, verts_num);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void freeData(ModifierData *md)
{
  LaplacianDeformModifierData *lmd = (LaplacianDeformModifierData *)md;
  LaplacianSystem *sys = (LaplacianSystem *)lmd->cache_system;
  if (sys) {
    deleteLaplacianSystem(sys);
  }
  MEM_SAFE_FREE(lmd->vertexco);
  lmd->verts_num = 0;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *row;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  bool is_bind = RNA_boolean_get(ptr, "is_bind");
  bool has_vertex_group = RNA_string_length(ptr, "vertex_group") != 0;

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "iterations", 0, NULL, ICON_NONE);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", NULL);

  uiItemS(layout);

  row = uiLayoutRow(layout, true);
  uiLayoutSetEnabled(row, has_vertex_group);
  uiItemO(row,
          is_bind ? IFACE_("Unbind") : IFACE_("Bind"),
          ICON_NONE,
          "OBJECT_OT_laplaciandeform_bind");

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_LaplacianDeform, panel_draw);
}

static void blendWrite(BlendWriter *writer, const ID *id_owner, const ModifierData *md)
{
  LaplacianDeformModifierData lmd = *(const LaplacianDeformModifierData *)md;
  const bool is_undo = BLO_write_is_undo(writer);

  if (ID_IS_OVERRIDE_LIBRARY(id_owner) && !is_undo) {
    BLI_assert(!ID_IS_LINKED(id_owner));
    const bool is_local = (md->flag & eModifierFlag_OverrideLibrary_Local) != 0;
    if (!is_local) {
      /* Modifier coming from linked data cannot be bound from an override, so we can remove all
       * binding data, can save a significant amount of memory. */
      lmd.verts_num = 0;
      lmd.vertexco = NULL;
    }
  }

  BLO_write_struct_at_address(writer, LaplacianDeformModifierData, md, &lmd);

  if (lmd.vertexco != NULL) {
    BLO_write_float3_array(writer, lmd.verts_num, lmd.vertexco);
  }
}

static void blendRead(BlendDataReader *reader, ModifierData *md)
{
  LaplacianDeformModifierData *lmd = (LaplacianDeformModifierData *)md;

  BLO_read_float3_array(reader, lmd->verts_num, &lmd->vertexco);
  lmd->cache_system = NULL;
}

ModifierTypeInfo modifierType_LaplacianDeform = {
    /* name */ N_("LaplacianDeform"),
    /* structName */ "LaplacianDeformModifierData",
    /* structSize */ sizeof(LaplacianDeformModifierData),
    /* srna */ &RNA_LaplacianDeformModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /* icon */ ICON_MOD_MESHDEFORM,
    /* copyData */ copyData,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ NULL,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ blendWrite,
    /* blendRead */ blendRead,
};
