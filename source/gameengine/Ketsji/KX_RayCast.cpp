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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * SCA_MouseFocusSensor determines mouse in/out/over events.
 */

/** \file gameengine/Ketsji/KX_RayCast.cpp
 *  \ingroup ketsji
 */

#include "KX_RayCast.h"

#include "CM_Message.h"
#include "KX_Globals.h"

using namespace blender;

KX_RayCast::KX_RayCast(PHY_IPhysicsController *ignoreController, bool faceNormal, bool faceUV)
    : PHY_IRayCastFilterCallback(ignoreController, faceNormal, faceUV)
{
}

void KX_RayCast::reportHit(PHY_RayCastResult *result)
{
  m_hitFound = true;
  m_hitPoint = MT_Vector3(result->m_hitPoint);
  m_hitNormal = MT_Vector3(result->m_hitNormal);
  m_hitUVOK = result->m_hitUVOK;
  m_hitUV = MT_Vector2(result->m_hitUV);
  m_hitMesh = result->m_meshObject;
  m_hitPolygon = result->m_polygon;
}

bool KX_RayCast::RayTest(PHY_IPhysicsEnvironment *physics_environment,
                         const MT_Vector3 &_frompoint,
                         const MT_Vector3 &topoint,
                         KX_RayCast &callback)
{
  if (physics_environment == nullptr)
    return false; /* prevents crashing in some cases */

  // Loops over all physics objects between frompoint and topoint,
  // calling callback.RayHit for each one.
  //
  // callback.RayHit should return true to stop looking, or false to continue.
  //
  // returns true if an object was found, false if not.

  MT_Vector3 frompoint(_frompoint);
  const MT_Vector3 todir((topoint - frompoint).safe_normalized());
  MT_Vector3 prevpoint(_frompoint + todir * (-1.f));

  PHY_IPhysicsController *hit_controller;

  bool result = false;
  while ((hit_controller = physics_environment->RayTest(callback,
                                                        frompoint.x(),
                                                        frompoint.y(),
                                                        frompoint.z(),
                                                        topoint.x(),
                                                        topoint.y(),
                                                        topoint.z())) != nullptr) {
    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(
        hit_controller->GetNewClientInfo());

    if (!info) {
      CM_Error("no info!");
      BLI_assert(info && "Physics controller with no client object info");
      break;
    }

    // The biggest danger to endless loop, prevent this by checking that the
    // hit point always progresses along the ray direction..
    prevpoint -= callback.m_hitPoint;
    if (prevpoint.length2() < MT_EPSILON)
      break;

    if (callback.RayHit(info)) {
      // caller may decide to stop the loop and still cancel the hit
      result = callback.m_hitFound;
      break;
    }

    // Skip past the object and keep tracing.
    // Note that retrieving in a single shot multiple hit points would be possible
    // but it would require some change in Bullet.
    prevpoint = callback.m_hitPoint;
    /* We add 0.001 of fudge, so that if the margin && radius == 0.0, we don't endless loop. */
    MT_Scalar marg = 0.001f + hit_controller->GetMargin();
    marg *= 2.f;
    /* Calculate the other side of this object */
    MT_Scalar h = MT_abs(todir.dot(callback.m_hitNormal));
    if (h <= 0.01f)
      // the normal is almost orthogonal to the ray direction, cannot compute the other side
      break;
    marg /= h;
    frompoint = callback.m_hitPoint + marg * todir;
    // verify that we are not passed the to point
    if ((topoint - frompoint).dot(todir) < 0.f)
      break;
  }

  /* Draw debug line if physics visualization is enabled */
  if (physics_environment->GetDebugMode() > 0) {
    MT_Vector3 endpoint = callback.m_hitFound ? callback.m_hitPoint : topoint;
    MT_Vector4 color = callback.m_hitFound ?
        MT_Vector4(0.7f, 1.0f, 0.7f, 1.0f) :
        MT_Vector4(1.0f, 0.0f, 1.0f, 1.0f);
    KX_RasterizerDrawDebugLine(_frompoint, endpoint, color);
  }

  return result;
}
