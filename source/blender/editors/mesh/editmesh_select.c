/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2004 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.h"
#include "BLI_bitmap.h"
#include "BLI_heap.h"
#include "BLI_linklist.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_bits.h"
#include "BLI_rand.h"
#include "BLI_string.h"
#include "BLI_utildefines_stack.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_layer.h"
#include "BKE_report.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "ED_mesh.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_select_utils.h"
#include "ED_transform.h"
#include "ED_view3d.h"

#include "BLT_translation.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "UI_resources.h"

#include "bmesh_tools.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "DRW_select_buffer.h"

#include "mesh_intern.h" /* own include */

/* use bmesh operator flags for a few operators */
#define BMO_ELE_TAG 1

/* -------------------------------------------------------------------- */
/** \name Select Mirror
 * \{ */

void EDBM_select_mirrored(BMEditMesh *em,
                          const Mesh *me,
                          const int axis,
                          const bool extend,
                          int *r_totmirr,
                          int *r_totfail)
{
  BMesh *bm = em->bm;
  BMIter iter;
  int totmirr = 0;
  int totfail = 0;
  bool use_topology = me->editflag & ME_EDIT_MIRROR_TOPO;

  *r_totmirr = *r_totfail = 0;

  /* select -> tag */
  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BMVert *v;
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      BM_elem_flag_set(v, BM_ELEM_TAG, BM_elem_flag_test(v, BM_ELEM_SELECT));
    }
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    BMEdge *e;
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      BM_elem_flag_set(e, BM_ELEM_TAG, BM_elem_flag_test(e, BM_ELEM_SELECT));
    }
  }
  else {
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      BM_elem_flag_set(f, BM_ELEM_TAG, BM_elem_flag_test(f, BM_ELEM_SELECT));
    }
  }

  EDBM_verts_mirror_cache_begin(em, axis, true, true, false, use_topology);

  if (!extend) {
    EDBM_flag_disable_all(em, BM_ELEM_SELECT);
  }

  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BMVert *v;
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN) && BM_elem_flag_test(v, BM_ELEM_TAG)) {
        BMVert *v_mirr = EDBM_verts_mirror_get(em, v);
        if (v_mirr && !BM_elem_flag_test(v_mirr, BM_ELEM_HIDDEN)) {
          BM_vert_select_set(bm, v_mirr, true);
          totmirr++;
        }
        else {
          totfail++;
        }
      }
    }
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    BMEdge *e;
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      if (!BM_elem_flag_test(e, BM_ELEM_HIDDEN) && BM_elem_flag_test(e, BM_ELEM_TAG)) {
        BMEdge *e_mirr = EDBM_verts_mirror_get_edge(em, e);
        if (e_mirr && !BM_elem_flag_test(e_mirr, BM_ELEM_HIDDEN)) {
          BM_edge_select_set(bm, e_mirr, true);
          totmirr++;
        }
        else {
          totfail++;
        }
      }
    }
  }
  else {
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN) && BM_elem_flag_test(f, BM_ELEM_TAG)) {
        BMFace *f_mirr = EDBM_verts_mirror_get_face(em, f);
        if (f_mirr && !BM_elem_flag_test(f_mirr, BM_ELEM_HIDDEN)) {
          BM_face_select_set(bm, f_mirr, true);
          totmirr++;
        }
        else {
          totfail++;
        }
      }
    }
  }

  EDBM_verts_mirror_cache_end(em);

  *r_totmirr = totmirr;
  *r_totfail = totfail;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Back-Buffer OpenGL Selection
 * \{ */

static BMElem *edbm_select_id_bm_elem_get(Base **bases, const uint sel_id, uint *r_base_index)
{
  uint elem_id;
  char elem_type = 0;
  bool success = DRW_select_buffer_elem_get(sel_id, &elem_id, r_base_index, &elem_type);

  if (success) {
    Object *obedit = bases[*r_base_index]->object;
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    switch (elem_type) {
      case SCE_SELECT_FACE:
        return (BMElem *)BM_face_at_index_find_or_table(em->bm, elem_id);
      case SCE_SELECT_EDGE:
        return (BMElem *)BM_edge_at_index_find_or_table(em->bm, elem_id);
      case SCE_SELECT_VERTEX:
        return (BMElem *)BM_vert_at_index_find_or_table(em->bm, elem_id);
      default:
        BLI_assert(0);
        return NULL;
    }
  }

  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Find Nearest Vert/Edge/Face
 *
 * \note Screen-space manhattan distances are used here,
 * since its faster and good enough for the purpose of selection.
 *
 * \note \a dist_bias is used so we can bias against selected items.
 * when choosing between elements of a single type, but return the real distance
 * to avoid the bias interfering with distance comparisons when mixing types.
 * \{ */

#define FIND_NEAR_SELECT_BIAS 5
#define FIND_NEAR_CYCLE_THRESHOLD_MIN 3

struct NearestVertUserData_Hit {
  float dist;
  float dist_bias;
  int index;
  BMVert *vert;
};

struct NearestVertUserData {
  float mval_fl[2];
  bool use_select_bias;
  bool use_cycle;
  int cycle_index_prev;

  struct NearestVertUserData_Hit hit;
  struct NearestVertUserData_Hit hit_cycle;
};

static void findnearestvert__doClosest(void *userData,
                                       BMVert *eve,
                                       const float screen_co[2],
                                       int index)
{
  struct NearestVertUserData *data = userData;
  float dist_test, dist_test_bias;

  dist_test = dist_test_bias = len_manhattan_v2v2(data->mval_fl, screen_co);

  if (data->use_select_bias && BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
    dist_test_bias += FIND_NEAR_SELECT_BIAS;
  }

  if (dist_test_bias < data->hit.dist_bias) {
    data->hit.dist_bias = dist_test_bias;
    data->hit.dist = dist_test;
    data->hit.index = index;
    data->hit.vert = eve;
  }

  if (data->use_cycle) {
    if ((data->hit_cycle.vert == NULL) && (index > data->cycle_index_prev) &&
        (dist_test_bias < FIND_NEAR_CYCLE_THRESHOLD_MIN)) {
      data->hit_cycle.dist_bias = dist_test_bias;
      data->hit_cycle.dist = dist_test;
      data->hit_cycle.index = index;
      data->hit_cycle.vert = eve;
    }
  }
}

BMVert *EDBM_vert_find_nearest_ex(ViewContext *vc,
                                  float *dist_px_manhattan_p,
                                  const bool use_select_bias,
                                  bool use_cycle,
                                  Base **bases,
                                  uint bases_len,
                                  uint *r_base_index)
{
  uint base_index = 0;

  if (!XRAY_FLAG_ENABLED(vc->v3d)) {
    uint dist_px_manhattan_test = (uint)ED_view3d_backbuf_sample_size_clamp(vc->region,
                                                                            *dist_px_manhattan_p);
    uint index;
    BMVert *eve;

    /* No afterqueue (yet), so we check it now, otherwise the bm_xxxofs indices are bad. */
    {
      DRW_select_buffer_context_create(bases, bases_len, SCE_SELECT_VERTEX);

      index = DRW_select_buffer_find_nearest_to_point(
          vc->depsgraph, vc->region, vc->v3d, vc->mval, 1, UINT_MAX, &dist_px_manhattan_test);

      if (index) {
        eve = (BMVert *)edbm_select_id_bm_elem_get(bases, index, &base_index);
      }
      else {
        eve = NULL;
      }
    }

    if (eve) {
      if (dist_px_manhattan_test < *dist_px_manhattan_p) {
        if (r_base_index) {
          *r_base_index = base_index;
        }
        *dist_px_manhattan_p = dist_px_manhattan_test;
        return eve;
      }
    }
    return NULL;
  }

  struct NearestVertUserData data = {{0}};
  const struct NearestVertUserData_Hit *hit = NULL;
  const eV3DProjTest clip_flag = RV3D_CLIPPING_ENABLED(vc->v3d, vc->rv3d) ?
                                     V3D_PROJ_TEST_CLIP_DEFAULT :
                                     V3D_PROJ_TEST_CLIP_DEFAULT & ~V3D_PROJ_TEST_CLIP_BB;
  BMesh *prev_select_bm = NULL;

  static struct {
    int index;
    const BMVert *elem;
    const BMesh *bm;
  } prev_select = {0};

  data.mval_fl[0] = vc->mval[0];
  data.mval_fl[1] = vc->mval[1];
  data.use_select_bias = use_select_bias;
  data.use_cycle = use_cycle;

  for (; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    ED_view3d_viewcontext_init_object(vc, base_iter->object);
    if (use_cycle && prev_select.bm == vc->em->bm &&
        prev_select.elem == BM_vert_at_index_find_or_table(vc->em->bm, prev_select.index)) {
      data.cycle_index_prev = prev_select.index;
      /* No need to compare in the rest of the loop. */
      use_cycle = false;
    }
    else {
      data.cycle_index_prev = 0;
    }

    data.hit.dist = data.hit_cycle.dist = data.hit.dist_bias = data.hit_cycle.dist_bias =
        *dist_px_manhattan_p;

    ED_view3d_init_mats_rv3d(vc->obedit, vc->rv3d);
    mesh_foreachScreenVert(vc, findnearestvert__doClosest, &data, clip_flag);

    hit = (data.use_cycle && data.hit_cycle.vert) ? &data.hit_cycle : &data.hit;

    if (hit->dist < *dist_px_manhattan_p) {
      if (r_base_index) {
        *r_base_index = base_index;
      }
      *dist_px_manhattan_p = hit->dist;
      prev_select_bm = vc->em->bm;
    }
  }

  if (hit == NULL) {
    return NULL;
  }

  prev_select.index = hit->index;
  prev_select.elem = hit->vert;
  prev_select.bm = prev_select_bm;

  return hit->vert;
}

BMVert *EDBM_vert_find_nearest(ViewContext *vc, float *dist_px_manhattan_p)
{
  Base *base = BKE_view_layer_base_find(vc->view_layer, vc->obact);
  return EDBM_vert_find_nearest_ex(vc, dist_px_manhattan_p, false, false, &base, 1, NULL);
}

/* find the distance to the edge we already have */
struct NearestEdgeUserData_ZBuf {
  float mval_fl[2];
  float dist;
  const BMEdge *edge_test;
};

static void find_nearest_edge_center__doZBuf(void *userData,
                                             BMEdge *eed,
                                             const float screen_co_a[2],
                                             const float screen_co_b[2],
                                             int UNUSED(index))
{
  struct NearestEdgeUserData_ZBuf *data = userData;

  if (eed == data->edge_test) {
    float dist_test;
    float screen_co_mid[2];

    mid_v2_v2v2(screen_co_mid, screen_co_a, screen_co_b);
    dist_test = len_manhattan_v2v2(data->mval_fl, screen_co_mid);

    if (dist_test < data->dist) {
      data->dist = dist_test;
    }
  }
}

struct NearestEdgeUserData_Hit {
  float dist;
  float dist_bias;
  int index;
  BMEdge *edge;

  /* edges only, un-biased manhattan distance to which ever edge we pick
   * (not used for choosing) */
  float dist_center_px_manhattan;
};

struct NearestEdgeUserData {
  ViewContext vc;
  float mval_fl[2];
  bool use_select_bias;
  bool use_cycle;
  int cycle_index_prev;

  struct NearestEdgeUserData_Hit hit;
  struct NearestEdgeUserData_Hit hit_cycle;
};

/* NOTE: uses v3d, so needs active 3d window. */
static void find_nearest_edge__doClosest(
    void *userData, BMEdge *eed, const float screen_co_a[2], const float screen_co_b[2], int index)
{
  struct NearestEdgeUserData *data = userData;
  float dist_test, dist_test_bias;

  float fac = line_point_factor_v2(data->mval_fl, screen_co_a, screen_co_b);
  float screen_co[2];

  if (fac <= 0.0f) {
    fac = 0.0f;
    copy_v2_v2(screen_co, screen_co_a);
  }
  else if (fac >= 1.0f) {
    fac = 1.0f;
    copy_v2_v2(screen_co, screen_co_b);
  }
  else {
    interp_v2_v2v2(screen_co, screen_co_a, screen_co_b, fac);
  }

  dist_test = dist_test_bias = len_manhattan_v2v2(data->mval_fl, screen_co);

  if (data->use_select_bias && BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
    dist_test_bias += FIND_NEAR_SELECT_BIAS;
  }

  if (data->vc.rv3d->rflag & RV3D_CLIPPING) {
    float vec[3];

    interp_v3_v3v3(vec, eed->v1->co, eed->v2->co, fac);
    if (ED_view3d_clipping_test(data->vc.rv3d, vec, true)) {
      return;
    }
  }

  if (dist_test_bias < data->hit.dist_bias) {
    float screen_co_mid[2];

    data->hit.dist_bias = dist_test_bias;
    data->hit.dist = dist_test;
    data->hit.index = index;
    data->hit.edge = eed;

    mid_v2_v2v2(screen_co_mid, screen_co_a, screen_co_b);
    data->hit.dist_center_px_manhattan = len_manhattan_v2v2(data->mval_fl, screen_co_mid);
  }

  if (data->use_cycle) {
    if ((data->hit_cycle.edge == NULL) && (index > data->cycle_index_prev) &&
        (dist_test_bias < FIND_NEAR_CYCLE_THRESHOLD_MIN)) {
      float screen_co_mid[2];

      data->hit_cycle.dist_bias = dist_test_bias;
      data->hit_cycle.dist = dist_test;
      data->hit_cycle.index = index;
      data->hit_cycle.edge = eed;

      mid_v2_v2v2(screen_co_mid, screen_co_a, screen_co_b);
      data->hit_cycle.dist_center_px_manhattan = len_manhattan_v2v2(data->mval_fl, screen_co_mid);
    }
  }
}

BMEdge *EDBM_edge_find_nearest_ex(ViewContext *vc,
                                  float *dist_px_manhattan_p,
                                  float *r_dist_center_px_manhattan,
                                  const bool use_select_bias,
                                  bool use_cycle,
                                  BMEdge **r_eed_zbuf,
                                  Base **bases,
                                  uint bases_len,
                                  uint *r_base_index)
{
  uint base_index = 0;

  if (!XRAY_FLAG_ENABLED(vc->v3d)) {
    uint dist_px_manhattan_test = (uint)ED_view3d_backbuf_sample_size_clamp(vc->region,
                                                                            *dist_px_manhattan_p);
    uint index;
    BMEdge *eed;

    /* No afterqueue (yet), so we check it now, otherwise the bm_xxxofs indices are bad. */
    {
      DRW_select_buffer_context_create(bases, bases_len, SCE_SELECT_EDGE);

      index = DRW_select_buffer_find_nearest_to_point(
          vc->depsgraph, vc->region, vc->v3d, vc->mval, 1, UINT_MAX, &dist_px_manhattan_test);

      if (index) {
        eed = (BMEdge *)edbm_select_id_bm_elem_get(bases, index, &base_index);
      }
      else {
        eed = NULL;
      }
    }

    if (r_eed_zbuf) {
      *r_eed_zbuf = eed;
    }

    /* exception for faces (verts don't need this) */
    if (r_dist_center_px_manhattan && eed) {
      struct NearestEdgeUserData_ZBuf data;

      data.mval_fl[0] = vc->mval[0];
      data.mval_fl[1] = vc->mval[1];
      data.dist = FLT_MAX;
      data.edge_test = eed;

      ED_view3d_init_mats_rv3d(vc->obedit, vc->rv3d);

      mesh_foreachScreenEdge(vc,
                             find_nearest_edge_center__doZBuf,
                             &data,
                             V3D_PROJ_TEST_CLIP_DEFAULT | V3D_PROJ_TEST_CLIP_CONTENT_DEFAULT);

      *r_dist_center_px_manhattan = data.dist;
    }
    /* end exception */

    if (eed) {
      if (dist_px_manhattan_test < *dist_px_manhattan_p) {
        if (r_base_index) {
          *r_base_index = base_index;
        }
        *dist_px_manhattan_p = dist_px_manhattan_test;
        return eed;
      }
    }
    return NULL;
  }

  struct NearestEdgeUserData data = {{0}};
  const struct NearestEdgeUserData_Hit *hit = NULL;
  /* interpolate along the edge before doing a clipping plane test */
  const eV3DProjTest clip_flag = V3D_PROJ_TEST_CLIP_DEFAULT & ~V3D_PROJ_TEST_CLIP_BB;
  BMesh *prev_select_bm = NULL;

  static struct {
    int index;
    const BMEdge *elem;
    const BMesh *bm;
  } prev_select = {0};

  data.vc = *vc;
  data.mval_fl[0] = vc->mval[0];
  data.mval_fl[1] = vc->mval[1];
  data.use_select_bias = use_select_bias;
  data.use_cycle = use_cycle;

  for (; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    ED_view3d_viewcontext_init_object(vc, base_iter->object);
    if (use_cycle && prev_select.bm == vc->em->bm &&
        prev_select.elem == BM_edge_at_index_find_or_table(vc->em->bm, prev_select.index)) {
      data.cycle_index_prev = prev_select.index;
      /* No need to compare in the rest of the loop. */
      use_cycle = false;
    }
    else {
      data.cycle_index_prev = 0;
    }

    data.hit.dist = data.hit_cycle.dist = data.hit.dist_bias = data.hit_cycle.dist_bias =
        *dist_px_manhattan_p;

    ED_view3d_init_mats_rv3d(vc->obedit, vc->rv3d);
    mesh_foreachScreenEdge(
        vc, find_nearest_edge__doClosest, &data, clip_flag | V3D_PROJ_TEST_CLIP_CONTENT_DEFAULT);

    hit = (data.use_cycle && data.hit_cycle.edge) ? &data.hit_cycle : &data.hit;

    if (hit->dist < *dist_px_manhattan_p) {
      if (r_base_index) {
        *r_base_index = base_index;
      }
      *dist_px_manhattan_p = hit->dist;
      prev_select_bm = vc->em->bm;
    }
  }

  if (hit == NULL) {
    return NULL;
  }

  if (r_dist_center_px_manhattan) {
    *r_dist_center_px_manhattan = hit->dist_center_px_manhattan;
  }

  prev_select.index = hit->index;
  prev_select.elem = hit->edge;
  prev_select.bm = prev_select_bm;

  return hit->edge;
}

BMEdge *EDBM_edge_find_nearest(ViewContext *vc, float *dist_px_manhattan_p)
{
  Base *base = BKE_view_layer_base_find(vc->view_layer, vc->obact);
  return EDBM_edge_find_nearest_ex(
      vc, dist_px_manhattan_p, NULL, false, false, NULL, &base, 1, NULL);
}

/* find the distance to the face we already have */
struct NearestFaceUserData_ZBuf {
  float mval_fl[2];
  float dist_px_manhattan;
  const BMFace *face_test;
};

static void find_nearest_face_center__doZBuf(void *userData,
                                             BMFace *efa,
                                             const float screen_co[2],
                                             int UNUSED(index))
{
  struct NearestFaceUserData_ZBuf *data = userData;

  if (efa == data->face_test) {
    const float dist_test = len_manhattan_v2v2(data->mval_fl, screen_co);

    if (dist_test < data->dist_px_manhattan) {
      data->dist_px_manhattan = dist_test;
    }
  }
}

struct NearestFaceUserData_Hit {
  float dist;
  float dist_bias;
  int index;
  BMFace *face;
};

struct NearestFaceUserData {
  float mval_fl[2];
  bool use_select_bias;
  bool use_cycle;
  int cycle_index_prev;

  struct NearestFaceUserData_Hit hit;
  struct NearestFaceUserData_Hit hit_cycle;
};

static void findnearestface__doClosest(void *userData,
                                       BMFace *efa,
                                       const float screen_co[2],
                                       int index)
{
  struct NearestFaceUserData *data = userData;
  float dist_test, dist_test_bias;

  dist_test = dist_test_bias = len_manhattan_v2v2(data->mval_fl, screen_co);

  if (data->use_select_bias && BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
    dist_test_bias += FIND_NEAR_SELECT_BIAS;
  }

  if (dist_test_bias < data->hit.dist_bias) {
    data->hit.dist_bias = dist_test_bias;
    data->hit.dist = dist_test;
    data->hit.index = index;
    data->hit.face = efa;
  }

  if (data->use_cycle) {
    if ((data->hit_cycle.face == NULL) && (index > data->cycle_index_prev) &&
        (dist_test_bias < FIND_NEAR_CYCLE_THRESHOLD_MIN)) {
      data->hit_cycle.dist_bias = dist_test_bias;
      data->hit_cycle.dist = dist_test;
      data->hit_cycle.index = index;
      data->hit_cycle.face = efa;
    }
  }
}

BMFace *EDBM_face_find_nearest_ex(ViewContext *vc,
                                  float *dist_px_manhattan_p,
                                  float *r_dist_center,
                                  const bool use_zbuf_single_px,
                                  const bool use_select_bias,
                                  bool use_cycle,
                                  BMFace **r_efa_zbuf,
                                  Base **bases,
                                  uint bases_len,
                                  uint *r_base_index)
{
  uint base_index = 0;

  if (!XRAY_FLAG_ENABLED(vc->v3d)) {
    float dist_test;
    uint index;
    BMFace *efa;

    {
      uint dist_px_manhattan_test = 0;
      if (*dist_px_manhattan_p != 0.0f && (use_zbuf_single_px == false)) {
        dist_px_manhattan_test = (uint)ED_view3d_backbuf_sample_size_clamp(vc->region,
                                                                           *dist_px_manhattan_p);
      }

      DRW_select_buffer_context_create(bases, bases_len, SCE_SELECT_FACE);

      if (dist_px_manhattan_test == 0) {
        index = DRW_select_buffer_sample_point(vc->depsgraph, vc->region, vc->v3d, vc->mval);
        dist_test = 0.0f;
      }
      else {
        index = DRW_select_buffer_find_nearest_to_point(
            vc->depsgraph, vc->region, vc->v3d, vc->mval, 1, UINT_MAX, &dist_px_manhattan_test);
        dist_test = dist_px_manhattan_test;
      }

      if (index) {
        efa = (BMFace *)edbm_select_id_bm_elem_get(bases, index, &base_index);
      }
      else {
        efa = NULL;
      }
    }

    if (r_efa_zbuf) {
      *r_efa_zbuf = efa;
    }

    /* exception for faces (verts don't need this) */
    if (r_dist_center && efa) {
      struct NearestFaceUserData_ZBuf data;

      data.mval_fl[0] = vc->mval[0];
      data.mval_fl[1] = vc->mval[1];
      data.dist_px_manhattan = FLT_MAX;
      data.face_test = efa;

      ED_view3d_init_mats_rv3d(vc->obedit, vc->rv3d);

      mesh_foreachScreenFace(
          vc, find_nearest_face_center__doZBuf, &data, V3D_PROJ_TEST_CLIP_DEFAULT);

      *r_dist_center = data.dist_px_manhattan;
    }
    /* end exception */

    if (efa) {
      if (dist_test < *dist_px_manhattan_p) {
        if (r_base_index) {
          *r_base_index = base_index;
        }
        *dist_px_manhattan_p = dist_test;
        return efa;
      }
    }
    return NULL;
  }

  struct NearestFaceUserData data = {{0}};
  const struct NearestFaceUserData_Hit *hit = NULL;
  const eV3DProjTest clip_flag = V3D_PROJ_TEST_CLIP_DEFAULT;
  BMesh *prev_select_bm = NULL;

  static struct {
    int index;
    const BMFace *elem;
    const BMesh *bm;
  } prev_select = {0};

  data.mval_fl[0] = vc->mval[0];
  data.mval_fl[1] = vc->mval[1];
  data.use_select_bias = use_select_bias;
  data.use_cycle = use_cycle;

  for (; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    ED_view3d_viewcontext_init_object(vc, base_iter->object);
    if (use_cycle && prev_select.bm == vc->em->bm &&
        prev_select.elem == BM_face_at_index_find_or_table(vc->em->bm, prev_select.index)) {
      data.cycle_index_prev = prev_select.index;
      /* No need to compare in the rest of the loop. */
      use_cycle = false;
    }
    else {
      data.cycle_index_prev = 0;
    }

    data.hit.dist = data.hit_cycle.dist = data.hit.dist_bias = data.hit_cycle.dist_bias =
        *dist_px_manhattan_p;

    ED_view3d_init_mats_rv3d(vc->obedit, vc->rv3d);
    mesh_foreachScreenFace(vc, findnearestface__doClosest, &data, clip_flag);

    hit = (data.use_cycle && data.hit_cycle.face) ? &data.hit_cycle : &data.hit;

    if (hit->dist < *dist_px_manhattan_p) {
      if (r_base_index) {
        *r_base_index = base_index;
      }
      *dist_px_manhattan_p = hit->dist;
      prev_select_bm = vc->em->bm;
    }
  }

  if (hit == NULL) {
    return NULL;
  }

  if (r_dist_center) {
    *r_dist_center = hit->dist;
  }

  prev_select.index = hit->index;
  prev_select.elem = hit->face;
  prev_select.bm = prev_select_bm;

  return hit->face;
}

BMFace *EDBM_face_find_nearest(ViewContext *vc, float *dist_px_manhattan_p)
{
  Base *base = BKE_view_layer_base_find(vc->view_layer, vc->obact);
  return EDBM_face_find_nearest_ex(
      vc, dist_px_manhattan_p, NULL, false, false, false, NULL, &base, 1, NULL);
}

#undef FIND_NEAR_SELECT_BIAS
#undef FIND_NEAR_CYCLE_THRESHOLD_MIN

/* best distance based on screen coords.
 * use em->selectmode to define how to use
 * selected vertices and edges get disadvantage
 * return 1 if found one
 */
static bool unified_findnearest(ViewContext *vc,
                                Base **bases,
                                const uint bases_len,
                                int *r_base_index,
                                BMVert **r_eve,
                                BMEdge **r_eed,
                                BMFace **r_efa)
{
  BMEditMesh *em = vc->em;

  const bool use_cycle = !WM_cursor_test_motion_and_update(vc->mval);
  const float dist_init = ED_view3d_select_dist_px();
  /* since edges select lines, we give dots advantage of ~20 pix */
  const float dist_margin = (dist_init / 2);
  float dist = dist_init;

  struct {
    struct {
      BMVert *ele;
      int base_index;
    } v;
    struct {
      BMEdge *ele;
      int base_index;
    } e, e_zbuf;
    struct {
      BMFace *ele;
      int base_index;
    } f, f_zbuf;
  } hit = {{NULL}};

  /* no afterqueue (yet), so we check it now, otherwise the em_xxxofs indices are bad */

  if ((dist > 0.0f) && (em->selectmode & SCE_SELECT_FACE)) {
    float dist_center = 0.0f;
    float *dist_center_p = (em->selectmode & (SCE_SELECT_EDGE | SCE_SELECT_VERTEX)) ?
                               &dist_center :
                               NULL;

    uint base_index = 0;
    BMFace *efa_zbuf = NULL;
    BMFace *efa_test = EDBM_face_find_nearest_ex(
        vc, &dist, dist_center_p, true, true, use_cycle, &efa_zbuf, bases, bases_len, &base_index);

    if (efa_test && dist_center_p) {
      dist = min_ff(dist_margin, dist_center);
    }
    if (efa_test) {
      hit.f.base_index = base_index;
      hit.f.ele = efa_test;
    }
    if (efa_zbuf) {
      hit.f_zbuf.base_index = base_index;
      hit.f_zbuf.ele = efa_zbuf;
    }
  }

  if ((dist > 0.0f) && (em->selectmode & SCE_SELECT_EDGE)) {
    float dist_center = 0.0f;
    float *dist_center_p = (em->selectmode & SCE_SELECT_VERTEX) ? &dist_center : NULL;

    uint base_index = 0;
    BMEdge *eed_zbuf = NULL;
    BMEdge *eed_test = EDBM_edge_find_nearest_ex(
        vc, &dist, dist_center_p, true, use_cycle, &eed_zbuf, bases, bases_len, &base_index);

    if (eed_test && dist_center_p) {
      dist = min_ff(dist_margin, dist_center);
    }
    if (eed_test) {
      hit.e.base_index = base_index;
      hit.e.ele = eed_test;
    }
    if (eed_zbuf) {
      hit.e_zbuf.base_index = base_index;
      hit.e_zbuf.ele = eed_zbuf;
    }
  }

  if ((dist > 0.0f) && (em->selectmode & SCE_SELECT_VERTEX)) {
    uint base_index = 0;
    BMVert *eve_test = EDBM_vert_find_nearest_ex(
        vc, &dist, true, use_cycle, bases, bases_len, &base_index);

    if (eve_test) {
      hit.v.base_index = base_index;
      hit.v.ele = eve_test;
    }
  }

  /* Return only one of 3 pointers, for front-buffer redraws. */
  if (hit.v.ele) {
    hit.f.ele = NULL;
    hit.e.ele = NULL;
  }
  else if (hit.e.ele) {
    hit.f.ele = NULL;
  }

  /* there may be a face under the cursor, who's center if too far away
   * use this if all else fails, it makes sense to select this */
  if ((hit.v.ele || hit.e.ele || hit.f.ele) == 0) {
    if (hit.e_zbuf.ele) {
      hit.e.base_index = hit.e_zbuf.base_index;
      hit.e.ele = hit.e_zbuf.ele;
    }
    else if (hit.f_zbuf.ele) {
      hit.f.base_index = hit.f_zbuf.base_index;
      hit.f.ele = hit.f_zbuf.ele;
    }
  }

  /* Only one element type will be non-null. */
  BLI_assert(((hit.v.ele != NULL) + (hit.e.ele != NULL) + (hit.f.ele != NULL)) <= 1);

  if (hit.v.ele) {
    *r_base_index = hit.v.base_index;
  }
  if (hit.e.ele) {
    *r_base_index = hit.e.base_index;
  }
  if (hit.f.ele) {
    *r_base_index = hit.f.base_index;
  }

  *r_eve = hit.v.ele;
  *r_eed = hit.e.ele;
  *r_efa = hit.f.ele;

  return (hit.v.ele || hit.e.ele || hit.f.ele);
}

#undef FAKE_SELECT_MODE_BEGIN
#undef FAKE_SELECT_MODE_END

bool EDBM_unified_findnearest(ViewContext *vc,
                              Base **bases,
                              const uint bases_len,
                              int *r_base_index,
                              BMVert **r_eve,
                              BMEdge **r_eed,
                              BMFace **r_efa)
{
  return unified_findnearest(vc, bases, bases_len, r_base_index, r_eve, r_eed, r_efa);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alternate Find Nearest Vert/Edge (optional boundary)
 *
 * \note This uses ray-cast method instead of back-buffer,
 * currently used for poly-build.
 * \{ */

bool EDBM_unified_findnearest_from_raycast(ViewContext *vc,
                                           Base **bases,
                                           const uint bases_len,
                                           bool use_boundary_vertices,
                                           bool use_boundary_edges,
                                           int *r_base_index_vert,
                                           int *r_base_index_edge,
                                           int *r_base_index_face,
                                           struct BMVert **r_eve,
                                           struct BMEdge **r_eed,
                                           struct BMFace **r_efa)
{

  const float mval_fl[2] = {UNPACK2(vc->mval)};
  float ray_origin[3], ray_direction[3];

  struct {
    uint base_index;
    BMElem *ele;
  } best = {0, NULL};
  /* Currently unused, keep since we may want to pick the best. */
  UNUSED_VARS(best);

  struct {
    uint base_index;
    BMElem *ele;
  } best_vert = {0, NULL};

  struct {
    uint base_index;
    BMElem *ele;
  } best_edge = {0, NULL};

  struct {
    uint base_index;
    BMElem *ele;
  } best_face = {0, NULL};

  if (ED_view3d_win_to_ray_clipped(
          vc->depsgraph, vc->region, vc->v3d, mval_fl, ray_origin, ray_direction, true)) {
    float dist_sq_best = FLT_MAX;
    float dist_sq_best_vert = FLT_MAX;
    float dist_sq_best_edge = FLT_MAX;
    float dist_sq_best_face = FLT_MAX;

    const bool use_vert = (r_eve != NULL);
    const bool use_edge = (r_eed != NULL);
    const bool use_face = (r_efa != NULL);

    for (uint base_index = 0; base_index < bases_len; base_index++) {
      Base *base_iter = bases[base_index];
      Object *obedit = base_iter->object;

      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      BMesh *bm = em->bm;
      float imat3[3][3];

      ED_view3d_viewcontext_init_object(vc, obedit);
      copy_m3_m4(imat3, obedit->obmat);
      invert_m3(imat3);

      const float(*coords)[3] = NULL;
      {
        Mesh *me_eval = (Mesh *)DEG_get_evaluated_id(vc->depsgraph, obedit->data);
        if (me_eval->runtime.edit_data) {
          coords = me_eval->runtime.edit_data->vertexCos;
        }
      }

      if (coords != NULL) {
        BM_mesh_elem_index_ensure(bm, BM_VERT);
      }

      if ((use_boundary_vertices || use_boundary_edges) && (use_vert || use_edge)) {
        BMEdge *e;
        BMIter eiter;
        BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
          if ((BM_elem_flag_test(e, BM_ELEM_HIDDEN) == false) && (BM_edge_is_boundary(e))) {
            if (use_vert && use_boundary_vertices) {
              for (uint j = 0; j < 2; j++) {
                BMVert *v = *((&e->v1) + j);
                float point[3];
                mul_v3_m4v3(point, obedit->obmat, coords ? coords[BM_elem_index_get(v)] : v->co);
                const float dist_sq_test = dist_squared_to_ray_v3_normalized(
                    ray_origin, ray_direction, point);
                if (dist_sq_test < dist_sq_best_vert) {
                  dist_sq_best_vert = dist_sq_test;
                  best_vert.base_index = base_index;
                  best_vert.ele = (BMElem *)v;
                }
                if (dist_sq_test < dist_sq_best) {
                  dist_sq_best = dist_sq_test;
                  best.base_index = base_index;
                  best.ele = (BMElem *)v;
                }
              }
            }

            if (use_edge && use_boundary_edges) {
              float point[3];
#if 0
              const float dist_sq_test = dist_squared_ray_to_seg_v3(
                  ray_origin, ray_direction, e->v1->co, e->v2->co, point, &depth);
#else
              if (coords) {
                mid_v3_v3v3(
                    point, coords[BM_elem_index_get(e->v1)], coords[BM_elem_index_get(e->v2)]);
              }
              else {
                mid_v3_v3v3(point, e->v1->co, e->v2->co);
              }
              mul_m4_v3(obedit->obmat, point);
              const float dist_sq_test = dist_squared_to_ray_v3_normalized(
                  ray_origin, ray_direction, point);
              if (dist_sq_test < dist_sq_best_edge) {
                dist_sq_best_edge = dist_sq_test;
                best_edge.base_index = base_index;
                best_edge.ele = (BMElem *)e;
              }
              if (dist_sq_test < dist_sq_best) {
                dist_sq_best = dist_sq_test;
                best.base_index = base_index;
                best.ele = (BMElem *)e;
              }
#endif
            }
          }
        }
      }
      /* Non boundary case. */
      if (use_vert && !use_boundary_vertices) {
        BMVert *v;
        BMIter viter;
        BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
          if (BM_elem_flag_test(v, BM_ELEM_HIDDEN) == false) {
            float point[3];
            mul_v3_m4v3(point, obedit->obmat, coords ? coords[BM_elem_index_get(v)] : v->co);
            const float dist_sq_test = dist_squared_to_ray_v3_normalized(
                ray_origin, ray_direction, point);
            if (dist_sq_test < dist_sq_best_vert) {
              dist_sq_best_vert = dist_sq_test;
              best_vert.base_index = base_index;
              best_vert.ele = (BMElem *)v;
            }
            if (dist_sq_test < dist_sq_best) {
              dist_sq_best = dist_sq_test;
              best.base_index = base_index;
              best.ele = (BMElem *)v;
            }
          }
        }
      }

      if (use_edge && !use_boundary_edges) {
        BMEdge *e;
        BMIter eiter;
        BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
          if (BM_elem_flag_test(e, BM_ELEM_HIDDEN) == false) {
            float point[3];
            if (coords) {
              mid_v3_v3v3(
                  point, coords[BM_elem_index_get(e->v1)], coords[BM_elem_index_get(e->v2)]);
            }
            else {
              mid_v3_v3v3(point, e->v1->co, e->v2->co);
            }
            mul_m4_v3(obedit->obmat, point);
            const float dist_sq_test = dist_squared_to_ray_v3_normalized(
                ray_origin, ray_direction, point);
            if (dist_sq_test < dist_sq_best_edge) {
              dist_sq_best_edge = dist_sq_test;
              best_edge.base_index = base_index;
              best_edge.ele = (BMElem *)e;
            }
            if (dist_sq_test < dist_sq_best) {
              dist_sq_best = dist_sq_test;
              best.base_index = base_index;
              best.ele = (BMElem *)e;
            }
          }
        }
      }

      if (use_face) {
        BMFace *f;
        BMIter fiter;
        BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
          if (BM_elem_flag_test(f, BM_ELEM_HIDDEN) == false) {
            float point[3];
            if (coords) {
              BM_face_calc_center_median_vcos(bm, f, point, coords);
            }
            else {
              BM_face_calc_center_median(f, point);
            }
            mul_m4_v3(obedit->obmat, point);
            const float dist_sq_test = dist_squared_to_ray_v3_normalized(
                ray_origin, ray_direction, point);
            if (dist_sq_test < dist_sq_best_face) {
              dist_sq_best_face = dist_sq_test;
              best_face.base_index = base_index;
              best_face.ele = (BMElem *)f;
            }
            if (dist_sq_test < dist_sq_best) {
              dist_sq_best = dist_sq_test;
              best.base_index = base_index;
              best.ele = (BMElem *)f;
            }
          }
        }
      }
    }
  }

  *r_base_index_vert = best_vert.base_index;
  *r_base_index_edge = best_edge.base_index;
  *r_base_index_face = best_face.base_index;

  if (r_eve) {
    *r_eve = NULL;
  }
  if (r_eed) {
    *r_eed = NULL;
  }
  if (r_efa) {
    *r_efa = NULL;
  }

  if (best_vert.ele) {
    *r_eve = (BMVert *)best_vert.ele;
  }
  if (best_edge.ele) {
    *r_eed = (BMEdge *)best_edge.ele;
  }
  if (best_face.ele) {
    *r_efa = (BMFace *)best_face.ele;
  }

  return (best_vert.ele != NULL || best_edge.ele != NULL || best_face.ele != NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Similar Region Operator
 * \{ */

static int edbm_select_similar_region_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMesh *bm = em->bm;
  bool changed = false;

  /* group vars */
  int *groups_array;
  int(*group_index)[2];
  int group_tot;
  int i;

  if (bm->totfacesel < 2) {
    BKE_report(op->reports, RPT_ERROR, "No face regions selected");
    return OPERATOR_CANCELLED;
  }

  groups_array = MEM_mallocN(sizeof(*groups_array) * bm->totfacesel, __func__);
  group_tot = BM_mesh_calc_face_groups(
      bm, groups_array, &group_index, NULL, NULL, NULL, BM_ELEM_SELECT, BM_VERT);

  BM_mesh_elem_table_ensure(bm, BM_FACE);

  for (i = 0; i < group_tot; i++) {
    ListBase faces_regions;
    int tot;

    const int fg_sta = group_index[i][0];
    const int fg_len = group_index[i][1];
    int j;
    BMFace **fg = MEM_mallocN(sizeof(*fg) * fg_len, __func__);

    for (j = 0; j < fg_len; j++) {
      fg[j] = BM_face_at_index(bm, groups_array[fg_sta + j]);
    }

    tot = BM_mesh_region_match(bm, fg, fg_len, &faces_regions);

    MEM_freeN(fg);

    if (tot) {
      LinkData *link;
      while ((link = BLI_pophead(&faces_regions))) {
        BMFace *f, **faces = link->data;
        while ((f = *(faces++))) {
          BM_face_select_set(bm, f, true);
        }
        MEM_freeN(link->data);
        MEM_freeN(link);

        changed = true;
      }
    }
  }

  MEM_freeN(groups_array);
  MEM_freeN(group_index);

  if (changed) {
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  else {
    BKE_report(op->reports, RPT_WARNING, "No matching face regions found");
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_select_similar_region(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Similar Regions";
  ot->idname = "MESH_OT_select_similar_region";
  ot->description = "Select similar face regions to the current selection";

  /* api callbacks */
  ot->exec = edbm_select_similar_region_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mode Vert/Edge/Face Operator
 * \{ */

static int edbm_select_mode_exec(bContext *C, wmOperator *op)
{
  const int type = RNA_enum_get(op->ptr, "type");
  const int action = RNA_enum_get(op->ptr, "action");
  const bool use_extend = RNA_boolean_get(op->ptr, "use_extend");
  const bool use_expand = RNA_boolean_get(op->ptr, "use_expand");

  if (EDBM_selectmode_toggle_multi(C, type, action, use_extend, use_expand)) {
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

static int edbm_select_mode_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* Bypass when in UV non sync-select mode, fall through to keymap that edits. */
  if (CTX_wm_space_image(C)) {
    ToolSettings *ts = CTX_data_tool_settings(C);
    if ((ts->uv_flag & UV_SYNC_SELECTION) == 0) {
      return OPERATOR_PASS_THROUGH;
    }
    /* Bypass when no action is needed. */
    if (!RNA_struct_property_is_set(op->ptr, "type")) {
      return OPERATOR_CANCELLED;
    }
  }

  /* detecting these options based on shift/ctrl here is weak, but it's done
   * to make this work when clicking buttons or menus */
  if (!RNA_struct_property_is_set(op->ptr, "use_extend")) {
    RNA_boolean_set(op->ptr, "use_extend", event->modifier & KM_SHIFT);
  }
  if (!RNA_struct_property_is_set(op->ptr, "use_expand")) {
    RNA_boolean_set(op->ptr, "use_expand", event->modifier & KM_CTRL);
  }

  return edbm_select_mode_exec(C, op);
}

static char *edbm_select_mode_get_description(struct bContext *UNUSED(C),
                                              struct wmOperatorType *UNUSED(op),
                                              struct PointerRNA *values)
{
  const int type = RNA_enum_get(values, "type");

  /* Because the special behavior for shift and ctrl click depend on user input, they may be
   * incorrect if the operator is used from a script or from a special button. So only return the
   * specialized descriptions if only the "type" is set, which conveys that the operator is meant
   * to be used with the logic in the `invoke` method. */
  if (RNA_struct_property_is_set(values, "type") &&
      !RNA_struct_property_is_set(values, "use_extend") &&
      !RNA_struct_property_is_set(values, "use_expand") &&
      !RNA_struct_property_is_set(values, "action")) {
    switch (type) {
      case SCE_SELECT_VERTEX:
        return BLI_strdup(TIP_(
            "Vertex select - Shift-Click for multiple modes, Ctrl-Click contracts selection"));
      case SCE_SELECT_EDGE:
        return BLI_strdup(
            TIP_("Edge select - Shift-Click for multiple modes, "
                 "Ctrl-Click expands/contracts selection depending on the current mode"));
      case SCE_SELECT_FACE:
        return BLI_strdup(
            TIP_("Face select - Shift-Click for multiple modes, Ctrl-Click expands selection"));
    }
  }

  return NULL;
}

void MESH_OT_select_mode(wmOperatorType *ot)
{
  PropertyRNA *prop;

  static const EnumPropertyItem actions_items[] = {
      {0, "DISABLE", 0, "Disable", "Disable selected markers"},
      {1, "ENABLE", 0, "Enable", "Enable selected markers"},
      {2, "TOGGLE", 0, "Toggle", "Toggle disabled flag for selected markers"},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Select Mode";
  ot->idname = "MESH_OT_select_mode";
  ot->description = "Change selection mode";

  /* api callbacks */
  ot->invoke = edbm_select_mode_invoke;
  ot->exec = edbm_select_mode_exec;
  ot->poll = ED_operator_editmesh;
  ot->get_description = edbm_select_mode_get_description;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  /* Hide all, not to show redo panel. */
  prop = RNA_def_boolean(ot->srna, "use_extend", false, "Extend", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "use_expand", false, "Expand", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  ot->prop = prop = RNA_def_enum(ot->srna, "type", rna_enum_mesh_select_mode_items, 0, "Type", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_enum(
      ot->srna, "action", actions_items, 2, "Action", "Selection action to execute");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Loop (Non Modal) Operator
 * \{ */

static void walker_select_count(BMEditMesh *em,
                                int walkercode,
                                void *start,
                                int r_count_by_select[2])
{
  BMesh *bm = em->bm;
  BMElem *ele;
  BMWalker walker;

  r_count_by_select[0] = r_count_by_select[1] = 0;

  BMW_init(&walker,
           bm,
           walkercode,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_FLAG_TEST_HIDDEN,
           BMW_NIL_LAY);

  for (ele = BMW_begin(&walker, start); ele; ele = BMW_step(&walker)) {
    r_count_by_select[BM_elem_flag_test(ele, BM_ELEM_SELECT) ? 1 : 0] += 1;

    /* Early exit when mixed (could be optional if needed. */
    if (r_count_by_select[0] && r_count_by_select[1]) {
      r_count_by_select[0] = r_count_by_select[1] = -1;
      break;
    }
  }

  BMW_end(&walker);
}

static void walker_select(BMEditMesh *em, int walkercode, void *start, const bool select)
{
  BMesh *bm = em->bm;
  BMElem *ele;
  BMWalker walker;

  BMW_init(&walker,
           bm,
           walkercode,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_FLAG_TEST_HIDDEN,
           BMW_NIL_LAY);

  for (ele = BMW_begin(&walker, start); ele; ele = BMW_step(&walker)) {
    if (!select) {
      BM_select_history_remove(bm, ele);
    }
    BM_elem_select_set(bm, ele, select);
  }
  BMW_end(&walker);
}

static int edbm_loop_multiselect_exec(bContext *C, wmOperator *op)
{
  const bool is_ring = RNA_boolean_get(op->ptr, "ring");
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    BMEdge *eed;
    BMEdge **edarray;
    int edindex;
    BMIter iter;
    int totedgesel = 0;

    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
        totedgesel++;
      }
    }

    edarray = MEM_mallocN(sizeof(BMEdge *) * totedgesel, "edge array");
    edindex = 0;

    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
        edarray[edindex] = eed;
        edindex++;
      }
    }

    if (is_ring) {
      for (edindex = 0; edindex < totedgesel; edindex += 1) {
        eed = edarray[edindex];
        walker_select(em, BMW_EDGERING, eed, true);
      }
      EDBM_selectmode_flush(em);
    }
    else {
      for (edindex = 0; edindex < totedgesel; edindex += 1) {
        eed = edarray[edindex];
        bool non_manifold = BM_edge_face_count_is_over(eed, 2);
        if (non_manifold) {
          walker_select(em, BMW_EDGELOOP_NONMANIFOLD, eed, true);
        }
        else {
          walker_select(em, BMW_EDGELOOP, eed, true);
        }
      }
      EDBM_selectmode_flush(em);
    }
    MEM_freeN(edarray);
    //  if (EM_texFaceCheck())

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_loop_multi_select(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Multi Select Loops";
  ot->idname = "MESH_OT_loop_multi_select";
  ot->description = "Select a loop of connected edges by connection type";

  /* api callbacks */
  ot->exec = edbm_loop_multiselect_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "ring", 0, "Ring", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Loop (Cursor Pick) Operator
 * \{ */

static void mouse_mesh_loop_face(BMEditMesh *em, BMEdge *eed, bool select, bool select_clear)
{
  if (select_clear) {
    EDBM_flag_disable_all(em, BM_ELEM_SELECT);
  }

  walker_select(em, BMW_FACELOOP, eed, select);
}

static void mouse_mesh_loop_edge_ring(BMEditMesh *em, BMEdge *eed, bool select, bool select_clear)
{
  if (select_clear) {
    EDBM_flag_disable_all(em, BM_ELEM_SELECT);
  }

  walker_select(em, BMW_EDGERING, eed, select);
}

static void mouse_mesh_loop_edge(
    BMEditMesh *em, BMEdge *eed, bool select, bool select_clear, bool select_cycle)
{
  bool edge_boundary = false;
  bool non_manifold = BM_edge_face_count_is_over(eed, 2);

  /* Cycle between BMW_EDGELOOP / BMW_EDGEBOUNDARY. */
  if (select_cycle && BM_edge_is_boundary(eed)) {
    int count_by_select[2];

    /* If the loops selected toggle the boundaries. */
    walker_select_count(em, BMW_EDGELOOP, eed, count_by_select);
    if (count_by_select[!select] == 0) {
      edge_boundary = true;

      /* If the boundaries selected, toggle back to the loop. */
      walker_select_count(em, BMW_EDGEBOUNDARY, eed, count_by_select);
      if (count_by_select[!select] == 0) {
        edge_boundary = false;
      }
    }
  }

  if (select_clear) {
    EDBM_flag_disable_all(em, BM_ELEM_SELECT);
  }

  if (edge_boundary) {
    walker_select(em, BMW_EDGEBOUNDARY, eed, select);
  }
  else if (non_manifold) {
    walker_select(em, BMW_EDGELOOP_NONMANIFOLD, eed, select);
  }
  else {
    walker_select(em, BMW_EDGELOOP, eed, select);
  }
}

static bool mouse_mesh_loop(
    bContext *C, const int mval[2], bool extend, bool deselect, bool toggle, bool ring)
{
  Base *basact = NULL;
  BMVert *eve = NULL;
  BMEdge *eed = NULL;
  BMFace *efa = NULL;

  ViewContext vc;
  BMEditMesh *em;
  bool select = true;
  bool select_clear = false;
  bool select_cycle = true;
  float mvalf[2];

  em_setup_viewcontext(C, &vc);
  mvalf[0] = (float)(vc.mval[0] = mval[0]);
  mvalf[1] = (float)(vc.mval[1] = mval[1]);

  BMEditMesh *em_original = vc.em;
  const short selectmode = em_original->selectmode;
  em_original->selectmode = SCE_SELECT_EDGE;

  uint bases_len;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode(vc.view_layer, vc.v3d, &bases_len);

  {
    int base_index = -1;
    if (EDBM_unified_findnearest(&vc, bases, bases_len, &base_index, &eve, &eed, &efa)) {
      basact = bases[base_index];
      ED_view3d_viewcontext_init_object(&vc, basact->object);
      em = vc.em;
    }
    else {
      em = NULL;
    }
  }

  em_original->selectmode = selectmode;

  if (em == NULL || eed == NULL) {
    MEM_freeN(bases);
    return false;
  }

  if (extend == false && deselect == false && toggle == false) {
    select_clear = true;
  }

  if (extend) {
    select = true;
  }
  else if (deselect) {
    select = false;
  }
  else if (select_clear || (BM_elem_flag_test(eed, BM_ELEM_SELECT) == 0)) {
    select = true;
  }
  else if (toggle) {
    select = false;
    select_cycle = false;
  }

  if (select_clear) {
    for (uint base_index = 0; base_index < bases_len; base_index++) {
      Base *base_iter = bases[base_index];
      Object *ob_iter = base_iter->object;
      BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);

      if (em_iter->bm->totvertsel == 0) {
        continue;
      }

      if (em_iter == em) {
        continue;
      }

      EDBM_flag_disable_all(em_iter, BM_ELEM_SELECT);
      DEG_id_tag_update(ob_iter->data, ID_RECALC_SELECT);
    }
  }

  if (em->selectmode & SCE_SELECT_FACE) {
    mouse_mesh_loop_face(em, eed, select, select_clear);
  }
  else {
    if (ring) {
      mouse_mesh_loop_edge_ring(em, eed, select, select_clear);
    }
    else {
      mouse_mesh_loop_edge(em, eed, select, select_clear, select_cycle);
    }
  }

  EDBM_selectmode_flush(em);

  /* sets as active, useful for other tools */
  if (select) {
    if (em->selectmode & SCE_SELECT_VERTEX) {
      /* Find nearest vert from mouse
       * (initialize to large values in case only one vertex can be projected) */
      float v1_co[2], v2_co[2];
      float length_1 = FLT_MAX;
      float length_2 = FLT_MAX;

      /* We can't be sure this has already been set... */
      ED_view3d_init_mats_rv3d(vc.obedit, vc.rv3d);

      if (ED_view3d_project_float_object(vc.region, eed->v1->co, v1_co, V3D_PROJ_TEST_CLIP_NEAR) ==
          V3D_PROJ_RET_OK) {
        length_1 = len_squared_v2v2(mvalf, v1_co);
      }

      if (ED_view3d_project_float_object(vc.region, eed->v2->co, v2_co, V3D_PROJ_TEST_CLIP_NEAR) ==
          V3D_PROJ_RET_OK) {
        length_2 = len_squared_v2v2(mvalf, v2_co);
      }
#if 0
      printf("mouse to v1: %f\nmouse to v2: %f\n",
             len_squared_v2v2(mvalf, v1_co),
             len_squared_v2v2(mvalf, v2_co));
#endif
      BM_select_history_store(em->bm, (length_1 < length_2) ? eed->v1 : eed->v2);
    }
    else if (em->selectmode & SCE_SELECT_EDGE) {
      BM_select_history_store(em->bm, eed);
    }
    else if (em->selectmode & SCE_SELECT_FACE) {
      /* Select the face of eed which is the nearest of mouse. */
      BMFace *f;
      BMIter iterf;
      float best_dist = FLT_MAX;
      efa = NULL;

      /* We can't be sure this has already been set... */
      ED_view3d_init_mats_rv3d(vc.obedit, vc.rv3d);

      BM_ITER_ELEM (f, &iterf, eed, BM_FACES_OF_EDGE) {
        if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
          float cent[3];
          float co[2], tdist;

          BM_face_calc_center_median(f, cent);
          if (ED_view3d_project_float_object(vc.region, cent, co, V3D_PROJ_TEST_CLIP_NEAR) ==
              V3D_PROJ_RET_OK) {
            tdist = len_squared_v2v2(mvalf, co);
            if (tdist < best_dist) {
              // printf("Best face: %p (%f)\n", f, tdist);
              best_dist = tdist;
              efa = f;
            }
          }
        }
      }
      if (efa) {
        BM_mesh_active_face_set(em->bm, efa);
        BM_select_history_store(em->bm, efa);
      }
    }
  }

  MEM_freeN(bases);

  DEG_id_tag_update(vc.obedit->data, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, vc.obedit->data);

  return true;
}

static int edbm_select_loop_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{

  view3d_operator_needs_opengl(C);

  if (mouse_mesh_loop(C,
                      event->mval,
                      RNA_boolean_get(op->ptr, "extend"),
                      RNA_boolean_get(op->ptr, "deselect"),
                      RNA_boolean_get(op->ptr, "toggle"),
                      RNA_boolean_get(op->ptr, "ring"))) {
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MESH_OT_loop_select(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Loop Select";
  ot->idname = "MESH_OT_loop_select";
  ot->description = "Select a loop of connected edges";

  /* api callbacks */
  ot->invoke = edbm_select_loop_invoke;
  ot->poll = ED_operator_editmesh_region_view3d;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  PropertyRNA *prop;

  prop = RNA_def_boolean(ot->srna, "extend", 0, "Extend Select", "Extend the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "deselect", 0, "Deselect", "Remove from the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "toggle", 0, "Toggle Select", "Toggle the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "ring", 0, "Select Ring", "Select ring");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

void MESH_OT_edgering_select(wmOperatorType *ot)
{
  /* description */
  ot->name = "Edge Ring Select";
  ot->idname = "MESH_OT_edgering_select";
  ot->description = "Select an edge ring";

  /* callbacks */
  ot->invoke = edbm_select_loop_invoke;
  ot->poll = ED_operator_editmesh_region_view3d;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* Properties. */
  PropertyRNA *prop;
  prop = RNA_def_boolean(ot->srna, "extend", 0, "Extend", "Extend the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "deselect", 0, "Deselect", "Remove from the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "toggle", 0, "Toggle Select", "Toggle the selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "ring", 1, "Select Ring", "Select ring");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name (De)Select All Operator
 * \{ */

static int edbm_select_all_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int action = RNA_enum_get(op->ptr, "action");

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;
    for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
      Object *obedit = objects[ob_index];
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      if (em->bm->totvertsel || em->bm->totedgesel || em->bm->totfacesel) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    switch (action) {
      case SEL_SELECT:
        EDBM_flag_enable_all(em, BM_ELEM_SELECT);
        break;
      case SEL_DESELECT:
        EDBM_flag_disable_all(em, BM_ELEM_SELECT);
        break;
      case SEL_INVERT:
        EDBM_select_swap(em);
        EDBM_selectmode_flush(em);
        break;
    }
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select All";
  ot->idname = "MESH_OT_select_all";
  ot->description = "(De)select all vertices, edges or faces";

  /* api callbacks */
  ot->exec = edbm_select_all_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Interior Faces Operator
 * \{ */

static int edbm_faces_select_interior_exec(bContext *C, wmOperator *UNUSED(op))
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (!EDBM_select_interior_faces(em)) {
      continue;
    }

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_interior_faces(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Interior Faces";
  ot->idname = "MESH_OT_select_interior_faces";
  ot->description = "Select faces where all edges have more than 2 face users";

  /* api callbacks */
  ot->exec = edbm_faces_select_interior_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Picking API
 *
 * Here actual select happens,
 * Gets called via generic mouse select operator.
 * \{ */

bool EDBM_select_pick(bContext *C, const int mval[2], const struct SelectPick_Params *params)
{
  ViewContext vc;

  int base_index_active = -1;
  BMVert *eve = NULL;
  BMEdge *eed = NULL;
  BMFace *efa = NULL;

  /* setup view context for argument to callbacks */
  em_setup_viewcontext(C, &vc);
  vc.mval[0] = mval[0];
  vc.mval[1] = mval[1];

  uint bases_len = 0;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode(vc.view_layer, vc.v3d, &bases_len);

  bool changed = false;
  bool found = unified_findnearest(&vc, bases, bases_len, &base_index_active, &eve, &eed, &efa);

  if (params->sel_op == SEL_OP_SET) {
    BMElem *ele = efa ? (BMElem *)efa : (eed ? (BMElem *)eed : (BMElem *)eve);
    if ((found && params->select_passthrough) && BM_elem_flag_test(ele, BM_ELEM_SELECT)) {
      found = false;
    }
    else if (found || params->deselect_all) {
      /* Deselect everything. */
      for (uint base_index = 0; base_index < bases_len; base_index++) {
        Base *base_iter = bases[base_index];
        Object *ob_iter = base_iter->object;
        EDBM_flag_disable_all(BKE_editmesh_from_object(ob_iter), BM_ELEM_SELECT);
        DEG_id_tag_update(ob_iter->data, ID_RECALC_SELECT);
        WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob_iter->data);
      }
      changed = true;
    }
  }

  if (found) {
    Base *basact = bases[base_index_active];
    ED_view3d_viewcontext_init_object(&vc, basact->object);

    if (efa) {
      switch (params->sel_op) {
        case SEL_OP_ADD: {
          BM_mesh_active_face_set(vc.em->bm, efa);

          /* Work-around: deselect first, so we can guarantee it will
           * be active even if it was already selected. */
          BM_select_history_remove(vc.em->bm, efa);
          BM_face_select_set(vc.em->bm, efa, false);
          BM_select_history_store(vc.em->bm, efa);
          BM_face_select_set(vc.em->bm, efa, true);
          break;
        }
        case SEL_OP_SUB: {
          BM_select_history_remove(vc.em->bm, efa);
          BM_face_select_set(vc.em->bm, efa, false);
          break;
        }
        case SEL_OP_XOR: {
          BM_mesh_active_face_set(vc.em->bm, efa);
          if (!BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, efa);
            BM_face_select_set(vc.em->bm, efa, true);
          }
          else {
            BM_select_history_remove(vc.em->bm, efa);
            BM_face_select_set(vc.em->bm, efa, false);
          }
          break;
        }
        case SEL_OP_SET: {
          BM_mesh_active_face_set(vc.em->bm, efa);
          if (!BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, efa);
            BM_face_select_set(vc.em->bm, efa, true);
          }
          break;
        }
        case SEL_OP_AND: {
          BLI_assert_unreachable(); /* Doesn't make sense for picking. */
          break;
        }
      }
    }
    else if (eed) {

      switch (params->sel_op) {
        case SEL_OP_ADD: {
          /* Work-around: deselect first, so we can guarantee it will
           * be active even if it was already selected. */
          BM_select_history_remove(vc.em->bm, eed);
          BM_edge_select_set(vc.em->bm, eed, false);
          BM_select_history_store(vc.em->bm, eed);
          BM_edge_select_set(vc.em->bm, eed, true);
          break;
        }
        case SEL_OP_SUB: {
          BM_select_history_remove(vc.em->bm, eed);
          BM_edge_select_set(vc.em->bm, eed, false);
          break;
        }
        case SEL_OP_XOR: {
          if (!BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, eed);
            BM_edge_select_set(vc.em->bm, eed, true);
          }
          else {
            BM_select_history_remove(vc.em->bm, eed);
            BM_edge_select_set(vc.em->bm, eed, false);
          }
          break;
        }
        case SEL_OP_SET: {
          if (!BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, eed);
            BM_edge_select_set(vc.em->bm, eed, true);
          }
          break;
        }
        case SEL_OP_AND: {
          BLI_assert_unreachable(); /* Doesn't make sense for picking. */
          break;
        }
      }
    }
    else if (eve) {
      switch (params->sel_op) {
        case SEL_OP_ADD: {
          /* Work-around: deselect first, so we can guarantee it will
           * be active even if it was already selected. */
          BM_select_history_remove(vc.em->bm, eve);
          BM_vert_select_set(vc.em->bm, eve, false);
          BM_select_history_store(vc.em->bm, eve);
          BM_vert_select_set(vc.em->bm, eve, true);
          break;
        }
        case SEL_OP_SUB: {
          BM_select_history_remove(vc.em->bm, eve);
          BM_vert_select_set(vc.em->bm, eve, false);
          break;
        }
        case SEL_OP_XOR: {
          if (!BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, eve);
            BM_vert_select_set(vc.em->bm, eve, true);
          }
          else {
            BM_select_history_remove(vc.em->bm, eve);
            BM_vert_select_set(vc.em->bm, eve, false);
          }
          break;
        }
        case SEL_OP_SET: {
          if (!BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
            BM_select_history_store(vc.em->bm, eve);
            BM_vert_select_set(vc.em->bm, eve, true);
          }
          break;
        }
        case SEL_OP_AND: {
          BLI_assert_unreachable(); /* Doesn't make sense for picking. */
          break;
        }
      }
    }

    EDBM_selectmode_flush(vc.em);

    if (efa) {
      /* Change active material on object. */
      if (efa->mat_nr != vc.obedit->actcol - 1) {
        vc.obedit->actcol = efa->mat_nr + 1;
        vc.em->mat_nr = efa->mat_nr;
        WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING_LINKS, NULL);
      }

      /* Change active face-map on object. */
      if (!BLI_listbase_is_empty(&vc.obedit->fmaps)) {
        const int cd_fmap_offset = CustomData_get_offset(&vc.em->bm->pdata, CD_FACEMAP);
        if (cd_fmap_offset != -1) {
          int map = *((int *)BM_ELEM_CD_GET_VOID_P(efa, cd_fmap_offset));
          if ((map < -1) || (map > BLI_listbase_count_at_most(&vc.obedit->fmaps, map))) {
            map = -1;
          }
          map += 1;
          if (map != vc.obedit->actfmap) {
            /* We may want to add notifiers later,
             * currently select update handles redraw. */
            vc.obedit->actfmap = map;
          }
        }
      }
    }

    /* Changing active object is handy since it allows us to
     * switch UV layers, vgroups for eg. */
    if (vc.view_layer->basact != basact) {
      ED_object_base_activate(C, basact);
    }

    DEG_id_tag_update(vc.obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, vc.obedit->data);

    changed = true;
  }

  MEM_freeN(bases);

  return changed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mode Utilities
 * \{ */

static void edbm_strip_selections(BMEditMesh *em)
{
  BMEditSelection *ese, *nextese;

  if (!(em->selectmode & SCE_SELECT_VERTEX)) {
    ese = em->bm->selected.first;
    while (ese) {
      nextese = ese->next;
      if (ese->htype == BM_VERT) {
        BLI_freelinkN(&(em->bm->selected), ese);
      }
      ese = nextese;
    }
  }
  if (!(em->selectmode & SCE_SELECT_EDGE)) {
    ese = em->bm->selected.first;
    while (ese) {
      nextese = ese->next;
      if (ese->htype == BM_EDGE) {
        BLI_freelinkN(&(em->bm->selected), ese);
      }
      ese = nextese;
    }
  }
  if (!(em->selectmode & SCE_SELECT_FACE)) {
    ese = em->bm->selected.first;
    while (ese) {
      nextese = ese->next;
      if (ese->htype == BM_FACE) {
        BLI_freelinkN(&(em->bm->selected), ese);
      }
      ese = nextese;
    }
  }
}

void EDBM_selectmode_set(BMEditMesh *em)
{
  BMVert *eve;
  BMEdge *eed;
  BMFace *efa;
  BMIter iter;

  em->bm->selectmode = em->selectmode;

  /* strip BMEditSelections from em->selected that are not relevant to new mode */
  edbm_strip_selections(em);

  if (em->bm->totvertsel == 0 && em->bm->totedgesel == 0 && em->bm->totfacesel == 0) {
    return;
  }

  if (em->selectmode & SCE_SELECT_VERTEX) {
    if (em->bm->totvertsel) {
      EDBM_select_flush(em);
    }
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    /* deselect vertices, and select again based on edge select */
    BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
      BM_vert_select_set(em->bm, eve, false);
    }

    if (em->bm->totedgesel) {
      BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
          BM_edge_select_set(em->bm, eed, true);
        }
      }

      /* selects faces based on edge status */
      EDBM_selectmode_flush(em);
    }
  }
  else if (em->selectmode & SCE_SELECT_FACE) {
    /* Deselect edges, and select again based on face select. */
    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      BM_edge_select_set(em->bm, eed, false);
    }

    if (em->bm->totfacesel) {
      BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
          BM_face_select_set(em->bm, efa, true);
        }
      }
    }
  }
}

void EDBM_selectmode_convert(BMEditMesh *em,
                             const short selectmode_old,
                             const short selectmode_new)
{
  BMesh *bm = em->bm;

  BMVert *eve;
  BMEdge *eed;
  BMFace *efa;
  BMIter iter;

  /* first tag-to-select, then select --- this avoids a feedback loop */

  /* Have to find out what the selection-mode was previously. */
  if (selectmode_old == SCE_SELECT_VERTEX) {
    if (bm->totvertsel == 0) {
      /* pass */
    }
    else if (selectmode_new == SCE_SELECT_EDGE) {
      /* flush up (vert -> edge) */

      /* select all edges associated with every selected vert */
      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        BM_elem_flag_set(eed, BM_ELEM_TAG, BM_edge_is_any_vert_flag_test(eed, BM_ELEM_SELECT));
      }

      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        if (BM_elem_flag_test(eed, BM_ELEM_TAG)) {
          BM_edge_select_set(bm, eed, true);
        }
      }
    }
    else if (selectmode_new == SCE_SELECT_FACE) {
      /* flush up (vert -> face) */

      /* select all faces associated with every selected vert */
      BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
        BM_elem_flag_set(efa, BM_ELEM_TAG, BM_face_is_any_vert_flag_test(efa, BM_ELEM_SELECT));
      }

      BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(efa, BM_ELEM_TAG)) {
          BM_face_select_set(bm, efa, true);
        }
      }
    }
  }
  else if (selectmode_old == SCE_SELECT_EDGE) {
    if (bm->totedgesel == 0) {
      /* pass */
    }
    else if (selectmode_new == SCE_SELECT_FACE) {
      /* flush up (edge -> face) */

      /* select all faces associated with every selected edge */
      BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
        BM_elem_flag_set(efa, BM_ELEM_TAG, BM_face_is_any_edge_flag_test(efa, BM_ELEM_SELECT));
      }

      BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(efa, BM_ELEM_TAG)) {
          BM_face_select_set(bm, efa, true);
        }
      }
    }
    else if (selectmode_new == SCE_SELECT_VERTEX) {
      /* flush down (edge -> vert) */

      BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
        if (!BM_vert_is_all_edge_flag_test(eve, BM_ELEM_SELECT, true)) {
          BM_vert_select_set(bm, eve, false);
        }
      }
      /* deselect edges without both verts selected */
      BM_mesh_deselect_flush(bm);
    }
  }
  else if (selectmode_old == SCE_SELECT_FACE) {
    if (bm->totfacesel == 0) {
      /* pass */
    }
    else if (selectmode_new == SCE_SELECT_EDGE) {
      /* flush down (face -> edge) */

      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        if (!BM_edge_is_all_face_flag_test(eed, BM_ELEM_SELECT, true)) {
          BM_edge_select_set(bm, eed, false);
        }
      }
      /* Deselect faces without edges selected. */
      BM_mesh_deselect_flush(bm);
    }
    else if (selectmode_new == SCE_SELECT_VERTEX) {
      /* flush down (face -> vert) */

      BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
        if (!BM_vert_is_all_face_flag_test(eve, BM_ELEM_SELECT, true)) {
          BM_vert_select_set(bm, eve, false);
        }
      }
      /* deselect faces without verts selected */
      BM_mesh_deselect_flush(bm);
    }
  }
}

bool EDBM_selectmode_toggle_multi(bContext *C,
                                  const short selectmode_new,
                                  const int action,
                                  const bool use_extend,
                                  const bool use_expand)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  ToolSettings *ts = CTX_data_tool_settings(C);
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = NULL;
  bool ret = false;

  if (obedit && obedit->type == OB_MESH) {
    em = BKE_editmesh_from_object(obedit);
  }

  if (em == NULL) {
    return ret;
  }

  bool only_update = false;
  switch (action) {
    case -1:
      /* already set */
      break;
    case 0: /* disable */
      /* check we have something to do */
      if ((em->selectmode & selectmode_new) == 0) {
        only_update = true;
        break;
      }
      em->selectmode &= ~selectmode_new;
      break;
    case 1: /* enable */
      /* check we have something to do */
      if ((em->selectmode & selectmode_new) != 0) {
        only_update = true;
        break;
      }
      em->selectmode |= selectmode_new;
      break;
    case 2: /* toggle */
      /* can't disable this flag if its the only one set */
      if (em->selectmode == selectmode_new) {
        only_update = true;
        break;
      }
      em->selectmode ^= selectmode_new;
      break;
    default:
      BLI_assert(0);
      break;
  }

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob_iter = objects[ob_index];
    BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);
    if (em_iter != em) {
      em_iter->selectmode = em->selectmode;
    }
  }

  if (only_update) {
    MEM_freeN(objects);
    return false;
  }

  if (use_extend == 0 || em->selectmode == 0) {
    if (use_expand) {
      const short selmode_max = highest_order_bit_s(ts->selectmode);
      for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
        Object *ob_iter = objects[ob_index];
        BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);
        EDBM_selectmode_convert(em_iter, selmode_max, selectmode_new);
      }
    }
  }

  switch (selectmode_new) {
    case SCE_SELECT_VERTEX:
      if (use_extend == 0 || em->selectmode == 0) {
        em->selectmode = SCE_SELECT_VERTEX;
      }
      ret = true;
      break;
    case SCE_SELECT_EDGE:
      if (use_extend == 0 || em->selectmode == 0) {
        em->selectmode = SCE_SELECT_EDGE;
      }
      ret = true;
      break;
    case SCE_SELECT_FACE:
      if (use_extend == 0 || em->selectmode == 0) {
        em->selectmode = SCE_SELECT_FACE;
      }
      ret = true;
      break;
    default:
      BLI_assert(0);
      break;
  }

  if (ret == true) {
    ts->selectmode = em->selectmode;
    em = NULL;
    for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
      Object *ob_iter = objects[ob_index];
      BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);
      em_iter->selectmode = ts->selectmode;
      EDBM_selectmode_set(em_iter);
      DEG_id_tag_update(ob_iter->data, ID_RECALC_COPY_ON_WRITE | ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob_iter->data);
    }
    WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, NULL);
    DEG_id_tag_update(&scene->id, ID_RECALC_COPY_ON_WRITE);
  }

  MEM_freeN(objects);
  return ret;
}

bool EDBM_selectmode_set_multi(bContext *C, const short selectmode)
{
  BLI_assert(selectmode != 0);
  bool changed = false;

  {
    Object *obedit = CTX_data_edit_object(C);
    BMEditMesh *em = NULL;
    if (obedit && obedit->type == OB_MESH) {
      em = BKE_editmesh_from_object(obedit);
    }
    if (em == NULL) {
      return changed;
    }
  }

  ViewLayer *view_layer = CTX_data_view_layer(C);
  Scene *scene = CTX_data_scene(C);
  ToolSettings *ts = scene->toolsettings;

  if (ts->selectmode != selectmode) {
    ts->selectmode = selectmode;
    changed = true;
  }

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob_iter = objects[ob_index];
    BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);
    if (em_iter->selectmode != ts->selectmode) {
      em_iter->selectmode = ts->selectmode;
      EDBM_selectmode_set(em_iter);
      DEG_id_tag_update(ob_iter->data, ID_RECALC_COPY_ON_WRITE | ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob_iter->data);
      changed = true;
    }
  }
  MEM_freeN(objects);

  if (changed) {
    WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, NULL);
    DEG_id_tag_update(&scene->id, ID_RECALC_COPY_ON_WRITE);
  }
  return changed;
}

bool EDBM_selectmode_disable(Scene *scene,
                             BMEditMesh *em,
                             const short selectmode_disable,
                             const short selectmode_fallback)
{
  /* note essential, but switch out of vertex mode since the
   * selected regions won't be nicely isolated after flushing */
  if (em->selectmode & selectmode_disable) {
    if (em->selectmode == selectmode_disable) {
      em->selectmode = selectmode_fallback;
    }
    else {
      em->selectmode &= ~selectmode_disable;
    }
    scene->toolsettings->selectmode = em->selectmode;
    EDBM_selectmode_set(em);

    WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);

    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Toggle
 * \{ */

bool EDBM_deselect_by_material(BMEditMesh *em, const short index, const bool select)
{
  BMIter iter;
  BMFace *efa;
  bool changed = false;

  BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
      continue;
    }
    if (efa->mat_nr == index) {
      changed = true;
      BM_face_select_set(em->bm, efa, select);
    }
  }
  return changed;
}

void EDBM_select_toggle_all(BMEditMesh *em) /* exported for UV */
{
  if (em->bm->totvertsel || em->bm->totedgesel || em->bm->totfacesel) {
    EDBM_flag_disable_all(em, BM_ELEM_SELECT);
  }
  else {
    EDBM_flag_enable_all(em, BM_ELEM_SELECT);
  }
}

void EDBM_select_swap(BMEditMesh *em) /* exported for UV */
{
  BMIter iter;
  BMVert *eve;
  BMEdge *eed;
  BMFace *efa;

  if (em->bm->selectmode & SCE_SELECT_VERTEX) {
    BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
      if (BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
        continue;
      }
      BM_vert_select_set(em->bm, eve, !BM_elem_flag_test(eve, BM_ELEM_SELECT));
    }
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
        continue;
      }
      BM_edge_select_set(em->bm, eed, !BM_elem_flag_test(eed, BM_ELEM_SELECT));
    }
  }
  else {
    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
        continue;
      }
      BM_face_select_set(em->bm, efa, !BM_elem_flag_test(efa, BM_ELEM_SELECT));
    }
  }
}

bool EDBM_mesh_deselect_all_multi_ex(struct Base **bases, const uint bases_len)
{
  bool changed_multi = false;
  for (uint base_index = 0; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    Object *ob_iter = base_iter->object;
    BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);

    if (em_iter->bm->totvertsel == 0) {
      continue;
    }

    EDBM_flag_disable_all(em_iter, BM_ELEM_SELECT);
    DEG_id_tag_update(ob_iter->data, ID_RECALC_SELECT);
    changed_multi = true;
  }
  return changed_multi;
}

bool EDBM_mesh_deselect_all_multi(struct bContext *C)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc;
  ED_view3d_viewcontext_init(C, &vc, depsgraph);
  uint bases_len = 0;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode_unique_data(
      vc.view_layer, vc.v3d, &bases_len);
  bool changed_multi = EDBM_mesh_deselect_all_multi_ex(bases, bases_len);
  MEM_freeN(bases);
  return changed_multi;
}

bool EDBM_selectmode_disable_multi_ex(Scene *scene,
                                      struct Base **bases,
                                      const uint bases_len,
                                      const short selectmode_disable,
                                      const short selectmode_fallback)
{
  bool changed_multi = false;
  for (uint base_index = 0; base_index < bases_len; base_index++) {
    Base *base_iter = bases[base_index];
    Object *ob_iter = base_iter->object;
    BMEditMesh *em_iter = BKE_editmesh_from_object(ob_iter);

    if (EDBM_selectmode_disable(scene, em_iter, selectmode_disable, selectmode_fallback)) {
      changed_multi = true;
    }
  }
  return changed_multi;
}

bool EDBM_selectmode_disable_multi(struct bContext *C,
                                   const short selectmode_disable,
                                   const short selectmode_fallback)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  ViewContext vc;
  ED_view3d_viewcontext_init(C, &vc, depsgraph);
  uint bases_len = 0;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode_unique_data(
      vc.view_layer, NULL, &bases_len);
  bool changed_multi = EDBM_selectmode_disable_multi_ex(
      scene, bases, bases_len, selectmode_disable, selectmode_fallback);
  MEM_freeN(bases);
  return changed_multi;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Interior Faces
 *
 * Overview of the algorithm:
 * - Groups faces surrounded by edges with 3+ faces using them.
 * - Calculates a cost of each face group comparing its angle with the faces
 *   connected to its non-manifold edges.
 * - Mark the face group as interior, and mark connected face groups for recalculation.
 * - Continue to remove the face groups with the highest 'cost'.
 *
 * \{ */

struct BMFaceLink {
  struct BMFaceLink *next, *prev;
  BMFace *face;
  float area;
};

static bool bm_interior_loop_filter_fn(const BMLoop *l, void *UNUSED(user_data))
{
  if (BM_elem_flag_test(l->e, BM_ELEM_TAG)) {
    return false;
  }
  return true;
}
static bool bm_interior_edge_is_manifold_except_face_index(BMEdge *e,
                                                           int face_index,
                                                           BMLoop *r_l_pair[2])
{

  BMLoop *l_iter = e->l;
  int loop_index = 0;
  do {
    BMFace *f = l_iter->f;
    int i = BM_elem_index_get(f);
    if (!ELEM(i, -1, face_index)) {
      if (loop_index == 2) {
        return false;
      }
      r_l_pair[loop_index++] = l_iter;
    }
  } while ((l_iter = l_iter->radial_next) != e->l);
  return (loop_index == 2);
}

/**
 * Calculate the cost of the face group.
 * A higher value means it's more likely to remove first.
 */
static float bm_interior_face_group_calc_cost(ListBase *ls, const float *edge_lengths)
{
  /* Dividing by the area is important so larger face groups (which will become the outer shell)
   * aren't detected as having a high cost. */
  float area = 0.0f;
  float cost = 0.0f;
  bool found = false;
  LISTBASE_FOREACH (struct BMFaceLink *, f_link, ls) {
    BMFace *f = f_link->face;
    area += f_link->area;
    int i = BM_elem_index_get(f);
    BLI_assert(i != -1);
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (BM_elem_flag_test(l_iter->e, BM_ELEM_TAG)) {
        float cost_test = 0.0f;
        int cost_count = 0;
        /* All other faces. */
        BMLoop *l_radial_iter = l_iter;
        do {
          int i_other = BM_elem_index_get(l_radial_iter->f);
          if (!ELEM(i_other, -1, i)) {
            float angle = angle_normalized_v3v3(f->no, l_radial_iter->f->no);
            /* Ignore face direction since in the case on non-manifold faces connecting edges,
             * the face flipping may not be meaningful. */
            if (angle > DEG2RADF(90)) {
              angle = DEG2RADF(180) - angle;
            }
            /* Avoid calculating it inline, pass in pre-calculated edge lengths. */
#if 0
            cost_test += BM_edge_calc_length(l_iter->e) * angle;
#else
            BLI_assert(edge_lengths[BM_elem_index_get(l_iter->e)] != -1.0f);
            cost_test += edge_lengths[BM_elem_index_get(l_iter->e)] * angle;
#endif
            cost_count += 1;
          }
        } while ((l_radial_iter = l_radial_iter->radial_next) != l_iter);

        if (cost_count >= 2) {
          cost += cost_test;
          found = true;
        }
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
  return found ? cost / area : FLT_MAX;
}

bool EDBM_select_interior_faces(BMEditMesh *em)
{
  BMesh *bm = em->bm;
  BMIter iter;
  bool changed = false;

  float *edge_lengths = MEM_mallocN(sizeof(*edge_lengths) * bm->totedge, __func__);

  {
    bool has_nonmanifold = false;
    BMEdge *e;
    int i;
    BM_ITER_MESH_INDEX (e, &iter, bm, BM_EDGES_OF_MESH, i) {
      const bool is_over = BM_edge_face_count_is_over(e, 2);
      if (is_over) {
        BM_elem_flag_enable(e, BM_ELEM_TAG);
        has_nonmanifold = true;
        edge_lengths[i] = BM_edge_calc_length(e);
      }
      else {
        BM_elem_flag_disable(e, BM_ELEM_TAG);
        edge_lengths[i] = -1.0;
      }

      BM_elem_index_set(e, i); /* set_inline */
    }
    bm->elem_index_dirty &= ~BM_EDGE;

    if (has_nonmanifold == false) {
      MEM_freeN(edge_lengths);
      return false;
    }
  }

  /* group vars */
  int *fgroup_array;
  int(*fgroup_index)[2];
  int fgroup_len;

  fgroup_array = MEM_mallocN(sizeof(*fgroup_array) * bm->totface, __func__);
  fgroup_len = BM_mesh_calc_face_groups(
      bm, fgroup_array, &fgroup_index, bm_interior_loop_filter_fn, NULL, NULL, 0, BM_EDGE);

  int *fgroup_recalc_stack = MEM_mallocN(sizeof(*fgroup_recalc_stack) * fgroup_len, __func__);
  STACK_DECLARE(fgroup_recalc_stack);
  STACK_INIT(fgroup_recalc_stack, fgroup_len);

  BM_mesh_elem_table_ensure(bm, BM_FACE);

  {
    BMFace *f;
    BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
      BM_elem_index_set(f, -1); /* set_dirty! */
    }
  }
  bm->elem_index_dirty |= BM_FACE;

  ListBase *fgroup_listbase = MEM_callocN(sizeof(*fgroup_listbase) * fgroup_len, __func__);
  struct BMFaceLink *f_link_array = MEM_callocN(sizeof(*f_link_array) * bm->totface, __func__);

  for (int i = 0; i < fgroup_len; i++) {
    const int fg_sta = fgroup_index[i][0];
    const int fg_len = fgroup_index[i][1];
    for (int j = 0; j < fg_len; j++) {
      const int face_index = fgroup_array[fg_sta + j];
      BMFace *f = BM_face_at_index(bm, face_index);
      BM_elem_index_set(f, i);

      struct BMFaceLink *f_link = &f_link_array[face_index];
      f_link->face = f;
      f_link->area = BM_face_calc_area(f);
      BLI_addtail(&fgroup_listbase[i], f_link);
    }
  }

  MEM_freeN(fgroup_array);
  MEM_freeN(fgroup_index);

  Heap *fgroup_heap = BLI_heap_new_ex(fgroup_len);
  HeapNode **fgroup_table = MEM_mallocN(sizeof(*fgroup_table) * fgroup_len, __func__);
  bool *fgroup_dirty = MEM_callocN(sizeof(*fgroup_dirty) * fgroup_len, __func__);

  for (int i = 0; i < fgroup_len; i++) {
    const float cost = bm_interior_face_group_calc_cost(&fgroup_listbase[i], edge_lengths);
    if (cost != FLT_MAX) {
      fgroup_table[i] = BLI_heap_insert(fgroup_heap, -cost, POINTER_FROM_INT(i));
    }
    else {
      fgroup_table[i] = NULL;
    }
  }

  /* Avoid re-running cost calculations for large face-groups which will end up forming the
   * outer shell and not be considered interior.
   * As these face groups become increasingly bigger - their chance of being considered
   * interior reduces as does the time to calculate their cost.
   *
   * This delays recalculating them until they are considered can dates to remove
   * which becomes less and less likely as they increase in area. */

#define USE_DELAY_FACE_GROUP_COST_CALC

  while (true) {

#if defined(USE_DELAY_FACE_GROUP_COST_CALC)
    while (!BLI_heap_is_empty(fgroup_heap)) {
      HeapNode *node_min = BLI_heap_top(fgroup_heap);
      const int i = POINTER_AS_INT(BLI_heap_node_ptr(node_min));
      if (fgroup_dirty[i]) {
        const float cost = bm_interior_face_group_calc_cost(&fgroup_listbase[i], edge_lengths);
        if (cost != FLT_MAX) {
          /* The cost may have improves (we may be able to skip this),
           * however the cost should _never_ make this a choice. */
          BLI_assert(-BLI_heap_node_value(node_min) >= cost);
          BLI_heap_node_value_update(fgroup_heap, fgroup_table[i], -cost);
        }
        else {
          BLI_heap_remove(fgroup_heap, fgroup_table[i]);
          fgroup_table[i] = NULL;
        }
        fgroup_dirty[i] = false;
      }
      else {
        break;
      }
    }
#endif

    if (BLI_heap_is_empty(fgroup_heap)) {
      break;
    }

    const int i_min = POINTER_AS_INT(BLI_heap_pop_min(fgroup_heap));
    BLI_assert(fgroup_table[i_min] != NULL);
    BLI_assert(fgroup_dirty[i_min] == false);
    fgroup_table[i_min] = NULL;
    changed = true;

    struct BMFaceLink *f_link;
    while ((f_link = BLI_pophead(&fgroup_listbase[i_min]))) {
      BMFace *f = f_link->face;
      BM_face_select_set(bm, f, true);
      BM_elem_index_set(f, -1); /* set-dirty */

      BMLoop *l_iter, *l_first;

      /* Loop over edges face edges, merging groups which are no longer separated
       * by non-manifold edges (when manifold check ignores faces from this group). */
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BMLoop *l_pair[2];
        if (bm_interior_edge_is_manifold_except_face_index(l_iter->e, i_min, l_pair)) {
          BM_elem_flag_disable(l_iter->e, BM_ELEM_TAG);

          int i_a = BM_elem_index_get(l_pair[0]->f);
          int i_b = BM_elem_index_get(l_pair[1]->f);
          if (i_a != i_b) {
            /* Only for predictable results that don't depend on the order of radial loops,
             * not essential. */
            if (i_a > i_b) {
              SWAP(int, i_a, i_b);
            }

            /* Merge the groups. */
            LISTBASE_FOREACH (LinkData *, n, &fgroup_listbase[i_b]) {
              BMFace *f_iter = n->data;
              BM_elem_index_set(f_iter, i_a);
            }
            BLI_movelisttolist(&fgroup_listbase[i_a], &fgroup_listbase[i_b]);

            /* This may have been added to 'fgroup_recalc_stack', instead of removing it,
             * just check the heap node isn't NULL before recalculating. */
            BLI_heap_remove(fgroup_heap, fgroup_table[i_b]);
            fgroup_table[i_b] = NULL;
            /* Keep the dirty flag as-is for 'i_b', because it may be in the 'fgroup_recalc_stack'
             * and we don't want to add it again.
             * Instead rely on the 'fgroup_table[i_b]' being NULL as a secondary check. */

            if (fgroup_dirty[i_a] == false) {
              BLI_assert(fgroup_table[i_a] != NULL);
              STACK_PUSH(fgroup_recalc_stack, i_a);
              fgroup_dirty[i_a] = true;
            }
          }
        }

        /* Mark all connected groups for re-calculation. */
        BMLoop *l_radial_iter = l_iter->radial_next;
        if (l_radial_iter != l_iter) {
          do {
            int i_other = BM_elem_index_get(l_radial_iter->f);
            if (!ELEM(i_other, -1, i_min)) {
              if ((fgroup_table[i_other] != NULL) && (fgroup_dirty[i_other] == false)) {
#if !defined(USE_DELAY_FACE_GROUP_COST_CALC)
                STACK_PUSH(fgroup_recalc_stack, i_other);
#endif
                fgroup_dirty[i_other] = true;
              }
            }
          } while ((l_radial_iter = l_radial_iter->radial_next) != l_iter);
        }

      } while ((l_iter = l_iter->next) != l_first);
    }

    for (int index = 0; index < STACK_SIZE(fgroup_recalc_stack); index++) {
      const int i = fgroup_recalc_stack[index];
      if (fgroup_table[i] != NULL && fgroup_dirty[i] == true) {
        /* First update edge tags. */
        const float cost = bm_interior_face_group_calc_cost(&fgroup_listbase[i], edge_lengths);
        if (cost != FLT_MAX) {
          BLI_heap_node_value_update(fgroup_heap, fgroup_table[i], -cost);
        }
        else {
          BLI_heap_remove(fgroup_heap, fgroup_table[i]);
          fgroup_table[i] = NULL;
        }
      }
      fgroup_dirty[i] = false;
    }
    STACK_CLEAR(fgroup_recalc_stack);
  }

  MEM_freeN(edge_lengths);
  MEM_freeN(f_link_array);
  MEM_freeN(fgroup_listbase);
  MEM_freeN(fgroup_recalc_stack);
  MEM_freeN(fgroup_table);
  MEM_freeN(fgroup_dirty);

  BLI_heap_free(fgroup_heap, NULL);

  return changed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Linked Operator
 *
 * Support delimiting on different edge properties.
 * \{ */

/* so we can have last-used default depend on selection mode (rare exception!) */
#define USE_LINKED_SELECT_DEFAULT_HACK

struct DelimitData {
  int cd_loop_type;
  int cd_loop_offset;
};

static bool select_linked_delimit_test(BMEdge *e,
                                       int delimit,
                                       const struct DelimitData *delimit_data)
{
  BLI_assert(delimit);

  if (delimit & BMO_DELIM_SEAM) {
    if (BM_elem_flag_test(e, BM_ELEM_SEAM)) {
      return true;
    }
  }

  if (delimit & BMO_DELIM_SHARP) {
    if (BM_elem_flag_test(e, BM_ELEM_SMOOTH) == 0) {
      return true;
    }
  }

  if (delimit & BMO_DELIM_NORMAL) {
    if (!BM_edge_is_contiguous(e)) {
      return true;
    }
  }

  if (delimit & BMO_DELIM_MATERIAL) {
    if (e->l && e->l->radial_next != e->l) {
      const short mat_nr = e->l->f->mat_nr;
      BMLoop *l_iter = e->l->radial_next;
      do {
        if (l_iter->f->mat_nr != mat_nr) {
          return true;
        }
      } while ((l_iter = l_iter->radial_next) != e->l);
    }
  }

  if (delimit & BMO_DELIM_UV) {
    if (BM_edge_is_contiguous_loop_cd(
            e, delimit_data->cd_loop_type, delimit_data->cd_loop_offset) == 0) {
      return true;
    }
  }

  return false;
}

#ifdef USE_LINKED_SELECT_DEFAULT_HACK
/**
 * Gets the default from the operator fallback to own last-used value
 * (selected based on mode)
 */
static int select_linked_delimit_default_from_op(wmOperator *op, const int select_mode)
{
  static char delimit_last_store[2] = {0, BMO_DELIM_SEAM};
  int delimit_last_index = (select_mode & (SCE_SELECT_VERTEX | SCE_SELECT_EDGE)) == 0;
  char *delimit_last = &delimit_last_store[delimit_last_index];
  PropertyRNA *prop_delimit = RNA_struct_find_property(op->ptr, "delimit");
  int delimit;

  if (RNA_property_is_set(op->ptr, prop_delimit)) {
    delimit = RNA_property_enum_get(op->ptr, prop_delimit);
    *delimit_last = delimit;
  }
  else {
    delimit = *delimit_last;
    RNA_property_enum_set(op->ptr, prop_delimit, delimit);
  }
  return delimit;
}
#endif

static void select_linked_delimit_validate(BMesh *bm, int *delimit)
{
  if ((*delimit) & BMO_DELIM_UV) {
    if (!CustomData_has_layer(&bm->ldata, CD_MLOOPUV)) {
      (*delimit) &= ~BMO_DELIM_UV;
    }
  }
}

static void select_linked_delimit_begin(BMesh *bm, int delimit)
{
  struct DelimitData delimit_data = {0};

  if (delimit & BMO_DELIM_UV) {
    delimit_data.cd_loop_type = CD_MLOOPUV;
    delimit_data.cd_loop_offset = CustomData_get_offset(&bm->ldata, delimit_data.cd_loop_type);
    if (delimit_data.cd_loop_offset == -1) {
      delimit &= ~BMO_DELIM_UV;
    }
  }

  /* grr, shouldn't need to alloc BMO flags here */
  BM_mesh_elem_toolflags_ensure(bm);

  {
    BMIter iter;
    BMEdge *e;

    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      const bool is_walk_ok = ((select_linked_delimit_test(e, delimit, &delimit_data) == false));

      BMO_edge_flag_set(bm, e, BMO_ELE_TAG, is_walk_ok);
    }
  }
}

static void select_linked_delimit_end(BMEditMesh *em)
{
  BMesh *bm = em->bm;

  BM_mesh_elem_toolflags_clear(bm);
}

static int edbm_select_linked_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

#ifdef USE_LINKED_SELECT_DEFAULT_HACK
  const int delimit_init = select_linked_delimit_default_from_op(op,
                                                                 scene->toolsettings->selectmode);
#else
  const int delimit_init = RNA_enum_get(op->ptr, "delimit");
#endif

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];

    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMIter iter;
    BMWalker walker;

    int delimit = delimit_init;

    select_linked_delimit_validate(bm, &delimit);

    if (delimit) {
      select_linked_delimit_begin(em->bm, delimit);
    }

    if (em->selectmode & SCE_SELECT_VERTEX) {
      BMVert *v;

      BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
        BM_elem_flag_set(v, BM_ELEM_TAG, BM_elem_flag_test(v, BM_ELEM_SELECT));
      }

      /* exclude all delimited verts */
      if (delimit) {
        BMEdge *e;
        BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
          if (!BMO_edge_flag_test(bm, e, BMO_ELE_TAG)) {
            /* Check the edge for selected faces,
             * this supports stepping off isolated vertices which would otherwise be ignored. */
            if (BM_edge_is_any_face_flag_test(e, BM_ELEM_SELECT)) {
              BM_elem_flag_disable(e->v1, BM_ELEM_TAG);
              BM_elem_flag_disable(e->v2, BM_ELEM_TAG);
            }
          }
        }
      }

      BMW_init(&walker,
               em->bm,
               delimit ? BMW_LOOP_SHELL_WIRE : BMW_VERT_SHELL,
               BMW_MASK_NOP,
               delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
               BMW_MASK_NOP,
               BMW_FLAG_TEST_HIDDEN,
               BMW_NIL_LAY);

      if (delimit) {
        BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
          if (BM_elem_flag_test(v, BM_ELEM_TAG)) {
            BMElem *ele_walk;
            BMW_ITER (ele_walk, &walker, v) {
              if (ele_walk->head.htype == BM_LOOP) {
                BMVert *v_step = ((BMLoop *)ele_walk)->v;
                BM_vert_select_set(em->bm, v_step, true);
                BM_elem_flag_disable(v_step, BM_ELEM_TAG);
              }
              else {
                BMEdge *e_step = (BMEdge *)ele_walk;
                BLI_assert(ele_walk->head.htype == BM_EDGE);
                BM_edge_select_set(em->bm, e_step, true);
                BM_elem_flag_disable(e_step->v1, BM_ELEM_TAG);
                BM_elem_flag_disable(e_step->v2, BM_ELEM_TAG);
              }
            }
          }
        }
      }
      else {
        BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
          if (BM_elem_flag_test(v, BM_ELEM_TAG)) {
            BMEdge *e_walk;
            BMW_ITER (e_walk, &walker, v) {
              BM_edge_select_set(em->bm, e_walk, true);
              BM_elem_flag_disable(e_walk, BM_ELEM_TAG);
            }
          }
        }
      }

      BMW_end(&walker);

      EDBM_selectmode_flush(em);
    }
    else if (em->selectmode & SCE_SELECT_EDGE) {
      BMEdge *e;

      if (delimit) {
        BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
          /* Check the edge for selected faces,
           * this supports stepping off isolated edges which would otherwise be ignored. */
          BM_elem_flag_set(e,
                           BM_ELEM_TAG,
                           (BM_elem_flag_test(e, BM_ELEM_SELECT) &&
                            (BMO_edge_flag_test(bm, e, BMO_ELE_TAG) ||
                             !BM_edge_is_any_face_flag_test(e, BM_ELEM_SELECT))));
        }
      }
      else {
        BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
          BM_elem_flag_set(e, BM_ELEM_TAG, BM_elem_flag_test(e, BM_ELEM_SELECT));
        }
      }

      BMW_init(&walker,
               em->bm,
               delimit ? BMW_LOOP_SHELL_WIRE : BMW_VERT_SHELL,
               BMW_MASK_NOP,
               delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
               BMW_MASK_NOP,
               BMW_FLAG_TEST_HIDDEN,
               BMW_NIL_LAY);

      if (delimit) {
        BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
          if (BM_elem_flag_test(e, BM_ELEM_TAG)) {
            BMElem *ele_walk;
            BMW_ITER (ele_walk, &walker, e) {
              if (ele_walk->head.htype == BM_LOOP) {
                BMLoop *l_step = (BMLoop *)ele_walk;
                BM_edge_select_set(em->bm, l_step->e, true);
                BM_edge_select_set(em->bm, l_step->prev->e, true);
                BM_elem_flag_disable(l_step->e, BM_ELEM_TAG);
              }
              else {
                BMEdge *e_step = (BMEdge *)ele_walk;
                BLI_assert(ele_walk->head.htype == BM_EDGE);
                BM_edge_select_set(em->bm, e_step, true);
                BM_elem_flag_disable(e_step, BM_ELEM_TAG);
              }
            }
          }
        }
      }
      else {
        BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
          if (BM_elem_flag_test(e, BM_ELEM_TAG)) {
            BMEdge *e_walk;
            BMW_ITER (e_walk, &walker, e) {
              BM_edge_select_set(em->bm, e_walk, true);
              BM_elem_flag_disable(e_walk, BM_ELEM_TAG);
            }
          }
        }
      }

      BMW_end(&walker);

      EDBM_selectmode_flush(em);
    }
    else {
      BMFace *f;

      BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
        BM_elem_flag_set(f, BM_ELEM_TAG, BM_elem_flag_test(f, BM_ELEM_SELECT));
      }

      BMW_init(&walker,
               bm,
               BMW_ISLAND,
               BMW_MASK_NOP,
               delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
               BMW_MASK_NOP,
               BMW_FLAG_TEST_HIDDEN,
               BMW_NIL_LAY);

      BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
        if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
          BMFace *f_walk;
          BMW_ITER (f_walk, &walker, f) {
            BM_face_select_set(bm, f_walk, true);
            BM_elem_flag_disable(f_walk, BM_ELEM_TAG);
          }
        }
      }

      BMW_end(&walker);
    }

    if (delimit) {
      select_linked_delimit_end(em);
    }

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_linked(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Linked All";
  ot->idname = "MESH_OT_select_linked";
  ot->description = "Select all vertices connected to the current selection";

  /* api callbacks */
  ot->exec = edbm_select_linked_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_enum_flag(ot->srna,
                           "delimit",
                           rna_enum_mesh_delimit_mode_items,
                           BMO_DELIM_SEAM,
                           "Delimit",
                           "Delimit selected region");
#ifdef USE_LINKED_SELECT_DEFAULT_HACK
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
#else
  UNUSED_VARS(prop);
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Linked (Cursor Pick) Operator
 * \{ */

static int edbm_select_linked_pick_exec(bContext *C, wmOperator *op);

static void edbm_select_linked_pick_ex(BMEditMesh *em, BMElem *ele, bool sel, int delimit)
{
  BMesh *bm = em->bm;
  BMWalker walker;

  select_linked_delimit_validate(bm, &delimit);

  if (delimit) {
    select_linked_delimit_begin(bm, delimit);
  }

  /* NOTE: logic closely matches #edbm_select_linked_exec, keep in sync. */

  if (ele->head.htype == BM_VERT) {
    BMVert *eve = (BMVert *)ele;

    BMW_init(&walker,
             bm,
             delimit ? BMW_LOOP_SHELL_WIRE : BMW_VERT_SHELL,
             BMW_MASK_NOP,
             delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
             BMW_MASK_NOP,
             BMW_FLAG_TEST_HIDDEN,
             BMW_NIL_LAY);

    if (delimit) {
      BMElem *ele_walk;
      BMW_ITER (ele_walk, &walker, eve) {
        if (ele_walk->head.htype == BM_LOOP) {
          BMVert *v_step = ((BMLoop *)ele_walk)->v;
          BM_vert_select_set(bm, v_step, sel);
        }
        else {
          BMEdge *e_step = (BMEdge *)ele_walk;
          BLI_assert(ele_walk->head.htype == BM_EDGE);
          BM_edge_select_set(bm, e_step, sel);
        }
      }
    }
    else {
      BMEdge *e_walk;
      BMW_ITER (e_walk, &walker, eve) {
        BM_edge_select_set(bm, e_walk, sel);
      }
    }

    BMW_end(&walker);

    EDBM_selectmode_flush(em);
  }
  else if (ele->head.htype == BM_EDGE) {
    BMEdge *eed = (BMEdge *)ele;

    BMW_init(&walker,
             bm,
             delimit ? BMW_LOOP_SHELL_WIRE : BMW_VERT_SHELL,
             BMW_MASK_NOP,
             delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
             BMW_MASK_NOP,
             BMW_FLAG_TEST_HIDDEN,
             BMW_NIL_LAY);

    if (delimit) {
      BMElem *ele_walk;
      BMW_ITER (ele_walk, &walker, eed) {
        if (ele_walk->head.htype == BM_LOOP) {
          BMEdge *e_step = ((BMLoop *)ele_walk)->e;
          BM_edge_select_set(bm, e_step, sel);
        }
        else {
          BMEdge *e_step = (BMEdge *)ele_walk;
          BLI_assert(ele_walk->head.htype == BM_EDGE);
          BM_edge_select_set(bm, e_step, sel);
        }
      }
    }
    else {
      BMEdge *e_walk;
      BMW_ITER (e_walk, &walker, eed) {
        BM_edge_select_set(bm, e_walk, sel);
      }
    }

    BMW_end(&walker);

    EDBM_selectmode_flush(em);
  }
  else if (ele->head.htype == BM_FACE) {
    BMFace *efa = (BMFace *)ele;

    BMW_init(&walker,
             bm,
             BMW_ISLAND,
             BMW_MASK_NOP,
             delimit ? BMO_ELE_TAG : BMW_MASK_NOP,
             BMW_MASK_NOP,
             BMW_FLAG_TEST_HIDDEN,
             BMW_NIL_LAY);

    {
      BMFace *f_walk;
      BMW_ITER (f_walk, &walker, efa) {
        BM_face_select_set(bm, f_walk, sel);
        BM_elem_flag_disable(f_walk, BM_ELEM_TAG);
      }
    }

    BMW_end(&walker);
  }

  if (delimit) {
    select_linked_delimit_end(em);
  }
}

static int edbm_select_linked_pick_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ViewContext vc;
  Base *basact = NULL;
  BMVert *eve;
  BMEdge *eed;
  BMFace *efa;
  const bool sel = !RNA_boolean_get(op->ptr, "deselect");
  int index;

  if (RNA_struct_property_is_set(op->ptr, "index")) {
    return edbm_select_linked_pick_exec(C, op);
  }

  /* #unified_findnearest needs OpenGL. */
  view3d_operator_needs_opengl(C);

  /* setup view context for argument to callbacks */
  em_setup_viewcontext(C, &vc);

  uint bases_len;
  Base **bases = BKE_view_layer_array_from_bases_in_edit_mode(vc.view_layer, vc.v3d, &bases_len);

  {
    bool has_edges = false;
    for (uint base_index = 0; base_index < bases_len; base_index++) {
      Object *ob_iter = bases[base_index]->object;
      ED_view3d_viewcontext_init_object(&vc, ob_iter);
      if (vc.em->bm->totedge) {
        has_edges = true;
      }
    }
    if (has_edges == false) {
      MEM_freeN(bases);
      return OPERATOR_CANCELLED;
    }
  }

  vc.mval[0] = event->mval[0];
  vc.mval[1] = event->mval[1];

  /* return warning! */
  {
    int base_index = -1;
    const bool ok = unified_findnearest(&vc, bases, bases_len, &base_index, &eve, &eed, &efa);
    if (!ok) {
      MEM_freeN(bases);
      return OPERATOR_CANCELLED;
    }
    basact = bases[base_index];
  }

  ED_view3d_viewcontext_init_object(&vc, basact->object);
  BMEditMesh *em = vc.em;
  BMesh *bm = em->bm;

#ifdef USE_LINKED_SELECT_DEFAULT_HACK
  int delimit = select_linked_delimit_default_from_op(op, vc.scene->toolsettings->selectmode);
#else
  int delimit = RNA_enum_get(op->ptr, "delimit");
#endif

  BMElem *ele = EDBM_elem_from_selectmode(em, eve, eed, efa);

  edbm_select_linked_pick_ex(em, ele, sel, delimit);

  /* To support redo. */
  {
    /* Note that the `base_index` can't be used as the index depends on the 3D Viewport
     * which might not be available on redo. */
    BM_mesh_elem_index_ensure(bm, ele->head.htype);
    int object_index;
    index = EDBM_elem_to_index_any_multi(vc.view_layer, em, ele, &object_index);
    BLI_assert(object_index >= 0);
    RNA_int_set(op->ptr, "object_index", object_index);
    RNA_int_set(op->ptr, "index", index);
  }

  DEG_id_tag_update(basact->object->data, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, basact->object->data);

  MEM_freeN(bases);
  return OPERATOR_FINISHED;
}

static int edbm_select_linked_pick_exec(bContext *C, wmOperator *op)
{
  Object *obedit = NULL;
  BMElem *ele;

  {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    /* Intentionally wrap negative values so the lookup fails. */
    const uint object_index = (uint)RNA_int_get(op->ptr, "object_index");
    const uint index = (uint)RNA_int_get(op->ptr, "index");
    ele = EDBM_elem_from_index_any_multi(view_layer, object_index, index, &obedit);
  }

  if (ele == NULL) {
    return OPERATOR_CANCELLED;
  }

  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  const bool sel = !RNA_boolean_get(op->ptr, "deselect");

#ifdef USE_LINKED_SELECT_DEFAULT_HACK
  int delimit = select_linked_delimit_default_from_op(op, em->selectmode);
#else
  int delimit = RNA_enum_get(op->ptr, "delimit");
#endif

  edbm_select_linked_pick_ex(em, ele, sel, delimit);

  DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_linked_pick(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Linked";
  ot->idname = "MESH_OT_select_linked_pick";
  ot->description = "(De)select all vertices linked to the edge under the mouse cursor";

  /* api callbacks */
  ot->invoke = edbm_select_linked_pick_invoke;
  ot->exec = edbm_select_linked_pick_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "deselect", 0, "Deselect", "");
  prop = RNA_def_enum_flag(ot->srna,
                           "delimit",
                           rna_enum_mesh_delimit_mode_items,
                           BMO_DELIM_SEAM,
                           "Delimit",
                           "Delimit selected region");
#ifdef USE_LINKED_SELECT_DEFAULT_HACK
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
#endif

  /* use for redo */
  prop = RNA_def_int(ot->srna, "object_index", -1, -1, INT_MAX, "", "", 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_int(ot->srna, "index", -1, -1, INT_MAX, "", "", 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Face by Sides Operator
 * \{ */

static int edbm_select_face_by_sides_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const int numverts = RNA_int_get(op->ptr, "number");
  const int type = RNA_enum_get(op->ptr, "type");
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMFace *efa;
    BMIter iter;

    if (!extend) {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
    }

    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      bool select;

      switch (type) {
        case 0:
          select = (efa->len < numverts);
          break;
        case 1:
          select = (efa->len == numverts);
          break;
        case 2:
          select = (efa->len > numverts);
          break;
        case 3:
          select = (efa->len != numverts);
          break;
        default:
          BLI_assert(0);
          select = false;
          break;
      }

      if (select) {
        BM_face_select_set(em->bm, efa, true);
      }
    }

    EDBM_selectmode_flush(em);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_face_by_sides(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {0, "LESS", 0, "Less Than", ""},
      {1, "EQUAL", 0, "Equal To", ""},
      {2, "GREATER", 0, "Greater Than", ""},
      {3, "NOTEQUAL", 0, "Not Equal To", ""},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Select Faces by Sides";
  ot->description = "Select vertices or faces by the number of polygon sides";
  ot->idname = "MESH_OT_select_face_by_sides";

  /* api callbacks */
  ot->exec = edbm_select_face_by_sides_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_int(ot->srna, "number", 4, 3, INT_MAX, "Number of Vertices", "", 3, INT_MAX);
  RNA_def_enum(ot->srna, "type", type_items, 1, "Type", "Type of comparison to make");
  RNA_def_boolean(ot->srna, "extend", true, "Extend", "Extend the selection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Loose Operator
 * \{ */

static int edbm_select_loose_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool extend = RNA_boolean_get(op->ptr, "extend");

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    BMIter iter;

    if (!extend) {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
    }

    if (em->selectmode & SCE_SELECT_VERTEX) {
      BMVert *eve;
      BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
        if (!eve->e) {
          BM_vert_select_set(bm, eve, true);
        }
      }
    }

    if (em->selectmode & SCE_SELECT_EDGE) {
      BMEdge *eed;
      BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
        if (BM_edge_is_wire(eed)) {
          BM_edge_select_set(bm, eed, true);
        }
      }
    }

    if (em->selectmode & SCE_SELECT_FACE) {
      BMFace *efa;
      BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
        BMIter liter;
        BMLoop *l;
        bool is_loose = true;
        BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
          if (!BM_edge_is_boundary(l->e)) {
            is_loose = false;
            break;
          }
        }
        if (is_loose) {
          BM_face_select_set(bm, efa, true);
        }
      }
    }

    EDBM_selectmode_flush(em);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_loose(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Loose Geometry";
  ot->description = "Select loose geometry based on the selection mode";
  ot->idname = "MESH_OT_select_loose";

  /* api callbacks */
  ot->exec = edbm_select_loose_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend the selection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mirror Operator
 * \{ */

static int edbm_select_mirror_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const int axis_flag = RNA_enum_get(op->ptr, "axis");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  Object *obedit_active = CTX_data_edit_object(C);
  BMEditMesh *em_active = BKE_editmesh_from_object(obedit_active);
  const int select_mode = em_active->bm->selectmode;
  int tot_mirr = 0, tot_fail = 0;

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totvertsel == 0) {
      continue;
    }

    int tot_mirr_iter = 0, tot_fail_iter = 0;

    for (int axis = 0; axis < 3; axis++) {
      if ((1 << axis) & axis_flag) {
        EDBM_select_mirrored(em, obedit->data, axis, extend, &tot_mirr_iter, &tot_fail_iter);
      }
    }

    if (tot_mirr_iter) {
      EDBM_selectmode_flush(em);

      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    }

    tot_fail += tot_fail_iter;
    tot_mirr += tot_mirr_iter;
  }
  MEM_freeN(objects);

  if (tot_mirr || tot_fail) {
    ED_mesh_report_mirror_ex(op, tot_mirr, tot_fail, select_mode);
  }
  return OPERATOR_FINISHED;
}

void MESH_OT_select_mirror(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Mirror";
  ot->description = "Select mesh items at mirrored locations";
  ot->idname = "MESH_OT_select_mirror";

  /* api callbacks */
  ot->exec = edbm_select_mirror_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_enum_flag(ot->srna, "axis", rna_enum_axis_flag_xyz_items, (1 << 0), "Axis", "");

  RNA_def_boolean(ot->srna, "extend", 0, "Extend", "Extend the existing selection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select More Operator
 * \{ */

static int edbm_select_more_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool use_face_step = RNA_boolean_get(op->ptr, "use_face_step");

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if ((bm->totvertsel == 0) && (bm->totedgesel == 0) && (bm->totfacesel == 0)) {
      continue;
    }

    EDBM_select_more(em, use_face_step);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_more(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select More";
  ot->idname = "MESH_OT_select_more";
  ot->description = "Select more vertices, edges or faces connected to initial selection";

  /* api callbacks */
  ot->exec = edbm_select_more_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "use_face_step", true, "Face Step", "Connected faces (instead of edges)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select More Operator
 * \{ */

static int edbm_select_less_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool use_face_step = RNA_boolean_get(op->ptr, "use_face_step");

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if ((bm->totvertsel == 0) && (bm->totedgesel == 0) && (bm->totfacesel == 0)) {
      continue;
    }

    EDBM_select_less(em, use_face_step);
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_less(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Less";
  ot->idname = "MESH_OT_select_less";
  ot->description = "Deselect vertices, edges or faces at the boundary of each selection region";

  /* api callbacks */
  ot->exec = edbm_select_less_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "use_face_step", true, "Face Step", "Connected faces (instead of edges)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select N'th Operator
 * \{ */

/**
 * Check if we're connected to another selected edge.
 */
static bool bm_edge_is_select_isolated(BMEdge *e)
{
  BMIter viter;
  BMVert *v;

  BM_ITER_ELEM (v, &viter, e, BM_VERTS_OF_EDGE) {
    BMIter eiter;
    BMEdge *e_other;

    BM_ITER_ELEM (e_other, &eiter, v, BM_EDGES_OF_VERT) {
      if ((e_other != e) && BM_elem_flag_test(e_other, BM_ELEM_SELECT)) {
        return false;
      }
    }
  }
  return true;
}

/* Walk all reachable elements of the same type as h_act in breadth-first
 * order, starting from h_act. Deselects elements if the depth when they
 * are reached is not a multiple of "nth". */
static void walker_deselect_nth(BMEditMesh *em,
                                const struct CheckerIntervalParams *op_params,
                                BMHeader *h_act)
{
  BMElem *ele;
  BMesh *bm = em->bm;
  BMWalker walker;
  BMIter iter;
  int walktype = 0, itertype = 0, flushtype = 0;
  short mask_vert = 0, mask_edge = 0, mask_face = 0;

  /* No active element from which to start - nothing to do */
  if (h_act == NULL) {
    return;
  }

  /* Determine which type of iter, walker, and select flush to use
   * based on type of the elements being deselected */
  switch (h_act->htype) {
    case BM_VERT:
      itertype = BM_VERTS_OF_MESH;
      walktype = BMW_CONNECTED_VERTEX;
      flushtype = SCE_SELECT_VERTEX;
      mask_vert = BMO_ELE_TAG;
      break;
    case BM_EDGE:
      /* When an edge has no connected-selected edges,
       * use face-stepping (supports edge-rings) */
      itertype = BM_EDGES_OF_MESH;
      walktype = bm_edge_is_select_isolated((BMEdge *)h_act) ? BMW_FACE_SHELL : BMW_VERT_SHELL;
      flushtype = SCE_SELECT_EDGE;
      mask_edge = BMO_ELE_TAG;
      break;
    case BM_FACE:
      itertype = BM_FACES_OF_MESH;
      walktype = BMW_ISLAND;
      flushtype = SCE_SELECT_FACE;
      mask_face = BMO_ELE_TAG;
      break;
  }

  /* grr, shouldn't need to alloc BMO flags here */
  BM_mesh_elem_toolflags_ensure(bm);

  /* Walker restrictions uses BMO flags, not header flags,
   * so transfer BM_ELEM_SELECT from HFlags onto a BMO flag layer. */
  BMO_push(bm, NULL);
  BM_ITER_MESH (ele, &iter, bm, itertype) {
    if (BM_elem_flag_test(ele, BM_ELEM_SELECT)) {
      BMO_elem_flag_enable(bm, (BMElemF *)ele, BMO_ELE_TAG);
    }
  }

  /* Walk over selected elements starting at active */
  BMW_init(&walker,
           bm,
           walktype,
           mask_vert,
           mask_edge,
           mask_face,
           BMW_FLAG_NOP, /* don't use BMW_FLAG_TEST_HIDDEN here since we want to desel all */
           BMW_NIL_LAY);

  /* use tag to avoid touching the same verts twice */
  BM_ITER_MESH (ele, &iter, bm, itertype) {
    BM_elem_flag_disable(ele, BM_ELEM_TAG);
  }

  BLI_assert(walker.order == BMW_BREADTH_FIRST);
  for (ele = BMW_begin(&walker, h_act); ele != NULL; ele = BMW_step(&walker)) {
    if (!BM_elem_flag_test(ele, BM_ELEM_TAG)) {
      /* Deselect elements that aren't at "nth" depth from active */
      const int depth = BMW_current_depth(&walker) - 1;
      if (!WM_operator_properties_checker_interval_test(op_params, depth)) {
        BM_elem_select_set(bm, ele, false);
      }
      BM_elem_flag_enable(ele, BM_ELEM_TAG);
    }
  }
  BMW_end(&walker);

  BMO_pop(bm);

  /* Flush selection up */
  EDBM_selectmode_flush_ex(em, flushtype);
}

static void deselect_nth_active(BMEditMesh *em, BMVert **r_eve, BMEdge **r_eed, BMFace **r_efa)
{
  BMIter iter;
  BMElem *ele;

  *r_eve = NULL;
  *r_eed = NULL;
  *r_efa = NULL;

  EDBM_selectmode_flush(em);
  ele = BM_mesh_active_elem_get(em->bm);

  if (ele && BM_elem_flag_test(ele, BM_ELEM_SELECT)) {
    switch (ele->head.htype) {
      case BM_VERT:
        *r_eve = (BMVert *)ele;
        return;
      case BM_EDGE:
        *r_eed = (BMEdge *)ele;
        return;
      case BM_FACE:
        *r_efa = (BMFace *)ele;
        return;
    }
  }

  if (em->selectmode & SCE_SELECT_VERTEX) {
    BMVert *v;
    BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
      if (BM_elem_flag_test(v, BM_ELEM_SELECT)) {
        *r_eve = v;
        return;
      }
    }
  }
  else if (em->selectmode & SCE_SELECT_EDGE) {
    BMEdge *e;
    BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        *r_eed = e;
        return;
      }
    }
  }
  else if (em->selectmode & SCE_SELECT_FACE) {
    BMFace *f = BM_mesh_active_face_get(em->bm, true, false);
    if (f && BM_elem_flag_test(f, BM_ELEM_SELECT)) {
      *r_efa = f;
      return;
    }
  }
}

static bool edbm_deselect_nth(BMEditMesh *em, const struct CheckerIntervalParams *op_params)
{
  BMVert *v;
  BMEdge *e;
  BMFace *f;

  deselect_nth_active(em, &v, &e, &f);

  if (v) {
    walker_deselect_nth(em, op_params, &v->head);
    return true;
  }
  if (e) {
    walker_deselect_nth(em, op_params, &e->head);
    return true;
  }
  if (f) {
    walker_deselect_nth(em, op_params, &f->head);
    return true;
  }

  return false;
}

static int edbm_select_nth_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  struct CheckerIntervalParams op_params;
  WM_operator_properties_checker_interval_from_op(op, &op_params);
  bool found_active_elt = false;

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if ((em->bm->totvertsel == 0) && (em->bm->totedgesel == 0) && (em->bm->totfacesel == 0)) {
      continue;
    }

    if (edbm_deselect_nth(em, &op_params) == true) {
      found_active_elt = true;
      EDBM_update(obedit->data,
                  &(const struct EDBMUpdate_Params){
                      .calc_looptri = false,
                      .calc_normals = false,
                      .is_destructive = false,
                  });
    }
  }
  MEM_freeN(objects);

  if (!found_active_elt) {
    BKE_report(op->reports, RPT_ERROR, "Mesh object(s) have no active vertex/edge/face");
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_select_nth(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Checker Deselect";
  ot->idname = "MESH_OT_select_nth";
  ot->description = "Deselect every Nth element starting from the active vertex, edge or face";

  /* api callbacks */
  ot->exec = edbm_select_nth_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_checker_interval(ot, false);
}

void em_setup_viewcontext(bContext *C, ViewContext *vc)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ED_view3d_viewcontext_init(C, vc, depsgraph);

  if (vc->obedit) {
    vc->em = BKE_editmesh_from_object(vc->obedit);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Sharp Edges Operator
 * \{ */

static int edbm_select_sharp_edges_exec(bContext *C, wmOperator *op)
{
  /* Find edges that have exactly two neighboring faces,
   * check the angle between those faces, and if angle is
   * small enough, select the edge
   */
  const float angle_limit_cos = cosf(RNA_float_get(op->ptr, "sharpness"));

  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMIter iter;
    BMEdge *e;
    BMLoop *l1, *l2;

    BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_HIDDEN) == false && BM_edge_loop_pair(e, &l1, &l2)) {
        /* edge has exactly two neighboring faces, check angle */
        const float angle_cos = dot_v3v3(l1->f->no, l2->f->no);

        if (angle_cos < angle_limit_cos) {
          BM_edge_select_set(em->bm, e, true);
        }
      }
    }

    if ((em->bm->selectmode & (SCE_SELECT_VERTEX | SCE_SELECT_EDGE)) == 0) {
      /* Since we can't select individual edges, select faces connected to them. */
      EDBM_selectmode_convert(em, SCE_SELECT_EDGE, SCE_SELECT_FACE);
    }
    else {
      EDBM_selectmode_flush(em);
    }
    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_edges_select_sharp(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Sharp Edges";
  ot->description = "Select all sharp enough edges";
  ot->idname = "MESH_OT_edges_select_sharp";

  /* api callbacks */
  ot->exec = edbm_select_sharp_edges_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  prop = RNA_def_float_rotation(ot->srna,
                                "sharpness",
                                0,
                                NULL,
                                DEG2RADF(0.01f),
                                DEG2RADF(180.0f),
                                "Sharpness",
                                "",
                                DEG2RADF(1.0f),
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(30.0f));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Linked Flat Faces Operator
 * \{ */

static int edbm_select_linked_flat_faces_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  const float angle_limit_cos = cosf(RNA_float_get(op->ptr, "sharpness"));

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (bm->totfacesel == 0) {
      continue;
    }

    BLI_LINKSTACK_DECLARE(stack, BMFace *);

    BMIter iter, liter, liter2;
    BMFace *f;
    BMLoop *l, *l2;

    BM_mesh_elem_hflag_disable_all(bm, BM_FACE, BM_ELEM_TAG, false);

    BLI_LINKSTACK_INIT(stack);

    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if ((BM_elem_flag_test(f, BM_ELEM_HIDDEN) != 0) ||
          (BM_elem_flag_test(f, BM_ELEM_TAG) != 0) ||
          (BM_elem_flag_test(f, BM_ELEM_SELECT) == 0)) {
        continue;
      }

      BLI_assert(BLI_LINKSTACK_SIZE(stack) == 0);

      do {
        BM_face_select_set(bm, f, true);

        BM_elem_flag_enable(f, BM_ELEM_TAG);

        BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
          BM_ITER_ELEM (l2, &liter2, l, BM_LOOPS_OF_LOOP) {
            float angle_cos;

            if (BM_elem_flag_test(l2->f, BM_ELEM_TAG) ||
                BM_elem_flag_test(l2->f, BM_ELEM_HIDDEN)) {
              continue;
            }

            angle_cos = dot_v3v3(f->no, l2->f->no);

            if (angle_cos > angle_limit_cos) {
              BLI_LINKSTACK_PUSH(stack, l2->f);
            }
          }
        }
      } while ((f = BLI_LINKSTACK_POP(stack)));
    }

    BLI_LINKSTACK_FREE(stack);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_faces_select_linked_flat(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Linked Flat Faces";
  ot->description = "Select linked faces by angle";
  ot->idname = "MESH_OT_faces_select_linked_flat";

  /* api callbacks */
  ot->exec = edbm_select_linked_flat_faces_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  prop = RNA_def_float_rotation(ot->srna,
                                "sharpness",
                                0,
                                NULL,
                                DEG2RADF(0.01f),
                                DEG2RADF(180.0f),
                                "Sharpness",
                                "",
                                DEG2RADF(1.0f),
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(1.0f));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Non-Manifold Operator
 * \{ */

static int edbm_select_non_manifold_exec(bContext *C, wmOperator *op)
{
  const bool use_extend = RNA_boolean_get(op->ptr, "extend");
  const bool use_wire = RNA_boolean_get(op->ptr, "use_wire");
  const bool use_boundary = RNA_boolean_get(op->ptr, "use_boundary");
  const bool use_multi_face = RNA_boolean_get(op->ptr, "use_multi_face");
  const bool use_non_contiguous = RNA_boolean_get(op->ptr, "use_non_contiguous");
  const bool use_verts = RNA_boolean_get(op->ptr, "use_verts");

  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMVert *v;
    BMEdge *e;
    BMIter iter;

    if (!use_extend) {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
    }

    /* Selects isolated verts, and edges that do not have 2 neighboring
     * faces
     */

    if (em->selectmode == SCE_SELECT_FACE) {
      BKE_report(op->reports, RPT_ERROR, "Does not work in face selection mode");
      MEM_freeN(objects);
      return OPERATOR_CANCELLED;
    }

    if (use_verts) {
      BM_ITER_MESH (v, &iter, em->bm, BM_VERTS_OF_MESH) {
        if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
          if (!BM_vert_is_manifold(v)) {
            BM_vert_select_set(em->bm, v, true);
          }
        }
      }
    }

    if (use_wire || use_boundary || use_multi_face || use_non_contiguous) {
      BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (!BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
          if ((use_wire && BM_edge_is_wire(e)) || (use_boundary && BM_edge_is_boundary(e)) ||
              (use_non_contiguous && (BM_edge_is_manifold(e) && !BM_edge_is_contiguous(e))) ||
              (use_multi_face && (BM_edge_face_count_is_over(e, 2)))) {
            /* check we never select perfect edge (in test above) */
            BLI_assert(!(BM_edge_is_manifold(e) && BM_edge_is_contiguous(e)));

            BM_edge_select_set(em->bm, e, true);
          }
        }
      }
    }

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);

    EDBM_selectmode_flush(em);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_select_non_manifold(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Non-Manifold";
  ot->description = "Select all non-manifold vertices or edges";
  ot->idname = "MESH_OT_select_non_manifold";

  /* api callbacks */
  ot->exec = edbm_select_non_manifold_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "extend", true, "Extend", "Extend the selection");
  /* edges */
  RNA_def_boolean(ot->srna, "use_wire", true, "Wire", "Wire edges");
  RNA_def_boolean(ot->srna, "use_boundary", true, "Boundaries", "Boundary edges");
  RNA_def_boolean(
      ot->srna, "use_multi_face", true, "Multiple Faces", "Edges shared by more than two faces");
  RNA_def_boolean(ot->srna,
                  "use_non_contiguous",
                  true,
                  "Non Contiguous",
                  "Edges between faces pointing in alternate directions");
  /* verts */
  RNA_def_boolean(
      ot->srna, "use_verts", true, "Vertices", "Vertices connecting multiple face regions");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Random Operator
 * \{ */

static int edbm_select_random_exec(bContext *C, wmOperator *op)
{
  const bool select = (RNA_enum_get(op->ptr, "action") == SEL_SELECT);
  const float randfac = RNA_float_get(op->ptr, "ratio");
  const int seed = WM_operator_properties_select_random_seed_increment_get(op);

  ViewLayer *view_layer = CTX_data_view_layer(C);

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMIter iter;
    int seed_iter = seed;

    /* This gives a consistent result regardless of object order. */
    if (ob_index) {
      seed_iter += BLI_ghashutil_strhash_p(obedit->id.name);
    }

    if (em->selectmode & SCE_SELECT_VERTEX) {
      int elem_map_len = 0;
      BMVert **elem_map = MEM_mallocN(sizeof(*elem_map) * em->bm->totvert, __func__);
      BMVert *eve;
      BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
        if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
          elem_map[elem_map_len++] = eve;
        }
      }

      BLI_array_randomize(elem_map, sizeof(*elem_map), elem_map_len, seed_iter);
      const int count_select = elem_map_len * randfac;
      for (int i = 0; i < count_select; i++) {
        BM_vert_select_set(em->bm, elem_map[i], select);
      }
      MEM_freeN(elem_map);
    }
    else if (em->selectmode & SCE_SELECT_EDGE) {
      int elem_map_len = 0;
      BMEdge **elem_map = MEM_mallocN(sizeof(*elem_map) * em->bm->totedge, __func__);
      BMEdge *eed;
      BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
        if (!BM_elem_flag_test(eed, BM_ELEM_HIDDEN)) {
          elem_map[elem_map_len++] = eed;
        }
      }
      BLI_array_randomize(elem_map, sizeof(*elem_map), elem_map_len, seed_iter);
      const int count_select = elem_map_len * randfac;
      for (int i = 0; i < count_select; i++) {
        BM_edge_select_set(em->bm, elem_map[i], select);
      }
      MEM_freeN(elem_map);
    }
    else {
      int elem_map_len = 0;
      BMFace **elem_map = MEM_mallocN(sizeof(*elem_map) * em->bm->totface, __func__);
      BMFace *efa;
      BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
        if (!BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
          elem_map[elem_map_len++] = efa;
        }
      }
      BLI_array_randomize(elem_map, sizeof(*elem_map), elem_map_len, seed_iter);
      const int count_select = elem_map_len * randfac;
      for (int i = 0; i < count_select; i++) {
        BM_face_select_set(em->bm, elem_map[i], select);
      }
      MEM_freeN(elem_map);
    }

    if (select) {
      /* was EDBM_select_flush, but it over select in edge/face mode */
      EDBM_selectmode_flush(em);
    }
    else {
      EDBM_deselect_flush(em);
    }

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }

  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_random(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Random";
  ot->description = "Randomly select vertices";
  ot->idname = "MESH_OT_select_random";

  /* api callbacks */
  ot->exec = edbm_select_random_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  WM_operator_properties_select_random(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Ungrouped Operator
 * \{ */

static bool edbm_select_ungrouped_poll(bContext *C)
{
  if (ED_operator_editmesh(C)) {
    Object *obedit = CTX_data_edit_object(C);
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    const int cd_dvert_offset = CustomData_get_offset(&em->bm->vdata, CD_MDEFORMVERT);

    const ListBase *defbase = BKE_object_defgroup_list(obedit);
    if ((em->selectmode & SCE_SELECT_VERTEX) == 0) {
      CTX_wm_operator_poll_msg_set(C, "Must be in vertex selection mode");
    }
    else if (BLI_listbase_is_empty(defbase) || cd_dvert_offset == -1) {
      CTX_wm_operator_poll_msg_set(C, "No weights/vertex groups on object");
    }
    else {
      return true;
    }
  }
  return false;
}

static int edbm_select_ungrouped_exec(bContext *C, wmOperator *op)
{
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  ViewLayer *view_layer = CTX_data_view_layer(C);

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    const int cd_dvert_offset = CustomData_get_offset(&em->bm->vdata, CD_MDEFORMVERT);

    if (cd_dvert_offset == -1) {
      continue;
    }

    BMVert *eve;
    BMIter iter;

    bool changed = false;

    if (!extend) {
      if (em->bm->totvertsel) {
        EDBM_flag_disable_all(em, BM_ELEM_SELECT);
        changed = true;
      }
    }

    BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
      if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
        MDeformVert *dv = BM_ELEM_CD_GET_VOID_P(eve, cd_dvert_offset);
        /* no dv or dv set with no weight */
        if (ELEM(NULL, dv, dv->dw)) {
          BM_vert_select_set(em->bm, eve, true);
          changed = true;
        }
      }
    }

    if (changed) {
      EDBM_selectmode_flush(em);
      DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
    }
  }
  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_ungrouped(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Ungrouped";
  ot->idname = "MESH_OT_select_ungrouped";
  ot->description = "Select vertices without a group";

  /* api callbacks */
  ot->exec = edbm_select_ungrouped_exec;
  ot->poll = edbm_select_ungrouped_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend the selection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Axis Operator
 * \{ */

enum {
  SELECT_AXIS_POS = 0,
  SELECT_AXIS_NEG = 1,
  SELECT_AXIS_ALIGN = 2,
};

static int edbm_select_axis_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obedit = CTX_data_edit_object(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMVert *v_act = BM_mesh_active_vert_get(em->bm);
  const int orientation = RNA_enum_get(op->ptr, "orientation");
  const int axis = RNA_enum_get(op->ptr, "axis");
  const int sign = RNA_enum_get(op->ptr, "sign");

  if (v_act == NULL) {
    BKE_report(
        op->reports, RPT_WARNING, "This operator requires an active vertex (last selected)");
    return OPERATOR_CANCELLED;
  }

  const float limit = RNA_float_get(op->ptr, "threshold");

  float value;
  float axis_mat[3][3];

  /* 3D view variables may be NULL, (no need to check in poll function). */
  ED_transform_calc_orientation_from_type_ex(scene,
                                             view_layer,
                                             CTX_wm_view3d(C),
                                             CTX_wm_region_view3d(C),
                                             obedit,
                                             obedit,
                                             orientation,
                                             V3D_AROUND_ACTIVE,
                                             axis_mat);

  const float *axis_vector = axis_mat[axis];

  {
    float vertex_world[3];
    mul_v3_m4v3(vertex_world, obedit->obmat, v_act->co);
    value = dot_v3v3(axis_vector, vertex_world);
  }

  if (sign == SELECT_AXIS_NEG) {
    value += limit;
  }
  else if (sign == SELECT_AXIS_POS) {
    value -= limit;
  }

  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit_iter = objects[ob_index];
    BMEditMesh *em_iter = BKE_editmesh_from_object(obedit_iter);
    BMesh *bm = em_iter->bm;

    if (bm->totvert == bm->totvertsel) {
      continue;
    }

    BMIter iter;
    BMVert *v;
    bool changed = false;

    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN | BM_ELEM_SELECT)) {
        float v_iter_world[3];
        mul_v3_m4v3(v_iter_world, obedit_iter->obmat, v->co);
        const float value_iter = dot_v3v3(axis_vector, v_iter_world);
        switch (sign) {
          case SELECT_AXIS_ALIGN:
            if (fabsf(value_iter - value) < limit) {
              BM_vert_select_set(bm, v, true);
              changed = true;
            }
            break;
          case SELECT_AXIS_NEG:
            if (value_iter < value) {
              BM_vert_select_set(bm, v, true);
              changed = true;
            }
            break;
          case SELECT_AXIS_POS:
            if (value_iter > value) {
              BM_vert_select_set(bm, v, true);
              changed = true;
            }
            break;
        }
      }
    }
    if (changed) {
      EDBM_selectmode_flush(em_iter);
      WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit_iter->data);
      DEG_id_tag_update(obedit_iter->data, ID_RECALC_SELECT);
    }
  }
  MEM_freeN(objects);
  return OPERATOR_FINISHED;
}

void MESH_OT_select_axis(wmOperatorType *ot)
{
  static const EnumPropertyItem axis_sign_items[] = {
      {SELECT_AXIS_POS, "POS", 0, "Positive Axis", ""},
      {SELECT_AXIS_NEG, "NEG", 0, "Negative Axis", ""},
      {SELECT_AXIS_ALIGN, "ALIGN", 0, "Aligned Axis", ""},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Select Axis";
  ot->description = "Select all data in the mesh on a single axis";
  ot->idname = "MESH_OT_select_axis";

  /* api callbacks */
  ot->exec = edbm_select_axis_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_enum(ot->srna,
               "orientation",
               rna_enum_transform_orientation_items,
               V3D_ORIENT_LOCAL,
               "Axis Mode",
               "Axis orientation");
  RNA_def_enum(ot->srna, "sign", axis_sign_items, SELECT_AXIS_POS, "Axis Sign", "Side to select");
  RNA_def_enum(ot->srna,
               "axis",
               rna_enum_axis_xyz_items,
               0,
               "Axis",
               "Select the axis to compare each vertex on");
  RNA_def_float(
      ot->srna, "threshold", 0.0001f, 0.000001f, 50.0f, "Threshold", "", 0.00001f, 10.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Region to Loop Operator
 * \{ */

static int edbm_region_to_loop_exec(bContext *C, wmOperator *UNUSED(op))
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totfacesel == 0) {
      continue;
    }
    BMFace *f;
    BMEdge *e;
    BMIter iter;

    BM_mesh_elem_hflag_disable_all(em->bm, BM_EDGE, BM_ELEM_TAG, false);

    BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
      BMLoop *l1, *l2;
      BMIter liter1, liter2;

      BM_ITER_ELEM (l1, &liter1, f, BM_LOOPS_OF_FACE) {
        int tot = 0, totsel = 0;

        BM_ITER_ELEM (l2, &liter2, l1->e, BM_LOOPS_OF_EDGE) {
          tot++;
          totsel += BM_elem_flag_test(l2->f, BM_ELEM_SELECT) != 0;
        }

        if ((tot != totsel && totsel > 0) || (totsel == 1 && tot == 1)) {
          BM_elem_flag_enable(l1->e, BM_ELEM_TAG);
        }
      }
    }

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
      if (BM_elem_flag_test(e, BM_ELEM_TAG)) {
        BM_edge_select_set(em->bm, e, true);
      }
    }

    /* If in face-only select mode, switch to edge select mode so that
     * an edge-only selection is not inconsistent state */
    if (em->selectmode == SCE_SELECT_FACE) {
      em->selectmode = SCE_SELECT_EDGE;
      EDBM_selectmode_set(em);
      EDBM_selectmode_to_scene(C);
    }

    DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_region_to_loop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Boundary Loop";
  ot->idname = "MESH_OT_region_to_loop";
  ot->description = "Select boundary edges around the selected faces";

  /* api callbacks */
  ot->exec = edbm_region_to_loop_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Loop to Region Operator
 * \{ */

static int loop_find_region(BMLoop *l, int flag, GSet *visit_face_set, BMFace ***region_out)
{
  BMFace **region = NULL;
  BMFace **stack = NULL;
  BLI_array_declare(region);
  BLI_array_declare(stack);
  BMFace *f;

  BLI_array_append(stack, l->f);
  BLI_gset_insert(visit_face_set, l->f);

  while (BLI_array_len(stack) > 0) {
    BMIter liter1, liter2;
    BMLoop *l1, *l2;

    f = BLI_array_pop(stack);
    BLI_array_append(region, f);

    BM_ITER_ELEM (l1, &liter1, f, BM_LOOPS_OF_FACE) {
      if (BM_elem_flag_test(l1->e, flag)) {
        continue;
      }

      BM_ITER_ELEM (l2, &liter2, l1->e, BM_LOOPS_OF_EDGE) {
        /* avoids finding same region twice
         * (otherwise) the logic works fine without */
        if (BM_elem_flag_test(l2->f, BM_ELEM_TAG)) {
          continue;
        }

        if (BLI_gset_add(visit_face_set, l2->f)) {
          BLI_array_append(stack, l2->f);
        }
      }
    }
  }

  BLI_array_free(stack);

  *region_out = region;
  return BLI_array_len(region);
}

static int verg_radial(const void *va, const void *vb)
{
  const BMEdge *e_a = *((const BMEdge **)va);
  const BMEdge *e_b = *((const BMEdge **)vb);

  const int a = BM_edge_face_count(e_a);
  const int b = BM_edge_face_count(e_b);

  if (a > b) {
    return -1;
  }
  if (a < b) {
    return 1;
  }
  return 0;
}

/**
 * This function leaves faces tagged which are a part of the new region.
 *
 * \note faces already tagged are ignored, to avoid finding the same regions twice:
 * important when we have regions with equal face counts, see: T40309
 */
static int loop_find_regions(BMEditMesh *em, const bool selbigger)
{
  GSet *visit_face_set;
  BMIter iter;
  const int edges_len = em->bm->totedgesel;
  BMEdge *e, **edges;
  int count = 0, i;

  visit_face_set = BLI_gset_ptr_new_ex(__func__, edges_len);
  edges = MEM_mallocN(sizeof(*edges) * edges_len, __func__);

  i = 0;
  BM_ITER_MESH (e, &iter, em->bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
      edges[i++] = e;
      BM_elem_flag_enable(e, BM_ELEM_TAG);
    }
    else {
      BM_elem_flag_disable(e, BM_ELEM_TAG);
    }
  }

  /* sort edges by radial cycle length */
  qsort(edges, edges_len, sizeof(*edges), verg_radial);

  for (i = 0; i < edges_len; i++) {
    BMIter liter;
    BMLoop *l;
    BMFace **region = NULL, **region_out;
    int c, tot = 0;

    e = edges[i];

    if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
      continue;
    }

    BM_ITER_ELEM (l, &liter, e, BM_LOOPS_OF_EDGE) {
      if (BLI_gset_haskey(visit_face_set, l->f)) {
        continue;
      }

      c = loop_find_region(l, BM_ELEM_SELECT, visit_face_set, &region_out);

      if (!region || (selbigger ? c >= tot : c < tot)) {
        /* this region is the best seen so far */
        tot = c;
        if (region) {
          /* free the previous best */
          MEM_freeN(region);
        }
        /* track the current region as the new best */
        region = region_out;
      }
      else {
        /* this region is not as good as best so far, just free it */
        MEM_freeN(region_out);
      }
    }

    if (region) {
      int j;

      for (j = 0; j < tot; j++) {
        BM_elem_flag_enable(region[j], BM_ELEM_TAG);
        BM_ITER_ELEM (l, &liter, region[j], BM_LOOPS_OF_FACE) {
          BM_elem_flag_disable(l->e, BM_ELEM_TAG);
        }
      }

      count += tot;

      MEM_freeN(region);
    }
  }

  MEM_freeN(edges);
  BLI_gset_free(visit_face_set, NULL);

  return count;
}

static int edbm_loop_to_region_exec(bContext *C, wmOperator *op)
{
  const bool select_bigger = RNA_boolean_get(op->ptr, "select_bigger");

  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      view_layer, CTX_wm_view3d(C), &objects_len);
  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    BMIter iter;
    BMFace *f;

    /* find the set of regions with smallest number of total faces */
    BM_mesh_elem_hflag_disable_all(em->bm, BM_FACE, BM_ELEM_TAG, false);
    const int a = loop_find_regions(em, select_bigger);
    const int b = loop_find_regions(em, !select_bigger);

    BM_mesh_elem_hflag_disable_all(em->bm, BM_FACE, BM_ELEM_TAG, false);
    loop_find_regions(em, ((a <= b) != select_bigger) ? select_bigger : !select_bigger);

    EDBM_flag_disable_all(em, BM_ELEM_SELECT);

    BM_ITER_MESH (f, &iter, em->bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_TAG) && !BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        BM_face_select_set(em->bm, f, true);
      }
    }

    EDBM_selectmode_flush(em);

    DEG_id_tag_update(obedit->data, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, obedit->data);
  }
  MEM_freeN(objects);

  return OPERATOR_FINISHED;
}

void MESH_OT_loop_to_region(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Loop Inner-Region";
  ot->idname = "MESH_OT_loop_to_region";
  ot->description = "Select region of faces inside of a selected loop of edges";

  /* api callbacks */
  ot->exec = edbm_loop_to_region_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "select_bigger",
                  0,
                  "Select Bigger",
                  "Select bigger regions instead of smaller ones");
}

/** \} */
