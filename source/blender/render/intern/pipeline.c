/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup render
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "DNA_anim_types.h"
#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_timecode.h"

#include "BLT_translation.h"

#include "BKE_anim_data.h"
#include "BKE_animsys.h" /* <------ should this be here?, needed for sequencer update */
#include "BKE_callbacks.h"
#include "BKE_camera.h"
#include "BKE_colortools.h"
#include "BKE_context.h" /* XXX needed by wm_window.h */
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BKE_image_save.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_lib_remap.h"
#include "BKE_mask.h"
#include "BKE_modifier.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_pointcache.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_sound.h"
#include "BKE_writeavi.h" /* <------ should be replaced once with generic movie module */

#include "NOD_composite.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_debug.h"
#include "DEG_depsgraph_query.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_metadata.h"
#include "PIL_time.h"

#include "RE_engine.h"
#include "RE_pipeline.h"
#include "RE_texture.h"

#include "SEQ_relations.h"
#include "SEQ_render.h"

#include "../../windowmanager/WM_api.h"    /* XXX */
#include "../../windowmanager/wm_window.h" /* XXX */
#include "GPU_context.h"

#ifdef WITH_FREESTYLE
#  include "FRS_freestyle.h"
#endif

/* internal */
#include "pipeline.h"
#include "render_result.h"
#include "render_types.h"

/* render flow
 *
 * 1) Initialize state
 * - state data, tables
 * - movie/image file init
 * - everything that doesn't change during animation
 *
 * 2) Initialize data
 * - camera, world, matrices
 * - make render verts, faces, halos, strands
 * - everything can change per frame/field
 *
 * 3) Render Processor
 * - multiple layers
 * - tiles, rect, baking
 * - layers/tiles optionally to disk or directly in Render Result
 *
 * 4) Composite Render Result
 * - also read external files etc
 *
 * 5) Image Files
 * - save file or append in movie
 */

/* -------------------------------------------------------------------- */
/** \name Globals
 * \{ */

/* here we store all renders */
static struct {
  ListBase renderlist;
} RenderGlobal = {{NULL, NULL}};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks
 * \{ */

static void render_callback_exec_null(Render *re, Main *bmain, eCbEvent evt)
{
  if (re->r.scemode & R_BUTS_PREVIEW) {
    return;
  }
  BKE_callback_exec_null(bmain, evt);
}

static void render_callback_exec_id(Render *re, Main *bmain, ID *id, eCbEvent evt)
{
  if (re->r.scemode & R_BUTS_PREVIEW) {
    return;
  }
  BKE_callback_exec_id(bmain, id, evt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Allocation & Free
 * \{ */

static bool do_write_image_or_movie(Render *re,
                                    Main *bmain,
                                    Scene *scene,
                                    bMovieHandle *mh,
                                    const int totvideos,
                                    const char *name_override);

/* default callbacks, set in each new render */
static void result_nothing(void *UNUSED(arg), RenderResult *UNUSED(rr))
{
}
static void result_rcti_nothing(void *UNUSED(arg),
                                RenderResult *UNUSED(rr),
                                struct rcti *UNUSED(rect))
{
}
static void current_scene_nothing(void *UNUSED(arg), Scene *UNUSED(scene))
{
}
static void stats_nothing(void *UNUSED(arg), RenderStats *UNUSED(rs))
{
}
static void float_nothing(void *UNUSED(arg), float UNUSED(val))
{
}
static int default_break(void *UNUSED(arg))
{
  return G.is_break == true;
}

static void stats_background(void *UNUSED(arg), RenderStats *rs)
{
  if (rs->infostr == NULL) {
    return;
  }

  uintptr_t mem_in_use, peak_memory;
  float megs_used_memory, megs_peak_memory;
  char info_time_str[32];

  mem_in_use = MEM_get_memory_in_use();
  peak_memory = MEM_get_peak_memory();

  megs_used_memory = (mem_in_use) / (1024.0 * 1024.0);
  megs_peak_memory = (peak_memory) / (1024.0 * 1024.0);

  BLI_timecode_string_from_time_simple(
      info_time_str, sizeof(info_time_str), PIL_check_seconds_timer() - rs->starttime);

  /* Compositor calls this from multiple threads, mutex lock to ensure we don't
   * get garbled output. */
  static ThreadMutex mutex = BLI_MUTEX_INITIALIZER;
  BLI_mutex_lock(&mutex);

  fprintf(stdout,
          TIP_("Fra:%d Mem:%.2fM (Peak %.2fM) "),
          rs->cfra,
          megs_used_memory,
          megs_peak_memory);

  fprintf(stdout, TIP_("| Time:%s | "), info_time_str);

  fprintf(stdout, "%s", rs->infostr);

  /* Flush stdout to be sure python callbacks are printing stuff after blender. */
  fflush(stdout);

  /* NOTE: using G_MAIN seems valid here???
   * Not sure it's actually even used anyway, we could as well pass NULL? */
  BKE_callback_exec_null(G_MAIN, BKE_CB_EVT_RENDER_STATS);

  fputc('\n', stdout);
  fflush(stdout);

  BLI_mutex_unlock(&mutex);
}

void RE_FreeRenderResult(RenderResult *rr)
{
  render_result_free(rr);
}

float *RE_RenderLayerGetPass(RenderLayer *rl, const char *name, const char *viewname)
{
  RenderPass *rpass = RE_pass_find_by_name(rl, name, viewname);
  return rpass ? rpass->rect : NULL;
}

RenderLayer *RE_GetRenderLayer(RenderResult *rr, const char *name)
{
  if (rr == NULL) {
    return NULL;
  }

  return BLI_findstring(&rr->layers, name, offsetof(RenderLayer, name));
}

bool RE_HasSingleLayer(Render *re)
{
  return (re->r.scemode & R_SINGLE_LAYER);
}

RenderResult *RE_MultilayerConvert(
    void *exrhandle, const char *colorspace, bool predivide, int rectx, int recty)
{
  return render_result_new_from_exr(exrhandle, colorspace, predivide, rectx, recty);
}

RenderLayer *render_get_active_layer(Render *re, RenderResult *rr)
{
  ViewLayer *view_layer = BLI_findlink(&re->view_layers, re->active_view_layer);

  if (view_layer) {
    RenderLayer *rl = BLI_findstring(&rr->layers, view_layer->name, offsetof(RenderLayer, name));

    if (rl) {
      return rl;
    }
  }

  return rr->layers.first;
}

static bool render_scene_has_layers_to_render(Scene *scene, ViewLayer *single_layer)
{
  if (single_layer) {
    return true;
  }

  ViewLayer *view_layer;
  for (view_layer = scene->view_layers.first; view_layer; view_layer = view_layer->next) {
    if (view_layer->flag & VIEW_LAYER_RENDER) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Render API
 * \{ */

Render *RE_GetRender(const char *name)
{
  Render *re;

  /* search for existing renders */
  for (re = RenderGlobal.renderlist.first; re; re = re->next) {
    if (STREQLEN(re->name, name, RE_MAXNAME)) {
      break;
    }
  }

  return re;
}

RenderResult *RE_AcquireResultRead(Render *re)
{
  if (re) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_READ);
    return re->result;
  }

  return NULL;
}

RenderResult *RE_AcquireResultWrite(Render *re)
{
  if (re) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);
    render_result_passes_allocated_ensure(re->result);
    return re->result;
  }

  return NULL;
}

void RE_ClearResult(Render *re)
{
  if (re) {
    render_result_free(re->result);
    re->result = NULL;
  }
}

void RE_SwapResult(Render *re, RenderResult **rr)
{
  /* for keeping render buffers */
  if (re) {
    SWAP(RenderResult *, re->result, *rr);
  }
}

void RE_ReleaseResult(Render *re)
{
  if (re) {
    BLI_rw_mutex_unlock(&re->resultmutex);
  }
}

Scene *RE_GetScene(Render *re)
{
  if (re) {
    return re->scene;
  }
  return NULL;
}

void RE_SetScene(Render *re, Scene *sce)
{
  if (re) {
    re->scene = sce;
  }
}

void RE_AcquireResultImageViews(Render *re, RenderResult *rr)
{
  memset(rr, 0, sizeof(RenderResult));

  if (re) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_READ);

    if (re->result) {
      RenderLayer *rl;
      RenderView *rv, *rview;

      rr->rectx = re->result->rectx;
      rr->recty = re->result->recty;

      /* creates a temporary duplication of views */
      render_result_views_shallowcopy(rr, re->result);

      rv = rr->views.first;
      rr->have_combined = (rv->rectf != NULL);

      /* active layer */
      rl = render_get_active_layer(re, re->result);

      if (rl) {
        if (rv->rectf == NULL) {
          for (rview = (RenderView *)rr->views.first; rview; rview = rview->next) {
            rview->rectf = RE_RenderLayerGetPass(rl, RE_PASSNAME_COMBINED, rview->name);
          }
        }

        if (rv->rectz == NULL) {
          for (rview = (RenderView *)rr->views.first; rview; rview = rview->next) {
            rview->rectz = RE_RenderLayerGetPass(rl, RE_PASSNAME_Z, rview->name);
          }
        }
      }

      rr->layers = re->result->layers;
      rr->xof = re->disprect.xmin;
      rr->yof = re->disprect.ymin;
      rr->stamp_data = re->result->stamp_data;
    }
  }
}

void RE_ReleaseResultImageViews(Render *re, RenderResult *rr)
{
  if (re) {
    if (rr) {
      render_result_views_shallowdelete(rr);
    }
    BLI_rw_mutex_unlock(&re->resultmutex);
  }
}

void RE_AcquireResultImage(Render *re, RenderResult *rr, const int view_id)
{
  memset(rr, 0, sizeof(RenderResult));

  if (re) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_READ);

    if (re->result) {
      RenderLayer *rl;
      RenderView *rv;

      rr->rectx = re->result->rectx;
      rr->recty = re->result->recty;

      /* actview view */
      rv = RE_RenderViewGetById(re->result, view_id);
      rr->have_combined = (rv->rectf != NULL);

      rr->rectf = rv->rectf;
      rr->rectz = rv->rectz;
      rr->rect32 = rv->rect32;

      /* active layer */
      rl = render_get_active_layer(re, re->result);

      if (rl) {
        if (rv->rectf == NULL) {
          rr->rectf = RE_RenderLayerGetPass(rl, RE_PASSNAME_COMBINED, rv->name);
        }

        if (rv->rectz == NULL) {
          rr->rectz = RE_RenderLayerGetPass(rl, RE_PASSNAME_Z, rv->name);
        }
      }

      rr->layers = re->result->layers;
      rr->views = re->result->views;

      rr->xof = re->disprect.xmin;
      rr->yof = re->disprect.ymin;

      rr->stamp_data = re->result->stamp_data;
    }
  }
}

void RE_ReleaseResultImage(Render *re)
{
  if (re) {
    BLI_rw_mutex_unlock(&re->resultmutex);
  }
}

void RE_ResultGet32(Render *re, unsigned int *rect)
{
  RenderResult rres;
  const int view_id = BKE_scene_multiview_view_id_get(&re->r, re->viewname);

  RE_AcquireResultImageViews(re, &rres);
  render_result_rect_get_pixels(&rres,
                                rect,
                                re->rectx,
                                re->recty,
                                &re->scene->view_settings,
                                &re->scene->display_settings,
                                view_id);
  RE_ReleaseResultImageViews(re, &rres);
}

void RE_AcquiredResultGet32(Render *re,
                            RenderResult *result,
                            unsigned int *rect,
                            const int view_id)
{
  render_result_rect_get_pixels(result,
                                rect,
                                re->rectx,
                                re->recty,
                                &re->scene->view_settings,
                                &re->scene->display_settings,
                                view_id);
}

RenderStats *RE_GetStats(Render *re)
{
  return &re->i;
}

Render *RE_NewRender(const char *name)
{
  Render *re;

  /* only one render per name exists */
  re = RE_GetRender(name);
  if (re == NULL) {

    /* new render data struct */
    re = MEM_callocN(sizeof(Render), "new render");
    BLI_addtail(&RenderGlobal.renderlist, re);
    BLI_strncpy(re->name, name, RE_MAXNAME);
    BLI_rw_mutex_init(&re->resultmutex);
    BLI_mutex_init(&re->engine_draw_mutex);
    BLI_mutex_init(&re->highlighted_tiles_mutex);
  }

  RE_InitRenderCB(re);

  return re;
}

/* MAX_ID_NAME + sizeof(Library->name) + space + null-terminator. */
#define MAX_SCENE_RENDER_NAME (MAX_ID_NAME + 1024 + 2)

static void scene_render_name_get(const Scene *scene, const size_t max_size, char *render_name)
{
  if (ID_IS_LINKED(scene)) {
    BLI_snprintf(render_name, max_size, "%s %s", scene->id.lib->id.name, scene->id.name);
  }
  else {
    BLI_snprintf(render_name, max_size, "%s", scene->id.name);
  }
}

Render *RE_GetSceneRender(const Scene *scene)
{
  char render_name[MAX_SCENE_RENDER_NAME];
  scene_render_name_get(scene, sizeof(render_name), render_name);
  return RE_GetRender(render_name);
}

Render *RE_NewSceneRender(const Scene *scene)
{
  char render_name[MAX_SCENE_RENDER_NAME];
  scene_render_name_get(scene, sizeof(render_name), render_name);
  return RE_NewRender(render_name);
}

void RE_InitRenderCB(Render *re)
{
  /* set default empty callbacks */
  re->display_init = result_nothing;
  re->display_clear = result_nothing;
  re->display_update = result_rcti_nothing;
  re->current_scene_update = current_scene_nothing;
  re->progress = float_nothing;
  re->test_break = default_break;
  if (G.background) {
    re->stats_draw = stats_background;
  }
  else {
    re->stats_draw = stats_nothing;
  }
  /* clear callback handles */
  re->dih = re->dch = re->duh = re->sdh = re->prh = re->tbh = NULL;
}

void RE_FreeRender(Render *re)
{
  if (re->engine) {
    RE_engine_free(re->engine);
  }

  BLI_rw_mutex_end(&re->resultmutex);
  BLI_mutex_end(&re->engine_draw_mutex);
  BLI_mutex_end(&re->highlighted_tiles_mutex);

  BLI_freelistN(&re->view_layers);
  BLI_freelistN(&re->r.views);

  BKE_curvemapping_free_data(&re->r.mblur_shutter_curve);

  if (re->highlighted_tiles != NULL) {
    BLI_gset_free(re->highlighted_tiles, MEM_freeN);
  }

  /* main dbase can already be invalid now, some database-free code checks it */
  re->main = NULL;
  re->scene = NULL;

  render_result_free(re->result);
  render_result_free(re->pushedresult);

  BLI_remlink(&RenderGlobal.renderlist, re);
  MEM_freeN(re);
}

void RE_FreeAllRender(void)
{
  while (RenderGlobal.renderlist.first) {
    RE_FreeRender(RenderGlobal.renderlist.first);
  }

#ifdef WITH_FREESTYLE
  /* finalize Freestyle */
  FRS_exit();
#endif
}

void RE_FreeAllRenderResults(void)
{
  Render *re;

  for (re = RenderGlobal.renderlist.first; re; re = re->next) {
    render_result_free(re->result);
    render_result_free(re->pushedresult);

    re->result = NULL;
    re->pushedresult = NULL;
  }
}

void RE_FreeAllPersistentData(void)
{
  Render *re;
  for (re = RenderGlobal.renderlist.first; re != NULL; re = re->next) {
    if (re->engine != NULL) {
      BLI_assert(!(re->engine->flag & RE_ENGINE_RENDERING));
      RE_engine_free(re->engine);
      re->engine = NULL;
    }
  }
}

static void re_free_persistent_data(Render *re)
{
  /* If engine is currently rendering, just wait for it to be freed when it finishes rendering. */
  if (re->engine && !(re->engine->flag & RE_ENGINE_RENDERING)) {
    RE_engine_free(re->engine);
    re->engine = NULL;
  }
}

void RE_FreePersistentData(const Scene *scene)
{
  /* Render engines can be kept around for quick re-render, this clears all or one scene. */
  if (scene) {
    Render *re = RE_GetSceneRender(scene);
    if (re) {
      re_free_persistent_data(re);
    }
  }
  else {
    for (Render *re = RenderGlobal.renderlist.first; re; re = re->next) {
      re_free_persistent_data(re);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Initialize State
 * \{ */

static void re_init_resolution(Render *re, Render *source, int winx, int winy, rcti *disprect)
{
  re->winx = winx;
  re->winy = winy;
  if (source && (source->r.mode & R_BORDER)) {
    /* NOTE(@sergey): doesn't seem original bordered `disprect` is storing anywhere
     * after insertion on black happening in #do_render_engine(),
     * so for now simply re-calculate `disprect` using border from source renderer. */

    re->disprect.xmin = source->r.border.xmin * winx;
    re->disprect.xmax = source->r.border.xmax * winx;

    re->disprect.ymin = source->r.border.ymin * winy;
    re->disprect.ymax = source->r.border.ymax * winy;

    re->rectx = BLI_rcti_size_x(&re->disprect);
    re->recty = BLI_rcti_size_y(&re->disprect);

    /* copy border itself, since it could be used by external engines */
    re->r.border = source->r.border;
  }
  else if (disprect) {
    re->disprect = *disprect;
    re->rectx = BLI_rcti_size_x(&re->disprect);
    re->recty = BLI_rcti_size_y(&re->disprect);
  }
  else {
    re->disprect.xmin = re->disprect.ymin = 0;
    re->disprect.xmax = winx;
    re->disprect.ymax = winy;
    re->rectx = winx;
    re->recty = winy;
  }
}

void render_copy_renderdata(RenderData *to, RenderData *from)
{
  BLI_freelistN(&to->views);
  BKE_curvemapping_free_data(&to->mblur_shutter_curve);

  *to = *from;

  BLI_duplicatelist(&to->views, &from->views);
  BKE_curvemapping_copy_data(&to->mblur_shutter_curve, &from->mblur_shutter_curve);
}

void RE_InitState(Render *re,
                  Render *source,
                  RenderData *rd,
                  ListBase *render_layers,
                  ViewLayer *single_layer,
                  int winx,
                  int winy,
                  rcti *disprect)
{
  bool had_freestyle = (re->r.mode & R_EDGE_FRS) != 0;

  re->ok = true; /* maybe flag */

  re->i.starttime = PIL_check_seconds_timer();

  /* copy render data and render layers for thread safety */
  render_copy_renderdata(&re->r, rd);
  BLI_freelistN(&re->view_layers);
  BLI_duplicatelist(&re->view_layers, render_layers);
  re->active_view_layer = 0;

  if (source) {
    /* reuse border flags from source renderer */
    re->r.mode &= ~(R_BORDER | R_CROP);
    re->r.mode |= source->r.mode & (R_BORDER | R_CROP);

    /* dimensions shall be shared between all renderers */
    re->r.xsch = source->r.xsch;
    re->r.ysch = source->r.ysch;
    re->r.size = source->r.size;
  }

  re_init_resolution(re, source, winx, winy, disprect);

  /* disable border if it's a full render anyway */
  if (re->r.border.xmin == 0.0f && re->r.border.xmax == 1.0f && re->r.border.ymin == 0.0f &&
      re->r.border.ymax == 1.0f) {
    re->r.mode &= ~R_BORDER;
  }

  if (re->rectx < 1 || re->recty < 1 ||
      (BKE_imtype_is_movie(rd->im_format.imtype) && (re->rectx < 16 || re->recty < 16))) {
    BKE_report(re->reports, RPT_ERROR, "Image too small");
    re->ok = 0;
    return;
  }

  if (single_layer) {
    int index = BLI_findindex(render_layers, single_layer);
    if (index != -1) {
      re->active_view_layer = index;
      re->r.scemode |= R_SINGLE_LAYER;
    }
  }

  /* if preview render, we try to keep old result */
  BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);

  if (re->r.scemode & R_BUTS_PREVIEW) {
    if (had_freestyle || (re->r.mode & R_EDGE_FRS)) {
      /* freestyle manipulates render layers so always have to free */
      render_result_free(re->result);
      re->result = NULL;
    }
    else if (re->result) {
      ViewLayer *active_render_layer = BLI_findlink(&re->view_layers, re->active_view_layer);
      RenderLayer *rl;
      bool have_layer = false;

      for (rl = re->result->layers.first; rl; rl = rl->next) {
        if (STREQ(rl->name, active_render_layer->name)) {
          have_layer = true;
        }
      }

      if (re->result->rectx == re->rectx && re->result->recty == re->recty && have_layer) {
        /* keep render result, this avoids flickering black tiles
         * when the preview changes */
      }
      else {
        /* free because resolution changed */
        render_result_free(re->result);
        re->result = NULL;
      }
    }
  }
  else {

    /* make empty render result, so display callbacks can initialize */
    render_result_free(re->result);
    re->result = MEM_callocN(sizeof(RenderResult), "new render result");
    re->result->rectx = re->rectx;
    re->result->recty = re->recty;
    render_result_view_new(re->result, "");
  }

  BLI_rw_mutex_unlock(&re->resultmutex);

  RE_init_threadcount(re);

  RE_point_density_fix_linking();
}

void render_update_anim_renderdata(Render *re, RenderData *rd, ListBase *render_layers)
{
  /* filter */
  re->r.gauss = rd->gauss;

  /* motion blur */
  re->r.blurfac = rd->blurfac;

  /* freestyle */
  re->r.line_thickness_mode = rd->line_thickness_mode;
  re->r.unit_line_thickness = rd->unit_line_thickness;

  /* render layers */
  BLI_freelistN(&re->view_layers);
  BLI_duplicatelist(&re->view_layers, render_layers);

  /* render views */
  BLI_freelistN(&re->r.views);
  BLI_duplicatelist(&re->r.views, &rd->views);
}

void RE_display_init_cb(Render *re, void *handle, void (*f)(void *handle, RenderResult *rr))
{
  re->display_init = f;
  re->dih = handle;
}
void RE_display_clear_cb(Render *re, void *handle, void (*f)(void *handle, RenderResult *rr))
{
  re->display_clear = f;
  re->dch = handle;
}
void RE_display_update_cb(Render *re,
                          void *handle,
                          void (*f)(void *handle, RenderResult *rr, rcti *rect))
{
  re->display_update = f;
  re->duh = handle;
}
void RE_current_scene_update_cb(Render *re, void *handle, void (*f)(void *handle, Scene *scene))
{
  re->current_scene_update = f;
  re->suh = handle;
}
void RE_stats_draw_cb(Render *re, void *handle, void (*f)(void *handle, RenderStats *rs))
{
  re->stats_draw = f;
  re->sdh = handle;
}
void RE_progress_cb(Render *re, void *handle, void (*f)(void *handle, float))
{
  re->progress = f;
  re->prh = handle;
}

void RE_draw_lock_cb(Render *re, void *handle, void (*f)(void *handle, bool lock))
{
  re->draw_lock = f;
  re->dlh = handle;
}

void RE_test_break_cb(Render *re, void *handle, int (*f)(void *handle))
{
  re->test_break = f;
  re->tbh = handle;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OpenGL Context
 * \{ */

void RE_gl_context_create(Render *re)
{
  /* Needs to be created in the main ogl thread. */
  re->gl_context = WM_opengl_context_create();
  /* So we activate the window's one afterwards. */
  wm_window_reset_drawable();
}

void RE_gl_context_destroy(Render *re)
{
  /* Needs to be called from the thread which used the ogl context for rendering. */
  if (re->gl_context) {
    if (re->gpu_context) {
      WM_opengl_context_activate(re->gl_context);
      GPU_context_active_set(re->gpu_context);
      GPU_context_discard(re->gpu_context);
      re->gpu_context = NULL;
    }

    WM_opengl_context_dispose(re->gl_context);
    re->gl_context = NULL;
  }
}

void *RE_gl_context_get(Render *re)
{
  return re->gl_context;
}

void *RE_gpu_context_get(Render *re)
{
  if (re->gpu_context == NULL) {
    re->gpu_context = GPU_context_create(NULL);
  }
  return re->gpu_context;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Render & Composite Scenes (Implementation & Public API)
 *
 * Main high-level functions defined here are:
 * - #RE_RenderFrame
 * - #RE_RenderAnim
 * \{ */

/* ************  This part uses API, for rendering Blender scenes ********** */

/* make sure disprect is not affected by the render border */
static void render_result_disprect_to_full_resolution(Render *re)
{
  re->disprect.xmin = re->disprect.ymin = 0;
  re->disprect.xmax = re->winx;
  re->disprect.ymax = re->winy;
  re->rectx = re->winx;
  re->recty = re->winy;
}

static void render_result_uncrop(Render *re)
{
  /* when using border render with crop disabled, insert render result into
   * full size with black pixels outside */
  if (re->result && (re->r.mode & R_BORDER)) {
    if ((re->r.mode & R_CROP) == 0) {
      RenderResult *rres;

      /* backup */
      const rcti orig_disprect = re->disprect;
      const int orig_rectx = re->rectx, orig_recty = re->recty;

      BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);

      /* sub-rect for merge call later on */
      re->result->tilerect = re->disprect;

      /* weak is: it chances disprect from border */
      render_result_disprect_to_full_resolution(re);

      rres = render_result_new(re, &re->disprect, RR_ALL_LAYERS, RR_ALL_VIEWS);
      rres->stamp_data = BKE_stamp_data_copy(re->result->stamp_data);

      render_result_clone_passes(re, rres, NULL);
      render_result_passes_allocated_ensure(rres);

      render_result_merge(rres, re->result);
      render_result_free(re->result);
      re->result = rres;

      /* Weak, the display callback wants an active render-layer pointer. */
      re->result->renlay = render_get_active_layer(re, re->result);

      BLI_rw_mutex_unlock(&re->resultmutex);

      re->display_init(re->dih, re->result);
      re->display_update(re->duh, re->result, NULL);

      /* restore the disprect from border */
      re->disprect = orig_disprect;
      re->rectx = orig_rectx;
      re->recty = orig_recty;
    }
    else {
      /* set offset (again) for use in compositor, disprect was manipulated. */
      re->result->xof = 0;
      re->result->yof = 0;
    }
  }
}

/* Render scene into render result, with a render engine. */
static void do_render_engine(Render *re)
{
  Object *camera = RE_GetCamera(re);
  /* also check for camera here */
  if (camera == NULL) {
    BKE_report(re->reports, RPT_ERROR, "Cannot render, no camera");
    G.is_break = true;
    return;
  }

  /* now use renderdata and camera to set viewplane */
  RE_SetCamera(re, camera);

  re->current_scene_update(re->suh, re->scene);
  RE_engine_render(re, false);

  /* when border render, check if we have to insert it in black */
  render_result_uncrop(re);
}

/* Render scene into render result, within a compositor node tree.
 * Uses the same image dimensions, does not recursively perform compositing. */
static void do_render_compositor_scene(Render *re, Scene *sce, int cfra)
{
  Render *resc = RE_NewSceneRender(sce);
  int winx = re->winx, winy = re->winy;

  sce->r.cfra = cfra;

  BKE_scene_camera_switch_update(sce);

  /* exception: scene uses own size (unfinished code) */
  if (0) {
    BKE_render_resolution(&sce->r, false, &winx, &winy);
  }

  /* initial setup */
  RE_InitState(resc, re, &sce->r, &sce->view_layers, NULL, winx, winy, &re->disprect);

  /* We still want to use 'rendercache' setting from org (main) scene... */
  resc->r.scemode = (resc->r.scemode & ~R_EXR_CACHE_FILE) | (re->r.scemode & R_EXR_CACHE_FILE);

  /* still unsure entity this... */
  resc->main = re->main;
  resc->scene = sce;

  /* copy callbacks */
  resc->display_update = re->display_update;
  resc->duh = re->duh;
  resc->test_break = re->test_break;
  resc->tbh = re->tbh;
  resc->stats_draw = re->stats_draw;
  resc->sdh = re->sdh;
  resc->current_scene_update = re->current_scene_update;
  resc->suh = re->suh;

  do_render_engine(resc);
}

/* helper call to detect if this scene needs a render,
 * or if there's a any render layer to render. */
static int compositor_needs_render(Scene *sce, int this_scene)
{
  bNodeTree *ntree = sce->nodetree;
  bNode *node;

  if (ntree == NULL) {
    return 1;
  }
  if (sce->use_nodes == false) {
    return 1;
  }
  if ((sce->r.scemode & R_DOCOMP) == 0) {
    return 1;
  }

  for (node = ntree->nodes.first; node; node = node->next) {
    if (node->type == CMP_NODE_R_LAYERS && (node->flag & NODE_MUTED) == 0) {
      if (this_scene == 0 || node->id == NULL || node->id == &sce->id) {
        return 1;
      }
    }
  }
  return 0;
}

/* Render all scenes within a compositor node tree. */
static void do_render_compositor_scenes(Render *re)
{
  bNode *node;
  int cfra = re->scene->r.cfra;
  Scene *restore_scene = re->scene;

  if (re->scene->nodetree == NULL) {
    return;
  }

  bool changed_scene = false;

  /* now foreach render-result node we do a full render */
  /* results are stored in a way compositor will find it */
  GSet *scenes_rendered = BLI_gset_ptr_new(__func__);
  for (node = re->scene->nodetree->nodes.first; node; node = node->next) {
    if (node->type == CMP_NODE_R_LAYERS && (node->flag & NODE_MUTED) == 0) {
      if (node->id && node->id != (ID *)re->scene) {
        Scene *scene = (Scene *)node->id;
        if (!BLI_gset_haskey(scenes_rendered, scene) &&
            render_scene_has_layers_to_render(scene, false)) {
          do_render_compositor_scene(re, scene, cfra);
          BLI_gset_add(scenes_rendered, scene);
          node->typeinfo->updatefunc(restore_scene->nodetree, node);

          if (scene != re->scene) {
            changed_scene = true;
          }
        }
      }
    }
  }
  BLI_gset_free(scenes_rendered, NULL);

  if (changed_scene) {
    /* If rendered another scene, switch back to the current scene with compositing nodes. */
    re->current_scene_update(re->suh, re->scene);
  }
}

/* bad call... need to think over proper method still */
static void render_compositor_stats(void *arg, const char *str)
{
  Render *re = (Render *)arg;

  RenderStats i;
  memcpy(&i, &re->i, sizeof(i));
  i.infostr = str;
  re->stats_draw(re->sdh, &i);
}

/* Render compositor nodes, along with any scenes required for them.
 * The result will be output into a compositing render layer in the render result. */
static void do_render_compositor(Render *re)
{
  bNodeTree *ntree = re->pipeline_scene_eval->nodetree;
  int update_newframe = 0;

  if (compositor_needs_render(re->pipeline_scene_eval, 1)) {
    /* save memory... free all cached images */
    ntreeFreeCache(ntree);

    /* render the frames
     * it could be optimized to render only the needed view
     * but what if a scene has a different number of views
     * than the main scene? */
    do_render_engine(re);
  }
  else {
    re->i.cfra = re->r.cfra;

    /* ensure new result gets added, like for regular renders */
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);

    render_result_free(re->result);
    if ((re->r.mode & R_CROP) == 0) {
      render_result_disprect_to_full_resolution(re);
    }
    re->result = render_result_new(re, &re->disprect, RR_ALL_LAYERS, RR_ALL_VIEWS);

    BLI_rw_mutex_unlock(&re->resultmutex);

    /* scene render process already updates animsys */
    update_newframe = 1;
  }

  /* swap render result */
  if (re->r.scemode & R_SINGLE_LAYER) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);
    render_result_single_layer_end(re);
    BLI_rw_mutex_unlock(&re->resultmutex);
  }

  if (!re->test_break(re->tbh)) {

    if (ntree) {
      ntreeCompositTagRender(re->pipeline_scene_eval);
    }

    if (ntree && re->scene->use_nodes && re->r.scemode & R_DOCOMP) {
      /* checks if there are render-result nodes that need scene */
      if ((re->r.scemode & R_SINGLE_LAYER) == 0) {
        do_render_compositor_scenes(re);
      }

      if (!re->test_break(re->tbh)) {
        ntree->stats_draw = render_compositor_stats;
        ntree->test_break = re->test_break;
        ntree->progress = re->progress;
        ntree->sdh = re;
        ntree->tbh = re->tbh;
        ntree->prh = re->prh;

        if (update_newframe) {
          /* If we have consistent depsgraph now would be a time to update them. */
        }

        RenderView *rv;
        for (rv = re->result->views.first; rv; rv = rv->next) {
          ntreeCompositExecTree(
              re->pipeline_scene_eval, ntree, &re->r, true, G.background == 0, rv->name);
        }

        ntree->stats_draw = NULL;
        ntree->test_break = NULL;
        ntree->progress = NULL;
        ntree->tbh = ntree->sdh = ntree->prh = NULL;
      }
    }
  }

  /* Weak: the display callback wants an active render-layer pointer. */
  if (re->result != NULL) {
    re->result->renlay = render_get_active_layer(re, re->result);
    re->display_update(re->duh, re->result, NULL);
  }
}

static void renderresult_stampinfo(Render *re)
{
  RenderResult rres;
  RenderView *rv;
  int nr;

  /* this is the basic trick to get the displayed float or char rect from render result */
  nr = 0;
  for (rv = re->result->views.first; rv; rv = rv->next, nr++) {
    RE_SetActiveRenderView(re, rv->name);
    RE_AcquireResultImage(re, &rres, nr);

    Object *ob_camera_eval = DEG_get_evaluated_object(re->pipeline_depsgraph, RE_GetCamera(re));
    BKE_image_stamp_buf(re->scene,
                        ob_camera_eval,
                        (re->r.stamp & R_STAMP_STRIPMETA) ? rres.stamp_data : NULL,
                        (unsigned char *)rres.rect32,
                        rres.rectf,
                        rres.rectx,
                        rres.recty,
                        4);
    RE_ReleaseResultImage(re);
  }
}

int RE_seq_render_active(Scene *scene, RenderData *rd)
{
  Editing *ed;
  Sequence *seq;

  ed = scene->ed;

  if (!(rd->scemode & R_DOSEQ) || !ed || !ed->seqbase.first) {
    return 0;
  }

  for (seq = ed->seqbase.first; seq; seq = seq->next) {
    if (seq->type != SEQ_TYPE_SOUND_RAM) {
      return 1;
    }
  }

  return 0;
}

/* Render sequencer strips into render result. */
static void do_render_sequencer(Render *re)
{
  static int recurs_depth = 0;
  struct ImBuf *out;
  RenderResult *rr; /* don't assign re->result here as it might change during give_ibuf_seq */
  int cfra = re->r.cfra;
  SeqRenderData context;
  int view_id, tot_views;
  struct ImBuf **ibuf_arr;
  int re_x, re_y;

  re->i.cfra = cfra;

  recurs_depth++;

  if ((re->r.mode & R_BORDER) && (re->r.mode & R_CROP) == 0) {
    /* if border rendering is used and cropping is disabled, final buffer should
     * be as large as the whole frame */
    re_x = re->winx;
    re_y = re->winy;
  }
  else {
    re_x = re->result->rectx;
    re_y = re->result->recty;
  }

  tot_views = BKE_scene_multiview_num_views_get(&re->r);
  ibuf_arr = MEM_mallocN(sizeof(ImBuf *) * tot_views, "Sequencer Views ImBufs");

  SEQ_render_new_render_data(re->main,
                             re->pipeline_depsgraph,
                             re->scene,
                             re_x,
                             re_y,
                             SEQ_RENDER_SIZE_SCENE,
                             true,
                             &context);

  /* The render-result gets destroyed during the rendering, so we first collect all ibufs
   * and then we populate the final render-result. */

  for (view_id = 0; view_id < tot_views; view_id++) {
    context.view_id = view_id;
    out = SEQ_render_give_ibuf(&context, cfra, 0);

    if (out) {
      ibuf_arr[view_id] = IMB_dupImBuf(out);
      IMB_metadata_copy(ibuf_arr[view_id], out);
      IMB_freeImBuf(out);
      SEQ_render_imbuf_from_sequencer_space(re->pipeline_scene_eval, ibuf_arr[view_id]);
    }
    else {
      ibuf_arr[view_id] = NULL;
    }
  }

  rr = re->result;

  BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);
  render_result_views_new(rr, &re->r);
  BLI_rw_mutex_unlock(&re->resultmutex);

  for (view_id = 0; view_id < tot_views; view_id++) {
    RenderView *rv = RE_RenderViewGetById(rr, view_id);
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);

    if (ibuf_arr[view_id]) {
      /* copy ibuf into combined pixel rect */
      RE_render_result_rect_from_ibuf(rr, ibuf_arr[view_id], view_id);

      if (ibuf_arr[view_id]->metadata && (re->r.stamp & R_STAMP_STRIPMETA)) {
        /* ensure render stamp info first */
        BKE_render_result_stamp_info(NULL, NULL, rr, true);
        BKE_stamp_info_from_imbuf(rr, ibuf_arr[view_id]);
      }

      if (recurs_depth == 0) { /* With nested scenes, only free on top-level. */
        Editing *ed = re->pipeline_scene_eval->ed;
        if (ed) {
          SEQ_relations_free_imbuf(re->pipeline_scene_eval, &ed->seqbase, true);
        }
      }
      IMB_freeImBuf(ibuf_arr[view_id]);
    }
    else {
      /* render result is delivered empty in most cases, nevertheless we handle all cases */
      render_result_rect_fill_zero(rr, view_id);
    }

    BLI_rw_mutex_unlock(&re->resultmutex);

    /* would mark display buffers as invalid */
    RE_SetActiveRenderView(re, rv->name);
    re->display_update(re->duh, re->result, NULL);
  }

  MEM_freeN(ibuf_arr);

  recurs_depth--;

  /* just in case this flag went missing at some point */
  re->r.scemode |= R_DOSEQ;

  /* set overall progress of sequence rendering */
  if (re->r.efra != re->r.sfra) {
    re->progress(re->prh, (float)(cfra - re->r.sfra) / (re->r.efra - re->r.sfra));
  }
  else {
    re->progress(re->prh, 1.0f);
  }
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Render full pipeline, using render engine, sequencer and compositing nodes. */
static void do_render_full_pipeline(Render *re)
{
  bool render_seq = false;

  re->current_scene_update(re->suh, re->scene);

  BKE_scene_camera_switch_update(re->scene);

  re->i.starttime = PIL_check_seconds_timer();

  /* ensure no images are in memory from previous animated sequences */
  BKE_image_all_free_anim_ibufs(re->main, re->r.cfra);
  SEQ_cache_cleanup(re->scene);

  if (RE_engine_render(re, true)) {
    /* in this case external render overrides all */
  }
  else if (RE_seq_render_active(re->scene, &re->r)) {
    /* NOTE: do_render_sequencer() frees rect32 when sequencer returns float images. */
    if (!re->test_break(re->tbh)) {
      do_render_sequencer(re);
      render_seq = true;
    }

    re->stats_draw(re->sdh, &re->i);
    re->display_update(re->duh, re->result, NULL);
  }
  else {
    do_render_compositor(re);
  }

  re->i.lastframetime = PIL_check_seconds_timer() - re->i.starttime;

  re->stats_draw(re->sdh, &re->i);

  /* save render result stamp if needed */
  if (re->result != NULL) {
    /* sequence rendering should have taken care of that already */
    if (!(render_seq && (re->r.stamp & R_STAMP_STRIPMETA))) {
      Object *ob_camera_eval = DEG_get_evaluated_object(re->pipeline_depsgraph, RE_GetCamera(re));
      BKE_render_result_stamp_info(re->scene, ob_camera_eval, re->result, false);
    }

    /* stamp image info here */
    if ((re->r.stamp & R_STAMP_ALL) && (re->r.stamp & R_STAMP_DRAW)) {
      renderresult_stampinfo(re);
      re->display_update(re->duh, re->result, NULL);
    }
  }
}

static bool check_valid_compositing_camera(Scene *scene, Object *camera_override)
{
  if (scene->r.scemode & R_DOCOMP && scene->use_nodes) {
    bNode *node = scene->nodetree->nodes.first;

    while (node) {
      if (node->type == CMP_NODE_R_LAYERS && (node->flag & NODE_MUTED) == 0) {
        Scene *sce = node->id ? (Scene *)node->id : scene;
        if (sce->camera == NULL) {
          sce->camera = BKE_view_layer_camera_find(BKE_view_layer_default_render(sce));
        }
        if (sce->camera == NULL) {
          /* all render layers nodes need camera */
          return false;
        }
      }
      node = node->next;
    }

    return true;
  }

  return (camera_override != NULL || scene->camera != NULL);
}

static bool check_valid_camera_multiview(Scene *scene, Object *camera, ReportList *reports)
{
  SceneRenderView *srv;
  bool active_view = false;

  if (camera == NULL || (scene->r.scemode & R_MULTIVIEW) == 0) {
    return true;
  }

  for (srv = scene->r.views.first; srv; srv = srv->next) {
    if (BKE_scene_multiview_is_render_view_active(&scene->r, srv)) {
      active_view = true;

      if (scene->r.views_format == SCE_VIEWS_FORMAT_MULTIVIEW) {
        Object *view_camera;
        view_camera = BKE_camera_multiview_render(scene, camera, srv->name);

        if (view_camera == camera) {
          /* if the suffix is not in the camera, means we are using the fallback camera */
          if (!BLI_str_endswith(view_camera->id.name + 2, srv->suffix)) {
            BKE_reportf(reports,
                        RPT_ERROR,
                        "Camera \"%s\" is not a multi-view camera",
                        camera->id.name + 2);
            return false;
          }
        }
      }
    }
  }

  if (!active_view) {
    BKE_reportf(reports, RPT_ERROR, "No active view found in scene \"%s\"", scene->id.name + 2);
    return false;
  }

  return true;
}

static int check_valid_camera(Scene *scene, Object *camera_override, ReportList *reports)
{
  const char *err_msg = "No camera found in scene \"%s\"";

  if (camera_override == NULL && scene->camera == NULL) {
    scene->camera = BKE_view_layer_camera_find(BKE_view_layer_default_render(scene));
  }

  if (!check_valid_camera_multiview(scene, scene->camera, reports)) {
    return false;
  }

  if (RE_seq_render_active(scene, &scene->r)) {
    if (scene->ed) {
      Sequence *seq = scene->ed->seqbase.first;

      while (seq) {
        if ((seq->type == SEQ_TYPE_SCENE) && ((seq->flag & SEQ_SCENE_STRIPS) == 0) &&
            (seq->scene != NULL)) {
          if (!seq->scene_camera) {
            if (!seq->scene->camera &&
                !BKE_view_layer_camera_find(BKE_view_layer_default_render(seq->scene))) {
              /* camera could be unneeded due to composite nodes */
              Object *override = (seq->scene == scene) ? camera_override : NULL;

              if (!check_valid_compositing_camera(seq->scene, override)) {
                BKE_reportf(reports, RPT_ERROR, err_msg, seq->scene->id.name + 2);
                return false;
              }
            }
          }
          else if (!check_valid_camera_multiview(seq->scene, seq->scene_camera, reports)) {
            return false;
          }
        }

        seq = seq->next;
      }
    }
  }
  else if (!check_valid_compositing_camera(scene, camera_override)) {
    BKE_reportf(reports, RPT_ERROR, err_msg, scene->id.name + 2);
    return false;
  }

  return true;
}

static bool node_tree_has_compositor_output(bNodeTree *ntree)
{
  bNode *node;

  for (node = ntree->nodes.first; node; node = node->next) {
    if (ELEM(node->type, CMP_NODE_COMPOSITE, CMP_NODE_OUTPUT_FILE)) {
      return true;
    }
    if (ELEM(node->type, NODE_GROUP, NODE_CUSTOM_GROUP)) {
      if (node->id) {
        if (node_tree_has_compositor_output((bNodeTree *)node->id)) {
          return true;
        }
      }
    }
  }

  return false;
}

static int check_compositor_output(Scene *scene)
{
  return node_tree_has_compositor_output(scene->nodetree);
}

bool RE_is_rendering_allowed(Scene *scene,
                             ViewLayer *single_layer,
                             Object *camera_override,
                             ReportList *reports)
{
  const int scemode = scene->r.scemode;

  if (scene->r.mode & R_BORDER) {
    if (scene->r.border.xmax <= scene->r.border.xmin ||
        scene->r.border.ymax <= scene->r.border.ymin) {
      BKE_report(reports, RPT_ERROR, "No border area selected");
      return 0;
    }
  }

  if (RE_seq_render_active(scene, &scene->r)) {
    /* Sequencer */
    if (scene->r.mode & R_BORDER) {
      BKE_report(reports, RPT_ERROR, "Border rendering is not supported by sequencer");
      return false;
    }
  }
  else if ((scemode & R_DOCOMP) && scene->use_nodes) {
    /* Compositor */
    if (!scene->nodetree) {
      BKE_report(reports, RPT_ERROR, "No node tree in scene");
      return 0;
    }

    if (!check_compositor_output(scene)) {
      BKE_report(reports, RPT_ERROR, "No render output node in scene");
      return 0;
    }
  }
  else {
    /* Regular Render */
    if (!render_scene_has_layers_to_render(scene, single_layer)) {
      BKE_report(reports, RPT_ERROR, "All render layers are disabled");
      return 0;
    }
  }

  /* check valid camera, without camera render is OK (compo, seq) */
  if (!check_valid_camera(scene, camera_override, reports)) {
    return 0;
  }

  return 1;
}

static void update_physics_cache(Render *re,
                                 Scene *scene,
                                 ViewLayer *view_layer,
                                 int UNUSED(anim_init))
{
  PTCacheBaker baker;

  memset(&baker, 0, sizeof(baker));
  baker.bmain = re->main;
  baker.scene = scene;
  baker.view_layer = view_layer;
  baker.depsgraph = BKE_scene_ensure_depsgraph(re->main, scene, view_layer);
  baker.bake = 0;
  baker.render = 1;
  baker.anim_init = 1;
  baker.quick_step = 1;

  BKE_ptcache_bake(&baker);
}

void RE_SetActiveRenderView(Render *re, const char *viewname)
{
  BLI_strncpy(re->viewname, viewname, sizeof(re->viewname));
}

const char *RE_GetActiveRenderView(Render *re)
{
  return re->viewname;
}

/* evaluating scene options for general Blender render */
static int render_init_from_main(Render *re,
                                 const RenderData *rd,
                                 Main *bmain,
                                 Scene *scene,
                                 ViewLayer *single_layer,
                                 Object *camera_override,
                                 int anim,
                                 int anim_init)
{
  int winx, winy;
  rcti disprect;

  /* r.xsch and r.ysch has the actual view window size
   * r.border is the clipping rect */

  /* calculate actual render result and display size */
  BKE_render_resolution(rd, false, &winx, &winy);

  /* We always render smaller part, inserting it in larger image is compositor business,
   * it uses 'disprect' for it. */
  if (scene->r.mode & R_BORDER) {
    disprect.xmin = rd->border.xmin * winx;
    disprect.xmax = rd->border.xmax * winx;

    disprect.ymin = rd->border.ymin * winy;
    disprect.ymax = rd->border.ymax * winy;
  }
  else {
    disprect.xmin = disprect.ymin = 0;
    disprect.xmax = winx;
    disprect.ymax = winy;
  }

  re->main = bmain;
  re->scene = scene;
  re->camera_override = camera_override;
  re->viewname[0] = '\0';

  /* not too nice, but it survives anim-border render */
  if (anim) {
    render_update_anim_renderdata(re, &scene->r, &scene->view_layers);
    re->disprect = disprect;
    return 1;
  }

  /*
   * Disabled completely for now,
   * can be later set as render profile option
   * and default for background render.
   */
  if (0) {
    /* make sure dynamics are up to date */
    ViewLayer *view_layer = BKE_view_layer_context_active_PLACEHOLDER(scene);
    update_physics_cache(re, scene, view_layer, anim_init);
  }

  if (single_layer || scene->r.scemode & R_SINGLE_LAYER) {
    BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);
    render_result_single_layer_begin(re);
    BLI_rw_mutex_unlock(&re->resultmutex);
  }

  RE_InitState(re, NULL, &scene->r, &scene->view_layers, single_layer, winx, winy, &disprect);
  if (!re->ok) { /* if an error was printed, abort */
    return 0;
  }

  /* initstate makes new result, have to send changed tags around */
  ntreeCompositTagRender(re->scene);

  re->display_init(re->dih, re->result);
  re->display_clear(re->dch, re->result);

  return 1;
}

void RE_SetReports(Render *re, ReportList *reports)
{
  re->reports = reports;
}

static void render_update_depsgraph(Render *re)
{
  Scene *scene = re->scene;
  DEG_evaluate_on_framechange(re->pipeline_depsgraph, BKE_scene_frame_get(scene));
  BKE_scene_update_sound(re->pipeline_depsgraph, re->main);
}

static void render_init_depsgraph(Render *re)
{
  Scene *scene = re->scene;
  ViewLayer *view_layer = BKE_view_layer_default_render(re->scene);

  re->pipeline_depsgraph = DEG_graph_new(re->main, scene, view_layer, DAG_EVAL_RENDER);
  DEG_debug_name_set(re->pipeline_depsgraph, "RENDER PIPELINE");

  /* Make sure there is a correct evaluated scene pointer. */
  DEG_graph_build_for_render_pipeline(re->pipeline_depsgraph);

  /* Update immediately so we have proper evaluated scene. */
  render_update_depsgraph(re);

  re->pipeline_scene_eval = DEG_get_evaluated_scene(re->pipeline_depsgraph);
}

/* Free data only needed during rendering operation. */
static void render_pipeline_free(Render *re)
{
  if (re->engine && !RE_engine_use_persistent_data(re->engine)) {
    RE_engine_free(re->engine);
    re->engine = NULL;
  }
  if (re->pipeline_depsgraph != NULL) {
    DEG_graph_free(re->pipeline_depsgraph);
    re->pipeline_depsgraph = NULL;
    re->pipeline_scene_eval = NULL;
  }
  /* Destroy the opengl context in the correct thread. */
  RE_gl_context_destroy(re);

  /* In the case the engine did not mark tiles as finished (un-highlight, which could happen in the
   * case of cancelled render) ensure the storage is empty. */
  if (re->highlighted_tiles != NULL) {
    BLI_mutex_lock(&re->highlighted_tiles_mutex);

    /* Rendering is supposed to be finished here, so no new tiles are expected to be written.
     * Only make it so possible read-only access to the highlighted tiles is thread-safe. */
    BLI_assert(re->highlighted_tiles);

    BLI_gset_free(re->highlighted_tiles, MEM_freeN);
    re->highlighted_tiles = NULL;

    BLI_mutex_unlock(&re->highlighted_tiles_mutex);
  }
}

void RE_RenderFrame(Render *re,
                    Main *bmain,
                    Scene *scene,
                    ViewLayer *single_layer,
                    Object *camera_override,
                    const int frame,
                    const float subframe,
                    const bool write_still)
{
  render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_INIT);

  /* Ugly global still...
   * is to prevent preview events and signal subdivision-surface etc to make full resolution. */
  G.is_rendering = true;

  scene->r.cfra = frame;
  scene->r.subframe = subframe;

  if (render_init_from_main(re, &scene->r, bmain, scene, single_layer, camera_override, 0, 0)) {
    const RenderData rd = scene->r;
    MEM_reset_peak_memory();

    render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_PRE);

    render_init_depsgraph(re);

    do_render_full_pipeline(re);

    if (write_still && !G.is_break) {
      if (BKE_imtype_is_movie(rd.im_format.imtype)) {
        /* operator checks this but in case its called from elsewhere */
        printf("Error: can't write single images with a movie format!\n");
      }
      else {
        char name[FILE_MAX];
        BKE_image_path_from_imformat(name,
                                     rd.pic,
                                     BKE_main_blendfile_path(bmain),
                                     scene->r.cfra,
                                     &rd.im_format,
                                     (rd.scemode & R_EXTENSION) != 0,
                                     false,
                                     NULL);

        /* reports only used for Movie */
        do_write_image_or_movie(re, bmain, scene, NULL, 0, name);
      }
    }

    /* keep after file save */
    render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_POST);
    if (write_still) {
      render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_WRITE);
    }
  }

  render_callback_exec_id(re,
                          re->main,
                          &scene->id,
                          G.is_break ? BKE_CB_EVT_RENDER_CANCEL : BKE_CB_EVT_RENDER_COMPLETE);

  render_pipeline_free(re);

  /* UGLY WARNING */
  G.is_rendering = false;
}

#ifdef WITH_FREESTYLE

/* Not freestyle specific, currently only used by free-style. */
static void change_renderdata_engine(Render *re, const char *new_engine)
{
  if (!STREQ(re->r.engine, new_engine)) {
    if (re->engine) {
      RE_engine_free(re->engine);
      re->engine = NULL;
    }
    BLI_strncpy(re->r.engine, new_engine, sizeof(re->r.engine));
  }
}

static bool use_eevee_for_freestyle_render(Render *re)
{
  RenderEngineType *type = RE_engines_find(re->r.engine);
  return !(type->flag & RE_USE_CUSTOM_FREESTYLE);
}

void RE_RenderFreestyleStrokes(Render *re, Main *bmain, Scene *scene, int render)
{
  re->result_ok = 0;
  if (render_init_from_main(re, &scene->r, bmain, scene, NULL, NULL, 0, 0)) {
    if (render) {
      char scene_engine[32];
      BLI_strncpy(scene_engine, re->r.engine, sizeof(scene_engine));
      if (use_eevee_for_freestyle_render(re)) {
        change_renderdata_engine(re, RE_engine_id_BLENDER_EEVEE);
      }

      RE_engine_render(re, false);

      change_renderdata_engine(re, scene_engine);
    }
  }
  re->result_ok = 1;
}

void RE_RenderFreestyleExternal(Render *re)
{
  if (re->test_break(re->tbh)) {
    return;
  }

  FRS_init_stroke_renderer(re);

  LISTBASE_FOREACH (RenderView *, rv, &re->result->views) {
    RE_SetActiveRenderView(re, rv->name);

    ViewLayer *active_view_layer = BLI_findlink(&re->view_layers, re->active_view_layer);
    FRS_begin_stroke_rendering(re);

    LISTBASE_FOREACH (ViewLayer *, view_layer, &re->view_layers) {
      if ((re->r.scemode & R_SINGLE_LAYER) && view_layer != active_view_layer) {
        continue;
      }

      if (FRS_is_freestyle_enabled(view_layer)) {
        FRS_do_stroke_rendering(re, view_layer);
      }
    }

    FRS_end_stroke_rendering(re);
  }
}
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read/Write Render Result (Images & Movies)
 * \{ */

bool RE_WriteRenderViewsMovie(ReportList *reports,
                              RenderResult *rr,
                              Scene *scene,
                              RenderData *rd,
                              bMovieHandle *mh,
                              void **movie_ctx_arr,
                              const int totvideos,
                              bool preview)
{
  bool ok = true;

  if (!rr) {
    return false;
  }

  ImageFormatData image_format;
  BKE_image_format_init_for_write(&image_format, scene, NULL);

  const bool is_mono = BLI_listbase_count_at_most(&rr->views, 2) < 2;
  const float dither = scene->r.dither_intensity;

  if (is_mono || (image_format.views_format == R_IMF_VIEWS_INDIVIDUAL)) {
    int view_id;
    for (view_id = 0; view_id < totvideos; view_id++) {
      const char *suffix = BKE_scene_multiview_view_id_suffix_get(&scene->r, view_id);
      ImBuf *ibuf = RE_render_result_rect_to_ibuf(rr, &rd->im_format, dither, view_id);

      IMB_colormanagement_imbuf_for_write(ibuf, true, false, &image_format);

      ok &= mh->append_movie(movie_ctx_arr[view_id],
                             rd,
                             preview ? scene->r.psfra : scene->r.sfra,
                             scene->r.cfra,
                             (int *)ibuf->rect,
                             ibuf->x,
                             ibuf->y,
                             suffix,
                             reports);

      /* imbuf knows which rects are not part of ibuf */
      IMB_freeImBuf(ibuf);
    }
    printf("Append frame %d\n", scene->r.cfra);
  }
  else { /* R_IMF_VIEWS_STEREO_3D */
    const char *names[2] = {STEREO_LEFT_NAME, STEREO_RIGHT_NAME};
    ImBuf *ibuf_arr[3] = {NULL};
    int i;

    BLI_assert((totvideos == 1) && (image_format.views_format == R_IMF_VIEWS_STEREO_3D));

    for (i = 0; i < 2; i++) {
      int view_id = BLI_findstringindex(&rr->views, names[i], offsetof(RenderView, name));
      ibuf_arr[i] = RE_render_result_rect_to_ibuf(rr, &rd->im_format, dither, view_id);

      IMB_colormanagement_imbuf_for_write(ibuf_arr[i], true, false, &image_format);
    }

    ibuf_arr[2] = IMB_stereo3d_ImBuf(&image_format, ibuf_arr[0], ibuf_arr[1]);

    ok = mh->append_movie(movie_ctx_arr[0],
                          rd,
                          preview ? scene->r.psfra : scene->r.sfra,
                          scene->r.cfra,
                          (int *)ibuf_arr[2]->rect,
                          ibuf_arr[2]->x,
                          ibuf_arr[2]->y,
                          "",
                          reports);

    for (i = 0; i < 3; i++) {
      /* imbuf knows which rects are not part of ibuf */
      IMB_freeImBuf(ibuf_arr[i]);
    }
  }

  BKE_image_format_free(&image_format);

  return ok;
}

static bool do_write_image_or_movie(Render *re,
                                    Main *bmain,
                                    Scene *scene,
                                    bMovieHandle *mh,
                                    const int totvideos,
                                    const char *name_override)
{
  char name[FILE_MAX];
  RenderResult rres;
  double render_time;
  bool ok = true;
  RenderEngineType *re_type = RE_engines_find(re->r.engine);

  /* Only disable file writing if postprocessing is also disabled. */
  const bool do_write_file = !(re_type->flag & RE_USE_NO_IMAGE_SAVE) ||
                             (re_type->flag & RE_USE_POSTPROCESS);

  if (do_write_file) {
    RE_AcquireResultImageViews(re, &rres);

    /* write movie or image */
    if (BKE_imtype_is_movie(scene->r.im_format.imtype)) {
      RE_WriteRenderViewsMovie(
          re->reports, &rres, scene, &re->r, mh, re->movie_ctx_arr, totvideos, false);
    }
    else {
      if (name_override) {
        BLI_strncpy(name, name_override, sizeof(name));
      }
      else {
        BKE_image_path_from_imformat(name,
                                     scene->r.pic,
                                     BKE_main_blendfile_path(bmain),
                                     scene->r.cfra,
                                     &scene->r.im_format,
                                     (scene->r.scemode & R_EXTENSION) != 0,
                                     true,
                                     NULL);
      }

      /* write images as individual images or stereo */
      ok = BKE_image_render_write(re->reports, &rres, scene, true, name);
    }

    RE_ReleaseResultImageViews(re, &rres);
  }

  render_time = re->i.lastframetime;
  re->i.lastframetime = PIL_check_seconds_timer() - re->i.starttime;

  BLI_timecode_string_from_time_simple(name, sizeof(name), re->i.lastframetime);
  printf(" Time: %s", name);

  /* Flush stdout to be sure python callbacks are printing stuff after blender. */
  fflush(stdout);

  /* NOTE: using G_MAIN seems valid here???
   * Not sure it's actually even used anyway, we could as well pass NULL? */
  render_callback_exec_null(re, G_MAIN, BKE_CB_EVT_RENDER_STATS);

  if (do_write_file) {
    BLI_timecode_string_from_time_simple(name, sizeof(name), re->i.lastframetime - render_time);
    printf(" (Saving: %s)\n", name);
  }

  fputc('\n', stdout);
  fflush(stdout);

  return ok;
}

static void get_videos_dimensions(const Render *re,
                                  const RenderData *rd,
                                  size_t *r_width,
                                  size_t *r_height)
{
  size_t width, height;
  if (re->r.mode & R_BORDER) {
    if ((re->r.mode & R_CROP) == 0) {
      width = re->winx;
      height = re->winy;
    }
    else {
      width = re->rectx;
      height = re->recty;
    }
  }
  else {
    width = re->rectx;
    height = re->recty;
  }

  BKE_scene_multiview_videos_dimensions_get(rd, width, height, r_width, r_height);
}

static void re_movie_free_all(Render *re, bMovieHandle *mh, int totvideos)
{
  int i;

  for (i = 0; i < totvideos; i++) {
    mh->end_movie(re->movie_ctx_arr[i]);
    mh->context_free(re->movie_ctx_arr[i]);
  }

  MEM_SAFE_FREE(re->movie_ctx_arr);
}

void RE_RenderAnim(Render *re,
                   Main *bmain,
                   Scene *scene,
                   ViewLayer *single_layer,
                   Object *camera_override,
                   int sfra,
                   int efra,
                   int tfra)
{
  /* Call hooks before taking a copy of scene->r, so user can alter the render settings prior to
   * copying (e.g. alter the output path). */
  render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_INIT);

  const RenderData rd = scene->r;
  bMovieHandle *mh = NULL;
  const int cfra_old = rd.cfra;
  const float subframe_old = rd.subframe;
  int nfra, totrendered = 0, totskipped = 0;
  const int totvideos = BKE_scene_multiview_num_videos_get(&rd);
  const bool is_movie = BKE_imtype_is_movie(rd.im_format.imtype);
  const bool is_multiview_name = ((rd.scemode & R_MULTIVIEW) != 0 &&
                                  (rd.im_format.views_format == R_IMF_VIEWS_INDIVIDUAL));

  /* do not fully call for each frame, it initializes & pops output window */
  if (!render_init_from_main(re, &rd, bmain, scene, single_layer, camera_override, 0, 1)) {
    return;
  }

  RenderEngineType *re_type = RE_engines_find(re->r.engine);

  /* Only disable file writing if postprocessing is also disabled. */
  const bool do_write_file = !(re_type->flag & RE_USE_NO_IMAGE_SAVE) ||
                             (re_type->flag & RE_USE_POSTPROCESS);

  render_init_depsgraph(re);

  if (is_movie && do_write_file) {
    size_t width, height;
    int i;
    bool is_error = false;

    get_videos_dimensions(re, &rd, &width, &height);

    mh = BKE_movie_handle_get(rd.im_format.imtype);
    if (mh == NULL) {
      BKE_report(re->reports, RPT_ERROR, "Movie format unsupported");
      return;
    }

    re->movie_ctx_arr = MEM_mallocN(sizeof(void *) * totvideos, "Movies' Context");

    for (i = 0; i < totvideos; i++) {
      const char *suffix = BKE_scene_multiview_view_id_suffix_get(&re->r, i);

      re->movie_ctx_arr[i] = mh->context_create();

      if (!mh->start_movie(re->movie_ctx_arr[i],
                           re->pipeline_scene_eval,
                           &re->r,
                           width,
                           height,
                           re->reports,
                           false,
                           suffix)) {
        is_error = true;
        break;
      }
    }

    if (is_error) {
      /* report is handled above */
      re_movie_free_all(re, mh, i + 1);
      render_pipeline_free(re);
      return;
    }
  }

  /* Ugly global still... is to prevent renderwin events and signal subdivision-surface etc
   * to make full resolution is also set by caller renderwin.c */
  G.is_rendering = true;

  re->flag |= R_ANIMATION;

  {
    scene->r.subframe = 0.0f;
    for (nfra = sfra, scene->r.cfra = sfra; scene->r.cfra <= efra; scene->r.cfra++) {
      char name[FILE_MAX];

      /* A feedback loop exists here -- render initialization requires updated
       * render layers settings which could be animated, but scene evaluation for
       * the frame happens later because it depends on what layers are visible to
       * render engine.
       *
       * The idea here is to only evaluate animation data associated with the scene,
       * which will make sure render layer settings are up-to-date, initialize the
       * render database itself and then perform full scene update with only needed
       * layers.
       *                                                              -sergey-
       */
      {
        float ctime = BKE_scene_ctime_get(scene);
        AnimData *adt = BKE_animdata_from_id(&scene->id);
        const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
            re->pipeline_depsgraph, ctime);
        BKE_animsys_evaluate_animdata(&scene->id, adt, &anim_eval_context, ADT_RECALC_ALL, false);
      }

      render_update_depsgraph(re);

      /* Only border now, TODO(ton): camera lens. */
      render_init_from_main(re, &rd, bmain, scene, single_layer, camera_override, 1, 0);

      if (nfra != scene->r.cfra) {
        /* Skip this frame, but could update for physics and particles system. */
        continue;
      }

      nfra += tfra;

      /* Touch/NoOverwrite options are only valid for image's */
      if (is_movie == false && do_write_file) {
        if (rd.mode & (R_NO_OVERWRITE | R_TOUCH)) {
          BKE_image_path_from_imformat(name,
                                       rd.pic,
                                       BKE_main_blendfile_path(bmain),
                                       scene->r.cfra,
                                       &rd.im_format,
                                       (rd.scemode & R_EXTENSION) != 0,
                                       true,
                                       NULL);
        }

        if (rd.mode & R_NO_OVERWRITE) {
          if (!is_multiview_name) {
            if (BLI_exists(name)) {
              printf("skipping existing frame \"%s\"\n", name);
              totskipped++;
              continue;
            }
          }
          else {
            SceneRenderView *srv;
            bool is_skip = false;
            char filepath[FILE_MAX];

            for (srv = scene->r.views.first; srv; srv = srv->next) {
              if (!BKE_scene_multiview_is_render_view_active(&scene->r, srv)) {
                continue;
              }

              BKE_scene_multiview_filepath_get(srv, name, filepath);

              if (BLI_exists(filepath)) {
                is_skip = true;
                printf("skipping existing frame \"%s\" for view \"%s\"\n", filepath, srv->name);
              }
            }

            if (is_skip) {
              totskipped++;
              continue;
            }
          }
        }

        if (rd.mode & R_TOUCH) {
          if (!is_multiview_name) {
            if (!BLI_exists(name)) {
              BLI_make_existing_file(name); /* makes the dir if its not there */
              BLI_file_touch(name);
            }
          }
          else {
            SceneRenderView *srv;
            char filepath[FILE_MAX];

            for (srv = scene->r.views.first; srv; srv = srv->next) {
              if (!BKE_scene_multiview_is_render_view_active(&scene->r, srv)) {
                continue;
              }

              BKE_scene_multiview_filepath_get(srv, name, filepath);

              if (!BLI_exists(filepath)) {
                BLI_make_existing_file(filepath); /* makes the dir if its not there */
                BLI_file_touch(filepath);
              }
            }
          }
        }
      }

      re->r.cfra = scene->r.cfra; /* weak.... */
      re->r.subframe = scene->r.subframe;

      /* run callbacks before rendering, before the scene is updated */
      render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_PRE);

      do_render_full_pipeline(re);
      totrendered++;

      if (re->test_break(re->tbh) == 0) {
        if (!G.is_break) {
          if (!do_write_image_or_movie(re, bmain, scene, mh, totvideos, NULL)) {
            G.is_break = true;
          }
        }
      }
      else {
        G.is_break = true;
      }

      if (G.is_break == true) {
        /* remove touched file */
        if (is_movie == false && do_write_file) {
          if (rd.mode & R_TOUCH) {
            if (!is_multiview_name) {
              if ((BLI_file_size(name) == 0)) {
                /* BLI_exists(name) is implicit */
                BLI_delete(name, false, false);
              }
            }
            else {
              SceneRenderView *srv;
              char filepath[FILE_MAX];

              for (srv = scene->r.views.first; srv; srv = srv->next) {
                if (!BKE_scene_multiview_is_render_view_active(&scene->r, srv)) {
                  continue;
                }

                BKE_scene_multiview_filepath_get(srv, name, filepath);

                if ((BLI_file_size(filepath) == 0)) {
                  /* BLI_exists(filepath) is implicit */
                  BLI_delete(filepath, false, false);
                }
              }
            }
          }
        }

        break;
      }

      if (G.is_break == false) {
        /* keep after file save */
        render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_POST);
        render_callback_exec_id(re, re->main, &scene->id, BKE_CB_EVT_RENDER_WRITE);
      }
    }
  }

  /* end movie */
  if (is_movie && do_write_file) {
    re_movie_free_all(re, mh, totvideos);
  }

  if (totskipped && totrendered == 0) {
    BKE_report(re->reports, RPT_INFO, "No frames rendered, skipped to not overwrite");
  }

  scene->r.cfra = cfra_old;
  scene->r.subframe = subframe_old;

  re->flag &= ~R_ANIMATION;

  render_callback_exec_id(re,
                          re->main,
                          &scene->id,
                          G.is_break ? BKE_CB_EVT_RENDER_CANCEL : BKE_CB_EVT_RENDER_COMPLETE);
  BKE_sound_reset_scene_specs(re->pipeline_scene_eval);

  render_pipeline_free(re);

  /* UGLY WARNING */
  G.is_rendering = false;
}

void RE_PreviewRender(Render *re, Main *bmain, Scene *sce)
{
  Object *camera;
  int winx, winy;

  BKE_render_resolution(&sce->r, false, &winx, &winy);

  RE_InitState(re, NULL, &sce->r, &sce->view_layers, NULL, winx, winy, NULL);

  re->main = bmain;
  re->scene = sce;

  camera = RE_GetCamera(re);
  RE_SetCamera(re, camera);

  RE_engine_render(re, false);

  /* No persistent data for preview render. */
  if (re->engine) {
    RE_engine_free(re->engine);
    re->engine = NULL;
  }
}

/* NOTE: repeated win/disprect calc... solve that nicer, also in compo. */

bool RE_ReadRenderResult(Scene *scene, Scene *scenode)
{
  Render *re;
  int winx, winy;
  bool success;
  rcti disprect;

  /* calculate actual render result and display size */
  BKE_render_resolution(&scene->r, false, &winx, &winy);

  /* only in movie case we render smaller part */
  if (scene->r.mode & R_BORDER) {
    disprect.xmin = scene->r.border.xmin * winx;
    disprect.xmax = scene->r.border.xmax * winx;

    disprect.ymin = scene->r.border.ymin * winy;
    disprect.ymax = scene->r.border.ymax * winy;
  }
  else {
    disprect.xmin = disprect.ymin = 0;
    disprect.xmax = winx;
    disprect.ymax = winy;
  }

  if (scenode) {
    scene = scenode;
  }

  /* get render: it can be called from UI with draw callbacks */
  re = RE_GetSceneRender(scene);
  if (re == NULL) {
    re = RE_NewSceneRender(scene);
  }
  RE_InitState(re, NULL, &scene->r, &scene->view_layers, NULL, winx, winy, &disprect);
  re->scene = scene;

  BLI_rw_mutex_lock(&re->resultmutex, THREAD_LOCK_WRITE);
  success = render_result_exr_file_cache_read(re);
  BLI_rw_mutex_unlock(&re->resultmutex);

  render_result_uncrop(re);

  return success;
}

void RE_layer_load_from_file(
    RenderLayer *layer, ReportList *reports, const char *filepath, int x, int y)
{
  /* OCIO_TODO: assume layer was saved in default color space */
  ImBuf *ibuf = IMB_loadiffname(filepath, IB_rect, NULL);
  RenderPass *rpass = NULL;

  /* multiview: since the API takes no 'view', we use the first combined pass found */
  for (rpass = layer->passes.first; rpass; rpass = rpass->next) {
    if (STREQ(rpass->name, RE_PASSNAME_COMBINED)) {
      break;
    }
  }

  if (rpass == NULL) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s: no Combined pass found in the render layer '%s'",
                __func__,
                filepath);
  }

  if (ibuf && (ibuf->rect || ibuf->rect_float)) {
    if (ibuf->x == layer->rectx && ibuf->y == layer->recty) {
      if (ibuf->rect_float == NULL) {
        IMB_float_from_rect(ibuf);
      }

      memcpy(rpass->rect, ibuf->rect_float, sizeof(float[4]) * layer->rectx * layer->recty);
    }
    else {
      if ((ibuf->x - x >= layer->rectx) && (ibuf->y - y >= layer->recty)) {
        ImBuf *ibuf_clip;

        if (ibuf->rect_float == NULL) {
          IMB_float_from_rect(ibuf);
        }

        ibuf_clip = IMB_allocImBuf(layer->rectx, layer->recty, 32, IB_rectfloat);
        if (ibuf_clip) {
          IMB_rectcpy(ibuf_clip, ibuf, 0, 0, x, y, layer->rectx, layer->recty);

          memcpy(
              rpass->rect, ibuf_clip->rect_float, sizeof(float[4]) * layer->rectx * layer->recty);
          IMB_freeImBuf(ibuf_clip);
        }
        else {
          BKE_reportf(
              reports, RPT_ERROR, "%s: failed to allocate clip buffer '%s'", __func__, filepath);
        }
      }
      else {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "%s: incorrect dimensions for partial copy '%s'",
                    __func__,
                    filepath);
      }
    }

    IMB_freeImBuf(ibuf);
  }
  else {
    BKE_reportf(reports, RPT_ERROR, "%s: failed to load '%s'", __func__, filepath);
  }
}

void RE_result_load_from_file(RenderResult *result, ReportList *reports, const char *filepath)
{
  if (!render_result_exr_file_read_path(result, NULL, filepath)) {
    BKE_reportf(reports, RPT_ERROR, "%s: failed to load '%s'", __func__, filepath);
    return;
  }
}

bool RE_layers_have_name(struct RenderResult *rr)
{
  switch (BLI_listbase_count_at_most(&rr->layers, 2)) {
    case 0:
      return false;
    case 1:
      return (((RenderLayer *)rr->layers.first)->name[0] != '\0');
    default:
      return true;
  }
  return false;
}

bool RE_passes_have_name(struct RenderLayer *rl)
{
  LISTBASE_FOREACH (RenderPass *, rp, &rl->passes) {
    if (!STREQ(rp->name, "Combined")) {
      return true;
    }
  }

  return false;
}

RenderPass *RE_pass_find_by_name(RenderLayer *rl, const char *name, const char *viewname)
{
  RenderPass *rp = NULL;

  for (rp = rl->passes.last; rp; rp = rp->prev) {
    if (STREQ(rp->name, name)) {
      if (viewname == NULL || viewname[0] == '\0') {
        break;
      }
      if (STREQ(rp->view, viewname)) {
        break;
      }
    }
  }
  return rp;
}

RenderPass *RE_pass_find_by_type(RenderLayer *rl, int passtype, const char *viewname)
{
#define CHECK_PASS(NAME) \
  if (passtype == SCE_PASS_##NAME) { \
    return RE_pass_find_by_name(rl, RE_PASSNAME_##NAME, viewname); \
  } \
  ((void)0)

  CHECK_PASS(COMBINED);
  CHECK_PASS(Z);
  CHECK_PASS(VECTOR);
  CHECK_PASS(NORMAL);
  CHECK_PASS(UV);
  CHECK_PASS(EMIT);
  CHECK_PASS(SHADOW);
  CHECK_PASS(AO);
  CHECK_PASS(ENVIRONMENT);
  CHECK_PASS(INDEXOB);
  CHECK_PASS(INDEXMA);
  CHECK_PASS(MIST);
  CHECK_PASS(DIFFUSE_DIRECT);
  CHECK_PASS(DIFFUSE_INDIRECT);
  CHECK_PASS(DIFFUSE_COLOR);
  CHECK_PASS(GLOSSY_DIRECT);
  CHECK_PASS(GLOSSY_INDIRECT);
  CHECK_PASS(GLOSSY_COLOR);
  CHECK_PASS(TRANSM_DIRECT);
  CHECK_PASS(TRANSM_INDIRECT);
  CHECK_PASS(TRANSM_COLOR);
  CHECK_PASS(SUBSURFACE_DIRECT);
  CHECK_PASS(SUBSURFACE_INDIRECT);
  CHECK_PASS(SUBSURFACE_COLOR);

#undef CHECK_PASS

  return NULL;
}

RenderPass *RE_create_gp_pass(RenderResult *rr, const char *layername, const char *viewname)
{
  RenderLayer *rl = BLI_findstring(&rr->layers, layername, offsetof(RenderLayer, name));
  /* only create render layer if not exist */
  if (!rl) {
    rl = MEM_callocN(sizeof(RenderLayer), layername);
    BLI_addtail(&rr->layers, rl);
    BLI_strncpy(rl->name, layername, sizeof(rl->name));
    rl->layflag = SCE_LAY_SOLID;
    rl->passflag = SCE_PASS_COMBINED;
    rl->rectx = rr->rectx;
    rl->recty = rr->recty;
  }

  /* Clear previous pass if exist or the new image will be over previous one. */
  RenderPass *rp = RE_pass_find_by_name(rl, RE_PASSNAME_COMBINED, viewname);
  if (rp) {
    if (rp->rect) {
      MEM_freeN(rp->rect);
    }
    BLI_freelinkN(&rl->passes, rp);
  }
  /* create a totally new pass */
  return render_layer_add_pass(rr, rl, 4, RE_PASSNAME_COMBINED, viewname, "RGBA", true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Miscellaneous Public Render API
 * \{ */

bool RE_allow_render_generic_object(Object *ob)
{
  /* override not showing object when duplis are used with particles */
  if (ob->transflag & OB_DUPLIPARTS) {
    /* pass */ /* let particle system(s) handle showing vs. not showing */
  }
  else if (ob->transflag & OB_DUPLI) {
    return false;
  }
  return true;
}

void RE_init_threadcount(Render *re)
{
  re->r.threads = BKE_render_num_threads(&re->r);
}

/** \} */
