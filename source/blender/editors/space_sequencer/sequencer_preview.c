/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup spseq
 */

#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"

#include "BLI_listbase.h"
#include "BLI_threads.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_sound.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"

#include "MEM_guardedalloc.h"

#include "sequencer_intern.h"

typedef struct PreviewJob {
  ListBase previews;
  ThreadMutex *mutex;
  Scene *scene;
  int total;
  int processed;
} PreviewJob;

typedef struct PreviewJobAudio {
  struct PreviewJobAudio *next, *prev;
  struct Main *bmain;
  bSound *sound;
  int lr; /* Sample left or right. */
  int startframe;
  bool waveform; /* Reload sound or waveform. */
} PreviewJobAudio;

static void free_preview_job(void *data)
{
  PreviewJob *pj = (PreviewJob *)data;

  BLI_mutex_free(pj->mutex);
  BLI_freelistN(&pj->previews);
  MEM_freeN(pj);
}

/* Only this runs inside thread. */
static void preview_startjob(void *data, short *stop, short *do_update, float *progress)
{
  PreviewJob *pj = data;
  PreviewJobAudio *previewjb;

  BLI_mutex_lock(pj->mutex);
  previewjb = pj->previews.first;
  BLI_mutex_unlock(pj->mutex);

  while (previewjb) {
    PreviewJobAudio *preview_next;
    bSound *sound = previewjb->sound;

    BKE_sound_read_waveform(previewjb->bmain, sound, stop);

    if (*stop || G.is_break) {
      BLI_mutex_lock(pj->mutex);
      previewjb = previewjb->next;
      BLI_mutex_unlock(pj->mutex);
      while (previewjb) {
        sound = previewjb->sound;

        /* Make sure we cleanup the loading flag! */
        BLI_spin_lock(sound->spinlock);
        sound->tags &= ~SOUND_TAGS_WAVEFORM_LOADING;
        BLI_spin_unlock(sound->spinlock);

        BLI_mutex_lock(pj->mutex);
        previewjb = previewjb->next;
        BLI_mutex_unlock(pj->mutex);
      }

      BLI_mutex_lock(pj->mutex);
      BLI_freelistN(&pj->previews);
      pj->total = 0;
      pj->processed = 0;
      BLI_mutex_unlock(pj->mutex);
      break;
    }

    BLI_mutex_lock(pj->mutex);
    preview_next = previewjb->next;
    BLI_freelinkN(&pj->previews, previewjb);
    previewjb = preview_next;
    pj->processed++;
    *progress = (pj->total > 0) ? (float)pj->processed / (float)pj->total : 1.0f;
    *do_update = true;
    BLI_mutex_unlock(pj->mutex);
  }
}

static void preview_endjob(void *data)
{
  PreviewJob *pj = data;

  WM_main_add_notifier(NC_SCENE | ND_SEQUENCER, pj->scene);
}

void sequencer_preview_add_sound(const bContext *C, Sequence *seq)
{
  wmJob *wm_job;
  PreviewJob *pj;
  ScrArea *area = CTX_wm_area(C);
  PreviewJobAudio *audiojob = MEM_callocN(sizeof(PreviewJobAudio), "preview_audio");
  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       CTX_data_scene(C),
                       "Strip Previews",
                       WM_JOB_PROGRESS,
                       WM_JOB_TYPE_SEQ_BUILD_PREVIEW);

  /* Get the preview job if it exists. */
  pj = WM_jobs_customdata_get(wm_job);

  if (!pj) {
    pj = MEM_callocN(sizeof(PreviewJob), "preview rebuild job");

    pj->mutex = BLI_mutex_alloc();
    pj->scene = CTX_data_scene(C);

    WM_jobs_customdata_set(wm_job, pj, free_preview_job);
    WM_jobs_timer(wm_job, 0.1, NC_SCENE | ND_SEQUENCER, NC_SCENE | ND_SEQUENCER);
    WM_jobs_callbacks(wm_job, preview_startjob, NULL, NULL, preview_endjob);
  }

  audiojob->bmain = CTX_data_main(C);
  audiojob->sound = seq->sound;

  BLI_mutex_lock(pj->mutex);
  BLI_addtail(&pj->previews, audiojob);
  pj->total++;
  BLI_mutex_unlock(pj->mutex);

  if (!WM_jobs_is_running(wm_job)) {
    G.is_break = false;
    WM_jobs_start(CTX_wm_manager(C), wm_job);
  }

  ED_area_tag_redraw(area);
}
