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
 * with \a computer_cores. Auto (<= 0): every one of the computer's cores (logical processors), at
 * most 9. Jolt 5.7.0 on simulated 4 / 8 / 12 / 16-thread computers (2026-11-16): every thread
 * stepped big scenes 13 / 14 / 11 / 8% faster than the game's own core plus half (the rule
 * before), small ones the same; more than 9 made big scenes slower (32 threads on two CPU
 * chiplets: 4.3 ms a step with 7, 4.9 with 9, 5.7 with 12 or more). A number is used as it is,
 * never more than the computer has. */
inline int BKE_game_physics_cores(const int setting, int computer_cores)
{
  computer_cores = std::max(computer_cores, 1);
  if (setting <= 0) {
    return std::min(computer_cores, 9);
  }
  return std::min(setting, computer_cores);
}

}  // namespace blender
