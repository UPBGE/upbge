/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The scene's Physics CPU Use (#GameData.jolt_physics_threads): how many CPU cores Jolt physics
 * may keep busy at once. One rule for the game engine and the editor's "This computer" line.
 */

#include <algorithm>
#include <thread>

namespace blender {

/** This computer's CPU cores as the system counts them (logical processors), at least 1. */
inline int BKE_game_physics_computer_cores()
{
  return std::max(1, int(std::thread::hardware_concurrency()));
}

/** The most cores physics uses at once, the game's own included, for \a setting on a computer
 * with \a computer_cores. Auto (<= 0): the game's own core plus half of the computer's cores, at
 * most 9 in all. More made big scenes no faster (6000 tumbling boxes on 32 cores: 6.7 ms a step
 * with 9, 7.6 ms with 32), and half without the game's own core stepped 10-35% slower on 2 to 8
 * cores. A number is used as it is, never more than the computer has. */
inline int BKE_game_physics_cores(const int setting, int computer_cores)
{
  computer_cores = std::max(computer_cores, 1);
  if (setting <= 0) {
    return std::min(computer_cores / 2, 8) + 1;
  }
  return std::min(setting, computer_cores);
}

}  // namespace blender
