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

/** \file JoltDebugDraw.cpp
 *  \ingroup physjolt
 */

#include "JoltDebugDraw.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsEnvironment.h"

#include "KX_Globals.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

/* -------------------------------------------------------------------- */
/** \name Helper: draw a wireframe AABB
 * \{ */

static void DrawAABB(const JPH::AABox &box, const MT_Vector4 &color)
{
  /* Convert 8 corners from Jolt Y-up to Blender Z-up. */
  JPH::Vec3 corners[8] = {
      JPH::Vec3(box.mMin.GetX(), box.mMin.GetY(), box.mMin.GetZ()),
      JPH::Vec3(box.mMax.GetX(), box.mMin.GetY(), box.mMin.GetZ()),
      JPH::Vec3(box.mMax.GetX(), box.mMax.GetY(), box.mMin.GetZ()),
      JPH::Vec3(box.mMin.GetX(), box.mMax.GetY(), box.mMin.GetZ()),
      JPH::Vec3(box.mMin.GetX(), box.mMin.GetY(), box.mMax.GetZ()),
      JPH::Vec3(box.mMax.GetX(), box.mMin.GetY(), box.mMax.GetZ()),
      JPH::Vec3(box.mMax.GetX(), box.mMax.GetY(), box.mMax.GetZ()),
      JPH::Vec3(box.mMin.GetX(), box.mMax.GetY(), box.mMax.GetZ()),
  };

  MT_Vector3 v[8];
  for (int i = 0; i < 8; ++i) {
    v[i] = JoltMath::ToMT(corners[i]);
  }

  /* 12 edges of the box. */
  static const int edges[12][2] = {
      {0, 1}, {1, 2}, {2, 3}, {3, 0},
      {4, 5}, {5, 6}, {6, 7}, {7, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };

  for (int i = 0; i < 12; ++i) {
    KX_RasterizerDrawDebugLine(v[edges[i][0]], v[edges[i][1]], color);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DrawBodies
 * \{ */

void JoltDebugDraw::DrawBodies(JoltPhysicsEnvironment *env)
{
  if (!env || !env->GetPhysicsSystem()) {
    return;
  }

  JPH::PhysicsSystem *physSystem = env->GetPhysicsSystem();
  const JPH::BodyLockInterface &lockInterface = physSystem->GetBodyLockInterface();

  /* Color coding by motion type. */
  static const MT_Vector4 colorStatic(0.0f, 1.0f, 0.0f, 1.0f);
  static const MT_Vector4 colorDynamic(1.0f, 0.5f, 0.0f, 1.0f);
  static const MT_Vector4 colorKinematic(0.0f, 0.5f, 1.0f, 1.0f);
  static const MT_Vector4 colorSensor(1.0f, 1.0f, 0.0f, 1.0f);
  static const MT_Vector4 colorSleeping(0.5f, 0.5f, 0.5f, 1.0f);

  /* Get all body IDs. */
  JPH::BodyIDVector bodyIDs;
  physSystem->GetBodies(bodyIDs);

  for (const JPH::BodyID &id : bodyIDs) {
    JPH::BodyLockRead lock(lockInterface, id);
    if (!lock.Succeeded()) {
      continue;
    }

    const JPH::Body &body = lock.GetBody();
    JPH::AABox aabb = body.GetWorldSpaceBounds();

    const MT_Vector4 *color;
    if (!body.IsActive()) {
      color = &colorSleeping;
    }
    else if (body.IsSensor()) {
      color = &colorSensor;
    }
    else if (body.IsKinematic()) {
      color = &colorKinematic;
    }
    else if (body.IsDynamic()) {
      color = &colorDynamic;
    }
    else {
      color = &colorStatic;
    }

    DrawAABB(aabb, *color);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DrawConstraints
 * \{ */

void JoltDebugDraw::DrawConstraints(JoltPhysicsEnvironment *env)
{
  /* Constraint debug drawing requires JPH_DEBUG_RENDERER.
   * Without it, we skip constraint visualization. */
  (void)env;
}

/** \} */
