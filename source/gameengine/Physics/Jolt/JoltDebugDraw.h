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
 *  \brief Debug drawing utilities for Jolt Physics in UPBGE.
 *
 *  Since JPH_DEBUG_RENDERER is not enabled, this provides manual debug
 *  drawing by iterating bodies and drawing their AABBs/shapes as wireframes
 *  using UPBGE's KX_RasterizerDrawDebugLine.
 */

#pragma once

class JoltPhysicsEnvironment;

namespace JoltDebugDraw {

/** Draw wireframe AABBs for all bodies in the physics system.
 *  Uses KX_RasterizerDrawDebugLine for rendering. */
void DrawBodies(JoltPhysicsEnvironment *env);

/** Draw wireframe representations of all active constraints. */
void DrawConstraints(JoltPhysicsEnvironment *env);

}  // namespace JoltDebugDraw
