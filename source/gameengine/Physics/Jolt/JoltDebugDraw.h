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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltDebugDraw.h
 *  \ingroup physjolt
 *  \brief Physics Visualization for Jolt Physics in UPBGE.
 *
 *  Jolt's own debug renderer (JPH_DEBUG_RENDERER) is not built, so the world is drawn here from
 *  Jolt's public shape API as debug lines (2026-11-18; before, every body was its bounding box).
 */

#pragma once

#include <functional>

#include "MT_Vector3.h"
#include "MT_Vector4.h"

class JoltPhysicsEnvironment;

namespace JoltDebugDraw {

/** Where the lines go: the rasterizer's debug lines in a game, a list in the tests. */
using LineSink =
    std::function<void(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)>;

struct Settings {
  /** The physics environment's debug mode (Bullet's btIDebugDraw bits, bge.constraints' DBG_*):
   * any nonzero mode draws the shapes, contacts, constraints, wheels and characters;
   * kDrawAabbBit adds each body's bounding box. */
  int debug_mode = 1;
  /** Triangle meshes and height fields draw only their triangles within kMeshDrawRange of this
   * point (the camera); null: all of them (still within kMaxTrianglesPerDraw). */
  const MT_Vector3 *view_center = nullptr;
};

/** Bullet's DBG_DrawAabb. */
constexpr int kDrawAabbBit = 2;
constexpr float kMeshDrawRange = 50.0f;
/** Triangles drawn at most per draw, every body together (a level mesh would flood the view). */
constexpr int kMaxTrianglesPerDraw = 60000;
constexpr int kCircleSegments = 24;
/** A contact point's cross (half size) and its normal's length (m). */
constexpr float kContactSize = 0.05f;
constexpr float kContactNormalLength = 0.3f;
/** A character's Ground Normal and Wall Normal arrows (m). */
constexpr float kCharacterArrowLength = 0.6f;
/** A constraint's frame axes (m). */
constexpr float kConstraintAxisLength = 0.2f;

/** The colors, by what a body is (checked in this order): a sensor, static, a Character's,
 * sleeping (a Dynamic or Kinematic body at rest), Kinematic, Dynamic (soft bodies too). */
const MT_Vector4 &SensorColor();
const MT_Vector4 &StaticColor();
const MT_Vector4 &CharacterColor();
const MT_Vector4 &SleepingColor();
const MT_Vector4 &KinematicColor();
const MT_Vector4 &DynamicColor();
const MT_Vector4 &AabbColor();
const MT_Vector4 &ContactColor();
const MT_Vector4 &ConstraintColor();
const MT_Vector4 &WheelColor();
const MT_Vector4 &GroundNormalColor();
const MT_Vector4 &WallNormalColor();

/**
 * Draws the physics world (Physics Visualization):
 * - each body in the world as its collision shape (box, sphere, capsule, cylinder, convex hull
 *   faces, triangles of meshes, height fields and soft bodies; compounds, scaled and moved parts
 *   where they are), colored by what it is;
 * - Sensor-type objects Logic Nodes query outside Jolt's world (JoltPhysicsEnvironment::
 *   GetQuerySensors) where their objects are; bodies outside the world otherwise (Add Object's
 *   templates on hidden layers, suspended physics, compound children) are not drawn;
 * - the contact points of the last step and their normals, a character's touching contacts;
 * - constraints (their frames and the line between their bodies' anchors), vehicle wheels;
 * - a character's Ground Normal (from where it stands) and Wall Normal (from its middle).
 */
void DrawWorld(JoltPhysicsEnvironment &env, const Settings &settings, const LineSink &sink);

}  // namespace JoltDebugDraw
