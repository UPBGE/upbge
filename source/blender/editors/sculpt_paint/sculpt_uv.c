/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2002-2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edsculpt
 * UV Sculpt tools.
 */

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_math_base_safe.h"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_image.h"
#include "BKE_mesh_mapping.h"
#include "BKE_paint.h"

#include "DEG_depsgraph.h"

#include "ED_image.h"
#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_uvedit.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "paint_intern.h"
#include "uvedit_intern.h"

#include "UI_view2d.h"

/* When set, the UV element is on the boundary of the graph.
 * i.e. Instead of a 2-dimensional laplace operator, use a 1-dimensional version.
 * Visually, UV elements on the graph boundary appear as borders of the UV Island. */
#define MARK_BOUNDARY 1

typedef struct UvAdjacencyElement {
  /* pointer to original uvelement */
  UvElement *element;
  /* uv pointer for convenience. Caution, this points to the original UVs! */
  float *uv;
  /* Are we on locked in place? */
  bool is_locked;
  /* Are we on the boundary? */
  bool is_boundary;
} UvAdjacencyElement;

typedef struct UvEdge {
  uint uv1;
  uint uv2;
  /* Are we in the interior? */
  bool is_interior;
} UvEdge;

typedef struct UVInitialStrokeElement {
  /* index to unique uv */
  int uv;

  /* strength of brush on initial position */
  float strength;

  /* initial uv position */
  float initial_uv[2];
} UVInitialStrokeElement;

typedef struct UVInitialStroke {
  /* Initial Selection,for grab brushes for instance */
  UVInitialStrokeElement *initialSelection;

  /* Total initially selected UV's. */
  int totalInitialSelected;

  /* initial mouse coordinates */
  float init_coord[2];
} UVInitialStroke;

/* custom data for uv smoothing brush */
typedef struct UvSculptData {
  /* Contains the first of each set of coincident UV's.
   * These will be used to perform smoothing on and propagate the changes
   * to their coincident UV's */
  UvAdjacencyElement *uv;

  /* Total number of unique UVs. */
  int totalUniqueUvs;

  /* Edges used for adjacency info, used with laplacian smoothing */
  UvEdge *uvedges;

  /* Total number of #UvEdge. */
  int totalUvEdges;

  /* data for initial stroke, used by tools like grab */
  UVInitialStroke *initial_stroke;

  /* timer to be used for airbrush-type brush */
  wmTimer *timer;

  /* to determine quickly adjacent UV's */
  UvElementMap *elementMap;

  /* uvsmooth Paint for fast reference */
  Paint *uvsculpt;

  /* tool to use. duplicating here to change if modifier keys are pressed */
  char tool;

  /* store invert flag here */
  char invert;

  /* Is constrain to image bounds active? */
  bool constrain_to_bounds;

  /* Base for constrain_to_bounds. */
  float uv_base_offset[2];
} UvSculptData;

static void apply_sculpt_data_constraints(UvSculptData *sculptdata, float uv[2])
{
  if (!sculptdata->constrain_to_bounds) {
    return;
  }
  float u = sculptdata->uv_base_offset[0];
  float v = sculptdata->uv_base_offset[1];
  uv[0] = clamp_f(uv[0], u, u + 1.0f);
  uv[1] = clamp_f(uv[1], v, v + 1.0f);
}

/*********** Improved Laplacian Relaxation Operator ************************/
/* original code by Raul Fernandez Hernandez "farsthary"                   *
 * adapted to uv smoothing by Antony Riakiatakis                           *
 ***************************************************************************/

typedef struct Temp_UvData {
  float sum_co[2], p[2], b[2], sum_b[2];
  int ncounter;
} Temp_UVData;

static void HC_relaxation_iteration_uv(BMEditMesh *em,
                                       UvSculptData *sculptdata,
                                       const float mouse_coord[2],
                                       float alpha,
                                       float radius,
                                       float aspectRatio)
{
  Temp_UVData *tmp_uvdata;
  float diff[2];
  int i;
  float radius_root = sqrtf(radius);
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

  tmp_uvdata = (Temp_UVData *)MEM_callocN(sculptdata->totalUniqueUvs * sizeof(Temp_UVData),
                                          "Temporal data");

  /* counting neighbors */
  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    tmp_uvdata[tmpedge->uv1].ncounter++;
    tmp_uvdata[tmpedge->uv2].ncounter++;

    add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_co, sculptdata->uv[tmpedge->uv1].uv);
    add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_co, sculptdata->uv[tmpedge->uv2].uv);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    copy_v2_v2(diff, tmp_uvdata[i].sum_co);
    mul_v2_fl(diff, 1.0f / tmp_uvdata[i].ncounter);
    copy_v2_v2(tmp_uvdata[i].p, diff);

    tmp_uvdata[i].b[0] = diff[0] - sculptdata->uv[i].uv[0];
    tmp_uvdata[i].b[1] = diff[1] - sculptdata->uv[i].uv[1];
  }

  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_b, tmp_uvdata[tmpedge->uv2].b);
    add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_b, tmp_uvdata[tmpedge->uv1].b);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    if (sculptdata->uv[i].is_locked) {
      continue;
    }

    sub_v2_v2v2(diff, sculptdata->uv[i].uv, mouse_coord);
    diff[1] /= aspectRatio;
    float dist = dot_v2v2(diff, diff);
    if (dist <= radius) {
      UvElement *element;
      float strength;
      strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

      sculptdata->uv[i].uv[0] = (1.0f - strength) * sculptdata->uv[i].uv[0] +
                                strength *
                                    (tmp_uvdata[i].p[0] -
                                     0.5f * (tmp_uvdata[i].b[0] +
                                             tmp_uvdata[i].sum_b[0] / tmp_uvdata[i].ncounter));
      sculptdata->uv[i].uv[1] = (1.0f - strength) * sculptdata->uv[i].uv[1] +
                                strength *
                                    (tmp_uvdata[i].p[1] -
                                     0.5f * (tmp_uvdata[i].b[1] +
                                             tmp_uvdata[i].sum_b[1] / tmp_uvdata[i].ncounter));

      apply_sculpt_data_constraints(sculptdata, sculptdata->uv[i].uv);

      for (element = sculptdata->uv[i].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[i].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
      }
    }
  }

  MEM_SAFE_FREE(tmp_uvdata);
}

/* Legacy version which only does laplacian relaxation.
 * Probably a little faster as it caches UvEdges.
 * Mostly preserved for comparison with `HC_relaxation_iteration_uv`.
 * Once the HC method has been merged into `relaxation_iteration_uv`,
 * all the `HC_*` and `laplacian_*` specific functions can probably be removed.
 */

static void laplacian_relaxation_iteration_uv(BMEditMesh *em,
                                              UvSculptData *sculptdata,
                                              const float mouse_coord[2],
                                              float alpha,
                                              float radius,
                                              float aspectRatio)
{
  Temp_UVData *tmp_uvdata;
  float diff[2];
  int i;
  float radius_root = sqrtf(radius);
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

  tmp_uvdata = (Temp_UVData *)MEM_callocN(sculptdata->totalUniqueUvs * sizeof(Temp_UVData),
                                          "Temporal data");

  /* counting neighbors */
  for (i = 0; i < sculptdata->totalUvEdges; i++) {
    UvEdge *tmpedge = sculptdata->uvedges + i;
    bool code1 = sculptdata->uv[sculptdata->uvedges[i].uv1].is_boundary;
    bool code2 = sculptdata->uv[sculptdata->uvedges[i].uv2].is_boundary;
    if (code1 || (code1 == code2)) {
      tmp_uvdata[tmpedge->uv2].ncounter++;
      add_v2_v2(tmp_uvdata[tmpedge->uv2].sum_co, sculptdata->uv[tmpedge->uv1].uv);
    }
    if (code2 || (code1 == code2)) {
      tmp_uvdata[tmpedge->uv1].ncounter++;
      add_v2_v2(tmp_uvdata[tmpedge->uv1].sum_co, sculptdata->uv[tmpedge->uv2].uv);
    }
  }

  /* Original Laplacian algorithm included removal of normal component of translation.
   * here it is not needed since we translate along the UV plane always. */
  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    copy_v2_v2(tmp_uvdata[i].p, tmp_uvdata[i].sum_co);
    mul_v2_fl(tmp_uvdata[i].p, 1.0f / tmp_uvdata[i].ncounter);
  }

  for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
    if (sculptdata->uv[i].is_locked) {
      continue;
    }

    sub_v2_v2v2(diff, sculptdata->uv[i].uv, mouse_coord);
    diff[1] /= aspectRatio;
    float dist = dot_v2v2(diff, diff);
    if (dist <= radius) {
      UvElement *element;
      float strength;
      strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

      sculptdata->uv[i].uv[0] = (1.0f - strength) * sculptdata->uv[i].uv[0] +
                                strength * tmp_uvdata[i].p[0];
      sculptdata->uv[i].uv[1] = (1.0f - strength) * sculptdata->uv[i].uv[1] +
                                strength * tmp_uvdata[i].p[1];

      apply_sculpt_data_constraints(sculptdata, sculptdata->uv[i].uv);

      for (element = sculptdata->uv[i].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[i].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
      }
    }
  }

  MEM_SAFE_FREE(tmp_uvdata);
}

static void add_weighted_edge(float (*delta_buf)[3],
                              const UvElement *storage,
                              const UvElement *ele_next,
                              const UvElement *ele_prev,
                              const MLoopUV *luv_next,
                              const MLoopUV *luv_prev,
                              const float weight)
{
  float delta[2];
  sub_v2_v2v2(delta, luv_next->uv, luv_prev->uv);

  bool code1 = (ele_prev->flag & MARK_BOUNDARY);
  bool code2 = (ele_next->flag & MARK_BOUNDARY);
  if (code1 || (code1 == code2)) {
    int index_next = ele_next - storage;
    delta_buf[index_next][0] -= delta[0] * weight;
    delta_buf[index_next][1] -= delta[1] * weight;
    delta_buf[index_next][2] += fabsf(weight);
  }
  if (code2 || (code1 == code2)) {
    int index_prev = ele_prev - storage;
    delta_buf[index_prev][0] += delta[0] * weight;
    delta_buf[index_prev][1] += delta[1] * weight;
    delta_buf[index_prev][2] += fabsf(weight);
  }
}

static float tri_weight_v3(int method, const float *v1, const float *v2, const float *v3)
{
  switch (method) {
    case UV_SCULPT_TOOL_RELAX_LAPLACIAN:
    case UV_SCULPT_TOOL_RELAX_HC:
      return 1.0f;
    case UV_SCULPT_TOOL_RELAX_COTAN:
      return cotangent_tri_weight_v3(v1, v2, v3);
    default:
      BLI_assert_unreachable();
  }
  return 0.0f;
}

static void relaxation_iteration_uv(BMEditMesh *em,
                                    UvSculptData *sculptdata,
                                    const float mouse_coord[2],
                                    const float alpha,
                                    const float radius_squared,
                                    const float aspect_ratio,
                                    const int method)
{
  if (method == UV_SCULPT_TOOL_RELAX_HC) {
    HC_relaxation_iteration_uv(em, sculptdata, mouse_coord, alpha, radius_squared, aspect_ratio);
    return;
  }
  if (method == UV_SCULPT_TOOL_RELAX_LAPLACIAN) {
    laplacian_relaxation_iteration_uv(
        em, sculptdata, mouse_coord, alpha, radius_squared, aspect_ratio);
    return;
  }

  struct UvElement **head_table = BM_uv_element_map_ensure_head_table(sculptdata->elementMap);

  const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);
  BLI_assert(cd_loop_uv_offset >= 0);

  const int total_uvs = sculptdata->elementMap->total_uvs;
  float(*delta_buf)[3] = (float(*)[3])MEM_callocN(total_uvs * sizeof(float[3]), __func__);

  const UvElement *storage = sculptdata->elementMap->storage;
  for (int j = 0; j < total_uvs; j++) {
    const UvElement *ele_curr = storage + j;
    const BMFace *efa = ele_curr->l->f;
    const UvElement *ele_next = BM_uv_element_get(sculptdata->elementMap, efa, ele_curr->l->next);
    const UvElement *ele_prev = BM_uv_element_get(sculptdata->elementMap, efa, ele_curr->l->prev);

    const float *v_curr_co = ele_curr->l->v->co;
    const float *v_prev_co = ele_prev->l->v->co;
    const float *v_next_co = ele_next->l->v->co;

    const MLoopUV *luv_curr = BM_ELEM_CD_GET_VOID_P(ele_curr->l, cd_loop_uv_offset);
    const MLoopUV *luv_next = BM_ELEM_CD_GET_VOID_P(ele_next->l, cd_loop_uv_offset);
    const MLoopUV *luv_prev = BM_ELEM_CD_GET_VOID_P(ele_prev->l, cd_loop_uv_offset);

    const UvElement *head_curr = head_table[ele_curr - sculptdata->elementMap->storage];
    const UvElement *head_next = head_table[ele_next - sculptdata->elementMap->storage];
    const UvElement *head_prev = head_table[ele_prev - sculptdata->elementMap->storage];

    /* If the mesh is triangulated with no boundaries, only one edge is required. */
    const float weight_curr = tri_weight_v3(method, v_curr_co, v_prev_co, v_next_co);
    add_weighted_edge(delta_buf, storage, head_next, head_prev, luv_next, luv_prev, weight_curr);

    /* Triangulated with a boundary? We need the incoming edges to solve the boundary. */
    const float weight_prev = tri_weight_v3(method, v_prev_co, v_curr_co, v_next_co);
    add_weighted_edge(delta_buf, storage, head_next, head_curr, luv_next, luv_curr, weight_prev);

    if (method == UV_SCULPT_TOOL_RELAX_LAPLACIAN) {
      /* Laplacian method has zero weights on virtual edges. */
      continue;
    }

    /* Meshes with quads (or other n-gons) need "virtual" edges too. */
    const float weight_next = tri_weight_v3(method, v_next_co, v_curr_co, v_prev_co);
    add_weighted_edge(delta_buf, storage, head_prev, head_curr, luv_prev, luv_curr, weight_next);
  }

  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);
  for (int i = 0; i < sculptdata->totalUniqueUvs; i++) {
    UvAdjacencyElement *adj_el = &sculptdata->uv[i];
    if (adj_el->is_locked) {
      continue; /* Locked UVs can't move. */
    }

    /* Is UV within brush's influence? */
    float diff[2];
    sub_v2_v2v2(diff, adj_el->uv, mouse_coord);
    diff[1] /= aspect_ratio;
    const float dist_squared = len_squared_v2(diff);
    if (dist_squared > radius_squared) {
      continue;
    }
    const float strength = alpha * BKE_brush_curve_strength_clamped(
                                       brush, sqrtf(dist_squared), sqrtf(radius_squared));

    const float *delta_sum = delta_buf[adj_el->element - storage];

    {
      MLoopUV *luv = BM_ELEM_CD_GET_VOID_P(adj_el->element->l, cd_loop_uv_offset);
      BLI_assert(adj_el->uv == luv->uv); /* Only true for head. */
      adj_el->uv[0] = luv->uv[0] + strength * safe_divide(delta_sum[0], delta_sum[2]);
      adj_el->uv[1] = luv->uv[1] + strength * safe_divide(delta_sum[1], delta_sum[2]);
      apply_sculpt_data_constraints(sculptdata, adj_el->uv);
    }

    /* Copy UV co-ordinates to all UvElements. */
    UvElement *tail = adj_el->element;
    while (tail) {
      MLoopUV *luv = BM_ELEM_CD_GET_VOID_P(tail->l, cd_loop_uv_offset);
      copy_v2_v2(luv->uv, adj_el->uv);
      tail = tail->next;
      if (tail && tail->separate) {
        break;
      }
    }
  }

  MEM_SAFE_FREE(delta_buf);
}

static void uv_sculpt_stroke_apply(bContext *C,
                                   wmOperator *op,
                                   const wmEvent *event,
                                   Object *obedit)
{
  float co[2], radius, radius_root;
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  uint tool;
  UvSculptData *sculptdata = (UvSculptData *)op->customdata;
  SpaceImage *sima;
  int invert;
  int width, height;
  float aspectRatio;
  float alpha, zoomx, zoomy;
  Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);
  ToolSettings *toolsettings = CTX_data_tool_settings(C);
  tool = sculptdata->tool;
  invert = sculptdata->invert ? -1 : 1;
  alpha = BKE_brush_alpha_get(scene, brush);
  UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &co[0], &co[1]);

  sima = CTX_wm_space_image(C);
  ED_space_image_get_size(sima, &width, &height);
  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

  radius = BKE_brush_size_get(scene, brush) / (width * zoomx);
  aspectRatio = width / (float)height;

  /* We will compare squares to save some computation */
  radius = radius * radius;
  radius_root = sqrtf(radius);

  /*
   * Pinch Tool
   */
  if (tool == UV_SCULPT_TOOL_PINCH) {
    int i;
    alpha *= invert;
    for (i = 0; i < sculptdata->totalUniqueUvs; i++) {
      if (sculptdata->uv[i].is_locked) {
        continue;
      }

      float diff[2];
      sub_v2_v2v2(diff, sculptdata->uv[i].uv, co);
      diff[1] /= aspectRatio;
      float dist = dot_v2v2(diff, diff);
      if (dist <= radius) {
        UvElement *element;
        float strength;
        strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);
        normalize_v2(diff);

        sculptdata->uv[i].uv[0] -= strength * diff[0] * 0.001f;
        sculptdata->uv[i].uv[1] -= strength * diff[1] * 0.001f;

        apply_sculpt_data_constraints(sculptdata, sculptdata->uv[i].uv);

        for (element = sculptdata->uv[i].element; element; element = element->next) {
          MLoopUV *luv;
          BMLoop *l;

          if (element->separate && element != sculptdata->uv[i].element) {
            break;
          }

          l = element->l;
          luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
          copy_v2_v2(luv->uv, sculptdata->uv[i].uv);
        }
      }
    }
  }

  /*
   * Relax Tool
   */
  else if (tool == UV_SCULPT_TOOL_RELAX) {
    relaxation_iteration_uv(
        em, sculptdata, co, alpha, radius, aspectRatio, toolsettings->uv_relax_method);
  }

  /*
   * Grab Tool
   */
  else if (tool == UV_SCULPT_TOOL_GRAB) {
    int i;
    float diff[2];
    sub_v2_v2v2(diff, co, sculptdata->initial_stroke->init_coord);

    for (i = 0; i < sculptdata->initial_stroke->totalInitialSelected; i++) {
      UvElement *element;
      int uvindex = sculptdata->initial_stroke->initialSelection[i].uv;
      float strength = sculptdata->initial_stroke->initialSelection[i].strength;
      sculptdata->uv[uvindex].uv[0] =
          sculptdata->initial_stroke->initialSelection[i].initial_uv[0] + strength * diff[0];
      sculptdata->uv[uvindex].uv[1] =
          sculptdata->initial_stroke->initialSelection[i].initial_uv[1] + strength * diff[1];

      apply_sculpt_data_constraints(sculptdata, sculptdata->uv[uvindex].uv);

      for (element = sculptdata->uv[uvindex].element; element; element = element->next) {
        MLoopUV *luv;
        BMLoop *l;

        if (element->separate && element != sculptdata->uv[uvindex].element) {
          break;
        }

        l = element->l;
        luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);
        copy_v2_v2(luv->uv, sculptdata->uv[uvindex].uv);
      }
    }
    if (sima->flag & SI_LIVE_UNWRAP) {
      ED_uvedit_live_unwrap_re_solve();
    }
  }
}

static void uv_sculpt_stroke_exit(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima->flag & SI_LIVE_UNWRAP) {
    ED_uvedit_live_unwrap_end(false);
  }
  UvSculptData *data = op->customdata;
  if (data->timer) {
    WM_event_remove_timer(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
  }
  BM_uv_element_map_free(data->elementMap);
  data->elementMap = NULL;
  MEM_SAFE_FREE(data->uv);
  MEM_SAFE_FREE(data->uvedges);
  if (data->initial_stroke) {
    MEM_SAFE_FREE(data->initial_stroke->initialSelection);
    MEM_SAFE_FREE(data->initial_stroke);
  }

  MEM_SAFE_FREE(data);
  op->customdata = NULL;
}

static int uv_element_offset_from_face_get(
    UvElementMap *map, BMFace *efa, BMLoop *l, int island_index, const bool doIslands)
{
  UvElement *element = BM_uv_element_get(map, efa, l);
  if (!element || (doIslands && element->island != island_index)) {
    return -1;
  }
  return element - map->storage;
}

static uint uv_edge_hash(const void *key)
{
  const UvEdge *edge = key;
  return (BLI_ghashutil_uinthash(edge->uv2) + BLI_ghashutil_uinthash(edge->uv1));
}

static bool uv_edge_compare(const void *a, const void *b)
{
  const UvEdge *edge1 = a;
  const UvEdge *edge2 = b;

  if ((edge1->uv1 == edge2->uv1) && (edge1->uv2 == edge2->uv2)) {
    return false;
  }
  return true;
}

static void set_element_flag(UvElement *element, const int flag)
{
  while (element) {
    element->flag |= flag;
    element = element->next;
    if (!element || element->separate) {
      break;
    }
  }
}

static UvSculptData *uv_sculpt_stroke_init(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene *scene = CTX_data_scene(C);
  Object *obedit = CTX_data_edit_object(C);
  ToolSettings *ts = scene->toolsettings;
  UvSculptData *data = MEM_callocN(sizeof(*data), "UV Smooth Brush Data");
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  BMesh *bm = em->bm;

  op->customdata = data;

  BKE_curvemapping_init(ts->uvsculpt->paint.brush->curve);

  if (data) {
    ARegion *region = CTX_wm_region(C);
    float co[2];
    BMFace *efa;
    MLoopUV *luv;
    BMLoop *l;
    BMIter iter, liter;

    UvEdge *edges;
    GHash *edgeHash;
    GHashIterator gh_iter;

    bool do_island_optimization = !(ts->uv_sculpt_settings & UV_SCULPT_ALL_ISLANDS);
    int island_index = 0;
    data->tool = (RNA_enum_get(op->ptr, "mode") == BRUSH_STROKE_SMOOTH) ?
                     UV_SCULPT_TOOL_RELAX :
                     ts->uvsculpt->paint.brush->uv_sculpt_tool;
    data->invert = (RNA_enum_get(op->ptr, "mode") == BRUSH_STROKE_INVERT) ? 1 : 0;

    data->uvsculpt = &ts->uvsculpt->paint;

    /* Winding was added to island detection in 5197aa04c6bd
     * However the sculpt tools can flip faces, potentially creating orphaned islands.
     * See T100132 */
    bool use_winding = false;
    data->elementMap = BM_uv_element_map_create(
        bm, scene, false, use_winding, do_island_optimization);

    if (!data->elementMap) {
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    /* Mouse coordinates, useful for some functions like grab and sculpt all islands */
    UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &co[0], &co[1]);

    /* we need to find the active island here */
    if (do_island_optimization) {
      UvElement *element;
      UvNearestHit hit = UV_NEAREST_HIT_INIT_MAX(&region->v2d);
      uv_find_nearest_vert(scene, obedit, co, 0.0f, &hit);

      element = BM_uv_element_get(data->elementMap, hit.efa, hit.l);
      island_index = element->island;
    }

    /* Count 'unique' UV's */
    int unique_uvs = data->elementMap->total_unique_uvs;
    if (do_island_optimization) {
      unique_uvs = data->elementMap->island_total_unique_uvs[island_index];
    }

    /* Allocate the unique uv buffers */
    data->uv = MEM_callocN(sizeof(*data->uv) * unique_uvs, "uv_brush_unique_uvs");
    /* Holds, for each UvElement in elementMap, an index of its unique UV. */
    int *uniqueUv = MEM_mallocN(sizeof(*uniqueUv) * data->elementMap->total_uvs,
                                "uv_brush_unique_uv_map");
    edgeHash = BLI_ghash_new(uv_edge_hash, uv_edge_compare, "uv_brush_edge_hash");
    /* we have at most totalUVs edges */
    edges = MEM_callocN(sizeof(*edges) * data->elementMap->total_uvs, "uv_brush_all_edges");
    if (!data->uv || !uniqueUv || !edgeHash || !edges) {
      MEM_SAFE_FREE(edges);
      MEM_SAFE_FREE(uniqueUv);
      if (edgeHash) {
        BLI_ghash_free(edgeHash, NULL, NULL);
      }
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    data->totalUniqueUvs = unique_uvs;
    /* Index for the UvElements. */
    int counter = -1;
    /* initialize the unique UVs */
    for (int i = 0; i < bm->totvert; i++) {
      UvElement *element = data->elementMap->vertex[i];
      for (; element; element = element->next) {
        if (element->separate) {
          if (do_island_optimization && (element->island != island_index)) {
            /* skip this uv if not on the active island */
            for (; element->next && !(element->next->separate); element = element->next) {
              /* pass */
            }
            continue;
          }

          l = element->l;
          luv = CustomData_bmesh_get(&em->bm->ldata, l->head.data, CD_MLOOPUV);

          counter++;
          data->uv[counter].element = element;
          data->uv[counter].uv = luv->uv;
          if (data->tool != UV_SCULPT_TOOL_GRAB) {
            if (luv->flag & MLOOPUV_PINNED) {
              data->uv[counter].is_locked = true;
            }
          }
        }
        /* Pointer arithmetic to the rescue, as always :). */
        uniqueUv[element - data->elementMap->storage] = counter;
      }
    }
    BLI_assert(counter + 1 == unique_uvs);

    /* Now, on to generate our uv connectivity data */
    counter = 0;
    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        int offset1, itmp1 = uv_element_offset_from_face_get(
                         data->elementMap, efa, l, island_index, do_island_optimization);
        int offset2, itmp2 = uv_element_offset_from_face_get(
                         data->elementMap, efa, l->next, island_index, do_island_optimization);

        /* Skip edge if not found(unlikely) or not on valid island */
        if (itmp1 == -1 || itmp2 == -1) {
          continue;
        }

        offset1 = uniqueUv[itmp1];
        offset2 = uniqueUv[itmp2];

        /* Using an order policy, sort UV's according to address space.
         * This avoids having two different UvEdges with the same UV's on different positions. */
        if (offset1 < offset2) {
          edges[counter].uv1 = offset1;
          edges[counter].uv2 = offset2;
        }
        else {
          edges[counter].uv1 = offset2;
          edges[counter].uv2 = offset1;
        }
        UvEdge *prev_edge = BLI_ghash_lookup(edgeHash, &edges[counter]);
        if (prev_edge) {
          prev_edge->is_interior = true;
          edges[counter].is_interior = true;
        }
        else {
          BLI_ghash_insert(edgeHash, &edges[counter], &edges[counter]);
        }
        counter++;
      }
    }

    MEM_SAFE_FREE(uniqueUv);

    /* Allocate connectivity data, we allocate edges once */
    data->uvedges = MEM_callocN(sizeof(*data->uvedges) * BLI_ghash_len(edgeHash),
                                "uv_brush_edge_connectivity_data");
    if (!data->uvedges) {
      BLI_ghash_free(edgeHash, NULL, NULL);
      MEM_SAFE_FREE(edges);
      uv_sculpt_stroke_exit(C, op);
      return NULL;
    }

    /* fill the edges with data */
    {
      int i = 0;
      GHASH_ITER (gh_iter, edgeHash) {
        data->uvedges[i++] = *((UvEdge *)BLI_ghashIterator_getKey(&gh_iter));
      }
      data->totalUvEdges = BLI_ghash_len(edgeHash);
    }

    /* cleanup temporary stuff */
    BLI_ghash_free(edgeHash, NULL, NULL);
    MEM_SAFE_FREE(edges);

    /* transfer boundary edge property to UV's */
    for (int i = 0; i < data->totalUvEdges; i++) {
      if (!data->uvedges[i].is_interior) {
        data->uv[data->uvedges[i].uv1].is_boundary = true;
        data->uv[data->uvedges[i].uv2].is_boundary = true;
        if (ts->uv_sculpt_settings & UV_SCULPT_LOCK_BORDERS) {
          data->uv[data->uvedges[i].uv1].is_locked = true;
          data->uv[data->uvedges[i].uv2].is_locked = true;
        }
        set_element_flag(data->uv[data->uvedges[i].uv1].element, MARK_BOUNDARY);
        set_element_flag(data->uv[data->uvedges[i].uv2].element, MARK_BOUNDARY);
      }
    }

    SpaceImage *sima = CTX_wm_space_image(C);
    data->constrain_to_bounds = (sima->flag & SI_CLIP_UV);
    BKE_image_find_nearest_tile_with_offset(sima->image, co, data->uv_base_offset);

    /* Allocate initial selection for grab tool */
    if (data->tool == UV_SCULPT_TOOL_GRAB) {
      float radius, radius_root;
      UvSculptData *sculptdata = (UvSculptData *)op->customdata;
      int width, height;
      float aspectRatio;
      float alpha, zoomx, zoomy;
      Brush *brush = BKE_paint_brush(sculptdata->uvsculpt);

      alpha = BKE_brush_alpha_get(scene, brush);

      radius = BKE_brush_size_get(scene, brush);
      ED_space_image_get_size(sima, &width, &height);
      ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

      aspectRatio = width / (float)height;
      radius /= (width * zoomx);
      radius = radius * radius;
      radius_root = sqrtf(radius);

      /* Allocate selection stack */
      data->initial_stroke = MEM_mallocN(sizeof(*data->initial_stroke),
                                         "uv_sculpt_initial_stroke");
      if (!data->initial_stroke) {
        uv_sculpt_stroke_exit(C, op);
      }
      data->initial_stroke->initialSelection = MEM_mallocN(
          sizeof(*data->initial_stroke->initialSelection) * data->totalUniqueUvs,
          "uv_sculpt_initial_selection");
      if (!data->initial_stroke->initialSelection) {
        uv_sculpt_stroke_exit(C, op);
      }

      copy_v2_v2(data->initial_stroke->init_coord, co);

      counter = 0;
      for (int i = 0; i < data->totalUniqueUvs; i++) {
        if (data->uv[i].is_locked) {
          continue;
        }

        float diff[2];
        sub_v2_v2v2(diff, data->uv[i].uv, co);
        diff[1] /= aspectRatio;
        float dist = dot_v2v2(diff, diff);
        if (dist <= radius) {
          float strength;
          strength = alpha * BKE_brush_curve_strength_clamped(brush, sqrtf(dist), radius_root);

          data->initial_stroke->initialSelection[counter].uv = i;
          data->initial_stroke->initialSelection[counter].strength = strength;
          copy_v2_v2(data->initial_stroke->initialSelection[counter].initial_uv, data->uv[i].uv);
          counter++;
        }
      }

      data->initial_stroke->totalInitialSelected = counter;
      if (sima->flag & SI_LIVE_UNWRAP) {
        ED_uvedit_live_unwrap_begin(scene, obedit);
      }
    }
  }

  return op->customdata;
}

static int uv_sculpt_stroke_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  UvSculptData *data;
  Object *obedit = CTX_data_edit_object(C);

  if (!(data = uv_sculpt_stroke_init(C, op, event))) {
    return OPERATOR_CANCELLED;
  }

  uv_sculpt_stroke_apply(C, op, event, obedit);

  data->timer = WM_event_add_timer(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.001f);

  if (!data->timer) {
    uv_sculpt_stroke_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int uv_sculpt_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  UvSculptData *data = (UvSculptData *)op->customdata;
  Object *obedit = CTX_data_edit_object(C);

  switch (event->type) {
    case LEFTMOUSE:
    case MIDDLEMOUSE:
    case RIGHTMOUSE:
      uv_sculpt_stroke_exit(C, op);
      return OPERATOR_FINISHED;

    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE:
      uv_sculpt_stroke_apply(C, op, event, obedit);
      break;
    case TIMER:
      if (event->customdata == data->timer) {
        uv_sculpt_stroke_apply(C, op, event, obedit);
      }
      break;
    default:
      return OPERATOR_RUNNING_MODAL;
  }

  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);
  DEG_id_tag_update(obedit->data, 0);
  return OPERATOR_RUNNING_MODAL;
}

static bool uv_sculpt_stroke_poll(bContext *C)
{
  if (ED_operator_uvedit_space_image(C)) {
    /* While these values could be initialized on demand,
     * the only case this would be useful is running from the operator search popup.
     * This is such a corner case that it's simpler to check a brush has already been created
     * (something the tool system ensures). */
    Scene *scene = CTX_data_scene(C);
    ToolSettings *ts = scene->toolsettings;
    Brush *brush = BKE_paint_brush(&ts->uvsculpt->paint);
    if (brush != NULL) {
      return true;
    }
  }
  return false;
}

void SCULPT_OT_uv_sculpt_stroke(wmOperatorType *ot)
{
  static const EnumPropertyItem stroke_mode_items[] = {
      {BRUSH_STROKE_NORMAL, "NORMAL", 0, "Regular", "Apply brush normally"},
      {BRUSH_STROKE_INVERT,
       "INVERT",
       0,
       "Invert",
       "Invert action of brush for duration of stroke"},
      {BRUSH_STROKE_SMOOTH,
       "RELAX",
       0,
       "Relax",
       "Switch brush to relax mode for duration of stroke"},
      {0},
  };

  /* identifiers */
  ot->name = "Sculpt UVs";
  ot->description = "Sculpt UVs using a brush";
  ot->idname = "SCULPT_OT_uv_sculpt_stroke";

  /* api callbacks */
  ot->invoke = uv_sculpt_stroke_invoke;
  ot->modal = uv_sculpt_stroke_modal;
  ot->poll = uv_sculpt_stroke_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_enum(ot->srna, "mode", stroke_mode_items, BRUSH_STROKE_NORMAL, "Mode", "Stroke Mode");
}
