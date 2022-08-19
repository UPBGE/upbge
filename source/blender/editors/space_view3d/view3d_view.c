/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spview3d
 */

#include "MEM_guardedalloc.h"

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rect.h"

#include "BKE_action.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "DEG_depsgraph_query.h"

#include "UI_resources.h"

#include "GPU_matrix.h"
#include "GPU_select.h"
#include "GPU_state.h"

#include "WM_api.h"

#include "ED_object.h"
#include "ED_screen.h"

#include "DRW_engine.h"

#include "RNA_access.h"
#include "RNA_define.h"

#ifdef WITH_GAMEENGINE
#  include "BKE_callbacks.h"
#  include "BKE_image.h"
#  include "BLI_listbase.h"

#  include "LA_SystemCommandLine.h"
#endif

#include "view3d_intern.h" /* own include */
#include "view3d_navigate.h"

/* -------------------------------------------------------------------- */
/** \name Camera to View Operator
 * \{ */

static int view3d_camera_to_view_exec(bContext *C, wmOperator *UNUSED(op))
{
  const Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  View3D *v3d;
  ARegion *region;
  RegionView3D *rv3d;

  ObjectTfmProtectedChannels obtfm;

  ED_view3d_context_user_region(C, &v3d, &region);
  rv3d = region->regiondata;

  ED_view3d_lastview_store(rv3d);

  BKE_object_tfm_protected_backup(v3d->camera, &obtfm);

  ED_view3d_to_object(depsgraph, v3d->camera, rv3d->ofs, rv3d->viewquat, rv3d->dist);

  BKE_object_tfm_protected_restore(v3d->camera, &obtfm, v3d->camera->protectflag);

  DEG_id_tag_update(&v3d->camera->id, ID_RECALC_TRANSFORM);
  rv3d->persp = RV3D_CAMOB;

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, v3d->camera);

  return OPERATOR_FINISHED;
}

static bool view3d_camera_to_view_poll(bContext *C)
{
  View3D *v3d;
  ARegion *region;

  if (ED_view3d_context_user_region(C, &v3d, &region)) {
    RegionView3D *rv3d = region->regiondata;
    if (v3d && v3d->camera && BKE_id_is_editable(CTX_data_main(C), &v3d->camera->id)) {
      if (rv3d && (RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ANY_TRANSFORM) == 0) {
        if (rv3d->persp != RV3D_CAMOB) {
          return true;
        }
      }
    }
  }

  return false;
}

void VIEW3D_OT_camera_to_view(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Align Camera to View";
  ot->description = "Set camera view to active view";
  ot->idname = "VIEW3D_OT_camera_to_view";

  /* api callbacks */
  ot->exec = view3d_camera_to_view_exec;
  ot->poll = view3d_camera_to_view_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Camera Fit Frame to Selected Operator
 * \{ */

/* unlike VIEW3D_OT_view_selected this is for framing a render and not
 * meant to take into account vertex/bone selection for eg. */
static int view3d_camera_to_view_selected_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  View3D *v3d = CTX_wm_view3d(C); /* can be NULL */
  Object *camera_ob = v3d ? v3d->camera : scene->camera;

  if (camera_ob == NULL) {
    BKE_report(op->reports, RPT_ERROR, "No active camera");
    return OPERATOR_CANCELLED;
  }

  if (ED_view3d_camera_to_view_selected(bmain, depsgraph, scene, camera_ob)) {
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, camera_ob);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_camera_to_view_selected(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Camera Fit Frame to Selected";
  ot->description = "Move the camera so selected objects are framed";
  ot->idname = "VIEW3D_OT_camera_to_view_selected";

  /* api callbacks */
  ot->exec = view3d_camera_to_view_selected_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object as Camera Operator
 * \{ */

static void sync_viewport_camera_smoothview(bContext *C,
                                            View3D *v3d,
                                            Object *ob,
                                            const int smooth_viewtx)
{
  Main *bmain = CTX_data_main(C);
  for (bScreen *screen = bmain->screens.first; screen != NULL; screen = screen->id.next) {
    for (ScrArea *area = screen->areabase.first; area != NULL; area = area->next) {
      for (SpaceLink *space_link = area->spacedata.first; space_link != NULL;
           space_link = space_link->next) {
        if (space_link->spacetype == SPACE_VIEW3D) {
          View3D *other_v3d = (View3D *)space_link;
          if (other_v3d == v3d) {
            continue;
          }
          if (other_v3d->camera == ob) {
            continue;
          }
          if (v3d->scenelock) {
            ListBase *lb = (space_link == area->spacedata.first) ? &area->regionbase :
                                                                   &space_link->regionbase;
            for (ARegion *other_region = lb->first; other_region != NULL;
                 other_region = other_region->next) {
              if (other_region->regiontype == RGN_TYPE_WINDOW) {
                if (other_region->regiondata) {
                  RegionView3D *other_rv3d = other_region->regiondata;
                  if (other_rv3d->persp == RV3D_CAMOB) {
                    Object *other_camera_old = other_v3d->camera;
                    other_v3d->camera = ob;
                    ED_view3d_lastview_store(other_rv3d);
                    ED_view3d_smooth_view(C,
                                          other_v3d,
                                          other_region,
                                          smooth_viewtx,
                                          &(const V3D_SmoothParams){
                                              .camera_old = other_camera_old,
                                              .camera = other_v3d->camera,
                                              .ofs = other_rv3d->ofs,
                                              .quat = other_rv3d->viewquat,
                                              .dist = &other_rv3d->dist,
                                              .lens = &other_v3d->lens,
                                              /* No undo because this switches cameras. */
                                              .undo_str = NULL,
                                          });
                  }
                  else {
                    other_v3d->camera = ob;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

static int view3d_setobjectascamera_exec(bContext *C, wmOperator *op)
{
  View3D *v3d;
  ARegion *region;
  RegionView3D *rv3d;

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);

  const int smooth_viewtx = WM_operator_smooth_viewtx_get(op);

  /* no NULL check is needed, poll checks */
  ED_view3d_context_user_region(C, &v3d, &region);
  rv3d = region->regiondata;

  if (ob) {
    Object *camera_old = (rv3d->persp == RV3D_CAMOB) ? V3D_CAMERA_SCENE(scene, v3d) : NULL;
    rv3d->persp = RV3D_CAMOB;
    v3d->camera = ob;
    if (v3d->scenelock && scene->camera != ob) {
      scene->camera = ob;
      DEG_id_tag_update(&scene->id, ID_RECALC_COPY_ON_WRITE);
    }

    /* unlikely but looks like a glitch when set to the same */
    if (camera_old != ob) {
      ED_view3d_lastview_store(rv3d);

      ED_view3d_smooth_view(C,
                            v3d,
                            region,
                            smooth_viewtx,
                            &(const V3D_SmoothParams){
                                .camera_old = camera_old,
                                .camera = v3d->camera,
                                .ofs = rv3d->ofs,
                                .quat = rv3d->viewquat,
                                .dist = &rv3d->dist,
                                .lens = &v3d->lens,
                                /* No undo because this switches cameras. */
                                .undo_str = NULL,
                            });
    }

    if (v3d->scenelock) {
      sync_viewport_camera_smoothview(C, v3d, ob, smooth_viewtx);
      WM_event_add_notifier(C, NC_SCENE, scene);
    }
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, scene);
  }

  return OPERATOR_FINISHED;
}

bool ED_operator_rv3d_user_region_poll(bContext *C)
{
  View3D *v3d_dummy;
  ARegion *region_dummy;

  return ED_view3d_context_user_region(C, &v3d_dummy, &region_dummy);
}

void VIEW3D_OT_object_as_camera(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Active Object as Camera";
  ot->description = "Set the active object as the active camera for this view or scene";
  ot->idname = "VIEW3D_OT_object_as_camera";

  /* api callbacks */
  ot->exec = view3d_setobjectascamera_exec;
  ot->poll = ED_operator_rv3d_user_region_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window and View Matrix Calculation
 * \{ */

void view3d_winmatrix_set(Depsgraph *depsgraph,
                          ARegion *region,
                          const View3D *v3d,
                          const rcti *rect)
{
  RegionView3D *rv3d = region->regiondata;
  rctf full_viewplane;
  float clipsta, clipend;
  bool is_ortho;

  is_ortho = ED_view3d_viewplane_get(
      depsgraph, v3d, rv3d, region->winx, region->winy, &full_viewplane, &clipsta, &clipend, NULL);
  rv3d->is_persp = !is_ortho;

#if 0
  printf("%s: %d %d %f %f %f %f %f %f\n",
         __func__,
         winx,
         winy,
         full_viewplane.xmin,
         full_viewplane.ymin,
         full_viewplane.xmax,
         full_viewplane.ymax,
         clipsta,
         clipend);
#endif

  /* Note the code here was tweaked to avoid an apparent compiler bug in clang 13 (see T91680). */
  rctf viewplane;
  if (rect) {
    /* Smaller viewplane subset for selection picking. */
    viewplane.xmin = full_viewplane.xmin +
                     (BLI_rctf_size_x(&full_viewplane) * (rect->xmin / (float)region->winx));
    viewplane.ymin = full_viewplane.ymin +
                     (BLI_rctf_size_y(&full_viewplane) * (rect->ymin / (float)region->winy));
    viewplane.xmax = full_viewplane.xmin +
                     (BLI_rctf_size_x(&full_viewplane) * (rect->xmax / (float)region->winx));
    viewplane.ymax = full_viewplane.ymin +
                     (BLI_rctf_size_y(&full_viewplane) * (rect->ymax / (float)region->winy));
  }
  else {
    viewplane = full_viewplane;
  }

  if (is_ortho) {
    GPU_matrix_ortho_set(
        viewplane.xmin, viewplane.xmax, viewplane.ymin, viewplane.ymax, clipsta, clipend);
  }
  else {
    GPU_matrix_frustum_set(
        viewplane.xmin, viewplane.xmax, viewplane.ymin, viewplane.ymax, clipsta, clipend);
  }

  /* update matrix in 3d view region */
  GPU_matrix_projection_get(rv3d->winmat);
}

static void obmat_to_viewmat(RegionView3D *rv3d, Object *ob)
{
  float bmat[4][4];

  rv3d->view = RV3D_VIEW_USER; /* don't show the grid */

  normalize_m4_m4(bmat, ob->obmat);
  invert_m4_m4(rv3d->viewmat, bmat);

  /* view quat calculation, needed for add object */
  // mat4_normalized_to_quat(rv3d->viewquat, rv3d->viewmat);

  /* UPBGE (to avoid an annoying assert -> will normalize anyway) */
  mat4_to_quat(rv3d->viewquat, rv3d->viewmat);
}

void view3d_viewmatrix_set(Depsgraph *depsgraph,
                           const Scene *scene,
                           const View3D *v3d,
                           RegionView3D *rv3d,
                           const float rect_scale[2])
{
  if (rv3d->persp == RV3D_CAMOB) { /* obs/camera */
    if (v3d->camera) {
      Object *ob_camera_eval = DEG_get_evaluated_object(depsgraph, v3d->camera);
      obmat_to_viewmat(rv3d, ob_camera_eval);
    }
    else {
      quat_to_mat4(rv3d->viewmat, rv3d->viewquat);
      rv3d->viewmat[3][2] -= rv3d->dist;
    }
  }
  else {
    bool use_lock_ofs = false;

    /* should be moved to better initialize later on XXX */
    if (RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ROTATION) {
      ED_view3d_lock(rv3d);
    }

    quat_to_mat4(rv3d->viewmat, rv3d->viewquat);
    if (rv3d->persp == RV3D_PERSP) {
      rv3d->viewmat[3][2] -= rv3d->dist;
    }
    if (v3d->ob_center) {
      Object *ob_eval = DEG_get_evaluated_object(depsgraph, v3d->ob_center);
      float vec[3];

      copy_v3_v3(vec, ob_eval->obmat[3]);
      if (ob_eval->type == OB_ARMATURE && v3d->ob_center_bone[0]) {
        bPoseChannel *pchan = BKE_pose_channel_find_name(ob_eval->pose, v3d->ob_center_bone);
        if (pchan) {
          copy_v3_v3(vec, pchan->pose_mat[3]);
          mul_m4_v3(ob_eval->obmat, vec);
        }
      }
      translate_m4(rv3d->viewmat, -vec[0], -vec[1], -vec[2]);
      use_lock_ofs = true;
    }
    else if (v3d->ob_center_cursor) {
      float vec[3];
      copy_v3_v3(vec, scene->cursor.location);
      translate_m4(rv3d->viewmat, -vec[0], -vec[1], -vec[2]);
      use_lock_ofs = true;
    }
    else {
      translate_m4(rv3d->viewmat, rv3d->ofs[0], rv3d->ofs[1], rv3d->ofs[2]);
    }

    /* lock offset */
    if (use_lock_ofs) {
      float persmat[4][4], persinv[4][4];
      float vec[3];

      /* we could calculate the real persmat/persinv here
       * but it would be unreliable so better to later */
      mul_m4_m4m4(persmat, rv3d->winmat, rv3d->viewmat);
      invert_m4_m4(persinv, persmat);

      mul_v2_v2fl(vec, rv3d->ofs_lock, rv3d->is_persp ? rv3d->dist : 1.0f);
      vec[2] = 0.0f;

      if (rect_scale) {
        /* Since 'RegionView3D.winmat' has been calculated and this function doesn't take the
         * 'ARegion' we don't know about the region size.
         * Use 'rect_scale' when drawing a sub-region to apply 2D offset,
         * scaled by the difference between the sub-region and the region size.
         */
        vec[0] /= rect_scale[0];
        vec[1] /= rect_scale[1];
      }

      mul_mat3_m4_v3(persinv, vec);
      translate_m4(rv3d->viewmat, vec[0], vec[1], vec[2]);
    }
    /* end lock offset */
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OpenGL Select Utilities
 * \{ */

void view3d_opengl_select_cache_begin(void)
{
  GPU_select_cache_begin();
}

void view3d_opengl_select_cache_end(void)
{
  GPU_select_cache_end();
}

struct DrawSelectLoopUserData {
  uint pass;
  uint hits;
  GPUSelectResult *buffer;
  uint buffer_len;
  const rcti *rect;
  eGPUSelectMode gpu_select_mode;
};

static bool drw_select_loop_pass(eDRWSelectStage stage, void *user_data)
{
  bool continue_pass = false;
  struct DrawSelectLoopUserData *data = user_data;
  if (stage == DRW_SELECT_PASS_PRE) {
    GPU_select_begin(
        data->buffer, data->buffer_len, data->rect, data->gpu_select_mode, data->hits);
    /* always run POST after PRE. */
    continue_pass = true;
  }
  else if (stage == DRW_SELECT_PASS_POST) {
    int hits = GPU_select_end();
    if (data->pass == 0) {
      /* quirk of GPU_select_end, only take hits value from first call. */
      data->hits = hits;
    }
    if (data->gpu_select_mode == GPU_SELECT_NEAREST_FIRST_PASS) {
      data->gpu_select_mode = GPU_SELECT_NEAREST_SECOND_PASS;
      continue_pass = (hits > 0);
    }
    data->pass += 1;
  }
  else {
    BLI_assert(0);
  }
  return continue_pass;
}

eV3DSelectObjectFilter ED_view3d_select_filter_from_mode(const Scene *scene, const Object *obact)
{
  if (scene->toolsettings->object_flag & SCE_OBJECT_MODE_LOCK) {
    if (obact && (obact->mode & OB_MODE_ALL_WEIGHT_PAINT) &&
        BKE_object_pose_armature_get((Object *)obact)) {
      return VIEW3D_SELECT_FILTER_WPAINT_POSE_MODE_LOCK;
    }
    return VIEW3D_SELECT_FILTER_OBJECT_MODE_LOCK;
  }
  return VIEW3D_SELECT_FILTER_NOP;
}

/** Implement #VIEW3D_SELECT_FILTER_OBJECT_MODE_LOCK. */
static bool drw_select_filter_object_mode_lock(Object *ob, void *user_data)
{
  const Object *obact = user_data;
  return BKE_object_is_mode_compat(ob, obact->mode);
}

/**
 * Implement #VIEW3D_SELECT_FILTER_WPAINT_POSE_MODE_LOCK for special case when
 * we want to select pose bones (this doesn't switch modes).
 */
static bool drw_select_filter_object_mode_lock_for_weight_paint(Object *ob, void *user_data)
{
  LinkNode *ob_pose_list = user_data;
  return ob_pose_list && (BLI_linklist_index(ob_pose_list, DEG_get_original_object(ob)) != -1);
}

int view3d_opengl_select_ex(ViewContext *vc,
                            GPUSelectResult *buffer,
                            uint buffer_len,
                            const rcti *input,
                            eV3DSelectMode select_mode,
                            eV3DSelectObjectFilter select_filter,
                            const bool do_material_slot_selection)
{
  struct bThemeState theme_state;
  const wmWindowManager *wm = CTX_wm_manager(vc->C);
  Depsgraph *depsgraph = vc->depsgraph;
  Scene *scene = vc->scene;
  View3D *v3d = vc->v3d;
  ARegion *region = vc->region;
  rcti rect;
  int hits = 0;
  const bool use_obedit_skip = (OBEDIT_FROM_VIEW_LAYER(vc->view_layer) != NULL) &&
                               (vc->obedit == NULL);
  const bool is_pick_select = (U.gpu_flag & USER_GPU_FLAG_NO_DEPT_PICK) == 0;
  const bool do_passes = ((is_pick_select == false) &&
                          (select_mode == VIEW3D_SELECT_PICK_NEAREST));
  const bool use_nearest = (is_pick_select && select_mode == VIEW3D_SELECT_PICK_NEAREST);
  bool draw_surface = true;

  eGPUSelectMode gpu_select_mode;

  /* case not a box select */
  if (input->xmin == input->xmax) {
    /* seems to be default value for bones only now */
    BLI_rcti_init_pt_radius(&rect, (const int[2]){input->xmin, input->ymin}, 12);
  }
  else {
    rect = *input;
  }

  if (is_pick_select) {
    if (select_mode == VIEW3D_SELECT_PICK_NEAREST) {
      gpu_select_mode = GPU_SELECT_PICK_NEAREST;
    }
    else if (select_mode == VIEW3D_SELECT_PICK_ALL) {
      gpu_select_mode = GPU_SELECT_PICK_ALL;
    }
    else {
      gpu_select_mode = GPU_SELECT_ALL;
    }
  }
  else {
    if (do_passes) {
      gpu_select_mode = GPU_SELECT_NEAREST_FIRST_PASS;
    }
    else {
      gpu_select_mode = GPU_SELECT_ALL;
    }
  }

  /* Re-use cache (rect must be smaller than the cached)
   * other context is assumed to be unchanged */
  if (GPU_select_is_cached()) {
    GPU_select_begin(buffer, buffer_len, &rect, gpu_select_mode, 0);
    GPU_select_cache_load_id();
    hits = GPU_select_end();
    goto finally;
  }

  /* Important to use 'vc->obact', not 'OBACT(vc->view_layer)' below,
   * so it will be NULL when hidden. */
  struct {
    DRW_ObjectFilterFn fn;
    void *user_data;
  } object_filter = {NULL, NULL};
  switch (select_filter) {
    case VIEW3D_SELECT_FILTER_OBJECT_MODE_LOCK: {
      Object *obact = vc->obact;
      if (obact && obact->mode != OB_MODE_OBJECT) {
        object_filter.fn = drw_select_filter_object_mode_lock;
        object_filter.user_data = obact;
      }
      break;
    }
    case VIEW3D_SELECT_FILTER_WPAINT_POSE_MODE_LOCK: {
      Object *obact = vc->obact;
      BLI_assert(obact && (obact->mode & OB_MODE_ALL_WEIGHT_PAINT));
      /* While this uses 'alloca' in a loop (which we typically avoid),
       * the number of items is nearly always 1, maybe 2..3 in rare cases. */
      LinkNode *ob_pose_list = NULL;
      if (obact->type == OB_GPENCIL) {
        GpencilVirtualModifierData virtualModifierData;
        const GpencilModifierData *md = BKE_gpencil_modifiers_get_virtual_modifierlist(
            obact, &virtualModifierData);
        for (; md; md = md->next) {
          if (md->type == eGpencilModifierType_Armature) {
            ArmatureGpencilModifierData *agmd = (ArmatureGpencilModifierData *)md;
            if (agmd->object && (agmd->object->mode & OB_MODE_POSE)) {
              BLI_linklist_prepend_alloca(&ob_pose_list, agmd->object);
            }
          }
        }
      }
      else {
        VirtualModifierData virtualModifierData;
        const ModifierData *md = BKE_modifiers_get_virtual_modifierlist(obact,
                                                                        &virtualModifierData);
        for (; md; md = md->next) {
          if (md->type == eModifierType_Armature) {
            ArmatureModifierData *amd = (ArmatureModifierData *)md;
            if (amd->object && (amd->object->mode & OB_MODE_POSE)) {
              BLI_linklist_prepend_alloca(&ob_pose_list, amd->object);
            }
          }
        }
      }
      object_filter.fn = drw_select_filter_object_mode_lock_for_weight_paint;
      object_filter.user_data = ob_pose_list;
      break;
    }
    case VIEW3D_SELECT_FILTER_NOP:
      break;
  }

  /* Tools may request depth outside of regular drawing code. */
  UI_Theme_Store(&theme_state);
  UI_SetTheme(SPACE_VIEW3D, RGN_TYPE_WINDOW);

  /* All of the queries need to be perform on the drawing context. */
  DRW_opengl_context_enable();

  G.f |= G_FLAG_PICKSEL;

  /* Important we use the 'viewmat' and don't re-calculate since
   * the object & bone view locking takes 'rect' into account, see: T51629. */
  ED_view3d_draw_setup_view(
      wm, vc->win, depsgraph, scene, region, v3d, vc->rv3d->viewmat, NULL, &rect);

  if (!XRAY_ACTIVE(v3d)) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  /* If in xray mode, we select the wires in priority. */
  if (XRAY_ACTIVE(v3d) && use_nearest) {
    /* We need to call "GPU_select_*" API's inside DRW_draw_select_loop
     * because the OpenGL context created & destroyed inside this function. */
    struct DrawSelectLoopUserData drw_select_loop_user_data = {
        .pass = 0,
        .hits = 0,
        .buffer = buffer,
        .buffer_len = buffer_len,
        .rect = &rect,
        .gpu_select_mode = gpu_select_mode,
    };
    draw_surface = false;
    DRW_draw_select_loop(depsgraph,
                         region,
                         v3d,
                         use_obedit_skip,
                         draw_surface,
                         use_nearest,
                         do_material_slot_selection,
                         &rect,
                         drw_select_loop_pass,
                         &drw_select_loop_user_data,
                         object_filter.fn,
                         object_filter.user_data);
    hits = drw_select_loop_user_data.hits;
    /* FIX: This cleanup the state before doing another selection pass.
     * (see T56695) */
    GPU_select_cache_end();
  }

  if (hits == 0) {
    /* We need to call "GPU_select_*" API's inside DRW_draw_select_loop
     * because the OpenGL context created & destroyed inside this function. */
    struct DrawSelectLoopUserData drw_select_loop_user_data = {
        .pass = 0,
        .hits = 0,
        .buffer = buffer,
        .buffer_len = buffer_len,
        .rect = &rect,
        .gpu_select_mode = gpu_select_mode,
    };
    /* If are not in wireframe mode, we need to use the mesh surfaces to check for hits */
    draw_surface = (v3d->shading.type > OB_WIRE) || !XRAY_ENABLED(v3d);
    DRW_draw_select_loop(depsgraph,
                         region,
                         v3d,
                         use_obedit_skip,
                         draw_surface,
                         use_nearest,
                         do_material_slot_selection,
                         &rect,
                         drw_select_loop_pass,
                         &drw_select_loop_user_data,
                         object_filter.fn,
                         object_filter.user_data);
    hits = drw_select_loop_user_data.hits;
  }

  G.f &= ~G_FLAG_PICKSEL;
  ED_view3d_draw_setup_view(
      wm, vc->win, depsgraph, scene, region, v3d, vc->rv3d->viewmat, NULL, NULL);

  if (!XRAY_ACTIVE(v3d)) {
    GPU_depth_test(GPU_DEPTH_NONE);
  }

  DRW_opengl_context_disable();

  UI_Theme_Restore(&theme_state);

finally:

  if (hits < 0) {
    printf("Too many objects in select buffer\n"); /* XXX make error message */
  }

  return hits;
}

int view3d_opengl_select(ViewContext *vc,
                         GPUSelectResult *buffer,
                         uint buffer_len,
                         const rcti *input,
                         eV3DSelectMode select_mode,
                         eV3DSelectObjectFilter select_filter)
{
  return view3d_opengl_select_ex(vc, buffer, buffer_len, input, select_mode, select_filter, false);
}

int view3d_opengl_select_with_id_filter(ViewContext *vc,
                                        GPUSelectResult *buffer,
                                        const uint buffer_len,
                                        const rcti *input,
                                        eV3DSelectMode select_mode,
                                        eV3DSelectObjectFilter select_filter,
                                        uint select_id)
{
  int hits = view3d_opengl_select(vc, buffer, buffer_len, input, select_mode, select_filter);

  /* Selection sometimes uses -1 for an invalid selection ID, remove these as they
   * interfere with detection of actual number of hits in the selection. */
  if (hits > 0) {
    hits = GPU_select_buffer_remove_by_id(buffer, hits, select_id);
  }
  return hits;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local View Operators
 * \{ */

static uint free_localview_bit(Main *bmain)
{
  ScrArea *area;
  bScreen *screen;

  ushort local_view_bits = 0;

  /* Sometimes we lose a local-view: when an area is closed.
   * Check all areas: which local-views are in use? */
  for (screen = bmain->screens.first; screen; screen = screen->id.next) {
    for (area = screen->areabase.first; area; area = area->next) {
      SpaceLink *sl = area->spacedata.first;
      for (; sl; sl = sl->next) {
        if (sl->spacetype == SPACE_VIEW3D) {
          View3D *v3d = (View3D *)sl;
          if (v3d->localvd) {
            local_view_bits |= v3d->local_view_uuid;
          }
        }
      }
    }
  }

  for (int i = 0; i < 16; i++) {
    if ((local_view_bits & (1 << i)) == 0) {
      return (1 << i);
    }
  }

  return 0;
}

static bool view3d_localview_init(const Depsgraph *depsgraph,
                                  wmWindowManager *wm,
                                  wmWindow *win,
                                  Main *bmain,
                                  ViewLayer *view_layer,
                                  ScrArea *area,
                                  const bool frame_selected,
                                  const int smooth_viewtx,
                                  ReportList *reports)
{
  View3D *v3d = area->spacedata.first;
  Base *base;
  float min[3], max[3], box[3];
  float size = 0.0f;
  uint local_view_bit;
  bool ok = false;

  if (v3d->localvd) {
    return ok;
  }

  INIT_MINMAX(min, max);

  local_view_bit = free_localview_bit(bmain);

  if (local_view_bit == 0) {
    /* TODO(dfelinto): We can kick one of the other 3D views out of local view
     * specially if it is not being used. */
    BKE_report(reports, RPT_ERROR, "No more than 16 local views");
    ok = false;
  }
  else {
    Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
    if (obedit) {
      for (base = FIRSTBASE(view_layer); base; base = base->next) {
        base->local_view_bits &= ~local_view_bit;
      }
      FOREACH_BASE_IN_EDIT_MODE_BEGIN (view_layer, v3d, base_iter) {
        BKE_object_minmax(base_iter->object, min, max, false);
        base_iter->local_view_bits |= local_view_bit;
        ok = true;
      }
      FOREACH_BASE_IN_EDIT_MODE_END;
    }
    else {
      for (base = FIRSTBASE(view_layer); base; base = base->next) {
        if (BASE_SELECTED(v3d, base)) {
          BKE_object_minmax(base->object, min, max, false);
          base->local_view_bits |= local_view_bit;
          ok = true;
        }
        else {
          base->local_view_bits &= ~local_view_bit;
        }
      }
    }

    sub_v3_v3v3(box, max, min);
    size = max_fff(box[0], box[1], box[2]);
  }

  if (ok == false) {
    return false;
  }

  ARegion *region;

  v3d->localvd = MEM_mallocN(sizeof(View3D), "localview");

  memcpy(v3d->localvd, v3d, sizeof(View3D));
  v3d->local_view_uuid = local_view_bit;

  for (region = area->regionbase.first; region; region = region->next) {
    if (region->regiontype == RGN_TYPE_WINDOW) {
      RegionView3D *rv3d = region->regiondata;
      bool ok_dist = true;

      /* New view values. */
      Object *camera_old = NULL;
      float dist_new, ofs_new[3];

      rv3d->localvd = MEM_mallocN(sizeof(RegionView3D), "localview region");
      memcpy(rv3d->localvd, rv3d, sizeof(RegionView3D));

      if (frame_selected) {
        float mid[3];
        mid_v3_v3v3(mid, min, max);
        negate_v3_v3(ofs_new, mid);

        if (rv3d->persp == RV3D_CAMOB) {
          rv3d->persp = RV3D_PERSP;
          camera_old = v3d->camera;
        }

        if (rv3d->persp == RV3D_ORTHO) {
          if (size < 0.0001f) {
            ok_dist = false;
          }
        }

        if (ok_dist) {
          dist_new = ED_view3d_radius_to_dist(
              v3d, region, depsgraph, rv3d->persp, true, (size / 2) * VIEW3D_MARGIN);

          if (rv3d->persp == RV3D_PERSP) {
            /* Don't zoom closer than the near clipping plane. */
            dist_new = max_ff(dist_new, v3d->clip_start * 1.5f);
          }
        }

        ED_view3d_smooth_view_ex(depsgraph,
                                 wm,
                                 win,
                                 area,
                                 v3d,
                                 region,
                                 smooth_viewtx,
                                 &(const V3D_SmoothParams){
                                     .camera_old = camera_old,
                                     .ofs = ofs_new,
                                     .quat = rv3d->viewquat,
                                     .dist = ok_dist ? &dist_new : NULL,
                                     .lens = &v3d->lens,
                                     /* No undo because this doesn't move the camera. */
                                     .undo_str = NULL,
                                 });
      }
    }
  }

  return ok;
}

static void view3d_localview_exit(const Depsgraph *depsgraph,
                                  wmWindowManager *wm,
                                  wmWindow *win,
                                  ViewLayer *view_layer,
                                  ScrArea *area,
                                  const bool frame_selected,
                                  const int smooth_viewtx)
{
  View3D *v3d = area->spacedata.first;

  if (v3d->localvd == NULL) {
    return;
  }

  for (Base *base = FIRSTBASE(view_layer); base; base = base->next) {
    if (base->local_view_bits & v3d->local_view_uuid) {
      base->local_view_bits &= ~v3d->local_view_uuid;
    }
  }

  Object *camera_old = v3d->camera;
  Object *camera_new = v3d->localvd->camera;

  v3d->local_view_uuid = 0;
  v3d->camera = v3d->localvd->camera;

  MEM_freeN(v3d->localvd);
  v3d->localvd = NULL;
  MEM_SAFE_FREE(v3d->runtime.local_stats);

  LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
    if (region->regiontype == RGN_TYPE_WINDOW) {
      RegionView3D *rv3d = region->regiondata;

      if (rv3d->localvd == NULL) {
        continue;
      }

      if (frame_selected) {
        Object *camera_old_rv3d, *camera_new_rv3d;

        camera_old_rv3d = (rv3d->persp == RV3D_CAMOB) ? camera_old : NULL;
        camera_new_rv3d = (rv3d->localvd->persp == RV3D_CAMOB) ? camera_new : NULL;

        rv3d->view = rv3d->localvd->view;
        rv3d->persp = rv3d->localvd->persp;
        rv3d->camzoom = rv3d->localvd->camzoom;

        ED_view3d_smooth_view_ex(depsgraph,
                                 wm,
                                 win,
                                 area,
                                 v3d,
                                 region,
                                 smooth_viewtx,
                                 &(const V3D_SmoothParams){
                                     .camera_old = camera_old_rv3d,
                                     .camera = camera_new_rv3d,
                                     .ofs = rv3d->localvd->ofs,
                                     .quat = rv3d->localvd->viewquat,
                                     .dist = &rv3d->localvd->dist,
                                     /* No undo because this doesn't move the camera. */
                                     .undo_str = NULL,
                                 });
      }

      MEM_freeN(rv3d->localvd);
      rv3d->localvd = NULL;
    }
  }
}

static int localview_exec(bContext *C, wmOperator *op)
{
  const Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const int smooth_viewtx = WM_operator_smooth_viewtx_get(op);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = CTX_wm_view3d(C);
  bool frame_selected = RNA_boolean_get(op->ptr, "frame_selected");
  bool changed;

  if (v3d->localvd) {
    view3d_localview_exit(depsgraph, wm, win, view_layer, area, frame_selected, smooth_viewtx);
    changed = true;
  }
  else {
    changed = view3d_localview_init(
        depsgraph, wm, win, bmain, view_layer, area, frame_selected, smooth_viewtx, op->reports);
  }

  if (changed) {
    DEG_id_type_tag(bmain, ID_OB);
    ED_area_tag_redraw(area);

    /* Unselected objects become selected when exiting. */
    if (v3d->localvd == NULL) {
      DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
    }
    else {
      DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
    }

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_localview(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Local View";
  ot->description = "Toggle display of selected object(s) separately and centered in view";
  ot->idname = "VIEW3D_OT_localview";

  /* api callbacks */
  ot->exec = localview_exec;
  ot->flag = OPTYPE_UNDO; /* localview changes object layer bitflags */

  ot->poll = ED_operator_view3d_active;

  RNA_def_boolean(ot->srna,
                  "frame_selected",
                  true,
                  "Frame Selected",
                  "Move the view to frame the selected objects");
}

static int localview_remove_from_exec(bContext *C, wmOperator *op)
{
  View3D *v3d = CTX_wm_view3d(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  bool changed = false;

  for (Base *base = FIRSTBASE(view_layer); base; base = base->next) {
    if (BASE_SELECTED(v3d, base)) {
      base->local_view_bits &= ~v3d->local_view_uuid;
      ED_object_base_select(base, BA_DESELECT);

      if (base == BASACT(view_layer)) {
        view_layer->basact = NULL;
      }
      changed = true;
    }
  }

  if (changed) {
    DEG_tag_on_visible_update(bmain, false);
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
    return OPERATOR_FINISHED;
  }

  BKE_report(op->reports, RPT_ERROR, "No object selected");
  return OPERATOR_CANCELLED;
}

static bool localview_remove_from_poll(bContext *C)
{
  if (CTX_data_edit_object(C) != NULL) {
    return false;
  }

  View3D *v3d = CTX_wm_view3d(C);
  return v3d && v3d->localvd;
}

void VIEW3D_OT_localview_remove_from(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove from Local View";
  ot->description = "Move selected objects out of local view";
  ot->idname = "VIEW3D_OT_localview_remove_from";

  /* api callbacks */
  ot->exec = localview_remove_from_exec;
  ot->poll = localview_remove_from_poll;
  ot->flag = OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Collections
 * \{ */

static uint free_localcollection_bit(Main *bmain, ushort local_collections_uuid, bool *r_reset)
{
  ScrArea *area;
  bScreen *screen;

  ushort local_view_bits = 0;

  /* Check all areas: which local-views are in use? */
  for (screen = bmain->screens.first; screen; screen = screen->id.next) {
    for (area = screen->areabase.first; area; area = area->next) {
      SpaceLink *sl = area->spacedata.first;
      for (; sl; sl = sl->next) {
        if (sl->spacetype == SPACE_VIEW3D) {
          View3D *v3d = (View3D *)sl;
          if (v3d->flag & V3D_LOCAL_COLLECTIONS) {
            local_view_bits |= v3d->local_collections_uuid;
          }
        }
      }
    }
  }

  /* First try to keep the old uuid. */
  if (local_collections_uuid && ((local_collections_uuid & local_view_bits) == 0)) {
    return local_collections_uuid;
  }

  /* Otherwise get the first free available. */
  for (int i = 0; i < 16; i++) {
    if ((local_view_bits & (1 << i)) == 0) {
      *r_reset = true;
      return (1 << i);
    }
  }

  return 0;
}

static void local_collections_reset_uuid(LayerCollection *layer_collection,
                                         const ushort local_view_bit)
{
  if (layer_collection->flag & LAYER_COLLECTION_HIDE) {
    layer_collection->local_collections_bits &= ~local_view_bit;
  }
  else {
    layer_collection->local_collections_bits |= local_view_bit;
  }

  LISTBASE_FOREACH (LayerCollection *, child, &layer_collection->layer_collections) {
    local_collections_reset_uuid(child, local_view_bit);
  }
}

static void view3d_local_collections_reset(Main *bmain, const uint local_view_bit)
{
  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
      LISTBASE_FOREACH (LayerCollection *, layer_collection, &view_layer->layer_collections) {
        local_collections_reset_uuid(layer_collection, local_view_bit);
      }
    }
  }
}

bool ED_view3d_local_collections_set(Main *bmain, struct View3D *v3d)
{
  if ((v3d->flag & V3D_LOCAL_COLLECTIONS) == 0) {
    return true;
  }

  bool reset = false;
  v3d->flag &= ~V3D_LOCAL_COLLECTIONS;
  uint local_view_bit = free_localcollection_bit(bmain, v3d->local_collections_uuid, &reset);

  if (local_view_bit == 0) {
    return false;
  }

  v3d->local_collections_uuid = local_view_bit;
  v3d->flag |= V3D_LOCAL_COLLECTIONS;

  if (reset) {
    view3d_local_collections_reset(bmain, local_view_bit);
  }

  return true;
}

void ED_view3d_local_collections_reset(struct bContext *C, const bool reset_all)
{
  Main *bmain = CTX_data_main(C);
  uint local_view_bit = ~(0);
  bool do_reset = false;

  /* Reset only the ones that are not in use. */
  LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
        if (sl->spacetype == SPACE_VIEW3D) {
          View3D *v3d = (View3D *)sl;
          if (v3d->local_collections_uuid) {
            if (v3d->flag & V3D_LOCAL_COLLECTIONS) {
              local_view_bit &= ~v3d->local_collections_uuid;
            }
            else {
              do_reset = true;
            }
          }
        }
      }
    }
  }

  if (do_reset) {
    view3d_local_collections_reset(bmain, local_view_bit);
  }
  else if (reset_all && (do_reset || (local_view_bit != ~(0)))) {
    view3d_local_collections_reset(bmain, ~(0));
    View3D v3d = {.local_collections_uuid = ~(0)};
    BKE_layer_collection_local_sync(CTX_data_view_layer(C), &v3d);
    DEG_id_tag_update(&CTX_data_scene(C)->id, ID_RECALC_BASE_FLAGS);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name XR Functionality
 * \{ */

#ifdef WITH_XR_OPENXR

static void view3d_xr_mirror_begin(RegionView3D *rv3d)
{
  /* If there is no session yet, changes below should not be applied! */
  BLI_assert(WM_xr_session_exists(&((wmWindowManager *)G_MAIN->wm.first)->xr));

  rv3d->runtime_viewlock |= RV3D_LOCK_ANY_TRANSFORM;
  /* Force perspective view. This isn't reset but that's not really an issue. */
  rv3d->persp = RV3D_PERSP;
}

static void view3d_xr_mirror_end(RegionView3D *rv3d)
{
  rv3d->runtime_viewlock &= ~RV3D_LOCK_ANY_TRANSFORM;
}

void ED_view3d_xr_mirror_update(const ScrArea *area, const View3D *v3d, const bool enable)
{
  ARegion *region_rv3d;

  BLI_assert(v3d->spacetype == SPACE_VIEW3D);

  if (ED_view3d_area_user_region(area, v3d, &region_rv3d)) {
    if (enable) {
      view3d_xr_mirror_begin(region_rv3d->regiondata);
    }
    else {
      view3d_xr_mirror_end(region_rv3d->regiondata);
    }
  }
}

void ED_view3d_xr_shading_update(wmWindowManager *wm, const View3D *v3d, const Scene *scene)
{
  if (v3d->runtime.flag & V3D_RUNTIME_XR_SESSION_ROOT) {
    View3DShading *xr_shading = &wm->xr.session_settings.shading;
    /* Flags that shouldn't be overridden by the 3D View shading. */
    int flag_copy = 0;
    if (v3d->shading.type != OB_SOLID) {
      /* Don't set V3D_SHADING_WORLD_ORIENTATION for solid shading since it results in distorted
       * lighting when the view matrix has a scale factor. */
      flag_copy |= V3D_SHADING_WORLD_ORIENTATION;
    }

    BLI_assert(WM_xr_session_exists(&wm->xr));

    if (v3d->shading.type == OB_RENDER) {
      if (!(BKE_scene_uses_blender_workbench(scene) || BKE_scene_uses_blender_eevee(scene))) {
        /* Keep old shading while using Cycles or another engine, they are typically not usable in
         * VR. */
        return;
      }
    }

    if (xr_shading->prop) {
      IDP_FreeProperty(xr_shading->prop);
      xr_shading->prop = NULL;
    }

    /* Copy shading from View3D to VR view. */
    const int old_xr_shading_flag = xr_shading->flag;
    *xr_shading = v3d->shading;
    xr_shading->flag = (xr_shading->flag & ~flag_copy) | (old_xr_shading_flag & flag_copy);
    if (v3d->shading.prop) {
      xr_shading->prop = IDP_CopyProperty(xr_shading->prop);
    }
  }
}

bool ED_view3d_is_region_xr_mirror_active(const wmWindowManager *wm,
                                          const View3D *v3d,
                                          const ARegion *region)
{
  return (v3d->flag & V3D_XR_SESSION_MIRROR) &&
         /* The free region (e.g. the camera region in quad-view) is always
          * the last in the list base. We don't want any other to be affected. */
         !region->next &&  //
         WM_xr_session_is_ready(&wm->xr);
}

#endif

/** \name Game Engine Operator
 *
 * Start the game engine (handles context switching).
 * \{ */

#ifdef WITH_GAMEENGINE

static ListBase events_queue_back;

static void game_engine_save_state(bContext *C, wmWindow *win)
{
  Object *obact = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);

  /*gpuPushAttr(GPU_DEPTH_BUFFER_BIT | GPU_ENABLE_BIT | GPU_SCISSOR_BIT | GPU_VIEWPORT_BIT |
              GPU_BLEND_BIT);*/

  if (obact && obact->mode & OB_MODE_TEXTURE_PAINT) {
    BKE_image_paint_set_mipmap(bmain, 1);
  }

  events_queue_back = win->event_queue;

  BLI_listbase_clear(&win->event_queue);
}

static void game_engine_restore_state(bContext *C, wmWindow *win)
{
  Object *obact = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);

  if (obact && obact->mode & OB_MODE_TEXTURE_PAINT) {
    BKE_image_paint_set_mipmap(bmain, 0);
  }
  /* check because closing win can set to NULL */
  if (win) {
    win->event_queue = events_queue_back;
  }

  /*GPU_state_init();

  gpuPopAttr();*/
}

/* was space_set_commmandline_options in 2.4x */
static void game_set_commmandline_options(GameData *gm)
{
  SYS_SystemHandle syshandle;
  int test;

  if ((syshandle = SYS_GetSystem())) {
    // GPU_set_mipmap(bmain, true);

    /* File specific settings: */
    /* Only test the first one. These two are switched
     * simultaneously. */
    test = (gm->flag & GAME_SHOW_FRAMERATE);
    SYS_WriteCommandLineInt(syshandle, "show_framerate", test);
    SYS_WriteCommandLineInt(syshandle, "show_profile", test);

    test = (gm->flag & GAME_SHOW_DEBUG_PROPS);
    SYS_WriteCommandLineInt(syshandle, "show_properties", test);

    test = (gm->flag & GAME_SHOW_PHYSICS);
    SYS_WriteCommandLineInt(syshandle, "show_physics", test);

    test = (gm->flag & GAME_ENABLE_ALL_FRAMES);
    SYS_WriteCommandLineInt(syshandle, "fixedtime", test);

    test = (gm->flag & GAME_ENABLE_ANIMATION_RECORD);
    SYS_WriteCommandLineInt(syshandle, "animation_record", test);

    test = (gm->flag & GAME_IGNORE_DEPRECATION_WARNINGS);
    SYS_WriteCommandLineInt(syshandle, "ignore_deprecation_warnings", test);
  }
}

#endif /* WITH_GAMEENGINE */

static bool game_engine_poll(bContext *C)
{
  const wmWindow *win = CTX_wm_window(C);
  // const Scene *scene = WM_window_get_active_scene(win);

  /* we need a context and area to launch BGE
   * it's a temporary solution to avoid crash at load time
   * if we try to auto run the BGE. Ideally we want the
   * context to be set as soon as we load the file. */

  if (win == NULL)
    return 0;
  if (CTX_wm_screen(C) == NULL)
    return 0;

  if (CTX_data_mode_enum(C) != CTX_MODE_OBJECT)
    return 0;

  // if (!BKE_scene_uses_blender_game(scene))
  // return 0;

  return 1;
}

static int game_engine_exec(bContext *C, wmOperator *op)
{
#ifdef WITH_GAMEENGINE
  Scene *startscene = CTX_data_scene(C);
  Main *bmain = CTX_data_main(C);
  ScrArea /* *sa, */ /* UNUSED */ *prevsa = CTX_wm_area(C);
  ARegion *ar, *prevar = CTX_wm_region(C);
  wmWindow *prevwin = CTX_wm_window(C);
  RegionView3D *rv3d;
  rcti cam_frame;

  UNUSED_VARS(op);

  /* Redraw 1 time before context switch (switch to view3d)
   * to avoid embedded button flickering when we start embedded
   * player from embedded start button (Issue on some computers (youle)).
   */
  if (prevsa == NULL || prevsa->spacetype != SPACE_VIEW3D) {
    ED_region_tag_redraw(
        prevar);  // "properties render" region (where is the embedded start button)
    WM_redraw_windows(C);
  }

  /* bad context switch .. */
  if (!ED_view3d_context_activate(C))
    return OPERATOR_CANCELLED;

#  ifdef WITH_XR_OPENXR
  wmWindowManager *wm = CTX_wm_manager(C);
  if (WM_xr_session_exists(&wm->xr)) {
    if (WM_xr_session_is_ready(&wm->xr)) {
      startscene->flag |= SCE_IS_GAME_XR_SESSION;
    }
  }
#  endif

  /* Calling this seems to avoid some UI flickering on windows
   * later during runtime. */
  ED_area_tag_redraw(CTX_wm_area(C));

  /* Redraw to hide any menus/popups, we don't go back to
   * the window manager until after this operator exits */
  WM_redraw_windows(C);

  BKE_callback_exec_null(bmain, BKE_CB_EVT_GAME_PRE);

  rv3d = CTX_wm_region_view3d(C);
  /* sa = CTX_wm_area(C); */ /* UNUSED */
  ar = CTX_wm_region(C);

  view3d_operator_needs_opengl(C);

  game_set_commmandline_options(&startscene->gm);

  if ((rv3d->persp == RV3D_CAMOB) && (startscene->gm.framing.type == SCE_GAMEFRAMING_BARS)) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    /* Letterbox */
    rctf cam_framef;
    ED_view3d_calc_camera_border(
        startscene, depsgraph, ar, CTX_wm_view3d(C), rv3d, &cam_framef, false);
    cam_frame.xmin = cam_framef.xmin + ar->winrct.xmin;
    cam_frame.xmax = cam_framef.xmax + ar->winrct.xmin;
    cam_frame.ymin = cam_framef.ymin + ar->winrct.ymin;
    cam_frame.ymax = cam_framef.ymax + ar->winrct.ymin;
    BLI_rcti_isect(&ar->winrct, &cam_frame, &cam_frame);
  }
  else {
    cam_frame.xmin = ar->winrct.xmin;
    cam_frame.xmax = ar->winrct.xmax;
    cam_frame.ymin = ar->winrct.ymin;
    cam_frame.ymax = ar->winrct.ymax;
  }

  game_engine_save_state(C, prevwin);

  /* We can kill existing threads by precaution before ge start */
  WM_jobs_kill_all(CTX_wm_manager(C));

  StartKetsjiShell(C, ar, &cam_frame, 1);

  /* window wasnt closed while the BGE was running */
  if (BLI_findindex(&CTX_wm_manager(C)->windows, prevwin) == -1) {
    prevwin = NULL;
    CTX_wm_window_set(C, NULL);
  }

  ED_area_tag_redraw(CTX_wm_area(C));

  if (prevwin) {
    /* restore context, in case it changed in the meantime, for
     * example by working in another window or closing it */
    CTX_wm_region_set(C, prevar);
    CTX_wm_window_set(C, prevwin);
    CTX_wm_area_set(C, prevsa);
  }

  CTX_data_scene(C)->flag &= ~SCE_IS_GAME_XR_SESSION;

  game_engine_restore_state(C, prevwin);

  // XXX restore_all_scene_cfra(scene_cfra_store);
  // BKE_scene_set_background(CTX_data_main(C), startscene);
  // XXX BKE_scene_graph_update_for_newframe(depsgraph, bmain);

  BKE_callback_exec_null(bmain, BKE_CB_EVT_GAME_POST);

  return OPERATOR_FINISHED;
#else
  UNUSED_VARS(C);
  BKE_report(op->reports, RPT_ERROR, "Game engine is disabled in this build");
  return OPERATOR_CANCELLED;
#endif
}

void VIEW3D_OT_game_start(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Start Game Engine";
  ot->description = "Start game engine";
  ot->idname = "VIEW3D_OT_game_start";

  /* api callbacks */
  ot->exec = game_engine_exec;

  ot->poll = game_engine_poll;
}

/** \} */
