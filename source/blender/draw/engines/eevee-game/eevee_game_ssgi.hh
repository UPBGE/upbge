/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

struct SSGISettings {
    float intensity = 1.0f;       // Strength of the indirect bounce
    float radius = 2.0f;          // Max distance for screen-space bounces
    float color_saturation = 1.0f; // Boosts or reduces the "bleed" effect
    int quality_steps = 8;        // Ray-marching steps per direction
    bool enabled = true;
};

class SSGIModule {
public:
    SSGIModule(class GameInstance &inst);
    
    void init();
    void sync();
    
    /**
     * Executes SSGI computation.
     * @param scene_color_tx The lighting result from the previous frame or current pass.
     * @param depth_tx Normal/Depth info for geometric validation.
     */
    void render(View &view, gpu::Texture *scene_color_tx, gpu::Texture *depth_tx);

    /* When false the result texture holds nothing meaningful. */
    bool enabled() const { return settings_.enabled; }

    gpu::Texture *get_result() { return ssgi_final_tx_; }

private:
    GameInstance *inst_;
    SSGISettings settings_;
    
    // Low-res buffer for the diffuse bounce (SFLOAT_16 for color)
    Texture ssgi_lowres_tx_ = {"ssgi_lowres_tx_"};
    // Final filtered buffer
    Texture ssgi_final_tx_ = {"ssgi_final_tx_"};

    PassSimple ssgi_main_ps_{"SSGI.Compute"};
    PassSimple ssgi_blur_ps_{"SSGI.BilateralBlur"};
};

} // namespace blender::eevee_game