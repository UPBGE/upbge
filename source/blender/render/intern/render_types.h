/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup render
 */

#pragma once

/* ------------------------------------------------------------------------- */
/* exposed internal in render module only! */
/* ------------------------------------------------------------------------- */

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_threads.h"

#include "BKE_main.h"

#include "RE_pipeline.h"

struct GHash;
struct GSet;
struct Main;
struct Object;
struct RenderEngine;
struct ReportList;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HighlightedTile {
  rcti rect;
} HighlightedTile;

/* controls state of render, everything that's read-only during render stage */
struct Render {
  struct Render *next, *prev;
  char name[RE_MAXNAME];
  int slot;

  /* state settings */
  short flag, ok, result_ok;

  /* result of rendering */
  RenderResult *result;
  /* if render with single-layer option, other rendered layers are stored here */
  RenderResult *pushedresult;
  /** A list of #RenderResults, for full-samples. */
  ListBase fullresult;
  /* read/write mutex, all internal code that writes to re->result must use a
   * write lock, all external code must use a read lock. internal code is assumed
   * to not conflict with writes, so no lock used for that */
  ThreadRWMutex resultmutex;

  /* Guard for drawing render result using engine's `draw()` callback. */
  ThreadMutex engine_draw_mutex;

  /** Window size, display rect, viewplane.
   * \note Buffer width and height with percentage applied
   * without border & crop. convert to long before multiplying together to avoid overflow. */
  int winx, winy;
  rcti disprect;  /* part within winx winy */
  rctf viewplane; /* mapped on winx winy */

  /* final picture width and height (within disprect) */
  int rectx, recty;

  /* Camera transform, only used by Freestyle. */
  float winmat[4][4];

  /* Clipping. */
  float clip_start;
  float clip_end;

  /* main, scene, and its full copy of renderdata and world */
  struct Main *main;
  Scene *scene;
  RenderData r;
  ListBase view_layers;
  int active_view_layer;
  struct Object *camera_override;

  ThreadMutex highlighted_tiles_mutex;
  struct GSet *highlighted_tiles;

  /* render engine */
  struct RenderEngine *engine;

  /* NOTE: This is a minimal dependency graph and evaluated scene which is enough to access view
   * layer visibility and use for postprocessing (compositor and sequencer). */
  Depsgraph *pipeline_depsgraph;
  Scene *pipeline_scene_eval;

  /* callbacks */
  void (*display_init)(void *handle, RenderResult *rr);
  void *dih;
  void (*display_clear)(void *handle, RenderResult *rr);
  void *dch;
  void (*display_update)(void *handle, RenderResult *rr, rcti *rect);
  void *duh;
  void (*current_scene_update)(void *handle, struct Scene *scene);
  void *suh;

  void (*stats_draw)(void *handle, RenderStats *ri);
  void *sdh;
  void (*progress)(void *handle, float i);
  void *prh;

  void (*draw_lock)(void *handle, bool lock);
  void *dlh;
  int (*test_break)(void *handle);
  void *tbh;

  RenderStats i;

  struct ReportList *reports;

  void **movie_ctx_arr;
  char viewname[MAX_NAME];

  /* TODO: replace by a whole draw manager. */
  void *gl_context;
  void *gpu_context;
};

/* **************** defines ********************* */

/** #R.flag */
#define R_ANIMATION 1

#ifdef __cplusplus
}
#endif
