/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_engine.h"
#include "eevee_game_instance.hh"

#include "BLT_translation.hh"

#include "DNA_node_types.h"

#include "RE_pipeline.h"
#include "DRW_render.hh"

#include "../eevee/eevee_shader.hh"

namespace blender::eevee_game {

DrawEngine *Engine::create_instance()
{
  return new GameInstance();
}

void Engine::free_static()
{
  /* Static shaders are owned by each GameInstance's ShaderModule and die with
   * it. Material shaders belong to EEVEE's shader module, which
   * eevee::Engine::free_static() releases; releasing it here as well would be
   * a second owner for something this engine only borrows. */
  ShaderModule::module_free();
}

/* ---- Render callback (F12 / Render Image) ----
 *
 * Same shape as eevee_render(): the instance is created inside the trampoline
 * because it needs an active DRWContext, and it is captured by the lambda so
 * nothing has to be stored on the shared RenderEngineType. */

static void eevee_game_render(RenderEngine *engine, Depsgraph *depsgraph)
{
  GameInstance *instance = nullptr;

  auto render_to_image = [&](RenderEngine *engine, RenderLayer *layer, const rcti rect) {
    /* One call per view in multiview renders. */
    delete instance;
    instance = new GameInstance();

    const int2 size = int2(engine->resolution_x, engine->resolution_y);
    instance->init(size, &rect);
    instance->render_frame(engine, layer);
  };

  DRW_render_to_image(engine, depsgraph, render_to_image, [](RenderResult *) {});

  delete instance;
}

static void eevee_game_render_update_passes(RenderEngine *engine,
                                            Scene        *scene,
                                            ViewLayer    *view_layer)
{
  RE_engine_register_pass(
      engine, scene, view_layer, RE_PASSNAME_COMBINED, 4, "RGBA", SOCK_RGBA);
  RE_engine_register_pass(
      engine, scene, view_layer, RE_PASSNAME_VECTOR, 4, "XYZW", SOCK_VECTOR);
}

/* ---- Engine Type ----
 *
 * Not registered with RE_engines_register() on purpose. The scene's render
 * engine stays EEVEE, so every panel, node poll and material setting keeps
 * working unchanged, and the Draw Manager selects this engine only inside
 * DRW_game_render_loop(). The type and the F12 path above are kept for the
 * day the engine is exposed in the render engine list. */
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
