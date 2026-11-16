/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view.hh"

namespace blender::eevee_game {

class GameInstance;

/**
 * Main camera view with frustum + Hi-Z occlusion culling.
 *
 * The DRW Manager already owns everything a culling pass needs: world space
 * bounds for every resource (`bounds_buf`) and a per-view visibility bitmask
 * that `draw_command_generate` turns into indirect draws. So culling here is
 * nothing more than a different shader writing that same bitmask. No extra
 * instance buffer, no CPU upload, no second draw path.
 *
 * Same extension point EEVEE uses for `ShadowModule::ShadowView`.
 */
class GameView : public draw::View {
 public:
  GameView(const char *name, GameInstance &inst) : draw::View(name), inst_(inst) {}

 protected:
  /* Called by the Manager on the first submit() after each sync(), i.e. once
   * per frame and before the depth prepass. The Hi-Z pyramid bound at that
   * point is therefore the one built during the previous frame. */
  void compute_visibility(draw::ObjectBoundsBuf &bounds,
                          draw::ObjectInfosBuf &infos,
                          uint resource_len,
                          bool debug_freeze) override;

 private:
  GameInstance &inst_;
};

}  // namespace blender::eevee_game
