/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2010 by Nicholas Bishop. All rights reserved. */

/** \file
 * \ingroup edsculpt
 * Implements the PBVH node hiding operator.
 */

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_ccg.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_multires.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_subsurf.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "bmesh.h"

#include "paint_intern.h"

/* For undo push. */
#include "sculpt_intern.h"

/* Return true if the element should be hidden/shown. */
static bool is_effected(PartialVisArea area,
                        float planes[4][4],
                        const float co[3],
                        const float mask)
{
  if (area == PARTIALVIS_ALL) {
    return true;
  }
  if (area == PARTIALVIS_MASKED) {
    return mask > 0.5f;
  }

  bool inside = isect_point_planes_v3(planes, 4, co);
  return ((inside && area == PARTIALVIS_INSIDE) || (!inside && area == PARTIALVIS_OUTSIDE));
}

static void partialvis_update_mesh(Object *ob,
                                   PBVH *pbvh,
                                   PBVHNode *node,
                                   PartialVisAction action,
                                   PartialVisArea area,
                                   float planes[4][4])
{
  Mesh *me = ob->data;
  MVert *mvert;
  const float *paint_mask;
  const int *vert_indices;
  int totvert, i;
  bool any_changed = false, any_visible = false;

  BKE_pbvh_node_num_verts(pbvh, node, NULL, &totvert);
  BKE_pbvh_node_get_verts(pbvh, node, &vert_indices, &mvert);
  paint_mask = CustomData_get_layer(&me->vdata, CD_PAINT_MASK);

  bool *hide_vert = CustomData_get_layer_named(&me->vdata, CD_PROP_BOOL, ".hide_vert");
  if (hide_vert == NULL) {
    hide_vert = CustomData_add_layer_named(
        &me->vdata, CD_PROP_BOOL, CD_CALLOC, NULL, me->totvert, ".hide_vert");
  }

  SCULPT_undo_push_node(ob, node, SCULPT_UNDO_HIDDEN);

  for (i = 0; i < totvert; i++) {
    MVert *v = &mvert[vert_indices[i]];
    float vmask = paint_mask ? paint_mask[vert_indices[i]] : 0;

    /* Hide vertex if in the hide volume. */
    if (is_effected(area, planes, v->co, vmask)) {
      hide_vert[vert_indices[i]] = (action == PARTIALVIS_HIDE);
      any_changed = true;
    }

    if (!hide_vert[vert_indices[i]]) {
      any_visible = true;
    }
  }

  if (any_changed) {
    BKE_pbvh_node_mark_rebuild_draw(node);
    BKE_pbvh_node_fully_hidden_set(node, !any_visible);
  }
}

/* Hide or show elements in multires grids with a special GridFlags
 * customdata layer. */
static void partialvis_update_grids(Depsgraph *depsgraph,
                                    Object *ob,
                                    PBVH *pbvh,
                                    PBVHNode *node,
                                    PartialVisAction action,
                                    PartialVisArea area,
                                    float planes[4][4])
{
  CCGElem **grids;
  BLI_bitmap **grid_hidden;
  int *grid_indices, totgrid;
  bool any_changed = false, any_visible = false;

  /* Get PBVH data. */
  BKE_pbvh_node_get_grids(pbvh, node, &grid_indices, &totgrid, NULL, NULL, &grids);
  grid_hidden = BKE_pbvh_grid_hidden(pbvh);
  CCGKey key = *BKE_pbvh_get_grid_key(pbvh);

  SCULPT_undo_push_node(ob, node, SCULPT_UNDO_HIDDEN);

  for (int i = 0; i < totgrid; i++) {
    int any_hidden = 0;
    int g = grid_indices[i];
    BLI_bitmap *gh = grid_hidden[g];

    if (!gh) {
      switch (action) {
        case PARTIALVIS_HIDE:
          /* Create grid flags data. */
          gh = grid_hidden[g] = BLI_BITMAP_NEW(key.grid_area, "partialvis_update_grids");
          break;
        case PARTIALVIS_SHOW:
          /* Entire grid is visible, nothing to show. */
          continue;
      }
    }
    else if (action == PARTIALVIS_SHOW && area == PARTIALVIS_ALL) {
      /* Special case if we're showing all, just free the grid. */
      MEM_freeN(gh);
      grid_hidden[g] = NULL;
      any_changed = true;
      any_visible = true;
      continue;
    }

    for (int y = 0; y < key.grid_size; y++) {
      for (int x = 0; x < key.grid_size; x++) {
        CCGElem *elem = CCG_grid_elem(&key, grids[g], x, y);
        const float *co = CCG_elem_co(&key, elem);
        float mask = key.has_mask ? *CCG_elem_mask(&key, elem) : 0.0f;

        /* Skip grid element if not in the effected area. */
        if (is_effected(area, planes, co, mask)) {
          /* Set or clear the hide flag. */
          BLI_BITMAP_SET(gh, y * key.grid_size + x, action == PARTIALVIS_HIDE);

          any_changed = true;
        }

        /* Keep track of whether any elements are still hidden. */
        if (BLI_BITMAP_TEST(gh, y * key.grid_size + x)) {
          any_hidden = true;
        }
        else {
          any_visible = true;
        }
      }
    }

    /* If everything in the grid is now visible, free the grid flags. */
    if (!any_hidden) {
      MEM_freeN(gh);
      grid_hidden[g] = NULL;
    }
  }

  /* Mark updates if anything was hidden/shown. */
  if (any_changed) {
    BKE_pbvh_node_mark_rebuild_draw(node);
    BKE_pbvh_node_fully_hidden_set(node, !any_visible);
    multires_mark_as_modified(depsgraph, ob, MULTIRES_HIDDEN_MODIFIED);
  }
}

static void partialvis_update_bmesh_verts(BMesh *bm,
                                          GSet *verts,
                                          PartialVisAction action,
                                          PartialVisArea area,
                                          float planes[4][4],
                                          bool *any_changed,
                                          bool *any_visible)
{
  GSetIterator gs_iter;

  GSET_ITER (gs_iter, verts) {
    BMVert *v = BLI_gsetIterator_getKey(&gs_iter);
    float *vmask = CustomData_bmesh_get(&bm->vdata, v->head.data, CD_PAINT_MASK);

    /* Hide vertex if in the hide volume. */
    if (is_effected(area, planes, v->co, *vmask)) {
      if (action == PARTIALVIS_HIDE) {
        BM_elem_flag_enable(v, BM_ELEM_HIDDEN);
      }
      else {
        BM_elem_flag_disable(v, BM_ELEM_HIDDEN);
      }
      (*any_changed) = true;
    }

    if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      (*any_visible) = true;
    }
  }
}

static void partialvis_update_bmesh_faces(GSet *faces)
{
  GSetIterator gs_iter;

  GSET_ITER (gs_iter, faces) {
    BMFace *f = BLI_gsetIterator_getKey(&gs_iter);

    if (paint_is_bmesh_face_hidden(f)) {
      BM_elem_flag_enable(f, BM_ELEM_HIDDEN);
    }
    else {
      BM_elem_flag_disable(f, BM_ELEM_HIDDEN);
    }
  }
}

static void partialvis_update_bmesh(Object *ob,
                                    PBVH *pbvh,
                                    PBVHNode *node,
                                    PartialVisAction action,
                                    PartialVisArea area,
                                    float planes[4][4])
{
  BMesh *bm;
  GSet *unique, *other, *faces;
  bool any_changed = false, any_visible = false;

  bm = BKE_pbvh_get_bmesh(pbvh);
  unique = BKE_pbvh_bmesh_node_unique_verts(node);
  other = BKE_pbvh_bmesh_node_other_verts(node);
  faces = BKE_pbvh_bmesh_node_faces(node);

  SCULPT_undo_push_node(ob, node, SCULPT_UNDO_HIDDEN);

  partialvis_update_bmesh_verts(bm, unique, action, area, planes, &any_changed, &any_visible);

  partialvis_update_bmesh_verts(bm, other, action, area, planes, &any_changed, &any_visible);

  /* Finally loop over node faces and tag the ones that are fully hidden. */
  partialvis_update_bmesh_faces(faces);

  if (any_changed) {
    BKE_pbvh_node_mark_rebuild_draw(node);
    BKE_pbvh_node_fully_hidden_set(node, !any_visible);
  }
}

static void rect_from_props(rcti *rect, PointerRNA *ptr)
{
  rect->xmin = RNA_int_get(ptr, "xmin");
  rect->ymin = RNA_int_get(ptr, "ymin");
  rect->xmax = RNA_int_get(ptr, "xmax");
  rect->ymax = RNA_int_get(ptr, "ymax");
}

static void clip_planes_from_rect(bContext *C,
                                  Depsgraph *depsgraph,
                                  float clip_planes[4][4],
                                  const rcti *rect)
{
  ViewContext vc;
  BoundBox bb;

  view3d_operator_needs_opengl(C);
  ED_view3d_viewcontext_init(C, &vc, depsgraph);
  ED_view3d_clipping_calc(&bb, clip_planes, vc.region, vc.obact, rect);
}

/* If mode is inside, get all PBVH nodes that lie at least partially
 * inside the clip_planes volume. If mode is outside, get all nodes
 * that lie at least partially outside the volume. If showing all, get
 * all nodes. */
static void get_pbvh_nodes(
    PBVH *pbvh, PBVHNode ***nodes, int *totnode, float clip_planes[4][4], PartialVisArea mode)
{
  BKE_pbvh_SearchCallback cb = NULL;

  /* Select search callback. */
  switch (mode) {
    case PARTIALVIS_INSIDE:
      cb = BKE_pbvh_node_frustum_contain_AABB;
      break;
    case PARTIALVIS_OUTSIDE:
      cb = BKE_pbvh_node_frustum_exclude_AABB;
      break;
    case PARTIALVIS_ALL:
    case PARTIALVIS_MASKED:
      break;
  }

  PBVHFrustumPlanes frustum = {.planes = clip_planes, .num_planes = 4};
  BKE_pbvh_search_gather(pbvh, cb, &frustum, nodes, totnode);
}

static int hide_show_exec(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Mesh *me = ob->data;
  PartialVisAction action;
  PartialVisArea area;
  PBVH *pbvh;
  PBVHNode **nodes;
  PBVHType pbvh_type;
  float clip_planes[4][4];
  rcti rect;
  int totnode;

  /* Read operator properties. */
  action = RNA_enum_get(op->ptr, "action");
  area = RNA_enum_get(op->ptr, "area");
  rect_from_props(&rect, op->ptr);

  clip_planes_from_rect(C, depsgraph, clip_planes, &rect);

  pbvh = BKE_sculpt_object_pbvh_ensure(depsgraph, ob);
  BLI_assert(ob->sculpt->pbvh == pbvh);

  get_pbvh_nodes(pbvh, &nodes, &totnode, clip_planes, area);
  pbvh_type = BKE_pbvh_type(pbvh);

  negate_m4(clip_planes);

  /* Start undo. */
  switch (action) {
    case PARTIALVIS_HIDE:
      SCULPT_undo_push_begin_ex(ob, "Hide area");
      break;
    case PARTIALVIS_SHOW:
      SCULPT_undo_push_begin_ex(ob, "Show area");
      break;
  }

  for (int i = 0; i < totnode; i++) {
    switch (pbvh_type) {
      case PBVH_FACES:
        partialvis_update_mesh(ob, pbvh, nodes[i], action, area, clip_planes);
        break;
      case PBVH_GRIDS:
        partialvis_update_grids(depsgraph, ob, pbvh, nodes[i], action, area, clip_planes);
        break;
      case PBVH_BMESH:
        partialvis_update_bmesh(ob, pbvh, nodes[i], action, area, clip_planes);
        break;
    }
  }

  if (nodes) {
    MEM_freeN(nodes);
  }

  /* End undo. */
  SCULPT_undo_push_end(ob);

  /* Ensure that edges and faces get hidden as well (not used by
   * sculpt but it looks wrong when entering editmode otherwise). */
  if (pbvh_type == PBVH_FACES) {
    BKE_mesh_flush_hidden_from_verts(me);
  }

  SCULPT_visibility_sync_all_vertex_to_face_sets(ob->sculpt);

  DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
  ED_region_tag_redraw(region);

  return OPERATOR_FINISHED;
}

static int hide_show_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  PartialVisArea area = RNA_enum_get(op->ptr, "area");

  if (!ELEM(area, PARTIALVIS_ALL, PARTIALVIS_MASKED)) {
    return WM_gesture_box_invoke(C, op, event);
  }
  return op->type->exec(C, op);
}

void PAINT_OT_hide_show(struct wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {PARTIALVIS_HIDE, "HIDE", 0, "Hide", "Hide vertices"},
      {PARTIALVIS_SHOW, "SHOW", 0, "Show", "Show vertices"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem area_items[] = {
      {PARTIALVIS_OUTSIDE, "OUTSIDE", 0, "Outside", "Hide or show vertices outside the selection"},
      {PARTIALVIS_INSIDE, "INSIDE", 0, "Inside", "Hide or show vertices inside the selection"},
      {PARTIALVIS_ALL, "ALL", 0, "All", "Hide or show all vertices"},
      {PARTIALVIS_MASKED,
       "MASKED",
       0,
       "Masked",
       "Hide or show vertices that are masked (minimum mask value of 0.5)"},
      {0, NULL, 0, NULL, NULL},
  };

  /* Identifiers. */
  ot->name = "Hide/Show";
  ot->idname = "PAINT_OT_hide_show";
  ot->description = "Hide/show some vertices";

  /* API callbacks. */
  ot->invoke = hide_show_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = hide_show_exec;
  /* Sculpt-only for now. */
  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* RNA. */
  RNA_def_enum(ot->srna,
               "action",
               action_items,
               PARTIALVIS_HIDE,
               "Action",
               "Whether to hide or show vertices");
  RNA_def_enum(
      ot->srna, "area", area_items, PARTIALVIS_INSIDE, "Area", "Which vertices to hide or show");

  WM_operator_properties_border(ot);
}
