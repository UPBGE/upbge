/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

class GameInstance;

struct SSRSettings {
    float max_roughness = 0.5f;     // Only reflect surfaces smoother than this
    int max_steps = 64;             // Max iterations for the ray march
    float thickness = 0.1f;         // Thickness of objects to prevent "leaking"
    float edge_fade = 0.1f;         // Fade reflections at screen edges
    /* Off until a game SSR trace shader exists (see RayTraceModule::sync). */
    bool enabled = false;
};

class RayTraceModule {
public:
    RayTraceModule(GameInstance *inst);
    ~RayTraceModule();

    void init();
    void sync();
    
    /**
     * Performs the Screen Space Reflection pass.
     * @param radiance_tx The current combined color buffer to sample from.
     * @param out_radiance The target buffer for the reflection results.
     */
    void render(gpu::Texture *radiance_tx, gpu::Texture *out_radiance);

private:
    GameInstance *inst_;
    SSRSettings settings_;
    
    // Internal buffers
    PassSimple ssr_ps_{"SSR.Trace"};
    
    // We reuse the HIZ buffer from the instance for accelerated tracing
    gpu::Texture *hiz_tx_ = nullptr;
};

} // namespace blender::eevee_game