/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. */

/** \file
 * \ingroup draw
 */

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_world_types.h"

#include "PIL_time.h"

#include "BLI_dynstr.h"
#include "BLI_listbase.h"
#include "BLI_string_utils.h"
#include "BLI_threads.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"

#include "DEG_depsgraph_query.h"

#include "GPU_capabilities.h"
#include "GPU_material.h"
#include "GPU_shader.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_window.h"

#include "draw_manager.h"

#include "CLG_log.h"

static CLG_LogRef LOG = {"draw.manager.shader"};

extern char datatoc_gpu_shader_depth_only_frag_glsl[];
extern char datatoc_common_fullscreen_vert_glsl[];

#define USE_DEFERRED_COMPILATION 1

/* -------------------------------------------------------------------- */
/** \name Deferred Compilation (DRW_deferred)
 *
 * Since compiling shader can take a long time, we do it in a non blocking
 * manner in another thread.
 *
 * \{ */

typedef struct DRWShaderCompiler {
  ListBase queue; /* GPUMaterial */
  SpinLock list_lock;

  void *gl_context;
  GPUContext *gpu_context;
  bool own_context;
} DRWShaderCompiler;

static void drw_deferred_shader_compilation_exec(
    void *custom_data,
    /* Cannot be const, this function implements wm_jobs_start_callback.
     * NOLINTNEXTLINE: readability-non-const-parameter. */
    short *stop,
    short *UNUSED(do_update),
    float *UNUSED(progress))
{
  GPU_render_begin();
  DRWShaderCompiler *comp = (DRWShaderCompiler *)custom_data;
  void *gl_context = comp->gl_context;
  GPUContext *gpu_context = comp->gpu_context;

  BLI_assert(gl_context != NULL);
  BLI_assert(gpu_context != NULL);

  const bool use_main_context_workaround = GPU_use_main_context_workaround();
  if (use_main_context_workaround) {
    BLI_assert(gl_context == DST.gl_context);
    GPU_context_main_lock();
  }

  WM_opengl_context_activate(gl_context);
  GPU_context_active_set(gpu_context);

  while (true) {
    if (*stop != 0) {
      /* We don't want user to be able to cancel the compilation
       * but wm can kill the task if we are closing blender. */
      break;
    }

    BLI_spin_lock(&comp->list_lock);
    /* Pop tail because it will be less likely to lock the main thread
     * if all GPUMaterials are to be freed (see DRW_deferred_shader_remove()). */
    LinkData *link = (LinkData *)BLI_poptail(&comp->queue);
    GPUMaterial *mat = link ? (GPUMaterial *)link->data : NULL;
    if (mat) {
      /* Avoid another thread freeing the material mid compilation. */
      GPU_material_acquire(mat);
    }
    BLI_spin_unlock(&comp->list_lock);

    if (mat) {
      /* Do the compilation. */
      GPU_material_compile(mat);
      GPU_material_release(mat);
      MEM_freeN(link);
    }
    else {
      break;
    }

    if (GPU_type_matches_ex(GPU_DEVICE_ANY, GPU_OS_ANY, GPU_DRIVER_ANY, GPU_BACKEND_OPENGL)) {
      GPU_flush();
    }
  }

  GPU_context_active_set(NULL);
  WM_opengl_context_release(gl_context);
  if (use_main_context_workaround) {
    GPU_context_main_unlock();
  }
  GPU_render_end();
}

static void drw_deferred_shader_compilation_free(void *custom_data)
{
  DRWShaderCompiler *comp = (DRWShaderCompiler *)custom_data;

  BLI_spin_lock(&comp->list_lock);
  BLI_freelistN(&comp->queue);
  BLI_spin_unlock(&comp->list_lock);

  if (comp->own_context) {
    /* Only destroy if the job owns the context. */
    WM_opengl_context_activate(comp->gl_context);
    GPU_context_active_set(comp->gpu_context);
    GPU_context_discard(comp->gpu_context);
    WM_opengl_context_dispose(comp->gl_context);

    wm_window_reset_drawable();
  }

  MEM_freeN(comp);
}

static void drw_deferred_shader_add(GPUMaterial *mat, bool deferred)
{
  if (ELEM(GPU_material_status(mat), GPU_MAT_SUCCESS, GPU_MAT_FAILED)) {
    return;
  }

  /* Use original scene ID since this is what the jobs template tests for. */
  Scene *scene = (Scene *)DEG_get_original_id(&DST.draw_ctx.scene->id);

  /* Do not defer the compilation if we are rendering for image.
   * deferred rendering is only possible when `evil_C` is available */
  if (DST.draw_ctx.evil_C == NULL || DRW_state_is_image_render() || !USE_DEFERRED_COMPILATION || (scene->flag & SCE_INTERACTIVE || (scene->flag & SCE_IS_BLENDERPLAYER))) {
    deferred = false;
  }

  if (!deferred) {
    DRW_deferred_shader_remove(mat);
    /* Shaders could already be compiling. Have to wait for compilation to finish. */
    while (GPU_material_status(mat) == GPU_MAT_QUEUED) {
      PIL_sleep_ms(20);
    }
    if (GPU_material_status(mat) == GPU_MAT_CREATED) {
      GPU_material_compile(mat);
    }
    return;
  }

  /* Don't add material to the queue twice. */
  if (GPU_material_status(mat) == GPU_MAT_QUEUED) {
    return;
  }

  const bool use_main_context = GPU_use_main_context_workaround();
  const bool job_own_context = !use_main_context;

  BLI_assert(DST.draw_ctx.evil_C);
  wmWindowManager *wm = CTX_wm_manager(DST.draw_ctx.evil_C);
  wmWindow *win = CTX_wm_window(DST.draw_ctx.evil_C);

  /* Get the running job or a new one if none is running. Can only have one job per type & owner.
   */
  wmJob *wm_job = WM_jobs_get(
      wm, win, wm, "Shaders Compilation", 0, WM_JOB_TYPE_SHADER_COMPILATION);

  DRWShaderCompiler *old_comp = (DRWShaderCompiler *)WM_jobs_customdata_get(wm_job);

  DRWShaderCompiler *comp = MEM_callocN(sizeof(DRWShaderCompiler), "DRWShaderCompiler");
  BLI_spin_init(&comp->list_lock);

  if (old_comp) {
    BLI_spin_lock(&old_comp->list_lock);
    BLI_movelisttolist(&comp->queue, &old_comp->queue);
    BLI_spin_unlock(&old_comp->list_lock);
    /* Do not recreate context, just pass ownership. */
    if (old_comp->gl_context) {
      comp->gl_context = old_comp->gl_context;
      comp->gpu_context = old_comp->gpu_context;
      old_comp->own_context = false;
      comp->own_context = job_own_context;
    }
  }

  GPU_material_status_set(mat, GPU_MAT_QUEUED);
  LinkData *node = BLI_genericNodeN(mat);
  BLI_addtail(&comp->queue, node);

  /* Create only one context. */
  if (comp->gl_context == NULL) {
    if (use_main_context) {
      comp->gl_context = DST.gl_context;
      comp->gpu_context = DST.gpu_context;
    }
    else {
      comp->gl_context = WM_opengl_context_create();
      comp->gpu_context = GPU_context_create(NULL);
      GPU_context_active_set(NULL);

      WM_opengl_context_activate(DST.gl_context);
      GPU_context_active_set(DST.gpu_context);
    }
    comp->own_context = job_own_context;
  }

  WM_jobs_customdata_set(wm_job, comp, drw_deferred_shader_compilation_free);
  WM_jobs_timer(wm_job, 0.1, NC_MATERIAL | ND_SHADING_DRAW, 0);
  WM_jobs_delay_start(wm_job, 0.1);
  WM_jobs_callbacks(wm_job, drw_deferred_shader_compilation_exec, NULL, NULL, NULL);

  G.is_break = false;

  WM_jobs_start(wm, wm_job);
}

void DRW_deferred_shader_remove(GPUMaterial *mat)
{
  LISTBASE_FOREACH (wmWindowManager *, wm, &G_MAIN->wm) {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      DRWShaderCompiler *comp = (DRWShaderCompiler *)WM_jobs_customdata_from_type(
          wm, wm, WM_JOB_TYPE_SHADER_COMPILATION);
      if (comp != NULL) {
        BLI_spin_lock(&comp->list_lock);
        LinkData *link = (LinkData *)BLI_findptr(&comp->queue, mat, offsetof(LinkData, data));
        if (link) {
          BLI_remlink(&comp->queue, link);
          GPU_material_status_set(link->data, GPU_MAT_CREATED);
        }
        BLI_spin_unlock(&comp->list_lock);

        MEM_SAFE_FREE(link);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */

/** \{ */

GPUShader *DRW_shader_create_ex(
    const char *vert, const char *geom, const char *frag, const char *defines, const char *name)
{
  return GPU_shader_create(vert, frag, geom, NULL, defines, name);
}

GPUShader *DRW_shader_create_with_lib_ex(const char *vert,
                                         const char *geom,
                                         const char *frag,
                                         const char *lib,
                                         const char *defines,
                                         const char *name)
{
  GPUShader *sh;
  char *vert_with_lib = NULL;
  char *frag_with_lib = NULL;
  char *geom_with_lib = NULL;

  vert_with_lib = BLI_string_joinN(lib, vert);
  frag_with_lib = BLI_string_joinN(lib, frag);
  if (geom) {
    geom_with_lib = BLI_string_joinN(lib, geom);
  }

  sh = GPU_shader_create(vert_with_lib, frag_with_lib, geom_with_lib, NULL, defines, name);

  MEM_freeN(vert_with_lib);
  MEM_freeN(frag_with_lib);
  if (geom) {
    MEM_freeN(geom_with_lib);
  }

  return sh;
}

GPUShader *DRW_shader_create_with_shaderlib_ex(const char *vert,
                                               const char *geom,
                                               const char *frag,
                                               const DRWShaderLibrary *lib,
                                               const char *defines,
                                               const char *name)
{
  GPUShader *sh;
  char *vert_with_lib = DRW_shader_library_create_shader_string(lib, vert);
  char *frag_with_lib = DRW_shader_library_create_shader_string(lib, frag);
  char *geom_with_lib = (geom) ? DRW_shader_library_create_shader_string(lib, geom) : NULL;

  sh = GPU_shader_create(vert_with_lib, frag_with_lib, geom_with_lib, NULL, defines, name);

  MEM_SAFE_FREE(vert_with_lib);
  MEM_SAFE_FREE(frag_with_lib);
  MEM_SAFE_FREE(geom_with_lib);

  return sh;
}

GPUShader *DRW_shader_create_with_transform_feedback(const char *vert,
                                                     const char *geom,
                                                     const char *defines,
                                                     const eGPUShaderTFBType prim_type,
                                                     const char **varying_names,
                                                     const int varying_count)
{
  return GPU_shader_create_ex(vert,
                              datatoc_gpu_shader_depth_only_frag_glsl,
                              geom,
                              NULL,
                              NULL,
                              defines,
                              prim_type,
                              varying_names,
                              varying_count,
                              __func__);
}

GPUShader *DRW_shader_create_fullscreen_ex(const char *frag, const char *defines, const char *name)
{
  return GPU_shader_create(datatoc_common_fullscreen_vert_glsl, frag, NULL, NULL, defines, name);
}

GPUShader *DRW_shader_create_fullscreen_with_shaderlib_ex(const char *frag,
                                                          const DRWShaderLibrary *lib,
                                                          const char *defines,
                                                          const char *name)
{

  GPUShader *sh;
  char *vert = datatoc_common_fullscreen_vert_glsl;
  char *frag_with_lib = DRW_shader_library_create_shader_string(lib, frag);

  sh = GPU_shader_create(vert, frag_with_lib, NULL, NULL, defines, name);

  MEM_SAFE_FREE(frag_with_lib);

  return sh;
}

GPUMaterial *DRW_shader_from_world(World *wo,
                                   struct bNodeTree *ntree,
                                   const uint64_t shader_id,
                                   const bool is_volume_shader,
                                   bool deferred,
                                   GPUCodegenCallbackFn callback,
                                   void *thunk)
{
  Scene *scene = (Scene *)DEG_get_original_id(&DST.draw_ctx.scene->id);
  GPUMaterial *mat = GPU_material_from_nodetree(scene,
                                                NULL,
                                                ntree,
                                                &wo->gpumaterial,
                                                wo->id.name,
                                                shader_id,
                                                is_volume_shader,
                                                false,
                                                callback,
                                                thunk);
  if (DRW_state_is_image_render()) {
    /* Do not deferred if doing render. */
    deferred = false;
  }

  drw_deferred_shader_add(mat, deferred);
  return mat;
}

GPUMaterial *DRW_shader_from_material(Material *ma,
                                      struct bNodeTree *ntree,
                                      const uint64_t shader_id,
                                      const bool is_volume_shader,
                                      bool deferred,
                                      GPUCodegenCallbackFn callback,
                                      void *thunk)
{
  Scene *scene = (Scene *)DEG_get_original_id(&DST.draw_ctx.scene->id);
  GPUMaterial *mat = GPU_material_from_nodetree(scene,
                                                ma,
                                                ntree,
                                                &ma->gpumaterial,
                                                ma->id.name,
                                                shader_id,
                                                is_volume_shader,
                                                false,
                                                callback,
                                                thunk);

  if (DRW_state_is_image_render()) {
    /* Do not deferred if doing render. */
    deferred = false;
  }

  drw_deferred_shader_add(mat, deferred);
  return mat;
}

void DRW_shader_free(GPUShader *shader)
{
  GPU_shader_free(shader);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Library
 *
 * Simple include system for glsl files.
 *
 * Usage: Create a DRWShaderLibrary and add the library in the right order.
 * You can have nested dependencies but each new library needs to have all its dependencies already
 * added to the DRWShaderLibrary.
 * Finally you can use DRW_shader_library_create_shader_string to get a shader string that also
 * contains the needed libraries for this shader.
 * \{ */

/* 64 because we use a 64bit bitmap. */
#define MAX_LIB 64
#define MAX_LIB_NAME 64
#define MAX_LIB_DEPS 8

struct DRWShaderLibrary {
  const char *libs[MAX_LIB];
  char libs_name[MAX_LIB][MAX_LIB_NAME];
  uint64_t libs_deps[MAX_LIB];
};

DRWShaderLibrary *DRW_shader_library_create(void)
{
  return MEM_callocN(sizeof(DRWShaderLibrary), "DRWShaderLibrary");
}

void DRW_shader_library_free(DRWShaderLibrary *lib)
{
  MEM_SAFE_FREE(lib);
}

static int drw_shader_library_search(const DRWShaderLibrary *lib, const char *name)
{
  for (int i = 0; i < MAX_LIB; i++) {
    if (lib->libs[i]) {
      if (!strncmp(lib->libs_name[i], name, strlen(lib->libs_name[i]))) {
        return i;
      }
    }
    else {
      break;
    }
  }
  return -1;
}

/* Return bitmap of dependencies. */
static uint64_t drw_shader_dependencies_get(const DRWShaderLibrary *lib,
                                            const char *pragma_str,
                                            const char *lib_code,
                                            const char *UNUSED(lib_name))
{
  /* Search dependencies. */
  uint pragma_len = strlen(pragma_str);
  uint64_t deps = 0;
  const char *haystack = lib_code;
  while ((haystack = strstr(haystack, pragma_str))) {
    haystack += pragma_len;
    int dep = drw_shader_library_search(lib, haystack);
    if (dep == -1) {
      char dbg_name[MAX_NAME];
      int i = 0;
      while ((*haystack != ')') && (i < (sizeof(dbg_name) - 2))) {
        dbg_name[i] = *haystack;
        haystack++;
        i++;
      }
      dbg_name[i] = '\0';

      CLOG_INFO(&LOG,
                0,
                "Dependency '%s' not found\n"
                "This might be due to bad lib ordering or overriding a builtin shader.\n",
                dbg_name);
    }
    else {
      deps |= 1llu << ((uint64_t)dep);
    }
  }
  return deps;
}

void DRW_shader_library_add_file(DRWShaderLibrary *lib, const char *lib_code, const char *lib_name)
{
  int index = -1;
  for (int i = 0; i < MAX_LIB; i++) {
    if (lib->libs[i] == NULL) {
      index = i;
      break;
    }
  }

  if (index > -1) {
    lib->libs[index] = lib_code;
    BLI_strncpy(lib->libs_name[index], lib_name, MAX_LIB_NAME);
    lib->libs_deps[index] = drw_shader_dependencies_get(
        lib, "BLENDER_REQUIRE(", lib_code, lib_name);
  }
  else {
    printf("Error: Too many libraries. Cannot add %s.\n", lib_name);
    BLI_assert(0);
  }
}

char *DRW_shader_library_create_shader_string(const DRWShaderLibrary *lib, const char *shader_code)
{
  uint64_t deps = drw_shader_dependencies_get(lib, "BLENDER_REQUIRE(", shader_code, "shader code");

  DynStr *ds = BLI_dynstr_new();
  /* Add all dependencies recursively. */
  for (int i = MAX_LIB - 1; i > -1; i--) {
    if (lib->libs[i] && (deps & (1llu << (uint64_t)i))) {
      deps |= lib->libs_deps[i];
    }
  }
  /* Concatenate all needed libs into one string. */
  for (int i = 0; i < MAX_LIB && deps != 0llu; i++, deps >>= 1llu) {
    if (deps & 1llu) {
      BLI_dynstr_append(ds, lib->libs[i]);
    }
  }

  BLI_dynstr_append(ds, shader_code);

  char *str = BLI_dynstr_get_cstring(ds);
  BLI_dynstr_free(ds);

  return str;
}

/** \} */
