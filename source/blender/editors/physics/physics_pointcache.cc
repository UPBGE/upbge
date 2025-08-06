/* SPDX-FileCopyrightText: 2007 by Janne Karhu. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edphys
 */

#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_duplilist.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_library.hh"
#include "BKE_pointcache.h"

#include "DEG_depsgraph.hh"

#include "ED_particle.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "physics_intern.hh"

static bool ptcache_bake_all_poll(bContext *C)
{
  return CTX_data_scene(C) != nullptr;
}

static bool ptcache_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);

  ID *id = ptr.owner_id;
  PointCache *point_cache = static_cast<PointCache *>(ptr.data);

  if (id == nullptr || point_cache == nullptr) {
    return false;
  }

  if (ID_IS_OVERRIDE_LIBRARY_REAL(id) && (point_cache->flag & PTCACHE_DISK_CACHE) == false) {
    CTX_wm_operator_poll_msg_set(C,
                                 "Library override data-blocks only support Disk Cache storage");
    return false;
  }

  if (!ID_IS_EDITABLE(id) && (point_cache->flag & PTCACHE_DISK_CACHE) == false) {
    CTX_wm_operator_poll_msg_set(C, "Linked data-blocks do not allow editing caches");
    return false;
  }

  return true;
}

static bool ptcache_add_remove_poll(bContext *C)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);

  ID *id = ptr.owner_id;
  PointCache *point_cache = static_cast<PointCache *>(ptr.data);

  if (id == nullptr || point_cache == nullptr) {
    return false;
  }

  if (ID_IS_OVERRIDE_LIBRARY_REAL(id) || !ID_IS_EDITABLE(id)) {
    CTX_wm_operator_poll_msg_set(
        C, "Linked or library override data-blocks do not allow adding or removing caches");
    return false;
  }

  return true;
}

struct PointCacheJob {
  wmWindowManager *wm;
  void *owner;
  bool *stop, *do_update;
  float *progress;

  PTCacheBaker *baker;
};

static void ptcache_job_free(void *customdata)
{
  PointCacheJob *job = static_cast<PointCacheJob *>(customdata);
  MEM_freeN(job->baker);
  MEM_freeN(job);
}

static int ptcache_job_break(void *customdata)
{
  PointCacheJob *job = static_cast<PointCacheJob *>(customdata);

  if (G.is_break) {
    return 1;
  }

  if (job->stop && *(job->stop)) {
    return 1;
  }

  return 0;
}

static void ptcache_job_update(void *customdata, float progress, int *cancel)
{
  PointCacheJob *job = static_cast<PointCacheJob *>(customdata);

  if (ptcache_job_break(job)) {
    *cancel = 1;
  }

  *(job->do_update) = true;
  *(job->progress) = progress;
}

static void ptcache_job_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  PointCacheJob *job = static_cast<PointCacheJob *>(customdata);

  job->stop = &worker_status->stop;
  job->do_update = &worker_status->do_update;
  job->progress = &worker_status->progress;

  G.is_break = false;

  /* XXX annoying hack: needed to prevent data corruption when changing
   * scene frame in separate threads
   */
  WM_locked_interface_set(job->wm, true);

  BKE_ptcache_bake(job->baker);

  worker_status->do_update = true;
  worker_status->stop = false;
}

static void ptcache_job_endjob(void *customdata)
{
  PointCacheJob *job = static_cast<PointCacheJob *>(customdata);
  Scene *scene = job->baker->scene;

  WM_locked_interface_set(job->wm, false);

  WM_main_add_notifier(NC_SCENE | ND_FRAME, scene);
  WM_main_add_notifier(NC_OBJECT | ND_POINTCACHE, job->baker->pid.owner_id);
}

static void ptcache_free_bake(PointCache *cache)
{
  if (cache->edit) {
    if (!cache->edit->edited || true) {  // XXX okee("Lose changes done in particle mode?")) {
      PE_free_ptcache_edit(cache->edit);
      cache->edit = nullptr;
      cache->flag &= ~PTCACHE_BAKED;
    }
  }
  else {
    cache->flag &= ~PTCACHE_BAKED;
  }
}

static PTCacheBaker *ptcache_baker_create(bContext *C, wmOperator *op, bool all)
{
  PTCacheBaker *baker = MEM_callocN<PTCacheBaker>("PTCacheBaker");

  baker->bmain = CTX_data_main(C);
  baker->scene = CTX_data_scene(C);
  baker->view_layer = CTX_data_view_layer(C);
  /* Depsgraph is used to sweep the frame range and evaluate scene at different times. */
  baker->depsgraph = CTX_data_depsgraph_pointer(C);
  baker->bake = RNA_boolean_get(op->ptr, "bake");
  baker->render = false;
  baker->anim_init = false;
  baker->quick_step = 1;

  if (!all) {
    PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);
    ID *id = ptr.owner_id;
    Object *ob = (GS(id->name) == ID_OB) ? (Object *)id : nullptr;
    PointCache *cache = static_cast<PointCache *>(ptr.data);
    baker->pid = BKE_ptcache_id_find(ob, baker->scene, cache);
  }

  return baker;
}

static wmOperatorStatus ptcache_bake_exec(bContext *C, wmOperator *op)
{
  bool all = STREQ(op->type->idname, "PTCACHE_OT_bake_all");

  PTCacheBaker *baker = ptcache_baker_create(C, op, all);
  BKE_ptcache_bake(baker);
  MEM_freeN(baker);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus ptcache_bake_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  bool all = STREQ(op->type->idname, "PTCACHE_OT_bake_all");

  PointCacheJob *job = MEM_mallocN<PointCacheJob>("PointCacheJob");
  job->wm = CTX_wm_manager(C);
  job->baker = ptcache_baker_create(C, op, all);
  job->baker->bake_job = job;
  job->baker->update_progress = ptcache_job_update;

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              CTX_data_scene(C),
                              "Baking point cache...",
                              WM_JOB_PROGRESS,
                              WM_JOB_TYPE_POINTCACHE);

  WM_jobs_customdata_set(wm_job, job, ptcache_job_free);
  WM_jobs_timer(wm_job, 0.1, NC_OBJECT | ND_POINTCACHE, NC_OBJECT | ND_POINTCACHE);
  WM_jobs_callbacks(wm_job, ptcache_job_startjob, nullptr, nullptr, ptcache_job_endjob);

  WM_locked_interface_set(CTX_wm_manager(C), true);

  WM_jobs_start(CTX_wm_manager(C), wm_job);

  WM_event_add_modal_handler(C, op);

  /* we must run modal until the bake job is done, otherwise the undo push
   * happens before the job ends, which can lead to race conditions between
   * the baking and file writing code */
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus ptcache_bake_modal(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  Scene *scene = (Scene *)op->customdata;

  /* no running blender, remove handler and pass through */
  if (0 == WM_jobs_test(CTX_wm_manager(C), scene, WM_JOB_TYPE_POINTCACHE)) {
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_PASS_THROUGH;
}

static void ptcache_bake_cancel(bContext *C, wmOperator *op)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  Scene *scene = (Scene *)op->customdata;

  /* kill on cancel, because job is using op->reports */
  WM_jobs_kill_type(wm, scene, WM_JOB_TYPE_POINTCACHE);
}

static wmOperatorStatus ptcache_free_bake_all_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  ListBase pidlist;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    BKE_ptcache_ids_from_object(&pidlist, ob, scene, MAX_DUPLI_RECUR);

    LISTBASE_FOREACH (PTCacheID *, pid, &pidlist) {
      ptcache_free_bake(pid->cache);
    }

    BLI_freelistN(&pidlist);

    WM_event_add_notifier(C, NC_OBJECT | ND_POINTCACHE, ob);
  }
  FOREACH_SCENE_OBJECT_END;

  WM_event_add_notifier(C, NC_SCENE | ND_FRAME, scene);

  return OPERATOR_FINISHED;
}

void PTCACHE_OT_bake_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake All Physics";
  ot->description = "Bake all physics";
  ot->idname = "PTCACHE_OT_bake_all";

  /* API callbacks. */
  ot->exec = ptcache_bake_exec;
  ot->invoke = ptcache_bake_invoke;
  ot->modal = ptcache_bake_modal;
  ot->cancel = ptcache_bake_cancel;
  ot->poll = ptcache_bake_all_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "bake", true, "Bake", "");
}
void PTCACHE_OT_free_bake_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete All Physics Bakes";
  ot->idname = "PTCACHE_OT_free_bake_all";
  ot->description = "Delete all baked caches of all objects in the current scene";

  /* API callbacks. */
  ot->exec = ptcache_free_bake_all_exec;
  ot->poll = ptcache_bake_all_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus ptcache_free_bake_exec(bContext *C, wmOperator * /*op*/)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);
  PointCache *cache = static_cast<PointCache *>(ptr.data);
  Object *ob = (Object *)ptr.owner_id;

  ptcache_free_bake(cache);

  WM_event_add_notifier(C, NC_OBJECT | ND_POINTCACHE, ob);

  return OPERATOR_FINISHED;
}
static wmOperatorStatus ptcache_bake_from_cache_exec(bContext *C, wmOperator * /*op*/)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);
  PointCache *cache = static_cast<PointCache *>(ptr.data);
  Object *ob = (Object *)ptr.owner_id;

  cache->flag |= PTCACHE_BAKED;

  WM_event_add_notifier(C, NC_OBJECT | ND_POINTCACHE, ob);

  return OPERATOR_FINISHED;
}
void PTCACHE_OT_bake(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Physics";
  ot->description = "Bake physics";
  ot->idname = "PTCACHE_OT_bake";

  /* API callbacks. */
  ot->exec = ptcache_bake_exec;
  ot->invoke = ptcache_bake_invoke;
  ot->modal = ptcache_bake_modal;
  ot->cancel = ptcache_bake_cancel;
  ot->poll = ptcache_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "bake", false, "Bake", "");
}
void PTCACHE_OT_free_bake(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Physics Bake";
  ot->description = "Delete physics bake";
  ot->idname = "PTCACHE_OT_free_bake";

  /* API callbacks. */
  ot->exec = ptcache_free_bake_exec;
  ot->poll = ptcache_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
void PTCACHE_OT_bake_from_cache(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake from Cache";
  ot->description = "Bake from cache";
  ot->idname = "PTCACHE_OT_bake_from_cache";

  /* API callbacks. */
  ot->exec = ptcache_bake_from_cache_exec;
  ot->poll = ptcache_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus ptcache_add_new_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);
  Object *ob = (Object *)ptr.owner_id;
  PointCache *cache = static_cast<PointCache *>(ptr.data);
  PTCacheID pid = BKE_ptcache_id_find(ob, scene, cache);

  if (pid.cache) {
    PointCache *cache_new = BKE_ptcache_add(pid.ptcaches);
    cache_new->step = pid.default_step;
    *(pid.cache_ptr) = cache_new;

    DEG_id_tag_update(&ob->id, ID_RECALC_POINT_CACHE);
    WM_event_add_notifier(C, NC_SCENE | ND_FRAME, scene);
    WM_event_add_notifier(C, NC_OBJECT | ND_POINTCACHE, ob);
  }

  return OPERATOR_FINISHED;
}
static wmOperatorStatus ptcache_remove_exec(bContext *C, wmOperator * /*op*/)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "point_cache", &RNA_PointCache);
  Scene *scene = CTX_data_scene(C);
  Object *ob = (Object *)ptr.owner_id;
  PointCache *cache = static_cast<PointCache *>(ptr.data);
  PTCacheID pid = BKE_ptcache_id_find(ob, scene, cache);

  /* don't delete last cache */
  if (pid.cache && pid.ptcaches->first != pid.ptcaches->last) {
    BLI_remlink(pid.ptcaches, pid.cache);
    BKE_ptcache_free(pid.cache);
    *(pid.cache_ptr) = static_cast<PointCache *>(pid.ptcaches->first);

    DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
    WM_event_add_notifier(C, NC_OBJECT | ND_POINTCACHE, ob);
  }

  return OPERATOR_FINISHED;
}
void PTCACHE_OT_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Cache";
  ot->description = "Add new cache";
  ot->idname = "PTCACHE_OT_add";

  /* API callbacks. */
  ot->exec = ptcache_add_new_exec;
  ot->poll = ptcache_add_remove_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
void PTCACHE_OT_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Current Cache";
  ot->description = "Delete current cache";
  ot->idname = "PTCACHE_OT_remove";

  /* API callbacks. */
  ot->exec = ptcache_remove_exec;
  ot->poll = ptcache_add_remove_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
