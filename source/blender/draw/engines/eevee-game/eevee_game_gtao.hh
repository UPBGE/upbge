/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

struct GTAOSettings {
    float radius = 2.0f;          // World space occlusion radius
    float falloff = 1.0f;         // Distance falloff power
    float intensity = 1.0f;       // Strength of the effect
    int quality_steps = 8;        // Steps along each direction
    bool use_multi_bounce = true; // Prevents "flatness" in dark areas
    bool enabled = true;
};

class GTAOModule {
public:
    GTAOModule(class GameInstance &inst);
    
    void init();
    void sync();
    
    /**
     * Executes GTAO logic. 
     * Computed at Half-Resolution for performance, then bilaterally upsampled.
     */
    void render(View &view, gpu::Texture *depth_tx);

    /* When false the result texture holds nothing meaningful. */
    bool enabled() const { return settings_.enabled; }

    gpu::Texture *get_result() { return gtao_final_tx_; }

private:
    GameInstance *inst_;
    GTAOSettings settings_;
    
    // Low-res buffer for main computation
    Texture gtao_lowres_tx_ = {"gtao_lowres_tx_"};
    // Final high-res buffer after upscaling
    Texture gtao_final_tx_ = {"gtao_final_tx_"};

    PassSimple gtao_main_ps_{"GTAO.Compute"};
    PassSimple gtao_upsample_ps_{"GTAO.Upsample"};
};

} // namespace blender::eevee_game