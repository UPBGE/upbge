/*
   Bullet Continuous Collision Detection and Physics Library
   Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

   This software is provided 'as-is', without any express or implied warranty.
   In no event will the authors be held liable for any damages arising from the use of this
   software. Permission is granted to anyone to use this software for any purpose, including
   commercial applications, and to alter it and redistribute it freely, subject to the following
   restrictions:

   1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be misrepresented as
   being the original software.
   3. This notice may not be removed or altered from any source distribution.
 */

/** \file PHY_DynamicTypes.h
 *  \ingroup phys
 */

#pragma once

#include "MT_Vector3.h"

struct KX_ClientObjectInfo;
class PHY_IPhysicsController;

enum {
  PHY_FH_RESPONSE,
  PHY_SENSOR_RESPONSE,   // Touch Sensors
  PHY_CAMERA_RESPONSE,   // Visibility Culling
  PHY_OBJECT_RESPONSE,   // Object Dynamic Geometry Response
  PHY_STATIC_RESPONSE,   // Static Geometry Response
  PHY_BROADPH_RESPONSE,  // broadphase Response

  PHY_NUM_RESPONSE
};

class PHY_ICollData {
 public:
  PHY_ICollData()
  {
  }

  virtual ~PHY_ICollData()
  {
  }

  virtual unsigned int GetNumContacts() const = 0;
  virtual MT_Vector3 GetLocalPointA(unsigned int index, bool first) const = 0;
  virtual MT_Vector3 GetLocalPointB(unsigned int index, bool first) const = 0;
  virtual MT_Vector3 GetWorldPoint(unsigned int index, bool first) const = 0;
  virtual MT_Vector3 GetNormal(unsigned int index, bool first) const = 0;
  virtual float GetCombinedFriction(unsigned int index, bool first) const = 0;
  virtual float GetCombinedRollingFriction(unsigned int index, bool first) const = 0;
  virtual float GetCombinedRestitution(unsigned int index, bool first) const = 0;
  virtual float GetAppliedImpulse(unsigned int index, bool first) const = 0;
};

struct PHY_CollisionTestResult
{
  bool collide;
  bool isFirst;
  PHY_ICollData *collData;
};

using PHY_ResponseCallback = bool (*)(void *client_data, PHY_IPhysicsController *ctrl1, PHY_IPhysicsController *ctrl2,
                                      const PHY_ICollData *coll_data, bool first);
using PHY_CullingCallback =  void (*)(KX_ClientObjectInfo *info, void *param);

/// PHY_ConstraintType enumerates all supported Constraint Types
typedef enum PHY_ConstraintType {
  PHY_POINT2POINT_CONSTRAINT = 1,
  PHY_LINEHINGE_CONSTRAINT = 2,
  PHY_ANGULAR_CONSTRAINT = 3,  // hinge without ball socket
  PHY_CONE_TWIST_CONSTRAINT = 4,
  PHY_VEHICLE_CONSTRAINT = 11,       // complex 'constraint' that turns a rigidbody into a vehicle
  PHY_GENERIC_6DOF_CONSTRAINT = 12,  // can leave any of the 6 degree of freedom 'free' or 'locked'

} PHY_ConstraintType;

typedef enum PHY_ShapeType {
  PHY_SHAPE_NONE,
  PHY_SHAPE_BOX,
  PHY_SHAPE_SPHERE,
  PHY_SHAPE_CYLINDER,
  PHY_SHAPE_CONE,
  PHY_SHAPE_CAPSULE,
  PHY_SHAPE_MESH,
  PHY_SHAPE_POLYTOPE,
  PHY_SHAPE_COMPOUND,
  PHY_SHAPE_EMPTY,
  PHY_SHAPE_PROXY
} PHY_ShapeType;

typedef enum PHY_SolverType {
  PHY_SOLVER_NONE,
  PHY_SOLVER_SEQUENTIAL,
  PHY_SOLVER_NNCG,
} PHY_SolverType;
