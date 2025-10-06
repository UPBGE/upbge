/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file PhysicsStateFactory.h
 *  \ingroup ketsji
 *  \brief Factory for creating physics state objects from DNA GameData
 */

#pragma once

#include <memory>

#include "DNA_scene_types.h"
#include "KX_KetsjiEngine.h"

/**
 * Factory for creating physics timestep state objects from DNA GameData.
 * Centralizes initialization to ensure consistent DNA→State field mappings.
 * 
 * DNA FIELD MAPPINGS:
 * ┌───────────────────────────┬──────────────────────┬─────────────────────────┐
 * │ DNA Field (GameData)      │ State Variable       │ Used By                 │
 * ├───────────────────────────┼──────────────────────┼─────────────────────────┤
 * │ FIXED MODE                │                      │                         │
 * │ physics_tick_rate         │ tickRate             │ Accumulator pattern     │
 * │ maxphystep                │ maxPhysicsSteps      │ GetFrameTimesFixed()    │
 * │ use_fixed_fps_cap         │ useFPSCap            │ NextFrameFixed()        │
 * │ fixed_logic_rate          │ logicRate            │ GetFrameTimesFixed()    │
 * │ fixed_render_cap_rate     │ renderCapRate        │ NextFrameFixed()        │
 * │ fixed_max_logic_step      │ maxLogicFrames       │ GetFrameTimesFixed()    │
 * ├───────────────────────────┼──────────────────────┼─────────────────────────┤
 * │ VARIABLE MODE (Legacy)    │                      │                         │
 * │ ticrate                   │ logicRate            │ GetFrameTimesVariable() │
 * │ maxlogicstep              │ maxLogicFrames       │ GetFrameTimesVariable() │
 * └───────────────────────────┴──────────────────────┴─────────────────────────┘
 * 
 * WARNING: Variable mode mappings preserve original BGE behavior.
 *          Do NOT modify without verifying backward compatibility!
 * 
 * Usage:
 *   auto state = PhysicsStateFactory::CreateFixed(scene->gm);
 *   auto state = PhysicsStateFactory::CreateVariable(scene->gm);
 */
class PhysicsStateFactory {
 public:
  /**
   * Create a FixedPhysicsState from GameData.
   * Returns polymorphic IPhysicsState pointer for unified handling.
   * Uses accumulator pattern for deterministic physics at constant rate.
   * See class-level table for complete DNA→State mappings.
   */
  static std::unique_ptr<IPhysicsState> CreateFixed(const GameData &gm)
  {
    return std::make_unique<FixedPhysicsState>(
        gm.physics_tick_rate,      // Physics simulation rate (Hz) - used by accumulator
        gm.maxphystep,              // Max physics substeps per frame - prevents spiral of death
        gm.use_fixed_fps_cap != 0,  // Render FPS cap toggle - enables deadline pacing
        gm.fixed_logic_rate,        // Logic update rate (Hz) - independent from physics
        gm.fixed_render_cap_rate,   // Render FPS target (Hz) - active when cap enabled
        gm.fixed_max_logic_step);   // Max logic frames per render - prevents falling behind
  }

  /**
   * Create a VariablePhysicsState from GameData.
   * Returns polymorphic IPhysicsState pointer for unified handling.
   * Couples physics to framerate (original BGE behavior).
   * CRITICAL: Preserves exact legacy behavior - DO NOT modify mappings!
   * See class-level table for complete DNA→State mappings.
   */
  static std::unique_ptr<IPhysicsState> CreateVariable(const GameData &gm)
  {
    return std::make_unique<VariablePhysicsState>(
        gm.ticrate,       // Logic/physics rate (Hz, double) - physics coupled to logic
        gm.maxlogicstep); // Max frames per render - limits both logic and physics
  }

 private:
  // Private constructor - this is a static factory class, no instances needed
  PhysicsStateFactory() = delete;
  ~PhysicsStateFactory() = delete;
  PhysicsStateFactory(const PhysicsStateFactory &) = delete;
  PhysicsStateFactory &operator=(const PhysicsStateFactory &) = delete;
};
