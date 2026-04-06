/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_engine.h"
#include "eevee_game_instance.hh"

#include "RE_pipeline.h"
#include "DRW_render.hh"

#include "../eevee/eevee_shader.hh"

namespace blender::eevee_game {

DrawEngine *Engine::create_instance()
{
  return new GameInstance();
}

void Engine::init_static()
{
  /* Acquire a reference to EEVEE's ShaderModule singleton.
   * eevee_game::ShaderModule::material_shader_get() delegates to eevee::ShaderModule for
   * codegen and pass callbacks — both require the singleton to be alive. */
  eevee::ShaderModule::module_get();
}

void Engine::free_static()
{
  ShaderModule::module_free();
  eevee::ShaderModule::module_free();
}

/* ---- Static render callback (F12 / Render Image) ----
 *
 * FIX: was using engine->type->type_data to pass the GameInstance pointer to the callback.
 * engine->type is a global shared across all engine instances and all viewports/renders.
 * Writing to engine->type->type_data from a per-render callback is a data race when
 * multiple renders or viewports are active simultaneously (e.g. background renders,
 * split viewports with different engine types).
 *
 * The correct pattern is engine->customdata (or engine->re_render.render_data), which is
 * per-engine-instance and not shared.  DRW_render_to_image's trampoline receives the
 * engine pointer, so we recover the GameInstance from engine->customdata. */

static void eevee_game_render_to_image(RenderEngine *engine,
                                       RenderLayer  *layer,
                                       const rcti    *rect)
{
  /* Recover the per-instance GameInstance from customdata (set by eevee_game_render below). */
  auto *instance = static_cast<GameInstance *>(engine->customdata);
  if (!instance) {
    return;
  }

  const int2 size = int2(engine->resolution_x, engine->resolution_y);
  instance->init(size, rect);
  instance->render_frame(engine, layer);
}

static void eevee_game_render(RenderEngine *engine, Depsgraph *depsgraph)
{
  /* Allocate a GameInstance that lives for the duration of this render job.
   * Store it in engine->customdata so the trampoline can reach it without
   * touching the shared engine->type->type_data pointer. */
  auto *instance = new GameInstance();
  engine->customdata = instance;

  DRW_render_to_image(engine, depsgraph, eevee_game_render_to_image, nullptr);

  /* Clean up after the render is complete. */
  engine->customdata = nullptr;
  delete instance;
}

static void eevee_game_render_update_passes(RenderEngine *engine,
                                            Scene        *scene,
                                            ViewLayer    *view_layer)
{
  RE_engine_register_pass(
      engine, scene, view_layer, RE_PASSNAME_COMBINED, 4, "RGBA", SOCK_RGBA);
  RE_engine_register_pass(
      engine, scene, view_layer, RE_PASSNAME_VELOCITY, 4, "XYZW", SOCK_VECTOR);

  if (scene->eevee.flag & SCE_EEVEE_GAME_DEBUG_FSR_MASK) {
    RE_engine_register_pass(
        engine, scene, view_layer, "FSR2_Reactive", 1, "R", SOCK_FLOAT);
  }
}

/* ---- Engine Type Registration ----
 * The idname "BLENDER_EEVEE_GAME" must also be registered in rna_scene.cc.
 *
 * IMPORTANT: Call Engine::init_static() from draw_context.cc immediately after
 * DRW_engines_register() for this engine type. */
RenderEngineType DRW_engine_viewport_eevee_game_type = {
    /* next */                nullptr,
    /* prev */                nullptr,
    /* idname */              "BLENDER_EEVEE_GAME",
    /* name */                N_("EEVEE Game"),
    /* flag */                RE_INTERNAL | RE_USE_GPU_CONTEXT | RE_USE_STEREO_VIEWPORT,
    /* update */              nullptr,
    /* render */              &eevee_game_render,
    /* render_frame_finish */ nullptr,
    /* draw */                nullptr,
    /* bake */                nullptr,
    /* view_update */         nullptr,
    /* view_draw */           nullptr,
    /* update_script_node */  nullptr,
    /* update_render_passes */ &eevee_game_render_update_passes,
    /* update_custom_camera */ nullptr,
    /* draw_engine */         nullptr,
    /* rna_ext */             {nullptr, nullptr, nullptr},
};

} // namespace blender::eevee_game
