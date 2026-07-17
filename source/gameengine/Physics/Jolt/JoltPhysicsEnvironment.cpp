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

/** \file JoltPhysicsEnvironment.cpp
 *  \ingroup physjolt
 */

#include "JoltPhysicsEnvironment.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>

#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include "JoltCharacter.h"
#include "JoltCollData.h"
#include "JoltConstraint.h"
#include "JoltDebugDraw.h"
#include "JoltDefaultMotionState.h"
#include "JoltGraphicController.h"
#include "JoltSoftBody.h"
#include "JoltVehicle.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltShapeBuilder.h"

#include "BKE_context.hh"
#include "BKE_effect.h"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_modifier_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "MEM_guardedalloc.h"

#include "BL_SceneConverter.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "PHY_IMotionState.h"
#include "RAS_MeshObject.h"
#include "SG_Node.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <iterator>
#include <unordered_set>

static int g_joltConstraintUid = 1;

#define CCD_CONSTRAINT_DISABLE_LINKED_COLLISION 0x80
#include <cstdio>
#include <thread>

/* -------------------------------------------------------------------- */
/** \name Bullet-style combine functions for friction and restitution
 * \{
 *
 * Bullet Physics uses multiply mode for both friction and restitution:
 * - Combined friction = friction1 * friction2
 * - Combined restitution = restitution1 * restitution2
 *
 * This means if either object has 0 restitution, the collision is inelastic.
 * Jolt's default is max() for restitution and geometric mean for friction.
 */

static float BulletCombineFriction(const JPH::Body &inBody1,
                                    const JPH::SubShapeID &inSubShapeID1,
                                    const JPH::Body &inBody2,
                                    const JPH::SubShapeID &inSubShapeID2)
{
  (void)inSubShapeID1;
  (void)inSubShapeID2;
  return inBody1.GetFriction() * inBody2.GetFriction();
}

static float BulletCombineRestitution(const JPH::Body &inBody1,
                                       const JPH::SubShapeID &inSubShapeID1,
                                       const JPH::Body &inBody2,
                                       const JPH::SubShapeID &inSubShapeID2)
{
  (void)inSubShapeID1;
  (void)inSubShapeID2;
  return inBody1.GetRestitution() * inBody2.GetRestitution();
}

static JPH::Vec3 JoltVehicleAxisVector(int axis)
{
  switch (axis) {
    case 0:
      return JPH::Vec3::sAxisX();
    case 1:
      return -JPH::Vec3::sAxisZ();
    case 2:
      return JPH::Vec3::sAxisY();
    case 3:
      return -JPH::Vec3::sAxisX();
    case 4:
      return JPH::Vec3::sAxisZ();
    case 5:
      return -JPH::Vec3::sAxisY();
    default:
      return JPH::Vec3::sAxisX();
  }
}

static JPH::RefConst<JPH::Shape> JoltApplyVehicleCenterOfMassOffset(
    JPH::RefConst<JPH::Shape> shape, const blender::Object *blenderobject)
{
  if (!shape || !blenderobject || !(blenderobject->gameflag2 & blender::OB_HAS_VEHICLE) ||
      !blenderobject->vehicle) {
    return shape;
  }

  const blender::GameVehicleSettings *vehicle_settings = blenderobject->vehicle;
  if (vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_CHASSIS &&
      vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_MOTORCYCLE_CHASSIS) {
    return shape;
  }

  const float normalized_offset = std::clamp(
      vehicle_settings->center_of_mass_offset, -1.0f, 1.0f);
  if (std::abs(normalized_offset) <= 1.0e-6f) {
    return shape;
  }

  const JPH::AABox bounds = shape->GetLocalBounds();
  if (!bounds.IsValid()) {
    return shape;
  }

  const JPH::Vec3 up = JoltVehicleAxisVector(vehicle_settings->up_axis);
  const JPH::Vec3 size = bounds.mMax - bounds.mMin;
  const float half_height = 0.5f * (std::abs(up.GetX()) * size.GetX() +
                                    std::abs(up.GetY()) * size.GetY() +
                                    std::abs(up.GetZ()) * size.GetZ());
  if (half_height <= 1.0e-6f) {
    return shape;
  }

  return new JPH::OffsetCenterOfMassShape(shape.GetPtr(),
                                          up * (normalized_offset * half_height));
}

static const JPH::StaticCompoundShape *JoltAsStaticCompoundShape(const JPH::Shape *shape)
{
  if (!shape) {
    return nullptr;
  }

  if (shape->GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass) {
    shape = static_cast<const JPH::OffsetCenterOfMassShape *>(shape)
                ->GetInnerShape();
  }

  if (shape && shape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
    return static_cast<const JPH::StaticCompoundShape *>(shape);
  }

  return nullptr;
}

static JPH::Vec3 JoltCompoundSubShapeLocalPosition(
    const JPH::StaticCompoundShape &compound, const JPH::CompoundShape::SubShape &subShape)
{
  return subShape.GetPositionCOM() + compound.GetCenterOfMass() -
         subShape.GetRotation() * subShape.mShape->GetCenterOfMass();
}

static void JoltAddExistingShapeToCompoundSettings(JPH::StaticCompoundShapeSettings &settings,
                                                   const JPH::Shape *shape)
{
  const JPH::StaticCompoundShape *compound = JoltAsStaticCompoundShape(shape);
  if (!compound) {
    if (shape) {
      settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape);
    }
    return;
  }

  for (JPH::uint i = 0; i < compound->GetNumSubShapes(); ++i) {
    const JPH::CompoundShape::SubShape &subShape = compound->GetSubShape(i);
    settings.AddShape(JoltCompoundSubShapeLocalPosition(*compound, subShape),
                      subShape.GetRotation(),
                      subShape.mShape,
                      subShape.mUserData);
  }
}

static bool JoltIsSoftBodyModifier(const blender::ModifierData *md)
{
  return md && md->type == blender::eModifierType_SimpleDeformBGE &&
         STRPREFIX(md->name, "joltSbModifier");
}

static KX_ClientObjectInfo *JoltGetClientInfo(const JPH::Body &body)
{
  void *userData = reinterpret_cast<void *>(body.GetUserData());
  return static_cast<KX_ClientObjectInfo *>(userData);
}

static bool JoltObjectsSharePhysicsHierarchy(const KX_GameObject *object1,
                                             const KX_GameObject *object2)
{
  const SG_Node *node1 = object1 ? object1->GetSGNode() : nullptr;
  const SG_Node *node2 = object2 ? object2->GetSGNode() : nullptr;
  return node1 && node2 && node1->GetRootSGParent() == node2->GetRootSGParent();
}

static JPH::RMat44 JoltLogicQueryTransform(const JoltPhysicsController &controller,
                                           const JPH::Shape &shape,
                                           JPH::RMat44Arg fallback)
{
  const PHY_IMotionState *motionState = controller.GetMotionState();
  if (!motionState) {
    return fallback;
  }

  const JPH::Quat rotation =
      JoltMath::ToJolt(motionState->GetWorldOrientation().getRotation()).Normalized();
  const JPH::RVec3 origin = JoltMath::ToJoltR(motionState->GetWorldPosition());
  const JPH::RVec3 centerOfMass = origin + rotation * shape.GetCenterOfMass();
  return JPH::RMat44::sRotationTranslation(rotation, centerOfMass);
}

static PHY_IPhysicsController *JoltGetPhysicsController(KX_ClientObjectInfo *clientInfo)
{
  if (!clientInfo || !clientInfo->m_gameobject) {
    return nullptr;
  }
  return static_cast<PHY_IPhysicsController *>(clientInfo->m_gameobject->GetPhysicsController());
}

static bool JoltIsObjectSensor(const KX_ClientObjectInfo *clientInfo)
{
  return clientInfo && (clientInfo->m_type == KX_ClientObjectInfo::OBSENSOR ||
                        clientInfo->m_type == KX_ClientObjectInfo::OBACTORSENSOR);
}

static bool JoltIsObjectSensor(JoltPhysicsController *controller)
{
  return controller && JoltIsObjectSensor(
                           static_cast<KX_ClientObjectInfo *>(controller->GetNewClientInfo()));
}

static MT_Vector3 JoltLogicContactNormal(const JPH::Vec3 &penetrationAxis)
{
  return penetrationAxis.LengthSq() > FLT_EPSILON ?
             JoltMath::ToMT(penetrationAxis.Normalized()) :
             MT_Vector3(0.0f, 0.0f, 0.0f);
}

/** Select Jolt broadphase trees for C++ Logic Nodes Sensor queries. Dynamic is always
 * searched (including sleeping bodies); static and Sensor trees are entered only when
 * the Sensor object's explicit opt-in is enabled. */
class JoltLogicObjectSensorBroadPhaseLayerFilter final : public JPH::BroadPhaseLayerFilter {
 public:
  explicit JoltLogicObjectSensorBroadPhaseLayerFilter(const bool includeStatic)
      : m_includeStatic(includeStatic)
  {
  }

  bool ShouldCollide(const JPH::BroadPhaseLayer layer) const override
  {
    const JPH::BroadPhaseLayer::Type category = (JPH::BroadPhaseLayer::Type)layer;
    return category == JOLT_BP_DYNAMIC ||
           (m_includeStatic &&
            (category == JOLT_BP_STATIC || category == JOLT_BP_SENSOR));
  }

 private:
  bool m_includeStatic;
};

/** Apply the non-layer rules used by UPBGE Sensor physics objects. Layer filtering is
 * handled by the query's object-layer filter before this callback is reached. */
class JoltLogicObjectSensorBodyFilter final : public JPH::BodyFilter {
 public:
  JoltLogicObjectSensorBodyFilter(
      const JPH::BodyID sensorBodyID,
      const JPH::CollisionGroup &sensorCollisionGroup,
      const KX_ClientObjectInfo &sensorInfo,
      const bool includeStatic,
      const std::set<JoltPhysicsController *> &controllers)
      : m_sensorBodyID(sensorBodyID),
        m_sensorCollisionGroup(sensorCollisionGroup),
        m_sensorInfo(sensorInfo),
        m_includeStatic(includeStatic),
        m_controllers(controllers)
  {
  }

  bool ShouldCollide(const JPH::BodyID &bodyID) const override
  {
    return bodyID != m_sensorBodyID;
  }

  bool ShouldCollideLocked(const JPH::Body &body) const override
  {
    /* Keep this body-level check as a correctness backstop for direct target queries and
     * for bodies whose broadphase category is being changed by deferred operations. */
    if (!m_includeStatic && body.GetMotionType() == JPH::EMotionType::Static) {
      return false;
    }

    KX_ClientObjectInfo *otherInfo = JoltGetClientInfo(body);
    /* Jolt uses Body::IsSensor() for both UPBGE Sensor objects and ordinary Ghost bodies.
     * Only the former are Sensor-vs-Sensor pairs; Ghost objects remain detectable. */
    if (!otherInfo || otherInfo->isSensor() || !otherInfo->m_gameobject) {
      return false;
    }

    KX_GameObject *sensorObject = m_sensorInfo.m_gameobject;
    KX_GameObject *otherObject = otherInfo->m_gameobject;
    if (!sensorObject || JoltObjectsSharePhysicsHierarchy(sensorObject, otherObject) ||
        (m_sensorInfo.m_type == KX_ClientObjectInfo::OBACTORSENSOR &&
         otherInfo->m_type != KX_ClientObjectInfo::ACTOR))
    {
      return false;
    }

    JoltPhysicsController *otherController = static_cast<JoltPhysicsController *>(
        JoltGetPhysicsController(otherInfo));
    return otherController && m_controllers.find(otherController) != m_controllers.end() &&
           otherController->GetBodyID() == body.GetID() &&
           !otherController->IsPhysicsSuspended() &&
           m_sensorCollisionGroup.CanCollide(body.GetCollisionGroup());
  }

 private:
  JPH::BodyID m_sensorBodyID;
  JPH::CollisionGroup m_sensorCollisionGroup;
  const KX_ClientObjectInfo &m_sensorInfo;
  bool m_includeStatic;
  const std::set<JoltPhysicsController *> &m_controllers;
};

class JoltRayCastBodyFilter : public JPH::BodyFilter {
 public:
  explicit JoltRayCastBodyFilter(PHY_IRayCastFilterCallback &filterCallback,
                                 const bool includeSensors = false)
      : m_filterCallback(filterCallback), m_includeSensors(includeSensors)
  {
  }

  virtual bool ShouldCollideLocked(const JPH::Body &body) const override
  {
    if (!m_includeSensors && body.IsSensor()) {
      return false;
    }

    KX_ClientObjectInfo *clientInfo = JoltGetClientInfo(body);
    if (!clientInfo || (!m_includeSensors && clientInfo->isSensor())) {
      return false;
    }

    PHY_IPhysicsController *ctrl = JoltGetPhysicsController(clientInfo);
    if (!ctrl || ctrl == m_filterCallback.m_ignoreController) {
      return false;
    }

    return m_filterCallback.needBroadphaseRayCast(ctrl);
  }

 private:
  PHY_IRayCastFilterCallback &m_filterCallback;
  bool m_includeSensors;
};

class JoltBoundedRayCastCollector final : public JPH::CastRayCollector {
 public:
  explicit JoltBoundedRayCastCollector(const uint32_t maxHits) : m_maxHits(maxHits)
  {
    m_hits.reserve(maxHits);
  }

  void OnBody(const JPH::Body & /*body*/) override
  {
    m_previousEarlyOutFraction = GetEarlyOutFraction();
    m_currentHadHit = false;
  }

  void AddHit(const JPH::RayCastResult &result) override
  {
    if (!m_currentHadHit || result.mFraction < m_currentHit.mFraction) {
      m_currentHit = result;
      m_currentHadHit = true;
      UpdateEarlyOutFraction(result.mFraction);
    }
  }

  void OnBodyEnd() override
  {
    ResetEarlyOutFraction(m_previousEarlyOutFraction);
    if (!m_currentHadHit || m_maxHits == 0) {
      return;
    }

    if (m_hits.size() < m_maxHits) {
      m_hits.push_back(m_currentHit);
      std::push_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
    }
    else if (HitIsCloser(m_currentHit, m_hits.front())) {
      std::pop_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
      m_hits.back() = m_currentHit;
      std::push_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
    }

    if (m_hits.size() == m_maxHits) {
      UpdateEarlyOutFraction(m_hits.front().mFraction);
    }
  }

  void Sort()
  {
    std::sort(m_hits.begin(), m_hits.end(), HitIsCloser);
  }

  JPH::Array<JPH::RayCastResult> m_hits;

 private:
  static bool HitIsCloser(const JPH::RayCastResult &a, const JPH::RayCastResult &b)
  {
    if (a.mFraction != b.mFraction) {
      return a.mFraction < b.mFraction;
    }
    return a.mBodyID.GetIndexAndSequenceNumber() < b.mBodyID.GetIndexAndSequenceNumber();
  }

  uint32_t m_maxHits;
  float m_previousEarlyOutFraction = 1.0f;
  JPH::RayCastResult m_currentHit;
  bool m_currentHadHit = false;
};

class JoltShapeCastBodyFilter : public JPH::BodyFilter {
 public:
  JoltShapeCastBodyFilter(PHY_IShapeCastFilterCallback &filterCallback,
                          const bool includeSensors)
      : m_filterCallback(filterCallback), m_includeSensors(includeSensors)
  {
  }

  bool ShouldCollideLocked(const JPH::Body &body) const override
  {
    KX_ClientObjectInfo *clientInfo = JoltGetClientInfo(body);
    if (!clientInfo || !clientInfo->m_gameobject) {
      return false;
    }
    if (!m_includeSensors && (body.IsSensor() || clientInfo->isSensor())) {
      return false;
    }

    PHY_IPhysicsController *controller = JoltGetPhysicsController(clientInfo);
    if (!controller || controller == m_filterCallback.m_ignoreController ||
        controller == m_filterCallback.m_extraIgnoreController)
    {
      return false;
    }
    return m_filterCallback.needBroadphaseShapeCast(controller);
  }

 private:
  PHY_IShapeCastFilterCallback &m_filterCallback;
  bool m_includeSensors;
};

class JoltBoundedShapeCastCollector final : public JPH::CastShapeCollector {
 public:
  explicit JoltBoundedShapeCastCollector(const uint32_t maxHits) : m_maxHits(maxHits)
  {
    m_hits.reserve(maxHits);
  }

  void OnBody(const JPH::Body & /*body*/) override
  {
    m_previousEarlyOutFraction = GetEarlyOutFraction();
    m_currentHadHit = false;
  }

  void AddHit(const JPH::ShapeCastResult &result) override
  {
    if (!m_currentHadHit ||
        result.GetEarlyOutFraction() < m_currentHit.GetEarlyOutFraction())
    {
      m_currentHit = result;
      m_currentHadHit = true;
      UpdateEarlyOutFraction(result.GetEarlyOutFraction());
    }
  }

  void OnBodyEnd() override
  {
    ResetEarlyOutFraction(m_previousEarlyOutFraction);
    if (!m_currentHadHit || m_maxHits == 0) {
      return;
    }

    if (m_hits.size() < m_maxHits) {
      m_hits.push_back(m_currentHit);
      std::push_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
    }
    else if (HitIsCloser(m_currentHit, m_hits.front())) {
      std::pop_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
      m_hits.back() = m_currentHit;
      std::push_heap(m_hits.begin(), m_hits.end(), HitIsCloser);
    }

    if (m_hits.size() == m_maxHits) {
      UpdateEarlyOutFraction(m_hits.front().GetEarlyOutFraction());
    }
  }

  void Sort()
  {
    std::sort(m_hits.begin(),
              m_hits.end(),
              HitIsCloser);
  }

  JPH::Array<JPH::ShapeCastResult> m_hits;

 private:
  static bool HitIsCloser(const JPH::ShapeCastResult &a, const JPH::ShapeCastResult &b)
  {
    const float aFraction = a.GetEarlyOutFraction();
    const float bFraction = b.GetEarlyOutFraction();
    if (aFraction != bFraction) {
      return aFraction < bFraction;
    }
    return a.mBodyID2.GetIndexAndSequenceNumber() < b.mBodyID2.GetIndexAndSequenceNumber();
  }

  uint32_t m_maxHits;
  float m_previousEarlyOutFraction = 1.0f;
  JPH::ShapeCastResult m_currentHit;
  bool m_currentHadHit = false;
};

static void JoltPopulateMeshQueryDetails(const JPH::Body &body,
                                         JoltPhysicsController &controller,
                                         const JPH::SubShapeID &subShapeID,
                                         const JPH::RVec3 &hitPoint,
                                         const JPH::Vec3 &supportDirection,
                                         const unsigned char detailFlags,
                                         int32_t &r_polygonIndex,
                                         bool &r_hasUV,
                                         MT_Vector2 &r_hitUV)
{
  if ((detailFlags & (PHY_RAY_QUERY_DETAIL_FACE_INDEX | PHY_RAY_QUERY_DETAIL_UV)) == 0) {
    return;
  }

  const JoltShapeQueryDataPtr &shapeQueryData = controller.GetShapeQueryData();
  JPH::SubShapeID triangleSubShape;
  const JPH::Shape *leafShape = shapeQueryData ?
                                          body.GetShape()->GetLeafShape(subShapeID,
                                                                        triangleSubShape) :
                                          nullptr;
  if (!leafShape || leafShape->GetType() != JPH::EShapeType::Mesh) {
    return;
  }

  const JoltMeshQueryData *meshData = shapeQueryData->FindMeshData(leafShape);
  if (!meshData) {
    return;
  }
  const JPH::uint32 triangleIndex =
      static_cast<const JPH::MeshShape *>(leafShape)->GetTriangleUserData(triangleSubShape);
  if (triangleIndex < meshData->polygonIndices.size()) {
    r_polygonIndex = meshData->polygonIndices[triangleIndex];
  }
  if ((detailFlags & PHY_RAY_QUERY_DETAIL_UV) == 0 ||
      triangleIndex >= meshData->triangleUVs.size())
  {
    return;
  }

  JPH::TransformedShape transformedLeaf = body.GetTransformedShape();
  JPH::SubShapeID remainingSubShape = subShapeID;
  for (int depth = 0;
       depth < 64 && transformedLeaf.mShape.GetPtr() != leafShape;
       ++depth)
  {
    JPH::SubShapeID nextSubShape;
    transformedLeaf = transformedLeaf.GetSubShapeTransformedShape(remainingSubShape,
                                                                   nextSubShape);
    remainingSubShape = nextSubShape;
    if (!transformedLeaf.mShape) {
      return;
    }
  }
  if (transformedLeaf.mShape.GetPtr() != leafShape) {
    return;
  }

  JPH::Shape::SupportingFace vertices;
  transformedLeaf.GetSupportingFace(
      triangleSubShape, supportDirection, hitPoint, vertices);
  if (vertices.size() != 3) {
    return;
  }

  const JPH::Vec3 edge0 = vertices[1] - vertices[0];
  const JPH::Vec3 edge1 = vertices[2] - vertices[0];
  const JPH::Vec3 point = -vertices[0];
  const float dot00 = edge0.Dot(edge0);
  const float dot01 = edge0.Dot(edge1);
  const float dot11 = edge1.Dot(edge1);
  const float dot20 = point.Dot(edge0);
  const float dot21 = point.Dot(edge1);
  const float denominator = dot00 * dot11 - dot01 * dot01;
  const float denominatorScale = std::max(dot00 * dot11, 1.0f);
  if (!std::isfinite(denominator) ||
      std::abs(denominator) <= 1.0e-12f * denominatorScale)
  {
    return;
  }

  float weight1 = (dot11 * dot20 - dot01 * dot21) / denominator;
  float weight2 = (dot00 * dot21 - dot01 * dot20) / denominator;
  float weight0 = 1.0f - weight1 - weight2;
  constexpr float barycentricTolerance = 1.0e-3f;
  if (!std::isfinite(weight0) || !std::isfinite(weight1) || !std::isfinite(weight2) ||
      weight0 < -barycentricTolerance || weight1 < -barycentricTolerance ||
      weight2 < -barycentricTolerance || weight0 > 1.0f + barycentricTolerance ||
      weight1 > 1.0f + barycentricTolerance || weight2 > 1.0f + barycentricTolerance)
  {
    return;
  }

  weight0 = std::clamp(weight0, 0.0f, 1.0f);
  weight1 = std::clamp(weight1, 0.0f, 1.0f);
  weight2 = std::clamp(weight2, 0.0f, 1.0f);
  const float weightSum = weight0 + weight1 + weight2;
  if (weightSum <= MT_EPSILON) {
    return;
  }

  const JoltTriangleUV &triangleUV = meshData->triangleUVs[triangleIndex];
  const JPH::Vec3 leafScale = transformedLeaf.GetShapeScale();
  const bool insideOut = leafScale.GetX() * leafScale.GetY() * leafScale.GetZ() < 0.0f;
  const int corner1 = insideOut ? 2 : 1;
  const int corner2 = insideOut ? 1 : 2;
  const MT_Vector2 uv = (triangleUV.corners[0] * weight0 +
                         triangleUV.corners[corner1] * weight1 +
                         triangleUV.corners[corner2] * weight2) /
                        weightSum;
  if (std::isfinite(uv[0]) && std::isfinite(uv[1])) {
    r_hitUV = uv;
    r_hasUV = true;
  }
}

class JoltFhBodyFilter : public JPH::BodyFilter {
 public:
  explicit JoltFhBodyFilter(const JPH::BodyID &ignoredBodyID) : m_ignoredBodyID(ignoredBodyID)
  {
  }

  virtual bool ShouldCollideLocked(const JPH::Body &body) const override
  {
    if (body.GetID() == m_ignoredBodyID || body.IsSensor()) {
      return false;
    }

    KX_ClientObjectInfo *clientInfo = JoltGetClientInfo(body);
    return clientInfo && !clientInfo->isSensor() && JoltGetPhysicsController(clientInfo);
  }

 private:
  JPH::BodyID m_ignoredBodyID;
};

class JoltBuoyancyBroadPhaseLayerFilter : public JPH::BroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::BroadPhaseLayer layer) const override
  {
    return (JPH::BroadPhaseLayer::Type)layer == JOLT_BP_DYNAMIC;
  }
};

class JoltBuoyancyObjectLayerFilter : public JPH::ObjectLayerFilter {
 public:
  JoltBuoyancyObjectLayerFilter(JPH::ObjectLayer volumeLayer,
                                const JoltObjectLayerPairFilter &layerPairFilter)
      : m_volumeLayer(volumeLayer), m_layerPairFilter(layerPairFilter)
  {
  }

  bool ShouldCollide(JPH::ObjectLayer layer) const override
  {
    return JoltGetCategory(layer) == JOLT_BP_DYNAMIC &&
           m_layerPairFilter.ShouldCollide(m_volumeLayer, layer);
  }

 private:
  JPH::ObjectLayer m_volumeLayer;
  const JoltObjectLayerPairFilter &m_layerPairFilter;
};

static bool JoltShapeSupportsBuoyancyImpulse(const JPH::Shape *shape)
{
  if (!shape) {
    return false;
  }

  switch (shape->GetType()) {
    case JPH::EShapeType::Convex:
    case JPH::EShapeType::Empty:
      return true;
    case JPH::EShapeType::Decorated:
      return JoltShapeSupportsBuoyancyImpulse(
          static_cast<const JPH::DecoratedShape *>(shape)->GetInnerShape());
    case JPH::EShapeType::Compound: {
      const JPH::CompoundShape *compound = static_cast<const JPH::CompoundShape *>(shape);
      for (const JPH::CompoundShape::SubShape &subShape : compound->GetSubShapes()) {
        if (!JoltShapeSupportsBuoyancyImpulse(subShape.mShape.GetPtr())) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

class JoltBuoyancyBodyFilter : public JPH::BodyFilter {
 public:
  explicit JoltBuoyancyBodyFilter(const JPH::BodyID &volumeBodyID)
      : m_volumeBodyID(volumeBodyID)
  {
  }

  bool ShouldCollide(const JPH::BodyID &bodyID) const override
  {
    return bodyID != m_volumeBodyID;
  }

  bool ShouldCollideLocked(const JPH::Body &body) const override
  {
    if (body.GetID() == m_volumeBodyID || !body.IsRigidBody() ||
        body.GetMotionType() != JPH::EMotionType::Dynamic) {
      return false;
    }

    KX_ClientObjectInfo *clientInfo = JoltGetClientInfo(body);
    /* Body::IsSensor() also represents the Ghost option. Dynamic Ghost rigid bodies are
     * valid fluid targets; only actual UPBGE Sensor physics objects are excluded. */
    if (!clientInfo || clientInfo->isSensor()) {
      return false;
    }
    JoltPhysicsController *ctrl =
        static_cast<JoltPhysicsController *>(JoltGetPhysicsController(clientInfo));
    return ctrl && ctrl->GetBodyID() == body.GetID() && ctrl->IsDynamic() &&
           !ctrl->IsPhysicsSuspended() &&
           !ctrl->IsDynamicsSuspended() && !ctrl->GetSoftBody() &&
           JoltShapeSupportsBuoyancyImpulse(body.GetShape());
  }

 private:
  JPH::BodyID m_volumeBodyID;
};

static constexpr float JOLT_DEFAULT_MAX_LINEAR_VELOCITY = 500.0f;
static constexpr float JOLT_DEFAULT_MAX_ANGULAR_VELOCITY = 0.25f * JPH::JPH_PI * 60.0f;

static float JoltLinearVelocityMaxOrDefault(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_LINEAR_VELOCITY;
}

static float JoltAngularVelocityMaxOrDefault(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_ANGULAR_VELOCITY;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jolt global initialization (once per process)
 * \{ */

static bool s_joltInitialized = false;

static void JoltTraceImpl(const char *inFMT, ...)
{
  va_list list;
  va_start(list, inFMT);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), inFMT, list);
  va_end(list);
  printf("Jolt: %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailedImpl(const char *inExpression,
                                 const char *inMessage,
                                 const char *inFile,
                                 JPH::uint inLine)
{
  printf("Jolt Assert: %s:%u: (%s) %s\n",
         inFile,
         inLine,
         inExpression,
         inMessage ? inMessage : "");
  return true;  /* trigger breakpoint */
}
#endif

static void EnsureJoltInitialized()
{
  if (!s_joltInitialized) {
    JPH::RegisterDefaultAllocator();

    JPH::Trace = JoltTraceImpl;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailedImpl;)

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    s_joltInitialized = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltContactListener
 * \{ */

JPH::ValidateResult JoltContactListener::OnContactValidate(
    const JPH::Body &inBody1,
    const JPH::Body &inBody2,
    JPH::RVec3Arg inBaseOffset,
    const JPH::CollideShapeResult &inCollisionResult)
{
  /* NOTE: This is called from physics threads during PhysicsSystem::Update().
   * Only reading from bodies is safe here.
   *
   * Collision collection filtering is handled at stage 2 (JoltObjectLayerPairFilter)
   * so no additional filtering is needed here. */

  return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

void JoltContactListener::OnContactAdded(const JPH::Body &inBody1,
                                          const JPH::Body &inBody2,
                                          const JPH::ContactManifold &inManifold,
                                          JPH::ContactSettings &ioSettings)
{
  StoreContact(inBody1, inBody2, inManifold, ioSettings, true);
}

void JoltContactListener::OnContactPersisted(const JPH::Body &inBody1,
                                              const JPH::Body &inBody2,
                                              const JPH::ContactManifold &inManifold,
                                              JPH::ContactSettings &ioSettings)
{
  StoreContact(inBody1, inBody2, inManifold, ioSettings, false);
}

void JoltContactListener::OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair)
{
  StoreContactRemoval(inSubShapePair);
}

void JoltContactListener::StoreContact(const JPH::Body &inBody1,
                                        const JPH::Body &inBody2,
                                        const JPH::ContactManifold &inManifold,
                                        const JPH::ContactSettings &ioSettings,
                                        bool isNew)
{
  /* Called from physics threads — must be thread-safe.
   * Skip contact capture entirely when no game-logic path needs it. */
  if (!m_env) {
    return;
  }
  const bool collectForCallbacks = m_env->m_collectContactsForCallbacks.load(
      std::memory_order_relaxed);
  const bool collectForLogicNodes = m_env->m_collectContactsForLogicNodes.load(
      std::memory_order_relaxed);
  const bool collectDetailsForLogicNodes =
      collectForLogicNodes &&
      m_env->m_collectContactDetailsForLogicNodes.load(std::memory_order_relaxed);
  if (!collectForCallbacks && !collectForLogicNodes) {
    return;
  }

  KX_ClientObjectInfo *info1 = JoltGetClientInfo(inBody1);
  KX_ClientObjectInfo *info2 = JoltGetClientInfo(inBody2);
  if (!info1 || !info2 || !info1->m_gameobject || !info2->m_gameobject) {
    return;
  }

  JoltPhysicsController *ctrl1 = static_cast<JoltPhysicsController *>(
      JoltGetPhysicsController(info1));
  JoltPhysicsController *ctrl2 = static_cast<JoltPhysicsController *>(
      JoltGetPhysicsController(info2));
  if (!ctrl1 || !ctrl2) {
    return;
  }

  JoltContactPair pair;
  pair.subShapePair = JPH::SubShapeIDPair(
      inBody1.GetID(), inManifold.mSubShapeID1, inBody2.GetID(), inManifold.mSubShapeID2);
  pair.bodyID1 = inBody1.GetID();
  pair.bodyID2 = inBody2.GetID();
  pair.ctrl1 = ctrl1;
  pair.ctrl2 = ctrl2;
  pair.object1 = info1->m_gameobject;
  pair.object2 = info2->m_gameobject;
  pair.contactNormal = inManifold.mWorldSpaceNormal;
  pair.penetrationDepth = inManifold.mPenetrationDepth;
  pair.combinedFriction = ioSettings.mCombinedFriction;
  pair.combinedRestitution = ioSettings.mCombinedRestitution;
  pair.appliedImpulse = 0.0f;
  pair.isNew = isNew;

  if (collectForCallbacks) {
    JPH::CollisionEstimationResult collisionEstimate;
    const JPH::PhysicsSettings &physicsSettings = m_env->m_physicsSystem->GetPhysicsSettings();
    JPH::EstimateCollisionResponse(inBody1,
                                   inBody2,
                                   inManifold,
                                   collisionEstimate,
                                   ioSettings.mCombinedFriction,
                                   ioSettings.mCombinedRestitution,
                                   physicsSettings.mMinVelocityForRestitution,
                                   std::max<JPH::uint>(1, physicsSettings.mNumVelocitySteps));
    if (collisionEstimate.mContactImpulse.size() > 0) {
      pair.appliedImpulse = collisionEstimate.mContactImpulse[0];
    }
  }

  pair.contactCount = uint16_t(std::min<size_t>(inManifold.mRelativeContactPointsOn1.size(),
                                                UINT16_MAX));
  if (collectDetailsForLogicNodes) {
    pair.storedContactCount = uint8_t(std::min<size_t>(
        inManifold.mRelativeContactPointsOn1.size(), JOLT_MAX_CACHED_CONTACT_POINTS));
    for (uint8_t i = 0; i < pair.storedContactCount; i++) {
      pair.contactPositions[i] = inManifold.mBaseOffset + inManifold.mRelativeContactPointsOn1[i];
    }
  }

  if (collectDetailsForLogicNodes && pair.storedContactCount > 0) {
    pair.contactPosition = pair.contactPositions[0];
  }
  else if (!inManifold.mRelativeContactPointsOn1.empty()) {
    pair.contactPosition = inManifold.mBaseOffset + inManifold.mRelativeContactPointsOn1[0];
  }
  else {
    pair.contactCount = 1;
    pair.contactPosition = inManifold.mBaseOffset;
  }

  const size_t shard = (size_t(pair.bodyID1.GetIndexAndSequenceNumber()) ^
                        (size_t(pair.bodyID2.GetIndexAndSequenceNumber()) * 0x9E3779B1u)) &
                       (JOLT_CONTACT_BUFFER_SHARDS - 1);
  JoltContactBuffer &buffer = m_contactBuffers[shard];
  std::lock_guard<std::mutex> lock(buffer.mutex);
  JPH_ASSERT(buffer.contacts.size() <= UINT32_MAX);
  const uint32_t contactIndex = uint32_t(buffer.contacts.size());
  buffer.contacts.push_back(pair);
  if (collectForLogicNodes) {
    buffer.events.push_back({contactIndex, JoltContactEventType::Contact});
  }
}

void JoltContactListener::StoreContactRemoval(const JPH::SubShapeIDPair &inSubShapePair)
{
  /* Called from physics threads. Do not touch bodies here: Jolt explicitly
   * disallows body access from OnContactRemoved because a body may already be
   * removed or being modified. */
  if (!m_env ||
      !m_env->m_collectContactsForLogicNodes.load(std::memory_order_relaxed))
  {
    return;
  }

  JoltRemovedContactPair removal;
  removal.subShapePair = inSubShapePair;
  removal.bodyID1 = inSubShapePair.GetBody1ID();
  removal.bodyID2 = inSubShapePair.GetBody2ID();

  const size_t shard = (size_t(removal.bodyID1.GetIndexAndSequenceNumber()) ^
                        (size_t(removal.bodyID2.GetIndexAndSequenceNumber()) * 0x9E3779B1u)) &
                       (JOLT_CONTACT_BUFFER_SHARDS - 1);
  JoltContactBuffer &buffer = m_contactBuffers[shard];
  std::lock_guard<std::mutex> lock(buffer.mutex);
  JPH_ASSERT(buffer.removals.size() <= UINT32_MAX);
  const uint32_t removalIndex = uint32_t(buffer.removals.size());
  buffer.removals.push_back(removal);
  buffer.events.push_back({removalIndex, JoltContactEventType::Removal});
}

void JoltContactListener::SwapContacts(std::vector<JoltContactPair> &outContacts)
{
  std::vector<JoltRemovedContactPair> ignoredRemovals;
  std::vector<JoltContactEvent> ignoredEvents;
  SwapContactEvents(outContacts, ignoredRemovals, ignoredEvents);
}

void JoltContactListener::SwapContactEvents(std::vector<JoltContactPair> &outContacts,
                                            std::vector<JoltRemovedContactPair> &outRemovals,
                                            std::vector<JoltContactEvent> &outEvents)
{
  outContacts.clear();
  outRemovals.clear();
  outEvents.clear();
  size_t contactCount = 0;
  size_t removalCount = 0;
  size_t eventCount = 0;
  for (JoltContactBuffer &buffer : m_contactBuffers) {
    std::lock_guard<std::mutex> lock(buffer.mutex);
    contactCount += buffer.contacts.size();
    removalCount += buffer.removals.size();
    eventCount += buffer.events.size();
  }
  JPH_ASSERT(contactCount <= UINT32_MAX);
  JPH_ASSERT(removalCount <= UINT32_MAX);
  outContacts.reserve(contactCount);
  outRemovals.reserve(removalCount);
  outEvents.reserve(eventCount);

  for (JoltContactBuffer &buffer : m_contactBuffers) {
    std::lock_guard<std::mutex> lock(buffer.mutex);
    JPH_ASSERT(outContacts.size() <= UINT32_MAX);
    JPH_ASSERT(outRemovals.size() <= UINT32_MAX);
    const uint32_t contactBase = uint32_t(outContacts.size());
    const uint32_t removalBase = uint32_t(outRemovals.size());
    if (!buffer.contacts.empty()) {
      outContacts.insert(outContacts.end(),
                         std::make_move_iterator(buffer.contacts.begin()),
                         std::make_move_iterator(buffer.contacts.end()));
      buffer.contacts.clear();
    }
    if (!buffer.removals.empty()) {
      outRemovals.insert(outRemovals.end(),
                         std::make_move_iterator(buffer.removals.begin()),
                         std::make_move_iterator(buffer.removals.end()));
      buffer.removals.clear();
    }
    for (JoltContactEvent event : buffer.events) {
      event.index += event.type == JoltContactEventType::Contact ? contactBase : removalBase;
      outEvents.push_back(event);
    }
    buffer.events.clear();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBodyContactListener
 * \{ */

JPH::SoftBodyValidateResult JoltSoftBodyContactListener::OnSoftBodyContactValidate(
    const JPH::Body &inSoftBody,
    const JPH::Body &inOtherBody,
    JPH::SoftBodyContactSettings &ioSettings)
{
  if (!m_hasNoPinCollisionPairs.load(std::memory_order_acquire)) {
    return JPH::SoftBodyValidateResult::AcceptContact;
  }

  /* Called from physics threads during soft body collision detection.
   * All bodies are locked; do NOT call any body-locking functions here. */
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_noPinCollisionMap.find(inSoftBody.GetID().GetIndexAndSequenceNumber());
  if (it != m_noPinCollisionMap.end() &&
      it->second == inOtherBody.GetID().GetIndexAndSequenceNumber()) {
    /* Keep the contact so the soft body still collides/deforms against the
     * pin/parent body, but disable impulse transfer to the pin/parent body.
     * This matches the "No Force on Pin Object" meaning. */
    ioSettings.mInvMassScale2 = 0.0f;
    ioSettings.mInvInertiaScale2 = 0.0f;
    ioSettings.mIsSensor = false;
  }
  return JPH::SoftBodyValidateResult::AcceptContact;
}

void JoltSoftBodyContactListener::OnSoftBodyContactAdded(
    const JPH::Body &inSoftBody,
    const JPH::SoftBodyManifold &inManifold)
{
  if (!m_collectPlasticityContacts.load(std::memory_order_acquire) ||
      !m_hasPlasticityContactSoftBodies.load(std::memory_order_acquire)) {
    return;
  }

  const JPH::uint32 softBodyKey = inSoftBody.GetID().GetIndexAndSequenceNumber();
  {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (m_plasticityContactSoftBodies.find(softBodyKey) ==
        m_plasticityContactSoftBodies.end()) {
      return;
    }
  }

  const JPH::Array<JPH::SoftBodyVertex> &vertices = inManifold.GetVertices();
  std::vector<JPH::uint32> contactVertices;
  contactVertices.reserve(8);

  for (JPH::uint32 vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex) {
    if (inManifold.HasContact(vertices[vertexIndex])) {
      contactVertices.push_back(vertexIndex);
    }
  }

  if (contactVertices.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_plasticityContactMutex);
  std::vector<JPH::uint32> &storedVertices =
      m_plasticityContactVertices[softBodyKey];
  storedVertices.insert(storedVertices.end(), contactVertices.begin(), contactVertices.end());
}

void JoltSoftBodyContactListener::SetCollectPlasticityContacts(bool collect)
{
  m_collectPlasticityContacts.store(collect, std::memory_order_release);
}

void JoltSoftBodyContactListener::RegisterPlasticityContactBody(JPH::BodyID softBodyID)
{
  if (softBodyID.IsInvalid()) {
    return;
  }

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_plasticityContactSoftBodies.insert(softBodyID.GetIndexAndSequenceNumber());
  m_hasPlasticityContactSoftBodies.store(true, std::memory_order_release);
}

void JoltSoftBodyContactListener::UnregisterPlasticityContactBody(JPH::BodyID softBodyID)
{
  if (softBodyID.IsInvalid()) {
    return;
  }

  const JPH::uint32 softBodyKey = softBodyID.GetIndexAndSequenceNumber();
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_plasticityContactSoftBodies.erase(softBodyKey);
  m_hasPlasticityContactSoftBodies.store(!m_plasticityContactSoftBodies.empty(),
                                         std::memory_order_release);
  lock.unlock();

  std::lock_guard<std::mutex> contactLock(m_plasticityContactMutex);
  m_plasticityContactVertices.erase(softBodyKey);
}

void JoltSoftBodyContactListener::ConsumePlasticityContacts(
    std::unordered_map<JPH::uint32, std::vector<JPH::uint32>> &contacts)
{
  contacts.clear();
  std::lock_guard<std::mutex> lock(m_plasticityContactMutex);
  contacts.swap(m_plasticityContactVertices);
}

void JoltSoftBodyContactListener::Register(JPH::BodyID softBodyID, JPH::BodyID pinBodyID)
{
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_noPinCollisionMap[softBodyID.GetIndexAndSequenceNumber()] =
      pinBodyID.GetIndexAndSequenceNumber();
  m_hasNoPinCollisionPairs.store(true, std::memory_order_release);
}

void JoltSoftBodyContactListener::Unregister(JPH::BodyID softBodyID)
{
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_noPinCollisionMap.erase(softBodyID.GetIndexAndSequenceNumber());
  m_hasNoPinCollisionPairs.store(!m_noPinCollisionMap.empty(), std::memory_order_release);
  lock.unlock();

  std::lock_guard<std::mutex> contactLock(m_plasticityContactMutex);
  m_plasticityContactVertices.erase(softBodyID.GetIndexAndSequenceNumber());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltBodyActivationListener
 * \{ */

void JoltBodyActivationListener::OnBodyActivated(const JPH::BodyID &inBodyID,
                                                   JPH::uint64 inBodyUserData)
{
  m_activeCount.fetch_add(1, std::memory_order_relaxed);
}

void JoltBodyActivationListener::OnBodyDeactivated(const JPH::BodyID &inBodyID,
                                                    JPH::uint64 inBodyUserData)
{
  m_activeCount.fetch_sub(1, std::memory_order_relaxed);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — Construction / Destruction
 * \{ */

JoltPhysicsEnvironment::JoltPhysicsEnvironment(blender::Scene *blenderscene,
                                               int numThreads,
                                               int maxBodies,
                                               int maxBodyPairs,
                                               int maxContactConstraints,
                                               int tempAllocatorMB,
                                               bool visualizePhysics)
    : m_blenderScene(blenderscene),
      m_numTimeSubSteps(1),
      m_debugMode(0),
      m_deactivationTime(2.0f),
      m_linearDeactivationThreshold(0.8f),
      m_angularDeactivationThreshold(1.0f)
{
  for (int i = 0; i < PHY_NUM_RESPONSE; i++) {
    m_triggerCallbacks[i] = nullptr;
    m_triggerCallbacksUserPtrs[i] = nullptr;
  }

  EnsureJoltInitialized();

  /* Reset per-session state so IDs don't grow unboundedly across sessions. */
  g_joltConstraintUid = 1;

  /* Create temporary allocator for per-frame physics allocations. */
  m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(
      tempAllocatorMB * 1024 * 1024);

  /* Create job system with configured thread count. */
  m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, numThreads);

  /* Create the physics system with 3 broadphase layers (static/dynamic/sensor).
   * Custom filter classes handle Stage 1 broadphase rejection and Stage 2
   * pair filtering using UPBGE collision layers. */
  m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();
  m_physicsSystem->Init(
      (JPH::uint)maxBodies,
      0,  /* numBodyMutexes: 0 = auto-detect */
      (JPH::uint)maxBodyPairs,
      (JPH::uint)maxContactConstraints,
      m_broadPhaseLayerInterface,
      m_objectVsBroadPhaseLayerFilter,
      m_objectLayerPairFilter);

  /* Create the constraint group filter used for "Disable Collisions" on
   * rigid body constraints.  This filter is shared by all bodies and checked
   * before narrow-phase GJK/EPA, so rejected pairs skip expensive work. */
  m_constraintGroupFilter = new JoltConstraintGroupFilter();

  /* Register listeners. */
  m_contactListener.SetEnvironment(this);
  m_physicsSystem->SetBodyActivationListener(&m_bodyActivationListener);
  m_physicsSystem->SetContactListener(&m_contactListener);
  m_physicsSystem->SetSoftBodyContactListener(&m_softBodyContactListener);

  /* Set Bullet-style combine functions for friction and restitution.
   * This makes the behavior consistent with Bullet Physics where
   * restitution = r1 * r2 (if either is 0, result is 0). */
  m_physicsSystem->SetCombineFriction(BulletCombineFriction);
  m_physicsSystem->SetCombineRestitution(BulletCombineRestitution);

  /* Set default gravity (Blender Z-up: -9.81 on Z → Jolt Y-up: -9.81 on Y). */
  m_physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

  if (visualizePhysics) {
    m_debugMode = 1;
  }
  m_debugErrors = blenderscene->gm.jolt_debug_errors != 0;

  const char *perfProbeEnv = std::getenv("UPBGE_JOLT_PERF_PROBE");
  const char *perfProbeWindowEnv = std::getenv("UPBGE_JOLT_PERF_PROBE_WINDOW");

  /* Probe defaults to debug-errors mode and can be forced through env vars.
   * This keeps regular gameplay builds quiet unless explicit investigation is enabled. */
  m_perfProbeEnabled = m_debugErrors;
  if (perfProbeEnv && perfProbeEnv[0] != '\0' && perfProbeEnv[0] != '0') {
    m_perfProbeEnabled = true;
  }

  if (perfProbeWindowEnv) {
    const int window = std::atoi(perfProbeWindowEnv);
    if (window > 0) {
      m_perfProbeWindowFrames = window;
    }
  }

  if (m_perfProbeEnabled) {
    printf("Jolt: Perf probe enabled (window=%d frames)\n", m_perfProbeWindowFrames);
  }
}

JoltPhysicsEnvironment::~JoltPhysicsEnvironment()
{
  /* Remove all constraints from the physics system first (they reference bodies). */
  for (auto &pair : m_constraintById) {
    JoltConstraint *con = pair.second;
    JPH::Constraint *joltCon = con->GetConstraint();
    if (joltCon) {
      m_physicsSystem->RemoveConstraint(joltCon);
    }
    delete con;
  }
  m_constraintById.clear();

  /* Delete characters (CharacterVirtual instances). */
  for (auto &pair : m_characterByObject) {
    delete pair.second;
  }
  m_characterByObject.clear();

  /* Delete vehicles. */
  for (JoltVehicle *veh : m_vehicles) {
    delete veh;
  }
  m_vehicles.clear();

  /* Delete soft bodies. */
  while (!m_softBodies.empty()) {
    RemoveSoftBody(m_softBodies.back());
  }
  FlushPendingSoftBodyBodyRemoves();

  /* Controllers are normally destroyed by KX_GameObject before we get here.
   * Each controller destructor removes its body and erases itself from m_controllers.
   * Handle any stragglers that weren't cleaned up by game objects. */
  {
    JPH::BodyInterface &bodyInterface = m_physicsSystem->GetBodyInterfaceNoLock();
    /* Copy the set since controller destructors modify m_controllers. */
    std::vector<JoltPhysicsController *> remaining(m_controllers.begin(), m_controllers.end());

    std::vector<JPH::BodyID> addedBodyIDs;
    std::vector<JPH::BodyID> destroyBodyIDs;
    addedBodyIDs.reserve(remaining.size());
    destroyBodyIDs.reserve(remaining.size());

    for (JoltPhysicsController *ctrl : remaining) {
      JPH::BodyID bodyID = ctrl->GetBodyID();
      if (!bodyID.IsInvalid()) {
        if (std::find(destroyBodyIDs.begin(), destroyBodyIDs.end(), bodyID) == destroyBodyIDs.end()) {
          destroyBodyIDs.push_back(bodyID);
        }
        if (bodyInterface.IsAdded(bodyID) &&
            std::find(addedBodyIDs.begin(), addedBodyIDs.end(), bodyID) == addedBodyIDs.end()) {
          addedBodyIDs.push_back(bodyID);
        }
      }
    }

    for (const JPH::BodyID bodyID : destroyBodyIDs) {
      RemoveLogicActiveContactsForBody(bodyID);
    }
    if (!addedBodyIDs.empty()) {
      bodyInterface.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
    }
    if (!destroyBodyIDs.empty()) {
      bodyInterface.DestroyBodies(destroyBodyIDs.data(), (int)destroyBodyIDs.size());
    }

    for (JoltPhysicsController *ctrl : remaining) {
      ctrl->SetEnvironment(nullptr);  /* Prevent controller destructor from double-freeing. */
      delete ctrl;  /* Free the controller and its motion state. */
    }

    m_pendingRigidBodyBodyAdds.clear();
    m_pendingCompoundSubShapesByController.clear();
    m_controllers.clear();
    m_controllersIterationCache.clear();
    m_fhControllersIterationCache.clear();
    m_buoyancyVolumesIterationCache.clear();
    m_buoyancyTargetBodyIDsScratch.clear();
    m_buoyancyTargetBodyKeysScratch.clear();
    m_controllersIterationCacheDirty = true;
    m_fhControllersIterationCacheDirty = true;
    m_buoyancyVolumesIterationCacheDirty = true;
  }

  /* Graphic controllers are owned by KX_GameObject, but clear stale pointers. */
  m_graphicControllers.clear();

  if (m_fallbackEffectorWeights) {
    MEM_delete(m_fallbackEffectorWeights);
    m_fallbackEffectorWeights = nullptr;
  }

  /* Destroy physics system before job system and allocator. */
  m_physicsSystem.reset();
  m_jobSystem.reset();
  m_tempAllocator.reset();

  /* Jolt global state (allocator, Factory, registered types) persists for the
   * entire process lifetime.  Repeated init/shutdown of these globals caused
   * memory growth from thread-stack caching and heap fragmentation. */
}

JoltPhysicsEnvironment *JoltPhysicsEnvironment::Create(blender::Scene *blenderscene,
                                                       bool visualizePhysics)
{
  /* Read configuration from scene's GameData. */
  int numThreads = blenderscene->gm.jolt_physics_threads;
  if (numThreads <= 0) {
    numThreads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
  }
  numThreads = std::clamp(numThreads, 1, (int)std::thread::hardware_concurrency());

  int maxBodies = blenderscene->gm.jolt_max_bodies;
  if (maxBodies <= 0) {
    maxBodies = 65536;
  }

  int maxBodyPairs = blenderscene->gm.jolt_max_body_pairs;
  if (maxBodyPairs <= 0) {
    maxBodyPairs = 10240;
  }

  int maxContactConstraints = blenderscene->gm.jolt_max_contact_constraints;
  if (maxContactConstraints <= 0) {
    maxContactConstraints = 10240;
  }

  /* Temp allocator needs memory for:
   * - Body pairs (maxBodyPairs * ~200 bytes)
   * - Contact constraints (maxContactConstraints * ~64 bytes)
   * - Per-body temp data (maxBodies * ~900 bytes)
   * Use a safe multiplier to avoid OOM during collision detection. */
  int tempAllocatorMB = std::max(64, (int)((int64_t)maxBodies * 2048 / (1024 * 1024)) + 32);
  JoltPhysicsEnvironment *env = new JoltPhysicsEnvironment(blenderscene,
                                                           numThreads,
                                                           maxBodies,
                                                           maxBodyPairs,
                                                           maxContactConstraints,
                                                           tempAllocatorMB,
                                                           visualizePhysics);

  /* Apply scene-level physics settings. */
  JPH::PhysicsSettings physicsSettings = env->m_physicsSystem->GetPhysicsSettings();
  physicsSettings.mNumVelocitySteps = (JPH::uint)std::clamp(
      int(blenderscene->gm.jolt_velocity_solver_iterations), 2, 255);
  physicsSettings.mNumPositionSteps = (JPH::uint)std::clamp(
      int(blenderscene->gm.jolt_position_solver_iterations), 1, 255);
  env->m_physicsSystem->SetPhysicsSettings(physicsSettings);
  env->SetGravity(0.0f, 0.0f, -(blenderscene->gm.gravity));
  env->SetDeactivationTime(blenderscene->gm.deactivationtime);
  env->SetDeactivationLinearTreshold(blenderscene->gm.lineardeactthreshold);
  env->SetDeactivationAngularTreshold(blenderscene->gm.angulardeactthreshold);
  env->SetERPNonContact(blenderscene->gm.erp);
  env->SetERPContact(blenderscene->gm.erp2);

  return env;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — PHY_IPhysicsEnvironment interface
 * \{ */

bool JoltPhysicsEnvironment::ProceedDeltaTime(double curTime, float timeStep, float interval)
{
  if (timeStep <= 0.0f) {
    return false;
  }

  using PerfClock = std::chrono::steady_clock;
  const bool perfProbeEnabled = m_perfProbeEnabled;
  const PerfClock::time_point perfStepStart =
      perfProbeEnabled ? PerfClock::now() : PerfClock::time_point();
  double perfBroadphaseUS = 0.0;
  double perfUpdateUS = 0.0;
  double perfBreakCheckUS = 0.0;
  double perfPrepUS = 0.0;
  double perfWriteKinematicUS = 0.0;
  double perfPinnedUpdateUS = 0.0;
  double perfDeferredUS = 0.0;
  double perfFhSpringUS = 0.0;
  double perfSimulationTickUS = 0.0;
  double perfSyncMotionUS = 0.0;
  double perfCharacterUS = 0.0;
  double perfVehicleUS = 0.0;
  double perfCallbackUS = 0.0;
  size_t perfBreakableChecked = 0;
  size_t perfBroken = 0;
  size_t perfDeferredOpsQueued = 0;
  size_t perfControllers = 0;
  size_t perfContactPairs = 0;
  size_t perfPendingSoftSingleAdds = 0;
  size_t perfPendingSoftBatchAdds = 0;
  size_t perfPendingRigidSingleAdds = 0;

  int collisionSteps = m_numTimeSubSteps;
  if (collisionSteps < 1) {
    collisionSteps = 1;
  }

  /* Full simulation step sequence:
   *   1. Sync motion states BEFORE physics (kinematic bodies read from game objects)
   *   1.75 Apply Blender effectors (force fields)
   *   1.8 Apply Jolt buoyancy volumes
   *   1.9 Vehicle pre-step + UPBGE minimum velocity clamps
   *   2. PhysicsSystem::Update()
   *   3. Process FH springs
   *   4. Sync motion states AFTER physics (dynamic bodies write to game objects)
   *   5. CallbackTriggers()
   */

  PerfClock::time_point prepStart;
  if (perfProbeEnabled) {
    prepStart = PerfClock::now();
    perfPendingSoftSingleAdds = (size_t)m_pendingSoftBodyAddsForOptimize;
    perfPendingSoftBatchAdds = (size_t)m_pendingSoftBodyBatchAddsForOptimize;
    perfPendingRigidSingleAdds = (size_t)m_pendingRigidBodyAddsForOptimize;
  }

  /* Finalize any queued compound-shape rebuilds before body adds so newly added
   * bodies enter the broadphase with their final collision shape. */
  FinalizePendingCompoundShapeBuilds();

  /* Finalize queued runtime body additions/removals before per-step work.
   * Rigid bodies and soft bodies both use Jolt's batched add APIs here. */
  FlushPendingRigidBodyBodyAdds();
  FlushPendingSoftBodyBodyRemoves();
  FlushPendingSoftBodyBodyAdds();
  ProcessPendingConstraintTopologyChanges();

  if (perfProbeEnabled) {
    perfPrepUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                     PerfClock::now() - prepStart)
                     .count();
  }

  /* Broadphase maintenance:
   *  - one-time optimize after scene load / environment merge;
   *  - runtime re-optimize for many one-by-one AddBody insertions;
   *  - optional safety optimize for very large batched spawn bursts.
   *
   * Jolt docs note batched insertion is already broadphase-friendly and that
   * OptimizeBroadPhase should not be called every frame. Keep a short cooldown
   * for runtime-triggered optimizes to avoid repeated spikes. */
  constexpr int kRuntimeSoftBodySingleAddOptimizeThreshold = 8;
  constexpr int kRuntimeSoftBodyBatchAddOptimizeThreshold = 128;
  constexpr int kRuntimeSoftBodySingleAddIdleFramesBeforeOptimize = 2;
  constexpr int kRuntimeSoftBodyBatchAddIdleFramesBeforeOptimize = 4;
  constexpr int kRuntimeRigidBodySingleAddOptimizeThreshold = 16;
  constexpr int kRuntimeRigidBodySingleAddIdleFramesBeforeOptimize = 2;
  constexpr int kRuntimeBroadPhaseOptimizeCooldownFrames = 30;

  if (m_broadPhaseOptimizeCooldownFrames > 0) {
    --m_broadPhaseOptimizeCooldownFrames;
  }

  if (m_pendingSoftBodyAddsForOptimize > 0 || m_pendingSoftBodyBatchAddsForOptimize > 0) {
    if (m_softBodyAddsSinceLastStep > 0) {
      m_softBodyAddIdleFrames = 0;
    }
    else {
      ++m_softBodyAddIdleFrames;
    }
  }

  if (m_pendingRigidBodyAddsForOptimize > 0) {
    if (m_rigidBodyAddsSinceLastStep > 0) {
      m_rigidBodyAddIdleFrames = 0;
    }
    else {
      ++m_rigidBodyAddIdleFrames;
    }
  }

  const bool shouldOptimizeForSingleAdds =
      m_pendingSoftBodyAddsForOptimize >= kRuntimeSoftBodySingleAddOptimizeThreshold &&
      m_softBodyAddIdleFrames >= kRuntimeSoftBodySingleAddIdleFramesBeforeOptimize;
  const bool shouldOptimizeForBatchAdds =
      m_pendingSoftBodyBatchAddsForOptimize >= kRuntimeSoftBodyBatchAddOptimizeThreshold &&
      m_softBodyAddIdleFrames >= kRuntimeSoftBodyBatchAddIdleFramesBeforeOptimize;
  const bool shouldOptimizeForRigidSingleAdds =
      m_pendingRigidBodyAddsForOptimize >= kRuntimeRigidBodySingleAddOptimizeThreshold &&
      m_rigidBodyAddIdleFrames >= kRuntimeRigidBodySingleAddIdleFramesBeforeOptimize;

  const bool shouldOptimizeForRuntimeAdds =
      shouldOptimizeForSingleAdds || shouldOptimizeForBatchAdds || shouldOptimizeForRigidSingleAdds;

  /* Keep startup optimize immediate. Runtime optimize requests are throttled to
   * avoid expensive repeated broadphase rebuilds in consecutive frames. */
  const bool shouldForceOptimizeNow = m_needsBroadPhaseOptimize && !m_hasSteppedSimulation;
  const bool shouldOptimizeNow =
      shouldForceOptimizeNow ||
      ((m_needsBroadPhaseOptimize || shouldOptimizeForRuntimeAdds) &&
       m_broadPhaseOptimizeCooldownFrames == 0);

  if (shouldOptimizeNow) {
    PerfClock::time_point broadPhaseStart;
    if (perfProbeEnabled) {
      broadPhaseStart = PerfClock::now();
    }

    m_physicsSystem->OptimizeBroadPhase();
    m_needsBroadPhaseOptimize = false;
    m_pendingSoftBodyAddsForOptimize = 0;
    m_pendingRigidBodyAddsForOptimize = 0;
    m_pendingSoftBodyBatchAddsForOptimize = 0;
    m_softBodyAddIdleFrames = 0;
    m_rigidBodyAddIdleFrames = 0;
    m_broadPhaseOptimizeCooldownFrames = kRuntimeBroadPhaseOptimizeCooldownFrames;

    if (perfProbeEnabled) {
      perfBroadphaseUS += (double)std::chrono::duration_cast<std::chrono::microseconds>(
                              PerfClock::now() - broadPhaseStart)
                              .count();
      ++m_perfProbeBroadphaseCallsAccum;
      if (shouldForceOptimizeNow) {
        ++m_perfProbeBroadphaseStartupCallsAccum;
      }
      if (shouldOptimizeForSingleAdds) {
        ++m_perfProbeBroadphaseSoftSingleCallsAccum;
      }
      if (shouldOptimizeForBatchAdds) {
        ++m_perfProbeBroadphaseSoftBatchCallsAccum;
      }
      if (shouldOptimizeForRigidSingleAdds) {
        ++m_perfProbeBroadphaseRigidSingleCallsAccum;
      }
      if (!shouldForceOptimizeNow && !shouldOptimizeForSingleAdds &&
          !shouldOptimizeForBatchAdds && !shouldOptimizeForRigidSingleAdds) {
        ++m_perfProbeBroadphaseOtherCallsAccum;
      }
    }
  }
  m_softBodyAddsSinceLastStep = 0;
  m_rigidBodyAddsSinceLastStep = 0;

  /* Step 1: Write game object transforms to kinematic bodies. */
  PerfClock::time_point writeKinematicStart;
  if (perfProbeEnabled) {
    writeKinematicStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->WriteMotionStateToDynamics(true);  /* nondynaonly = true → only kinematic/static */
  }

  if (perfProbeEnabled) {
    perfWriteKinematicUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                             PerfClock::now() - writeKinematicStart)
                             .count();
    perfControllers = GetControllersForIteration().size();
  }

  /* Step 1.5: Update kinematic (pinned) vertices on soft bodies so they follow their
   * pin objects this frame. Must happen after kinematic rigid bodies are written
   * (Step 1) but before the physics solver runs (Step 2). */
  PerfClock::time_point pinnedUpdateStart;
  if (perfProbeEnabled) {
    pinnedUpdateStart = PerfClock::now();
  }

  m_pinnedSoftBodyUpdatesScratch.clear();
  m_pinnedSoftBodyBodyIDsScratch.clear();
  m_pinnedSoftBodyUpdatesScratch.reserve(m_pinnedSoftBodies.size());
  m_pinnedSoftBodyBodyIDsScratch.reserve(m_pinnedSoftBodies.size());

  JPH::BodyInterface &pinnedBI = m_physicsSystem->GetBodyInterfaceNoLock();

  for (JoltSoftBody *sb : m_pinnedSoftBodies) {
    if (!sb || !sb->HasPinnedVertices()) {
      continue;
    }

    JoltPhysicsController *pinCtrl = sb->GetPinController();
    if (!pinCtrl || !pinCtrl->GetMotionState()) {
      continue;
    }

    const JPH::BodyID sbBodyID = sb->GetBodyID();
    if (sbBodyID.IsInvalid()) {
      continue;
    }
    if (!pinnedBI.IsAdded(sbBodyID)) {
      continue;
    }

    MT_Vector3 pinPos = pinCtrl->GetMotionState()->GetWorldPosition();
    /* Jolt soft bodies always report identity rotation (orientation is baked into
     * particle positions at creation, never updated as a body rotation).
     * For soft body pin objects we must use the stored initial Blender orientation,
     * which is their permanent effective rotation. For rigid body pin objects,
     * read the actual current orientation from the motion state. */
    MT_Matrix3x3 pinOri = pinCtrl->GetSoftBody() ?
                              sb->GetPinInitialOri() :
                              pinCtrl->GetMotionState()->GetWorldOrientation();

    PinnedSoftBodyUpdateEntry entry;
    entry.softBody = sb;
    entry.pinPos = pinPos;
    entry.pinOri = pinOri;

    JPH::RVec3 targetBodyPos;
    const JPH::RVec3 currentBodyPos = pinnedBI.GetPosition(sbBodyID);
    if (sb->PreparePinTransformFollowBodyMove(pinPos, pinOri, currentBodyPos, targetBodyPos)) {
      entry.pinFollowTransformChanged = true;
      entry.previousBodyPos = currentBodyPos;
      pinnedBI.SetPositionAndRotationWhenChanged(
          sbBodyID, targetBodyPos, pinnedBI.GetRotation(sbBodyID), JPH::EActivation::Activate);
      pinnedBI.ActivateBody(sbBodyID);
    }

    m_pinnedSoftBodyUpdatesScratch.push_back(entry);
    m_pinnedSoftBodyBodyIDsScratch.push_back(sbBodyID);
  }

  if (!m_pinnedSoftBodyBodyIDsScratch.empty()) {
    const JPH::BodyLockInterface &lockIf =
        m_physicsSystem->GetBodyLockInterfaceNoLock();
    JPH::BodyLockMultiWrite multiLock(lockIf,
                                      m_pinnedSoftBodyBodyIDsScratch.data(),
                                      (int)m_pinnedSoftBodyBodyIDsScratch.size());

    for (int i = 0; i < (int)m_pinnedSoftBodyBodyIDsScratch.size(); ++i) {
      JPH::Body *body = multiLock.GetBody(i);
      if (!body) {
        continue;
      }

      const PinnedSoftBodyUpdateEntry &entry = m_pinnedSoftBodyUpdatesScratch[(size_t)i];
      const JPH::RVec3 *previousBodyPos = entry.pinFollowTransformChanged ?
                                             &entry.previousBodyPos :
                                             nullptr;
      entry.softBody->UpdatePinnedVerticesLocked(
          entry.pinPos, entry.pinOri, *body, previousBodyPos);
    }
  }

  if (perfProbeEnabled) {
    perfPinnedUpdateUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - pinnedUpdateStart)
                           .count();
  }

  /* Step 1.75: Apply Blender effectors (force fields) to dynamic rigid bodies. */
  ApplyEffectorForces();

  /* Step 1.8: Apply Jolt sensor buoyancy volumes to dynamic rigid bodies. */
  ProcessBuoyancy(timeStep);

  for (JoltVehicle *veh : m_vehicles) {
    veh->PreStep(timeStep);
  }

  /* Step 1.9: Apply UPBGE minimum velocity clamps before Jolt evaluates
   * constraints, integration, and sleeping. Maximum linear/angular velocity is
   * handled natively by Jolt during Update(). */
  PerfClock::time_point simulationTickStart;
  if (perfProbeEnabled) {
    simulationTickStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->SimulationTick(timeStep);
  }

  if (perfProbeEnabled) {
    perfSimulationTickUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                               PerfClock::now() - simulationTickStart)
                               .count();
  }

  bool collectSoftBodyPlasticityContacts = false;
  for (JoltSoftBody *sb : m_softBodies) {
    if (sb && sb->UsesContactPlasticity()) {
      collectSoftBodyPlasticityContacts = true;
      break;
    }
  }
  m_softBodyContactListener.SetCollectPlasticityContacts(collectSoftBodyPlasticityContacts);

  /* Step 2: Run the physics simulation.
   * Set the updating flag to defer unsafe body modifications.
   * This prevents crashes when logic tries to modify bodies during physics. */
  PerfClock::time_point updateStart;
  if (perfProbeEnabled) {
    updateStart = PerfClock::now();
  }

  m_isPhysicsUpdating = true;
  JPH::EPhysicsUpdateError updateErr = m_physicsSystem->Update(
      timeStep, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());
  m_isPhysicsUpdating = false;

  if (perfProbeEnabled) {
    perfUpdateUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - updateStart)
                       .count();
  }

  if (m_debugErrors && updateErr != JPH::EPhysicsUpdateError::None) {
    printf("Jolt: Update returned error %d\n", (int)updateErr);
  }

  /* Process any deferred operations that were queued during the physics update. */
  PerfClock::time_point deferredStart;
  if (perfProbeEnabled) {
    deferredStart = PerfClock::now();
    perfDeferredOpsQueued = m_deferredOps.size();
  }

  ProcessDeferredOperations();
  FlushPendingSoftBodyBodyRemoves();

  if (perfProbeEnabled) {
    perfDeferredUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                         PerfClock::now() - deferredStart)
                         .count();
  }

  m_softBodyContactListener.ConsumePlasticityContacts(m_softBodyContactVerticesScratch);

  m_softBodiesToPlasticityUpdateScratch.clear();
  m_softBodyPlasticityUpdateIDsScratch.clear();
  m_softBodyPlasticityContactVerticesScratch.clear();
  m_softBodiesToPlasticityUpdateScratch.reserve(m_softBodies.size());
  m_softBodyPlasticityUpdateIDsScratch.reserve(m_softBodies.size());
  m_softBodyPlasticityContactVerticesScratch.reserve(m_softBodies.size());

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
  for (JoltSoftBody *sb : m_softBodies) {
    if (!sb) {
      continue;
    }

    const JPH::BodyID sbBodyID = sb->GetBodyID();
    if (sbBodyID.IsInvalid()) {
      continue;
    }
    if (!bi.IsAdded(sbBodyID)) {
      continue;
    }

    const auto contactIt = m_softBodyContactVerticesScratch.find(
        sbBodyID.GetIndexAndSequenceNumber());
    const bool hasStoredContacts = contactIt != m_softBodyContactVerticesScratch.end();
    const std::vector<JPH::uint32> *contactVertices =
        hasStoredContacts ? &contactIt->second : nullptr;
    const bool hasContactVertices = contactVertices && !contactVertices->empty();

    bool bodyIsActive = bi.IsActive(sbBodyID);
    if (!bodyIsActive && sb->NeedsPlasticityRepairWakeUp()) {
      bi.ActivateBody(sbBodyID);
      bodyIsActive = true;
    }

    if (!sb->NeedsPlasticityUpdate(hasContactVertices)) {
      continue;
    }

    m_softBodiesToPlasticityUpdateScratch.push_back(sb);
    m_softBodyPlasticityUpdateIDsScratch.push_back(sbBodyID);
    m_softBodyPlasticityContactVerticesScratch.push_back(contactVertices);
  }

  if (!m_softBodyPlasticityUpdateIDsScratch.empty()) {
    const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
    JPH::BodyLockMultiWrite multiLock(lockInterface,
                                      m_softBodyPlasticityUpdateIDsScratch.data(),
                                      (int)m_softBodyPlasticityUpdateIDsScratch.size());

    for (int i = 0; i < (int)m_softBodyPlasticityUpdateIDsScratch.size(); ++i) {
      JPH::Body *body = multiLock.GetBody(i);
      if (!body) {
        continue;
      }

      m_softBodiesToPlasticityUpdateScratch[(size_t)i]->ApplyPlasticityLocked(
          *body, timeStep, m_softBodyPlasticityContactVerticesScratch[(size_t)i]);
    }
  }

  /* Step 3: Process FH (Floating Height) springs. */
  PerfClock::time_point fhSpringStart;
  if (perfProbeEnabled) {
    fhSpringStart = PerfClock::now();
  }

  ProcessFhSprings(timeStep);

  if (perfProbeEnabled) {
    perfFhSpringUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - fhSpringStart)
                        .count();
  }

  /* Step 4: Read Jolt body transforms back to game objects. */
  PerfClock::time_point syncMotionStart;
  if (perfProbeEnabled) {
    syncMotionStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->SynchronizeMotionStates(timeStep);
  }

  if (perfProbeEnabled) {
    perfSyncMotionUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                          PerfClock::now() - syncMotionStart)
                          .count();
  }

  /* Step 5: Update characters. */
  PerfClock::time_point characterStart;
  if (perfProbeEnabled) {
    characterStart = PerfClock::now();
  }

  for (auto &pair : m_characterByObject) {
    pair.second->Update(timeStep);
  }

  if (perfProbeEnabled) {
    perfCharacterUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - characterStart)
                        .count();
  }

  /* Step 6: Sync vehicle wheels. */
  PerfClock::time_point vehicleStart;
  if (perfProbeEnabled) {
    vehicleStart = PerfClock::now();
  }

  for (JoltVehicle *veh : m_vehicles) {
    veh->SyncWheels();
  }

  if (perfProbeEnabled) {
    perfVehicleUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                      PerfClock::now() - vehicleStart)
                      .count();
  }

  /* Step 7: Check breaking thresholds on constraints. */
  PerfClock::time_point breakCheckStart;
  if (perfProbeEnabled) {
    breakCheckStart = PerfClock::now();
  }

  if (m_breakableConstraintsCacheDirty) {
    RebuildBreakableConstraintCache();
  }

  m_brokenConstraintIDsScratch.clear();
  for (JoltConstraint *con : m_breakableConstraintsCache) {
    if (con && con->CheckBreaking()) {
      m_brokenConstraintIDsScratch.push_back(con->GetIdentifier());
    }
  }

  for (int constraintID : m_brokenConstraintIDsScratch) {
    RemoveConstraintById(constraintID, true);
  }

  if (perfProbeEnabled) {
    perfBreakableChecked = m_breakableConstraintsCache.size();
    perfBroken = m_brokenConstraintIDsScratch.size();
    perfBreakCheckUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - breakCheckStart)
                           .count();
  }

  /* Step 8: Fire collision callbacks. */
  PerfClock::time_point callbackStart;
  if (perfProbeEnabled) {
    callbackStart = PerfClock::now();
  }

  CallbackTriggers();

  if (perfProbeEnabled) {
    perfCallbackUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - callbackStart)
                       .count();
    if (m_triggerCallbacks[PHY_OBJECT_RESPONSE]) {
      perfContactPairs = m_contactPairsScratch.size();
    }
  }

  /* Track active body count. */
  m_activeBodyCount = m_bodyActivationListener.GetActiveCount();
  m_hasSteppedSimulation = true;

  if (perfProbeEnabled) {
    const double perfStepUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                                  PerfClock::now() - perfStepStart)
                                  .count();

    m_perfProbeStepUSAccum += perfStepUS;
    m_perfProbeUpdateUSAccum += perfUpdateUS;
    m_perfProbeBroadphaseUSAccum += perfBroadphaseUS;
    m_perfProbeBreakCheckUSAccum += perfBreakCheckUS;
    m_perfProbePrepUSAccum += perfPrepUS;
    m_perfProbeWriteKinematicUSAccum += perfWriteKinematicUS;
    m_perfProbePinnedUpdateUSAccum += perfPinnedUpdateUS;
    m_perfProbeDeferredUSAccum += perfDeferredUS;
    m_perfProbeFhSpringUSAccum += perfFhSpringUS;
    m_perfProbeSimulationTickUSAccum += perfSimulationTickUS;
    m_perfProbeSyncMotionUSAccum += perfSyncMotionUS;
    m_perfProbeCharacterUSAccum += perfCharacterUS;
    m_perfProbeVehicleUSAccum += perfVehicleUS;
    m_perfProbeCallbackUSAccum += perfCallbackUS;
    m_perfProbeBreakableCheckedAccum += perfBreakableChecked;
    m_perfProbeBrokenAccum += perfBroken;
    m_perfProbeDeferredOpsQueuedAccum += perfDeferredOpsQueued;
    m_perfProbeControllersAccum += perfControllers;
    m_perfProbeContactPairsAccum += perfContactPairs;
    m_perfProbePendingSoftSingleAddsAccum += perfPendingSoftSingleAdds;
    m_perfProbePendingSoftBatchAddsAccum += perfPendingSoftBatchAdds;
    m_perfProbePendingRigidSingleAddsAccum += perfPendingRigidSingleAdds;
    ++m_perfProbeFramesAccum;

    const int probeWindow = std::max(1, m_perfProbeWindowFrames);
    if (m_perfProbeFramesAccum >= probeWindow) {
      const double invFrames = 1.0 / (double)m_perfProbeFramesAccum;
      const double stepAvgMS = (m_perfProbeStepUSAccum * invFrames) / 1000.0;
      const double updateAvgMS = (m_perfProbeUpdateUSAccum * invFrames) / 1000.0;
      const double broadphaseFrameAvgMS = (m_perfProbeBroadphaseUSAccum * invFrames) / 1000.0;
      const double breakCheckAvgMS = (m_perfProbeBreakCheckUSAccum * invFrames) / 1000.0;
      const double prepAvgMS = (m_perfProbePrepUSAccum * invFrames) / 1000.0;
      const double writeKinematicAvgMS = (m_perfProbeWriteKinematicUSAccum * invFrames) / 1000.0;
      const double pinnedUpdateAvgMS = (m_perfProbePinnedUpdateUSAccum * invFrames) / 1000.0;
      const double deferredAvgMS = (m_perfProbeDeferredUSAccum * invFrames) / 1000.0;
      const double fhSpringAvgMS = (m_perfProbeFhSpringUSAccum * invFrames) / 1000.0;
      const double simulationTickAvgMS = (m_perfProbeSimulationTickUSAccum * invFrames) / 1000.0;
      const double syncMotionAvgMS = (m_perfProbeSyncMotionUSAccum * invFrames) / 1000.0;
      const double characterAvgMS = (m_perfProbeCharacterUSAccum * invFrames) / 1000.0;
      const double vehicleAvgMS = (m_perfProbeVehicleUSAccum * invFrames) / 1000.0;
      const double callbackAvgMS = (m_perfProbeCallbackUSAccum * invFrames) / 1000.0;
      const double broadphaseCallAvgMS =
          m_perfProbeBroadphaseCallsAccum > 0 ?
              (m_perfProbeBroadphaseUSAccum / (double)m_perfProbeBroadphaseCallsAccum) / 1000.0 :
              0.0;
      const double breakableCheckedAvg = (double)m_perfProbeBreakableCheckedAccum * invFrames;
      const double deferredQueuedAvg = (double)m_perfProbeDeferredOpsQueuedAccum * invFrames;
      const double controllersAvg = (double)m_perfProbeControllersAccum * invFrames;
      const double contactPairsAvg = (double)m_perfProbeContactPairsAccum * invFrames;
      const double pendingSoftSingleAddsAvg = (double)m_perfProbePendingSoftSingleAddsAccum * invFrames;
      const double pendingSoftBatchAddsAvg = (double)m_perfProbePendingSoftBatchAddsAccum * invFrames;
      const double pendingRigidSingleAddsAvg =
          (double)m_perfProbePendingRigidSingleAddsAccum * invFrames;
      const double updateSoftBodiesFrameAvgMS =
          (m_perfProbeUpdateSoftBodiesUSAccum * invFrames) / 1000.0;
      const double updateSoftBodiesCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesDepsgraphCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesDepsgraphUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesFilterCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesFilterUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesMeshCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesMeshUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesRelTagCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesRelTagUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double softBodiesTotalAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesTotalAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;
      const double softBodiesCandidatesAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesCandidatesAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;
      const double softBodiesUpdatedAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesUpdatedAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;

      printf(
          "JoltPerfProbe: window=%d step_avg=%.3fms update_avg=%.3fms "
          "broadphase_frame_avg=%.3fms broadphase_calls=%zu broadphase_call_avg=%.3fms "
          "triggers[startup=%zu soft_single=%zu soft_batch=%zu rigid_single=%zu other=%zu] "
          "breakcheck_avg=%.3fms breakable_checked_avg=%.1f broken_total=%zu\n",
          m_perfProbeFramesAccum,
          stepAvgMS,
          updateAvgMS,
          broadphaseFrameAvgMS,
          m_perfProbeBroadphaseCallsAccum,
          broadphaseCallAvgMS,
          m_perfProbeBroadphaseStartupCallsAccum,
          m_perfProbeBroadphaseSoftSingleCallsAccum,
          m_perfProbeBroadphaseSoftBatchCallsAccum,
          m_perfProbeBroadphaseRigidSingleCallsAccum,
          m_perfProbeBroadphaseOtherCallsAccum,
          breakCheckAvgMS,
          breakableCheckedAvg,
          m_perfProbeBrokenAccum);

      printf(
          "JoltPerfProbeDetail: prep=%.3fms write_kinematic=%.3fms pinned_update=%.3fms "
          "deferred=%.3fms fh=%.3fms sim_tick=%.3fms sync=%.3fms chars=%.3fms "
          "vehicles=%.3fms callbacks=%.3fms controllers_avg=%.1f deferred_ops_avg=%.1f "
          "contacts_avg=%.1f pending_adds_avg[soft_single=%.1f soft_batch=%.1f rigid_single=%.1f]\n",
          prepAvgMS,
          writeKinematicAvgMS,
          pinnedUpdateAvgMS,
          deferredAvgMS,
          fhSpringAvgMS,
          simulationTickAvgMS,
          syncMotionAvgMS,
          characterAvgMS,
          vehicleAvgMS,
          callbackAvgMS,
          controllersAvg,
          deferredQueuedAvg,
          contactPairsAvg,
          pendingSoftSingleAddsAvg,
          pendingSoftBatchAddsAvg,
          pendingRigidSingleAddsAvg);

      printf(
          "JoltPerfProbeSoftBody: calls=%zu frame_avg=%.3fms call_avg=%.3fms "
          "call_breakdown[depsgraph=%.3fms filter=%.3fms mesh=%.3fms rel_tag=%.3fms] "
          "counts_per_call[total=%.1f candidates=%.1f updated=%.1f]\n",
          m_perfProbeUpdateSoftBodiesCallsAccum,
          updateSoftBodiesFrameAvgMS,
          updateSoftBodiesCallAvgMS,
          updateSoftBodiesDepsgraphCallAvgMS,
          updateSoftBodiesFilterCallAvgMS,
          updateSoftBodiesMeshCallAvgMS,
          updateSoftBodiesRelTagCallAvgMS,
          softBodiesTotalAvg,
          softBodiesCandidatesAvg,
          softBodiesUpdatedAvg);

      m_perfProbeFramesAccum = 0;
      m_perfProbeStepUSAccum = 0.0;
      m_perfProbeUpdateUSAccum = 0.0;
      m_perfProbeBroadphaseUSAccum = 0.0;
      m_perfProbeBreakCheckUSAccum = 0.0;
      m_perfProbePrepUSAccum = 0.0;
      m_perfProbeWriteKinematicUSAccum = 0.0;
      m_perfProbePinnedUpdateUSAccum = 0.0;
      m_perfProbeDeferredUSAccum = 0.0;
      m_perfProbeFhSpringUSAccum = 0.0;
      m_perfProbeSimulationTickUSAccum = 0.0;
      m_perfProbeSyncMotionUSAccum = 0.0;
      m_perfProbeCharacterUSAccum = 0.0;
      m_perfProbeVehicleUSAccum = 0.0;
      m_perfProbeCallbackUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesDepsgraphUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesFilterUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesMeshUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesRelTagUSAccum = 0.0;
      m_perfProbeBroadphaseCallsAccum = 0;
      m_perfProbeBroadphaseStartupCallsAccum = 0;
      m_perfProbeBroadphaseSoftSingleCallsAccum = 0;
      m_perfProbeBroadphaseSoftBatchCallsAccum = 0;
      m_perfProbeBroadphaseRigidSingleCallsAccum = 0;
      m_perfProbeBroadphaseOtherCallsAccum = 0;
      m_perfProbeBreakableCheckedAccum = 0;
      m_perfProbeBrokenAccum = 0;
      m_perfProbeDeferredOpsQueuedAccum = 0;
      m_perfProbeControllersAccum = 0;
      m_perfProbeContactPairsAccum = 0;
      m_perfProbePendingSoftSingleAddsAccum = 0;
      m_perfProbePendingSoftBatchAddsAccum = 0;
      m_perfProbePendingRigidSingleAddsAccum = 0;
      m_perfProbeUpdateSoftBodiesCallsAccum = 0;
      m_perfProbeSoftBodiesTotalAccum = 0;
      m_perfProbeSoftBodiesCandidatesAccum = 0;
      m_perfProbeSoftBodiesUpdatedAccum = 0;
    }
  }

  return true;
}

blender::Depsgraph *JoltPhysicsEnvironment::GetDepsgraph()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine == nullptr) {
    return nullptr;
  }

  blender::bContext *C = engine->GetContext();
  if (C == nullptr) {
    return nullptr;
  }

  blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  }

  return depsgraph;
}

blender::EffectorWeights *JoltPhysicsEnvironment::GetEffectorWeights()
{
  if (m_blenderScene == nullptr) {
    return nullptr;
  }

  blender::RigidBodyWorld *rbw = m_blenderScene->rigidbody_world;
  blender::EffectorWeights *weights = rbw ? rbw->effector_weights : nullptr;

  if (weights == nullptr) {
    if (m_fallbackEffectorWeights == nullptr) {
      m_fallbackEffectorWeights = blender::BKE_effector_add_weights(nullptr);
    }
    weights = m_fallbackEffectorWeights;
  }

  return weights;
}

void JoltPhysicsEnvironment::ApplyEffectorForces()
{
  if (m_blenderScene == nullptr) {
    return;
  }

  blender::Depsgraph *depsgraph = GetDepsgraph();
  if (depsgraph == nullptr) {
    return;
  }

  blender::EffectorWeights *effectorWeights = GetEffectorWeights();
  if (effectorWeights == nullptr) {
    return;
  }

  blender::ListBaseT<blender::EffectorCache> *effectors =
      blender::BKE_effectors_create(depsgraph, nullptr, nullptr, effectorWeights, false);
  if (effectors == nullptr) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    if (ctrl->GetSoftBody()) {
      continue;
    }

    const JPH::BodyID bodyID = ctrl->GetBodyID();
    if (bodyID.IsInvalid() || !bi.IsAdded(bodyID)) {
      continue;
    }

    JPH::RVec3 bodyPos;
    JPH::Vec3 bodyLinVel;
    {
      JPH::BodyLockRead bodyLock(m_physicsSystem->GetBodyLockInterface(), bodyID);
      if (!bodyLock.Succeeded()) {
        continue;
      }

      const JPH::Body &body = bodyLock.GetBody();
      if (!body.IsDynamic() || body.GetMotionProperties() == nullptr) {
        continue;
      }

      if (body.GetMotionProperties()->GetInverseMass() <= 0.0f) {
        continue;
      }

      bodyPos = body.GetCenterOfMassPosition();
      bodyLinVel = body.GetLinearVelocity();
    }

    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(ctrl->GetNewClientInfo());
    if (info == nullptr) {
      continue;
    }

    KX_GameObject *gameobj = KX_GameObject::GetClientObject(info);
    if (gameobj == nullptr) {
      continue;
    }

    blender::Object *blenderobj = gameobj->GetBlenderObject();
    if (blenderobj == nullptr) {
      continue;
    }

    if (blenderobj->pd && blenderobj->pd->forcefield != blender::PFIELD_NULL) {
      continue;
    }

    /* Force-field sampling must track the game-object origin used by gameplay
     * transforms/spawn logic. For hidden template replicas with non-unit scale,
     * Jolt body COM can diverge from that origin after shape scaling, which can
     * move effector sampling outside field falloff and yield zero force. */
    PHY_IMotionState *motionState = ctrl->GetMotionState();
    const MT_Vector3 worldPos = motionState ? motionState->GetWorldPosition() :
                                              JoltMath::ToMT(bodyPos);
    const MT_Vector3 worldVel = JoltMath::ToMT(bodyLinVel);

    float effLoc[3] = {float(worldPos.x()), float(worldPos.y()), float(worldPos.z())};
    float effVel[3] = {float(worldVel.x()), float(worldVel.y()), float(worldVel.z())};

    blender::EffectedPoint effectedPoint;
    blender::pd_point_from_loc(m_blenderScene, effLoc, effVel, 0, &effectedPoint);

    float effForce[3] = {0.0f, 0.0f, 0.0f};
    blender::BKE_effectors_apply(
        effectors, nullptr, effectorWeights, &effectedPoint, effForce, nullptr, nullptr);

    if (!blender::is_zero_v3(effForce)) {
      bi.AddForce(bodyID,
                  JoltMath::ToJolt(effForce[0], effForce[1], effForce[2]),
                  JPH::EActivation::Activate);
    }
  }

  blender::BKE_effectors_free(effectors);
}

void JoltPhysicsEnvironment::RegisterControllerForObject(blender::Object *obj,
                                                         JoltPhysicsController *ctrl)
{
  if (obj) {
    m_controllerByBlenderObject[obj] = ctrl;
  }
}

JoltPhysicsController *JoltPhysicsEnvironment::FindControllerByBlenderObject(blender::Object *obj)
{
  if (!obj) {
    return nullptr;
  }
  auto it = m_controllerByBlenderObject.find(obj);
  return (it != m_controllerByBlenderObject.end()) ? it->second : nullptr;
}

void JoltPhysicsEnvironment::AddSoftBodyReplica(JoltSoftBody *sb,
                                                JoltPhysicsController *pinCtrl)
{
  if (!sb) {
    return;
  }

  /* Track in the soft body update list. */
  m_softBodies.push_back(sb);
  if (sb->HasPinnedVertices()) {
    m_pinnedSoftBodies.push_back(sb);
  }
  if (sb->UsesContactPlasticity()) {
    m_softBodyContactListener.RegisterPlasticityContactBody(sb->GetBodyID());
  }

  /* Register no-pin-collision if the flag is set and pin body is valid. */
  if (pinCtrl && sb->GetNoPinCollision() && !pinCtrl->GetBodyID().IsInvalid()) {
    m_softBodyContactListener.Register(sb->GetBodyID(), pinCtrl->GetBodyID());
  }
}

void JoltPhysicsEnvironment::QueueRigidBodyBodyAdd(JPH::BodyID bodyID,
                                                   JPH::EActivation activation)
{
  if (bodyID.IsInvalid()) {
    return;
  }

  PendingBodyAddEntry entry;
  entry.bodyID = bodyID;
  entry.activation = activation;
  m_pendingRigidBodyBodyAdds.push_back(entry);
}

void JoltPhysicsEnvironment::RemovePendingRigidBodyBodyAdd(JPH::BodyID bodyID)
{
  if (bodyID.IsInvalid() || m_pendingRigidBodyBodyAdds.empty()) {
    return;
  }

  auto newEnd = std::remove_if(
      m_pendingRigidBodyBodyAdds.begin(),
      m_pendingRigidBodyBodyAdds.end(),
      [bodyID](const PendingBodyAddEntry &entry) {
        return entry.bodyID == bodyID;
      });
  if (newEnd != m_pendingRigidBodyBodyAdds.end()) {
    m_pendingRigidBodyBodyAdds.erase(newEnd, m_pendingRigidBodyBodyAdds.end());
  }
}

void JoltPhysicsEnvironment::QueuePendingCompoundChildShape(JoltPhysicsController *parentCtrl,
                                                            KX_GameObject *childObject,
                                                            const JPH::Vec3 &relativePos,
                                                            const JPH::Quat &relativeRot,
                                                            JPH::RefConst<JPH::Shape> childShape,
                                                            JoltShapeQueryDataPtr childQueryData)
{
  if (!parentCtrl || !childShape) {
    return;
  }

  std::vector<PendingCompoundSubShapeEntry> &pendingSubShapes =
      m_pendingCompoundSubShapesByController[parentCtrl];

  if (pendingSubShapes.empty()) {
    JPH::RefConst<JPH::Shape> parentShape = parentCtrl->GetShape();
    if (!parentShape) {
      m_pendingCompoundSubShapesByController.erase(parentCtrl);
      return;
    }

    if (const JPH::StaticCompoundShape *existingCompound = JoltAsStaticCompoundShape(
            parentShape.GetPtr())) {
      pendingSubShapes.reserve(existingCompound->GetNumSubShapes() + 1);
      for (JPH::uint i = 0; i < existingCompound->GetNumSubShapes(); ++i) {
        const JPH::CompoundShape::SubShape &sub = existingCompound->GetSubShape(i);
        PendingCompoundSubShapeEntry entry;
        entry.position = JoltCompoundSubShapeLocalPosition(*existingCompound, sub);
        entry.rotation = sub.GetRotation();
        entry.shape = sub.mShape;
        entry.userData = sub.mUserData;
        pendingSubShapes.push_back(entry);
      }
    }
    else {
      PendingCompoundSubShapeEntry rootEntry;
      rootEntry.position = JPH::Vec3::sZero();
      rootEntry.rotation = JPH::Quat::sIdentity();
      rootEntry.shape = parentShape;
      pendingSubShapes.push_back(rootEntry);
    }
  }

  PendingCompoundSubShapeEntry childEntry;
  childEntry.position = relativePos;
  childEntry.rotation = relativeRot;
  childEntry.shape = childShape;
  childEntry.queryData = std::move(childQueryData);
  childEntry.userData = parentCtrl->RegisterCompoundChildBinding(
      childObject, relativePos, relativeRot);
  pendingSubShapes.push_back(childEntry);
}

void JoltPhysicsEnvironment::FinalizePendingCompoundShapeBuilds()
{
  if (m_pendingCompoundSubShapesByController.empty()) {
    return;
  }

  for (auto &pair : m_pendingCompoundSubShapesByController) {
    JoltPhysicsController *parentCtrl = pair.first;
    std::vector<PendingCompoundSubShapeEntry> &subShapes = pair.second;
    if (!parentCtrl || subShapes.empty()) {
      continue;
    }

    const JPH::BodyID parentBodyID = parentCtrl->GetBodyID();
    if (parentBodyID.IsInvalid()) {
      continue;
    }

    JPH::RefConst<JPH::Shape> parentShapeAtFinalize = parentCtrl->GetShape();
    if (!parentShapeAtFinalize) {
      continue;
    }

    size_t baseShapeCount = 1;
    if (const JPH::StaticCompoundShape *existingCompound = JoltAsStaticCompoundShape(
            parentShapeAtFinalize.GetPtr())) {
      baseShapeCount = (size_t)existingCompound->GetNumSubShapes();
    }
    if (baseShapeCount > subShapes.size()) {
      baseShapeCount = subShapes.size();
    }

    JPH::StaticCompoundShapeSettings compoundSettings;
    JoltShapeQueryDataPtr combinedQueryData = parentCtrl->GetShapeQueryData();
    for (const PendingCompoundSubShapeEntry &entry : subShapes) {
      if (!entry.shape) {
        continue;
      }
      compoundSettings.AddShape(entry.position, entry.rotation, entry.shape, entry.userData);
      combinedQueryData = JoltMergeShapeQueryData(combinedQueryData, entry.queryData);
    }

    JPH::Shape::ShapeResult result = compoundSettings.Create();
    if (result.HasError()) {
      /* Fallback: preserve previous behavior by trying incremental child appends,
       * so an invalid late child does not discard all previously valid children. */
      JPH::RefConst<JPH::Shape> fallbackShape = parentShapeAtFinalize;
      JoltShapeQueryDataPtr fallbackQueryData = parentCtrl->GetShapeQueryData();
      for (size_t childIndex = baseShapeCount; childIndex < subShapes.size(); ++childIndex) {
        const PendingCompoundSubShapeEntry &childEntry = subShapes[childIndex];
        if (!childEntry.shape) {
          continue;
        }

        JPH::StaticCompoundShapeSettings incrementalSettings;
        JoltAddExistingShapeToCompoundSettings(incrementalSettings, fallbackShape.GetPtr());

        incrementalSettings.AddShape(childEntry.position,
                                     childEntry.rotation,
                                     childEntry.shape,
                                     childEntry.userData);
        JPH::Shape::ShapeResult incrementalResult = incrementalSettings.Create();
        if (incrementalResult.HasError()) {
          continue;
        }

        fallbackShape = incrementalResult.Get();
        fallbackQueryData = JoltMergeShapeQueryData(fallbackQueryData,
                                                     childEntry.queryData);
        parentCtrl->SetShapePreservingMassPropertiesAndCenterOfMass(fallbackShape,
                                                                    fallbackQueryData);
      }
      continue;
    }

    JPH::RefConst<JPH::Shape> newCompound = result.Get();
    parentCtrl->SetShapePreservingMassPropertiesAndCenterOfMass(newCompound,
                                                                combinedQueryData);
  }

  m_pendingCompoundSubShapesByController.clear();
}

void JoltPhysicsEnvironment::FlushPendingRigidBodyBodyAdds()
{
  if (m_pendingRigidBodyBodyAdds.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();

  auto flushRun = [&](std::vector<JPH::BodyID> &runIDs,
                      std::unordered_set<JPH::uint32> &runIDKeys,
                      JPH::EActivation activation) {
    if (runIDs.empty()) {
      return;
    }

    JPH::BodyID *bodyIDs = runIDs.data();
    const int bodyCount = (int)runIDs.size();
    JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(bodyIDs, bodyCount);
    if (addState) {
      bi.AddBodiesFinalize(bodyIDs, bodyCount, addState, activation);
    }
    else {
      /* Fallback path: preserve behavior if batched prepare fails. */
      for (int i = 0; i < bodyCount; ++i) {
        bi.AddBody(bodyIDs[i], activation);
      }

      /* Fallback used one-by-one insertion. Track this as a runtime add burst
       * so broadphase optimize can happen on the normal threshold/idle policy. */
      m_pendingRigidBodyAddsForOptimize += bodyCount;
      m_rigidBodyAddsSinceLastStep += bodyCount;
    }

    runIDs.clear();
    runIDKeys.clear();
  };

  std::vector<JPH::BodyID> runIDs;
  std::unordered_set<JPH::uint32> runIDKeys;
  runIDs.reserve(m_pendingRigidBodyBodyAdds.size());
  runIDKeys.reserve(m_pendingRigidBodyBodyAdds.size());
  JPH::EActivation runActivation = JPH::EActivation::DontActivate;
  bool hasRun = false;

  for (const PendingBodyAddEntry &entry : m_pendingRigidBodyBodyAdds) {
    if (entry.bodyID.IsInvalid() || bi.IsAdded(entry.bodyID)) {
      continue;
    }

    if (!hasRun) {
      runActivation = entry.activation;
      hasRun = true;
    }
    else if (entry.activation != runActivation) {
      flushRun(runIDs, runIDKeys, runActivation);
      runActivation = entry.activation;
    }

    if (runIDKeys.insert(entry.bodyID.GetIndexAndSequenceNumber()).second) {
      runIDs.push_back(entry.bodyID);
    }
  }

  if (hasRun) {
    flushRun(runIDs, runIDKeys, runActivation);
  }

  m_pendingRigidBodyBodyAdds.clear();
}

void JoltPhysicsEnvironment::QueueSoftBodyBodyAdd(JPH::BodyID bodyID)
{
  if (!bodyID.IsInvalid()) {
    m_pendingSoftBodyBodyAdds.push_back(bodyID);
  }
}

void JoltPhysicsEnvironment::NotifySoftBodyBodyAdded()
{
  ++m_pendingSoftBodyAddsForOptimize;
  ++m_softBodyAddsSinceLastStep;
}

void JoltPhysicsEnvironment::NotifyRigidBodyBodyAdded()
{
  ++m_pendingRigidBodyAddsForOptimize;
  ++m_rigidBodyAddsSinceLastStep;
}

void JoltPhysicsEnvironment::RequestSoftBodyRelationsTagUpdate()
{
  m_softBodyRelationsTagDirty = true;
}

void JoltPhysicsEnvironment::NotifyConstraintBreakingThresholdChanged()
{
  m_breakableConstraintsCacheDirty = true;
}

void JoltPhysicsEnvironment::NotifyConstraintTopologyChanged(JPH::BodyID bodyID1,
                                                             JPH::BodyID bodyID2)
{
  if (!bodyID1.IsInvalid()) {
    m_constraintTopologyDirtyBodies.push_back(bodyID1);
  }
  if (!bodyID2.IsInvalid()) {
    m_constraintTopologyDirtyBodies.push_back(bodyID2);
  }
}

void JoltPhysicsEnvironment::NotifyConstraintBodyMotionTypeChanged(JPH::BodyID bodyID)
{
  if (bodyID.IsInvalid()) {
    return;
  }

  for (const auto &pair : m_constraintById) {
    JoltConstraint *wrapper = pair.second;
    JPH::Constraint *constraint = wrapper ? wrapper->GetConstraint() : nullptr;
    if (!constraint || constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint) {
      continue;
    }

    JPH::TwoBodyConstraint *twoBody = static_cast<JPH::TwoBodyConstraint *>(constraint);
    const JPH::BodyID bodyID1 = twoBody->GetBody1()->GetID();
    const JPH::BodyID bodyID2 = twoBody->GetBody2()->GetID();
    if (bodyID1 == bodyID || bodyID2 == bodyID) {
      NotifyConstraintTopologyChanged(bodyID1, bodyID2);
    }
  }
}

void JoltPhysicsEnvironment::ProcessPendingConstraintTopologyChanges()
{
  if (m_constraintTopologyDirtyBodies.empty()) {
    return;
  }

  m_constraintAdjacencyScratch.clear();
  m_constraintAdjacencyScratch.reserve(2 * m_constraintById.size());

  for (const auto &pair : m_constraintById) {
    JoltConstraint *wrapper = pair.second;
    JPH::Constraint *constraint = wrapper ? wrapper->GetConstraint() : nullptr;
    if (!constraint) {
      continue;
    }

    if (!constraint->GetEnabled() ||
        constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
    {
      constraint->SetConstraintPriority(0);
      continue;
    }

    JPH::TwoBodyConstraint *twoBody = static_cast<JPH::TwoBodyConstraint *>(constraint);
    JPH::Body *body1 = twoBody->GetBody1();
    JPH::Body *body2 = twoBody->GetBody2();
    if (body1 && !body1->IsStatic()) {
      m_constraintAdjacencyScratch.emplace(body1->GetID().GetIndexAndSequenceNumber(), wrapper);
    }
    if (body2 && !body2->IsStatic()) {
      m_constraintAdjacencyScratch.emplace(body2->GetID().GetIndexAndSequenceNumber(), wrapper);
    }
  }

  m_constraintProcessedBodyKeysScratch.clear();
  m_constraintProcessedBodyKeysScratch.reserve(m_constraintTopologyDirtyBodies.size());

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  for (const JPH::BodyID bodyID : m_constraintTopologyDirtyBodies) {
    {
      JPH::BodyLockRead lock(lockInterface, bodyID);
      if (!lock.Succeeded() || lock.GetBody().IsStatic()) {
        continue;
      }
    }

    const JPH::uint32 seedKey = bodyID.GetIndexAndSequenceNumber();
    if (m_constraintProcessedBodyKeysScratch.find(seedKey) !=
        m_constraintProcessedBodyKeysScratch.end())
    {
      continue;
    }

    m_constraintComponentBodiesScratch.clear();
    m_constraintComponentBodyKeysScratch.clear();
    m_constraintComponentConstraintsScratch.clear();
    m_constraintComponentBodyKeysScratch.reserve(16);
    m_constraintComponentConstraintsScratch.reserve(16);
    m_constraintComponentBodiesScratch.push_back(bodyID);
    m_constraintComponentBodyKeysScratch.insert(seedKey);

    auto enqueue_body = [&](const JPH::Body *body) {
      if (!body || body->IsStatic()) {
        return;
      }
      const JPH::BodyID adjacentBodyID = body->GetID();
      const JPH::uint32 bodyKey = adjacentBodyID.GetIndexAndSequenceNumber();
      if (m_constraintComponentBodyKeysScratch.insert(bodyKey).second) {
        m_constraintComponentBodiesScratch.push_back(adjacentBodyID);
      }
    };

    for (size_t bodyIndex = 0; bodyIndex < m_constraintComponentBodiesScratch.size(); ++bodyIndex)
    {
      const JPH::uint32 bodyKey =
          m_constraintComponentBodiesScratch[bodyIndex].GetIndexAndSequenceNumber();
      const auto adjacent = m_constraintAdjacencyScratch.equal_range(bodyKey);
      for (auto it = adjacent.first; it != adjacent.second; ++it) {
        JoltConstraint *wrapper = it->second;
        if (!m_constraintComponentConstraintsScratch.insert(wrapper).second) {
          continue;
        }

        JPH::Constraint *constraint = wrapper->GetConstraint();
        constraint->ResetWarmStart();
        JPH::TwoBodyConstraint *twoBody = static_cast<JPH::TwoBodyConstraint *>(constraint);
        enqueue_body(twoBody->GetBody1());
        enqueue_body(twoBody->GetBody2());
      }
    }

    m_constraintPriorityAdjacencyScratch.clear();
    m_constraintPriorityBodiesScratch.clear();
    m_constraintPriorityParentsScratch.clear();
    m_constraintPriorityParentConstraintsScratch.clear();
    m_constraintPriorityHeightsScratch.clear();
    m_constraintPriorityTraversalScratch.clear();
    m_constraintPriorityAdjacencyScratch.reserve(2 *
                                                 m_constraintComponentConstraintsScratch.size());
    m_constraintPriorityBodiesScratch.reserve(m_constraintComponentBodiesScratch.size() + 1);
    m_constraintPriorityParentsScratch.reserve(m_constraintComponentBodiesScratch.size() + 1);
    m_constraintPriorityParentConstraintsScratch.reserve(
        m_constraintComponentBodiesScratch.size());
    m_constraintPriorityHeightsScratch.reserve(m_constraintComponentBodiesScratch.size() + 1);
    m_constraintPriorityTraversalScratch.reserve(m_constraintComponentBodiesScratch.size() + 1);

    /* Jolt solves lower priorities first. A unique static or kinematic root lets a tree be
     * solved from its leaves toward that root; all ambiguous topologies stay neutral. */
    const JPH::Body *anchor = nullptr;
    bool ambiguousAnchor = false;
    for (JoltConstraint *wrapper : m_constraintComponentConstraintsScratch) {
      JPH::TwoBodyConstraint *constraint = static_cast<JPH::TwoBodyConstraint *>(
          wrapper->GetConstraint());
      constraint->SetConstraintPriority(0);
      const JPH::Body *bodies[2] = {constraint->GetBody1(), constraint->GetBody2()};
      for (const JPH::Body *body : bodies) {
        m_constraintPriorityBodiesScratch.insert(body);
        m_constraintPriorityAdjacencyScratch.emplace(body, wrapper);
        if (!body->IsDynamic()) {
          if (!anchor) {
            anchor = body;
          }
          else if (anchor != body) {
            ambiguousAnchor = true;
          }
        }
      }
    }

    const bool isTree = m_constraintComponentConstraintsScratch.size() + 1 ==
                        m_constraintPriorityBodiesScratch.size();
    if (anchor && !ambiguousAnchor && isTree) {
      m_constraintPriorityParentsScratch.emplace(anchor, nullptr);
      m_constraintPriorityTraversalScratch.push_back(anchor);

      for (size_t bodyIndex = 0; bodyIndex < m_constraintPriorityTraversalScratch.size();
           ++bodyIndex)
      {
        const JPH::Body *body = m_constraintPriorityTraversalScratch[bodyIndex];
        const auto adjacent = m_constraintPriorityAdjacencyScratch.equal_range(body);
        for (auto it = adjacent.first; it != adjacent.second; ++it) {
          JoltConstraint *wrapper = it->second;
          JPH::TwoBodyConstraint *constraint = static_cast<JPH::TwoBodyConstraint *>(
              wrapper->GetConstraint());
          const JPH::Body *other = constraint->GetBody1() == body ? constraint->GetBody2() :
                                                                    constraint->GetBody1();
          if (m_constraintPriorityParentsScratch.emplace(other, body).second) {
            m_constraintPriorityParentConstraintsScratch.emplace(other, wrapper);
            m_constraintPriorityTraversalScratch.push_back(other);
          }
        }
      }

      if (m_constraintPriorityTraversalScratch.size() == m_constraintPriorityBodiesScratch.size())
      {
        for (auto it = m_constraintPriorityTraversalScratch.rbegin();
             it != m_constraintPriorityTraversalScratch.rend();
             ++it)
        {
          const JPH::Body *body = *it;
          const auto parentConstraint = m_constraintPriorityParentConstraintsScratch.find(body);
          if (parentConstraint == m_constraintPriorityParentConstraintsScratch.end()) {
            continue;
          }

          const JPH::uint32 priority = m_constraintPriorityHeightsScratch[body] + 1;
          parentConstraint->second->GetConstraint()->SetConstraintPriority(priority);
          const JPH::Body *parent = m_constraintPriorityParentsScratch[body];
          JPH::uint32 &parentHeight = m_constraintPriorityHeightsScratch[parent];
          parentHeight = std::max(parentHeight, priority);
        }
      }
    }

    JPH::BodyInterface &bodyInterface = GetBodyInterface();
    for (const JPH::BodyID componentBodyID : m_constraintComponentBodiesScratch) {
      m_constraintProcessedBodyKeysScratch.insert(componentBodyID.GetIndexAndSequenceNumber());
      bodyInterface.ActivateBody(componentBodyID);
    }
  }

  m_constraintTopologyDirtyBodies.clear();
}

void JoltPhysicsEnvironment::RebuildBreakableConstraintCache()
{
  m_breakableConstraintsCache.clear();
  m_breakableConstraintsCache.reserve(m_constraintById.size());

  for (const auto &pair : m_constraintById) {
    JoltConstraint *constraint = pair.second;
    if (constraint && constraint->GetBreakingThreshold() < FLT_MAX) {
      m_breakableConstraintsCache.push_back(constraint);
    }
  }

  m_breakableConstraintsCacheDirty = false;
}

void JoltPhysicsEnvironment::FlushPendingSoftBodyBodyAdds()
{
  if (m_pendingSoftBodyBodyAdds.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
  JPH::BodyID *bodyIDs = m_pendingSoftBodyBodyAdds.data();
  const int bodyCount = (int)m_pendingSoftBodyBodyAdds.size();

  JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(bodyIDs, bodyCount);
  if (addState) {
    bi.AddBodiesFinalize(bodyIDs, bodyCount, addState, JPH::EActivation::Activate);
    m_pendingSoftBodyBatchAddsForOptimize += bodyCount;
  }
  else {
    /* Fallback path (defensive): preserve behavior even if AddBodiesPrepare fails. */
    for (int i = 0; i < bodyCount; ++i) {
      bi.AddBody(bodyIDs[i], JPH::EActivation::Activate);
    }
    m_pendingSoftBodyAddsForOptimize += bodyCount;
  }

  m_softBodyAddsSinceLastStep += bodyCount;
  m_pendingSoftBodyBodyAdds.clear();
}

void JoltPhysicsEnvironment::FlushPendingSoftBodyBodyRemoves()
{
  if (m_pendingSoftBodyBodyRemoves.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
  std::vector<JPH::BodyID> addedBodyIDs;
  std::vector<JPH::BodyID> destroyBodyIDs;
  addedBodyIDs.reserve(m_pendingSoftBodyBodyRemoves.size());
  destroyBodyIDs.reserve(m_pendingSoftBodyBodyRemoves.size());

  for (const JPH::BodyID &bodyID : m_pendingSoftBodyBodyRemoves) {
    if (bodyID.IsInvalid()) {
      continue;
    }

    destroyBodyIDs.push_back(bodyID);
    if (bi.IsAdded(bodyID)) {
      addedBodyIDs.push_back(bodyID);
    }
  }

  for (const JPH::BodyID bodyID : destroyBodyIDs) {
    RemoveLogicActiveContactsForBody(bodyID);
  }
  if (!addedBodyIDs.empty()) {
    bi.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
  }
  if (!destroyBodyIDs.empty()) {
    bi.DestroyBodies(destroyBodyIDs.data(), (int)destroyBodyIDs.size());
  }

  m_pendingSoftBodyBodyRemoves.clear();
}

void JoltPhysicsEnvironment::RemoveSoftBody(JoltSoftBody *sb)
{
  if (!sb) {
    return;
  }

  const JPH::BodyID sbBodyID = sb->GetBodyID();
  if (!sbBodyID.IsInvalid()) {
    m_softBodyContactListener.Unregister(sbBodyID);
    m_softBodyContactListener.UnregisterPlasticityContactBody(sbBodyID);
  }

  JoltPhysicsController *ctrl = sb->GetController();
  if (ctrl) {
    auto ctrlIt = m_controllers.find(ctrl);
    if (ctrlIt != m_controllers.end() && ctrl->GetSoftBody() == sb) {
      ctrl->SetSoftBody(nullptr);
      if (ctrl->GetBodyID() == sbBodyID) {
        ctrl->SetBodyID(JPH::BodyID());
      }
    }
  }

  auto it = std::find(m_softBodies.begin(), m_softBodies.end(), sb);
  if (it != m_softBodies.end()) {
    m_softBodies.erase(it);
  }

  auto pinnedIt = std::find(m_pinnedSoftBodies.begin(), m_pinnedSoftBodies.end(), sb);
  if (pinnedIt != m_pinnedSoftBodies.end()) {
    m_pinnedSoftBodies.erase(pinnedIt);
  }

  if (!sbBodyID.IsInvalid()) {
    auto pendingBegin = m_pendingSoftBodyBodyAdds.begin();
    auto pendingEnd = std::remove(pendingBegin, m_pendingSoftBodyBodyAdds.end(), sbBodyID);
    if (pendingEnd != m_pendingSoftBodyBodyAdds.end()) {
      m_pendingSoftBodyBodyAdds.erase(pendingEnd, m_pendingSoftBodyBodyAdds.end());
    }

    auto removeBegin = m_pendingSoftBodyBodyRemoves.begin();
    auto removeEnd = std::remove(removeBegin, m_pendingSoftBodyBodyRemoves.end(), sbBodyID);
    if (removeEnd != m_pendingSoftBodyBodyRemoves.end()) {
      m_pendingSoftBodyBodyRemoves.erase(removeEnd, m_pendingSoftBodyBodyRemoves.end());
    }
    m_pendingSoftBodyBodyRemoves.push_back(sbBodyID);

    if (!m_isPhysicsUpdating) {
      JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
      if (bi.IsAdded(sbBodyID)) {
        RemoveLogicActiveContactsForBody(sbBodyID);
        bi.RemoveBody(sbBodyID);
      }
    }
  }

  sb->SetBodyDestructionHandledByEnvironment(true);
  delete sb;
}

void JoltPhysicsEnvironment::FinalizeSoftBodyPins()
{
  /* Startup conversion can queue rigid adds/compound updates before this method
   * is called from BL_DataConversion. Finalize them now to preserve startup
   * behavior while still batching conversion work. */
  FinalizePendingCompoundShapeBuilds();
  FlushPendingRigidBodyBodyAdds();

  m_pinnedSoftBodies.clear();

  for (JoltSoftBody *sb : m_softBodies) {
    if (!sb->HasPinnedVertices()) {
      continue;
    }
    m_pinnedSoftBodies.push_back(sb);

    /* Retrieve the pin object controller from the Blender object pointer stored during
     * ConvertObject. The lookup is safe even if pin_object has no physics body. */
    blender::Object *pinObj = sb->GetPinBlenderObject();
    if (pinObj) {
      JoltPhysicsController *pinCtrl = FindControllerByBlenderObject(pinObj);
      sb->SetPinController(pinCtrl); /* may be null — fixed-world-space pinning still works */

      /* If the "No Force on Pin Object" flag is set and the pin object has a
       * physics body, register the pair so the soft body contact listener can
       * zero out impulses transferred to the pin body. */
      if (pinCtrl && sb->GetNoPinCollision() && !pinCtrl->GetBodyID().IsInvalid()) {
        m_softBodyContactListener.Register(sb->GetBodyID(), pinCtrl->GetBodyID());
      }
    }
  }
}

void JoltPhysicsEnvironment::UpdateSoftBodies()
{
  using PerfClock = std::chrono::steady_clock;
  const bool perfProbeEnabled = m_perfProbeEnabled;
  const PerfClock::time_point updateSoftBodiesStart =
      perfProbeEnabled ? PerfClock::now() : PerfClock::time_point();
  double perfDepsgraphUS = 0.0;
  double perfFilterUS = 0.0;
  double perfMeshUS = 0.0;
  double perfRelTagUS = 0.0;
  size_t softBodiesTotal = m_softBodies.size();
  size_t softBodiesCandidates = 0;
  size_t softBodiesUpdated = 0;

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *C = engine ? engine->GetContext() : nullptr;

  /* Fast path: no soft-body work and no pending relations refresh request. */
  if (m_softBodies.empty() && !m_softBodyRelationsTagDirty) {
    if (perfProbeEnabled) {
      const double updateSoftBodiesUS =
          (double)std::chrono::duration_cast<std::chrono::microseconds>(
              PerfClock::now() - updateSoftBodiesStart)
              .count();

      m_perfProbeUpdateSoftBodiesUSAccum += updateSoftBodiesUS;
      ++m_perfProbeUpdateSoftBodiesCallsAccum;
      m_perfProbeSoftBodiesTotalAccum += softBodiesTotal;
    }
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();

  /* Build candidate list first. This avoids depsgraph lookup entirely when
   * no soft body needs mesh update this frame. */
  PerfClock::time_point filterStart;
  if (perfProbeEnabled) {
    filterStart = PerfClock::now();
  }

  m_softBodiesToMeshUpdateScratch.clear();
  m_softBodyMeshUpdateIDsScratch.clear();
  m_softBodiesToMeshUpdateScratch.reserve(m_softBodies.size());
  m_softBodyMeshUpdateIDsScratch.reserve(m_softBodies.size());

  for (JoltSoftBody *sb : m_softBodies) {
    if (!sb) {
      continue;
    }
    if (!sb->IsActive()) {
      continue;
    }
    const JPH::BodyID bodyID = sb->GetBodyID();
    if (bodyID.IsInvalid()) {
      continue;
    }
    if (!bi.IsAdded(bodyID)) {
      continue;
    }
    if (!bi.IsActive(bodyID) && !sb->HasPinnedVertices() && sb->HasMeshUpload()) {
      continue;
    }

    m_softBodiesToMeshUpdateScratch.push_back(sb);
    m_softBodyMeshUpdateIDsScratch.push_back(bodyID);
  }

  softBodiesCandidates = m_softBodyMeshUpdateIDsScratch.size();

  if (perfProbeEnabled) {
    perfFilterUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - filterStart)
                       .count();
  }

  blender::Depsgraph *depsgraph = nullptr;
  if (!m_softBodyMeshUpdateIDsScratch.empty()) {
    PerfClock::time_point depsgraphStart;
    if (perfProbeEnabled) {
      depsgraphStart = PerfClock::now();
    }

    if (C) {
      depsgraph = CTX_data_depsgraph_pointer(C);
      if (!depsgraph) {
        depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      }
    }

    if (perfProbeEnabled) {
      perfDepsgraphUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - depsgraphStart)
                           .count();
    }

    if (depsgraph) {
      PerfClock::time_point meshStart;
      if (perfProbeEnabled) {
        meshStart = PerfClock::now();
      }

      const JPH::BodyLockInterface &lockIf =
          m_physicsSystem->GetBodyLockInterfaceNoLock();
      JPH::BodyLockMultiRead multiLock(lockIf,
                                       m_softBodyMeshUpdateIDsScratch.data(),
                                       (int)m_softBodyMeshUpdateIDsScratch.size());

      for (int i = 0; i < (int)m_softBodyMeshUpdateIDsScratch.size(); ++i) {
        const JPH::Body *body = multiLock.GetBody(i);
        if (!body) {
          continue;
        }
        m_softBodiesToMeshUpdateScratch[(size_t)i]->UpdateMeshLocked(depsgraph, *body);
        ++softBodiesUpdated;
      }

      if (perfProbeEnabled) {
        perfMeshUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - meshStart)
                        .count();
      }
    }
  }

  if (m_softBodyRelationsTagDirty && C) {
    PerfClock::time_point relTagStart;
    if (perfProbeEnabled) {
      relTagStart = PerfClock::now();
    }

    DEG_relations_tag_update(CTX_data_main(C));
    m_softBodyRelationsTagDirty = false;

    if (perfProbeEnabled) {
      perfRelTagUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                          PerfClock::now() - relTagStart)
                          .count();
    }
  }

  if (perfProbeEnabled) {
    const double updateSoftBodiesUS =
        (double)std::chrono::duration_cast<std::chrono::microseconds>(
            PerfClock::now() - updateSoftBodiesStart)
            .count();

    m_perfProbeUpdateSoftBodiesUSAccum += updateSoftBodiesUS;
    m_perfProbeUpdateSoftBodiesDepsgraphUSAccum += perfDepsgraphUS;
    m_perfProbeUpdateSoftBodiesFilterUSAccum += perfFilterUS;
    m_perfProbeUpdateSoftBodiesMeshUSAccum += perfMeshUS;
    m_perfProbeUpdateSoftBodiesRelTagUSAccum += perfRelTagUS;
    ++m_perfProbeUpdateSoftBodiesCallsAccum;
    m_perfProbeSoftBodiesTotalAccum += softBodiesTotal;
    m_perfProbeSoftBodiesCandidatesAccum += softBodiesCandidates;
    m_perfProbeSoftBodiesUpdatedAccum += softBodiesUpdated;
  }
}

void JoltPhysicsEnvironment::DebugDrawWorld()
{
  if (m_debugMode <= 0) {
    return;
  }
  JoltDebugDraw::DrawBodies(this);
  JoltDebugDraw::DrawConstraints(this);
}

void JoltPhysicsEnvironment::SetFixedTimeStep(bool useFixedTimeStep, float fixedTimeStep)
{
  if (useFixedTimeStep && fixedTimeStep > 0.0f) {
    m_fixedTimeStep = fixedTimeStep;
    /* Compute substeps: a 60Hz ticRate with fixedTimeStep=1/120 needs 2 substeps. */
    float ticRate = 1.0f / 60.0f;
    int substeps = (int)std::ceil(ticRate / fixedTimeStep);
    if (substeps < 1) substeps = 1;
    m_numTimeSubSteps = substeps;
  }
  else {
    m_fixedTimeStep = 0.0f;
  }
}

float JoltPhysicsEnvironment::GetFixedTimeStep()
{
  return m_fixedTimeStep;
}

int JoltPhysicsEnvironment::GetDebugMode() const
{
  return m_debugMode;
}

void JoltPhysicsEnvironment::SetDebugMode(int debugMode)
{
  m_debugMode = debugMode;
}

void JoltPhysicsEnvironment::SetNumIterations(int numIter)
{
  if (numIter <= 0) {
    return;
  }

  /* Map iterative solver quality to Jolt's solver steps.
   * Keep Bullet-like behavior where one value controls iteration strength,
   * with position steps following Jolt's default 10:2 velocity/position ratio. */
  const int velocitySteps = std::clamp(numIter, 2, 255);
  const int positionSteps = std::clamp((velocitySteps + 4) / 5, 1, 255);

  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mNumVelocitySteps = (JPH::uint)velocitySteps;
  settings.mNumPositionSteps = (JPH::uint)positionSteps;
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetNumTimeSubSteps(int numTimeSubSteps)
{
  m_numTimeSubSteps = numTimeSubSteps;
}

int JoltPhysicsEnvironment::GetNumTimeSubSteps()
{
  return m_numTimeSubSteps;
}

void JoltPhysicsEnvironment::SetDeactivationTime(float dTime)
{
  m_deactivationTime = dTime;
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mTimeBeforeSleep = dTime;
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetDeactivationLinearTreshold(float linTresh)
{
  m_linearDeactivationThreshold = linTresh;
  /* Jolt exposes one scene-level sleep threshold in m/s for tracked point
   * velocity. The linear scene threshold is the closest direct mapping. */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mPointVelocitySleepThreshold = m_linearDeactivationThreshold;
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetDeactivationAngularTreshold(float angTresh)
{
  m_angularDeactivationThreshold = angTresh;
  /* Jolt does not expose a separate angular sleep threshold. Preserve the
   * scene value for UI / API compatibility, but the native sleep threshold is
   * driven solely by SetDeactivationLinearTreshold(). */
}

void JoltPhysicsEnvironment::SetERPNonContact(float erp)
{
  /* Jolt exposes a single Baumgarte stabilization factor for both contact
   * and non-contact position correction. Use the non-contact ERP as the
   * canonical scene mapping so Jolt keeps its upstream default behavior when
   * the Blender defaults are used (erp=0.2, erp2=0.8). */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mBaumgarte = std::clamp(erp, 0.0f, 1.0f);
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetERPContact(float erp2)
{
  /* Jolt has no separate contact ERP. Keep the API entry point for Python/UI
   * compatibility, but do not let Bullet's contact ERP slider overwrite the
   * single scene-wide Baumgarte value. */
  (void)erp2;
}

void JoltPhysicsEnvironment::SetCFM(float cfm)
{
  /* No direct Jolt equivalent. Store for reference. */
}

void JoltPhysicsEnvironment::SetContactBreakingTreshold(float contactBreakingTreshold)
{
  /* No direct Jolt mapping. Jolt manages contact cache internally. */
}

void JoltPhysicsEnvironment::SetSolverSorConstant(float sor)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetSolverType(PHY_SolverType solverType)
{
  /* Jolt has a single solver type. */
}

void JoltPhysicsEnvironment::SetSolverTau(float tau)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetSolverDamping(float damping)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetGravity(float x, float y, float z)
{
  /* Convert from Blender Z-up to Jolt Y-up. */
  JPH::Vec3 g = JoltMath::ToJolt(x, y, z);
  m_physicsSystem->SetGravity(g);
}

void JoltPhysicsEnvironment::GetGravity(MT_Vector3 &grav)
{
  JPH::Vec3 g = m_physicsSystem->GetGravity();
  grav = JoltMath::ToMT(g);
}

PHY_IConstraint *JoltPhysicsEnvironment::CreateConstraint(PHY_IPhysicsController *ctrl,
                                                          PHY_IPhysicsController *ctrl2,
                                                          PHY_ConstraintType type,
                                                          float pivotX,
                                                          float pivotY,
                                                          float pivotZ,
                                                          float axisX,
                                                          float axisY,
                                                          float axisZ,
                                                          float axis1X,
                                                          float axis1Y,
                                                          float axis1Z,
                                                          float axis2X,
                                                          float axis2Y,
                                                          float axis2Z,
                                                          int flag,
                                                          bool replicate_dupli)
{
  if (!m_isPhysicsUpdating) {
    FinalizePendingCompoundShapeBuilds();
    FlushPendingRigidBodyBodyAdds();
  }

  bool disableCollision = (0 != (flag & CCD_CONSTRAINT_DISABLE_LINKED_COLLISION));

  JoltPhysicsController *c0 = static_cast<JoltPhysicsController *>(ctrl);
  JoltPhysicsController *c1 = static_cast<JoltPhysicsController *>(ctrl2);

  if (!c0 || c0->GetBodyID().IsInvalid()) {
    return nullptr;
  }

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::BodyID bodyID0 = c0->GetBodyID();
  JPH::BodyID bodyID1 = (c1 && !c1->GetBodyID().IsInvalid()) ? c1->GetBodyID() : JPH::BodyID();

  /* Pivot in body-local space of body0 (convert from Blender Z-up to Jolt Y-up). */
  JPH::Vec3 pivotLocal = JoltMath::ToJolt(pivotX, pivotY, pivotZ);
  JPH::Vec3 axisIn = JoltMath::ToJolt(axisX, axisY, axisZ);
  if (axisIn.LengthSq() < 1e-6f) {
    axisIn = JPH::Vec3::sAxisY();  /* Default axis = Jolt Y (Blender Z). */
  }
  else {
    axisIn = axisIn.Normalized();
  }
  JPH::Vec3 axis1In = JoltMath::ToJolt(axis1X, axis1Y, axis1Z);
  if (axis1In.LengthSq() >= 1.0e-6f) {
    axis1In -= axisIn * axis1In.Dot(axisIn);
  }
  if (axis1In.LengthSq() < 1.0e-6f) {
    const JPH::Vec3 axis2Fallback = JoltMath::ToJolt(axis2X, axis2Y, axis2Z);
    if (axis2Fallback.LengthSq() >= 1.0e-6f) {
      axis1In = axis2Fallback.Cross(axisIn);
    }
  }
  if (axis1In.LengthSq() < 1.0e-6f) {
    axis1In = axisIn.GetNormalizedPerpendicular();
  }
  else {
    axis1In = axis1In.Normalized();
  }

  /* The public UPBGE pivot is local to the object origin, while Jolt local
   * constraint points are relative to the center of mass. */
  JPH::RVec3 origin0 = bi.GetPosition(bodyID0);
  JPH::RVec3 pos0 = bi.GetCenterOfMassPosition(bodyID0);
  JPH::Quat rot0 = bi.GetRotation(bodyID0);
  JPH::RVec3 pivotWorld = origin0 + rot0 * pivotLocal;
  JPH::Vec3 pivotLocal0 = JPH::Vec3(rot0.Conjugated() * (pivotWorld - pos0));

  /* Compute pivot in body1's local space. */
  JPH::Vec3 pivotLocal1;
  if (!bodyID1.IsInvalid()) {
    JPH::RVec3 pos1 = bi.GetCenterOfMassPosition(bodyID1);
    JPH::Quat rot1 = bi.GetRotation(bodyID1);
    pivotLocal1 = JPH::Vec3(rot1.Conjugated() * (pivotWorld - pos1));
  }
  else {
    pivotLocal1 = JPH::Vec3(pivotWorld);
  }

  /* Get body references for constraint creation.
   * Use the no-lock interface to avoid deadlocks: taking two sequential
   * BodyLockWrite can deadlock when both body IDs hash to the same mutex
   * bucket ("Resource deadlock avoided"). Constraint creation happens on
   * the main thread during scene setup, not during physics stepping, so
   * the no-lock interface is safe here. */
  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterfaceNoLock();
  JPH::Constraint *joltConstraint = nullptr;
  auto create_constraint = [&](JPH::Body &body0, JPH::Body &body1) {
    switch (type) {
      case PHY_POINT2POINT_CONSTRAINT: {
        JPH::PointConstraintSettings settings;
        if (!bodyID1.IsInvalid()) {
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          settings.mPoint1 = pivotLocal0;
          settings.mPoint2 = pivotLocal1;
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          settings.mPoint1 = JPH::Vec3(pivotWorld);
          settings.mPoint2 = JPH::Vec3(pivotWorld);
        }
        joltConstraint = settings.Create(body0, body1);
        return true;
      }

      case PHY_LINEHINGE_CONSTRAINT:
      case PHY_ANGULAR_CONSTRAINT: {
        JPH::HingeConstraintSettings settings;
        if (!bodyID1.IsInvalid()) {
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          settings.mPoint1 = pivotLocal0;
          settings.mHingeAxis1 = axisIn;
          settings.mNormalAxis1 = axis1In;
          JPH::Quat rot1 = bi.GetRotation(bodyID1);
          JPH::Vec3 worldAxis = rot0 * axisIn;
          JPH::Vec3 worldNormal = rot0 * axis1In;
          settings.mPoint2 = pivotLocal1;
          settings.mHingeAxis2 = rot1.Conjugated() * worldAxis;
          settings.mNormalAxis2 = rot1.Conjugated() * worldNormal;
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          JPH::Vec3 worldAxis = rot0 * axisIn;
          JPH::Vec3 worldNormal = rot0 * axis1In;
          settings.mPoint1 = JPH::Vec3(pivotWorld);
          settings.mHingeAxis1 = worldAxis;
          settings.mNormalAxis1 = worldNormal;
          settings.mPoint2 = settings.mPoint1;
          settings.mHingeAxis2 = settings.mHingeAxis1;
          settings.mNormalAxis2 = settings.mNormalAxis1;
        }
        joltConstraint = settings.Create(body0, body1);
        return true;
      }

      case PHY_CONE_TWIST_CONSTRAINT: {
        JPH::ConeConstraintSettings settings;
        settings.mHalfConeAngle = JPH::JPH_PI * 0.25f;
        if (!bodyID1.IsInvalid()) {
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          settings.mPoint1 = pivotLocal0;
          settings.mTwistAxis1 = axisIn;
          JPH::Quat rot1 = bi.GetRotation(bodyID1);
          JPH::Vec3 worldAxis = rot0 * axisIn;
          settings.mPoint2 = pivotLocal1;
          settings.mTwistAxis2 = rot1.Conjugated() * worldAxis;
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          JPH::Vec3 worldAxis = rot0 * axisIn;
          settings.mPoint1 = JPH::Vec3(pivotWorld);
          settings.mTwistAxis1 = worldAxis;
          settings.mPoint2 = settings.mPoint1;
          settings.mTwistAxis2 = settings.mTwistAxis1;
        }
        joltConstraint = settings.Create(body0, body1);
        return true;
      }

      case PHY_FIXED_CONSTRAINT: {
        JPH::FixedConstraintSettings settings;
        const JPH::Vec3 worldAxisX = rot0 * axisIn;
        const JPH::Vec3 worldAxisY = rot0 * axis1In;
        if (!bodyID1.IsInvalid()) {
          const JPH::Quat rot1 = bi.GetRotation(bodyID1);
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          settings.mPoint1 = pivotLocal0;
          settings.mAxisX1 = axisIn;
          settings.mAxisY1 = axis1In;
          settings.mPoint2 = pivotLocal1;
          settings.mAxisX2 = rot1.Conjugated() * worldAxisX;
          settings.mAxisY2 = rot1.Conjugated() * worldAxisY;
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          settings.mPoint1 = pivotWorld;
          settings.mAxisX1 = worldAxisX;
          settings.mAxisY1 = worldAxisY;
          settings.mPoint2 = pivotWorld;
          settings.mAxisX2 = worldAxisX;
          settings.mAxisY2 = worldAxisY;
        }
        joltConstraint = settings.Create(body0, body1);
        return true;
      }

      case PHY_SLIDER_CONSTRAINT: {
        JPH::SliderConstraintSettings settings;
        const JPH::Vec3 worldAxis = rot0 * axisIn;
        const JPH::Vec3 worldNormal = rot0 * axis1In;
        const bool swapBodies = body0.IsDynamic() &&
                                (!body1.IsDynamic() ||
                                 body1.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked() <
                                     body0.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked());

        if (!bodyID1.IsInvalid()) {
          const JPH::Quat rot1 = bi.GetRotation(bodyID1);
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          if (!swapBodies) {
            settings.mPoint1 = pivotLocal0;
            settings.mSliderAxis1 = axisIn;
            settings.mNormalAxis1 = axis1In;
            settings.mPoint2 = pivotLocal1;
            settings.mSliderAxis2 = rot1.Conjugated() * worldAxis;
            settings.mNormalAxis2 = rot1.Conjugated() * worldNormal;
          }
          else {
            settings.mPoint1 = pivotLocal1;
            settings.mSliderAxis1 = rot1.Conjugated() * -worldAxis;
            settings.mNormalAxis1 = rot1.Conjugated() * worldNormal;
            settings.mPoint2 = pivotLocal0;
            settings.mSliderAxis2 = -axisIn;
            settings.mNormalAxis2 = axis1In;
          }
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          settings.mPoint1 = pivotWorld;
          settings.mSliderAxis1 = swapBodies ? -worldAxis : worldAxis;
          settings.mNormalAxis1 = worldNormal;
          settings.mPoint2 = pivotWorld;
          settings.mSliderAxis2 = settings.mSliderAxis1;
          settings.mNormalAxis2 = worldNormal;
        }

        joltConstraint = swapBodies ? settings.Create(body1, body0) : settings.Create(body0, body1);
        return true;
      }

      case PHY_GENERIC_6DOF_CONSTRAINT:
      case PHY_GENERIC_6DOF_SPRING2_CONSTRAINT: {
        JPH::SixDOFConstraintSettings settings;
        settings.mSwingType = JPH::ESwingType::Pyramid;

        if (!bodyID1.IsInvalid()) {
          settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
          settings.mPosition1 = pivotLocal0;
          settings.mPosition2 = pivotLocal1;
          settings.mAxisX1 = axisIn;
          settings.mAxisY1 = axis1In;
          JPH::Quat rot1 = bi.GetRotation(bodyID1);
          JPH::Vec3 worldAxisX = rot0 * axisIn;
          JPH::Vec3 worldAxisY = rot0 * axis1In;
          settings.mAxisX2 = rot1.Conjugated() * worldAxisX;
          settings.mAxisY2 = rot1.Conjugated() * worldAxisY;
        }
        else {
          settings.mSpace = JPH::EConstraintSpace::WorldSpace;
          settings.mPosition1 = JPH::Vec3(pivotWorld);
          settings.mPosition2 = settings.mPosition1;
          JPH::Vec3 worldAxisX = rot0 * axisIn;
          JPH::Vec3 worldAxisY = rot0 * axis1In;
          settings.mAxisX1 = worldAxisX;
          settings.mAxisY1 = worldAxisY;
          settings.mAxisX2 = settings.mAxisX1;
          settings.mAxisY2 = settings.mAxisY1;
        }

        joltConstraint = settings.Create(body0, body1);
        return true;
      }

      default:
        return false;
    }
  };

  if (bodyID1.IsInvalid()) {
    JPH::BodyLockWrite lock0(lockInterface, bodyID0);
    if (!lock0.Succeeded()) {
      return nullptr;
    }

    JPH::Body &body0 = lock0.GetBody();
    JPH::Body &body1 = JPH::Body::sFixedToWorld;
    if (!create_constraint(body0, body1)) {
      return nullptr;
    }
  }
  else {
    JPH::BodyID ids[2] = {bodyID0, bodyID1};
    JPH::BodyLockMultiWrite multiLock(lockInterface, ids, 2);
    JPH::Body *body0 = multiLock.GetBody(0);
    JPH::Body *body1 = multiLock.GetBody(1);
    if (!body0 || !body1) {
      return nullptr;
    }

    if (!create_constraint(*body0, *body1)) {
      return nullptr;
    }
  }

  if (!joltConstraint) {
    return nullptr;
  }

  /* Add constraint to physics system. */
  m_physicsSystem->AddConstraint(joltConstraint);

  /* Register no-collide pair in the group filter so the bodies skip
   * narrow-phase collision detection. */
  if (disableCollision && !bodyID1.IsInvalid()) {
    m_constraintGroupFilter->DisableCollision(
        bodyID0.GetIndexAndSequenceNumber(),
        bodyID1.GetIndexAndSequenceNumber());
  }

  /* Create wrapper. */
  int uid = g_joltConstraintUid++;
  JoltConstraint *wrapper = new JoltConstraint(
      joltConstraint, type, uid, disableCollision, this);

  /* Track constraint for O(1) lookup by ID. */
  m_constraintById[uid] = wrapper;
  m_breakableConstraintsCacheDirty = true;
  NotifyConstraintTopologyChanged(bodyID0, bodyID1);

  return wrapper;
}

PHY_IVehicle *JoltPhysicsEnvironment::CreateVehicle(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return nullptr;
  }

  int uid = g_joltConstraintUid++;
  JoltVehicle *vehicle = new JoltVehicle(joltCtrl, this, uid);
  m_vehicles.push_back(vehicle);
  return vehicle;
}

void JoltPhysicsEnvironment::RemoveConstraintsForBody(JPH::BodyID bodyID)
{
  if (bodyID.IsInvalid() || m_constraintById.empty()) {
    return;
  }

  std::vector<int> constraintIDs;
  constraintIDs.reserve(m_constraintById.size());
  for (const auto &item : m_constraintById) {
    JoltConstraint *wrapper = item.second;
    JPH::Constraint *constraint = wrapper ? wrapper->GetConstraint() : nullptr;
    if (!constraint || constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint) {
      continue;
    }

    const JPH::TwoBodyConstraint *twoBody =
        static_cast<const JPH::TwoBodyConstraint *>(constraint);
    if (twoBody->GetBody1()->GetID() == bodyID || twoBody->GetBody2()->GetID() == bodyID) {
      constraintIDs.push_back(item.first);
    }
  }

  for (const int constraintID : constraintIDs) {
    RemoveConstraintById(constraintID, true);
  }
}

bool JoltPhysicsEnvironment::RemoveConstraintById(int constraintid, bool free)
{
  auto it = m_constraintById.find(constraintid);
  if (it != m_constraintById.end()) {
    JoltConstraint *wrapper = it->second;
    JPH::Constraint *joltCon = wrapper->GetConstraint();
    KX_GameObject *owner = wrapper->GetRigidBodyConstraintOwner();
    JPH::BodyID bodyID1;
    JPH::BodyID bodyID2;

    /* Remove no-collide pair from group filter before removing the constraint,
     * while the body references are still valid. */
    if (joltCon) {
      JPH::TwoBodyConstraint *tbc = static_cast<JPH::TwoBodyConstraint *>(joltCon);
      bodyID1 = tbc->GetBody1()->GetID();
      bodyID2 = tbc->GetBody2()->GetID();
      if (wrapper->GetDisableCollision()) {
        m_constraintGroupFilter->EnableCollision(bodyID1.GetIndexAndSequenceNumber(),
                                                 bodyID2.GetIndexAndSequenceNumber());
      }
    }

    if (joltCon) {
      m_physicsSystem->RemoveConstraint(joltCon);
    }

    if (free) {
      delete wrapper;
    }

    m_constraintById.erase(it);
    m_breakableConstraintsCacheDirty = true;
    NotifyConstraintTopologyChanged(bodyID1, bodyID2);

    /* Constraint graph changes must wake both bodies so the next physics step
     * rebuilds and solves the affected islands immediately. */
    JPH::BodyInterface &bodyInterface = GetBodyInterface();
    if (!bodyID1.IsInvalid()) {
      bodyInterface.ActivateBody(bodyID1);
    }
    if (!bodyID2.IsInvalid()) {
      bodyInterface.ActivateBody(bodyID2);
    }

    if (owner) {
      owner->RemoveRigidBodyConstraintId(constraintid);
    }
    return true;
  }

  for (auto it_vehicle = m_vehicles.begin(); it_vehicle != m_vehicles.end(); ++it_vehicle) {
    JoltVehicle *vehicle = *it_vehicle;
    if (vehicle->GetUserConstraintId() != constraintid) {
      continue;
    }

    if (free) {
      delete vehicle;
    }
    m_vehicles.erase(it_vehicle);
    return true;
  }
  return false;
}

bool JoltPhysicsEnvironment::IsRigidBodyConstraintEnabled(int constraintid)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end()) {
    return false;
  }
  return it->second->GetEnabled();
}

float JoltPhysicsEnvironment::GetAppliedImpulse(int constraintid)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end() || !it->second) {
    return 0.0f;
  }

  return it->second->GetAppliedImpulse();
}

PHY_IVehicle *JoltPhysicsEnvironment::GetVehicleConstraint(int constraintId)
{
  for (JoltVehicle *veh : m_vehicles) {
    if (veh->GetUserConstraintId() == constraintId) {
      return veh;
    }
  }
  return nullptr;
}

PHY_IVehicle *JoltPhysicsEnvironment::GetVehicleConstraint(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return nullptr;
  }

  for (JoltVehicle *veh : m_vehicles) {
    if (veh->UsesController(joltCtrl)) {
      return veh;
    }
  }
  return nullptr;
}

PHY_ICharacter *JoltPhysicsEnvironment::GetCharacterController(KX_GameObject *ob)
{
  auto it = m_characterByObject.find(ob);
  if (it != m_characterByObject.end()) {
    return it->second;
  }
  return nullptr;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::RayTest(
    PHY_IRayCastFilterCallback &filterCallback,
    float fromX,
    float fromY,
    float fromZ,
    float toX,
    float toY,
    float toZ)
{
  MT_Vector3 fromMT(fromX, fromY, fromZ);
  MT_Vector3 toMT(toX, toY, toZ);
  MT_Vector3 dir = toMT - fromMT;

  JPH::RVec3 origin = JoltMath::ToJolt(fromMT);
  JPH::Vec3 direction = JoltMath::ToJolt(dir);

  JPH::RRayCast ray(origin, direction);
  JPH::RayCastSettings raySettings;
  JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
  JoltRayCastBodyFilter bodyFilter(filterCallback);

  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  npq.CastRay(ray, raySettings, collector, {}, {}, bodyFilter);
  if (!collector.HadHit()) {
    return nullptr;
  }
  const JPH::RayCastResult &hit = collector.mHit;

  /* Look up the body and its controller. */
  JPH::BodyLockRead lock(m_physicsSystem->GetBodyLockInterface(), hit.mBodyID);
  if (!lock.Succeeded()) {
    return nullptr;
  }
  const JPH::Body &body = lock.GetBody();

  /* Find the JoltPhysicsController associated with this body via user data. */
  PHY_IPhysicsController *ctrl = JoltGetPhysicsController(JoltGetClientInfo(body));
  if (!ctrl) {
    return nullptr;
  }

  /* Fill result and report hit. */
  JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
  JPH::Vec3 hitNormal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);

  PHY_RayCastResult result;
  result.m_controller = ctrl;
  result.m_hitPoint = JoltMath::ToMT(JPH::Vec3(hitPoint));
  result.m_hitNormal = JoltMath::ToMT(hitNormal);

  filterCallback.reportHit(&result);

  return ctrl;
}

bool JoltPhysicsEnvironment::RayCast(const PHY_RayQuerySettings &settings,
                                     PHY_IRayCastFilterCallback &filterCallback,
                                     std::vector<PHY_RayCastResult> &results)
{
  results.clear();
  const MT_Vector3 displacement = settings.destination - settings.origin;
  if (displacement.length2() <= MT_EPSILON * MT_EPSILON) {
    return true;
  }

  const uint32_t maxResults = std::clamp(settings.max_results, 1u, 257u);
  const JPH::RVec3 origin = JoltMath::ToJoltR(settings.origin);
  const JPH::RRayCast ray(origin, JoltMath::ToJolt(displacement));
  JPH::RayCastSettings raySettings;
  raySettings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
  if (settings.hit_back_faces) {
    raySettings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
  }
  raySettings.mTreatConvexAsSolid = true;

  JoltRayCastBodyFilter bodyFilter(filterCallback, settings.include_sensors);
  JoltBoundedRayCastCollector collector(maxResults);
  m_physicsSystem->GetNarrowPhaseQuery().CastRay(ray, raySettings, collector, {}, {}, bodyFilter);
  collector.Sort();

  results.reserve(collector.m_hits.size());
  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  for (const JPH::RayCastResult &hit : collector.m_hits) {
    JPH::BodyLockRead lock(lockInterface, hit.mBodyID);
    if (!lock.Succeeded()) {
      continue;
    }
    const JPH::Body &body = lock.GetBody();
    PHY_IPhysicsController *controller = JoltGetPhysicsController(JoltGetClientInfo(body));
    if (!controller) {
      continue;
    }

    const float fraction = std::clamp(hit.mFraction, 0.0f, 1.0f);
    const JPH::RVec3 hitPoint = ray.GetPointOnRay(fraction);
    PHY_RayCastResult result;
    result.m_controller = controller;
    result.m_hitPoint = settings.origin + JoltMath::ToMT(hitPoint - origin);
    result.m_hitNormal = JoltMath::ToMT(
        body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint));
    result.m_fraction = fraction;

    bool hasUV = false;
    JoltPopulateMeshQueryDetails(body,
                                 *static_cast<JoltPhysicsController *>(controller),
                                 hit.mSubShapeID2,
                                 hitPoint,
                                 -ray.mDirection,
                                 settings.detail_flags,
                                 result.m_polygon,
                                 hasUV,
                                 result.m_hitUV);
    result.m_hitUVOK = hasUV ? 1 : 0;
    results.push_back(result);
  }
  return true;
}

void JoltPhysicsEnvironment::AddRayQueryDetailRequirements(const unsigned char detailFlags)
{
  unsigned char normalizedFlags = detailFlags &
                                  (PHY_RAY_QUERY_DETAIL_FACE_INDEX |
                                   PHY_RAY_QUERY_DETAIL_UV);
  if ((normalizedFlags & PHY_RAY_QUERY_DETAIL_UV) != 0) {
    normalizedFlags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
  }
  m_rayQueryDetailRequirements |= normalizedFlags;
}

bool JoltPhysicsEnvironment::ShapeCast(const PHY_ShapeCastSettings &settings,
                                       PHY_IShapeCastFilterCallback &filterCallback,
                                       std::vector<PHY_ShapeCastResult> &results)
{
  results.clear();
  const uint32_t maxResults = std::clamp(settings.max_results, 1u, 257u);
  const MT_Vector3 displacement = settings.destination - settings.origin;
  const JPH::RVec3 baseOffset = JoltMath::ToJoltR(settings.origin);
  const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
      JoltMath::ToJolt(settings.orientation.getRotation()), baseOffset);
  const JPH::Vec3 direction = JoltMath::ToJolt(displacement);

  JPH::ShapeCastSettings castSettings;
  castSettings.mExtraConvexRadius = settings.extra_radius;
  castSettings.mReturnDeepestPoint = true;
  castSettings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
  if (settings.hit_back_faces) {
    castSettings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
  }

  JoltShapeCastBodyFilter bodyFilter(filterCallback, settings.include_sensors);
  JoltBoundedShapeCastCollector collector(maxResults);
  const JPH::NarrowPhaseQuery &query = m_physicsSystem->GetNarrowPhaseQuery();

  auto castShape = [&](const JPH::Shape &shape) {
    const JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
        &shape, JPH::Vec3::sOne(), transform, direction);
    query.CastShape(shapeCast, castSettings, baseOffset, collector, {}, {}, bodyFilter);
  };

  switch (settings.type) {
    case PHY_ShapeCastType::Sphere: {
      const JPH::SphereShape shape(settings.radius);
      castShape(shape);
      break;
    }
    case PHY_ShapeCastType::Box: {
      const JPH::Vec3 halfExtents(settings.half_extents.x(),
                                  settings.half_extents.z(),
                                  settings.half_extents.y());
      const JPH::BoxShape shape(halfExtents, 0.0f);
      castShape(shape);
      break;
    }
    case PHY_ShapeCastType::Capsule: {
      const float cylinderHalfHeight = settings.height * 0.5f - settings.radius;
      if (cylinderHalfHeight <= MT_EPSILON) {
        const JPH::SphereShape shape(settings.radius);
        castShape(shape);
      }
      else {
        const JPH::CapsuleShape shape(cylinderHalfHeight, settings.radius);
        castShape(shape);
      }
      break;
    }
  }

  collector.Sort();
  results.reserve(collector.m_hits.size());
  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  for (const JPH::ShapeCastResult &hit : collector.m_hits) {
    JPH::BodyLockRead lock(lockInterface, hit.mBodyID2);
    if (!lock.Succeeded()) {
      continue;
    }
    const JPH::Body &body = lock.GetBody();
    PHY_IPhysicsController *controller = JoltGetPhysicsController(JoltGetClientInfo(body));
    if (!controller) {
      continue;
    }

    PHY_ShapeCastResult result;
    result.controller = controller;
    const JPH::RVec3 hitPoint = baseOffset + JPH::RVec3(hit.mContactPointOn2);
    result.point = settings.origin + JoltMath::ToMT(hit.mContactPointOn2);
    result.fraction = std::clamp(hit.mFraction, 0.0f, 1.0f);
    result.cast_position = settings.origin + displacement * result.fraction;
    result.penetration_depth = std::max(hit.mPenetrationDepth, 0.0f);
    result.started_overlapping = hit.mFraction <= 0.0f && hit.mPenetrationDepth > 0.0f;

    JPH::Vec3 normal = -hit.mPenetrationAxis;
    if (normal.LengthSq() <= FLT_EPSILON) {
      normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);
    }
    else {
      normal = normal.Normalized();
    }
    result.normal = JoltMath::ToMT(normal);
    JoltPopulateMeshQueryDetails(body,
                                 *static_cast<JoltPhysicsController *>(controller),
                                 hit.mSubShapeID2,
                                 hitPoint,
                                 normal,
                                 settings.detail_flags,
                                 result.polygon_index,
                                 result.has_uv,
                                 result.hit_uv);
    results.push_back(result);
  }
  return true;
}

bool JoltPhysicsEnvironment::CullingTest(PHY_CullingCallback callback,
                                          void *userData,
                                          const std::array<MT_Vector4, 6> &planes,
                                          int occlusionRes,
                                          const int *viewport,
                                          const MT_Matrix4x4 &matrix)
{
  /* Iterate all active graphic controllers and test their AABBs against
   * the frustum planes. This is a simple CPU-side frustum cull matching
   * Bullet's CcdGraphicController approach. */
  for (JoltGraphicController *gc : m_graphicControllers) {
    void *clientInfo = gc->GetNewClientInfo();
    if (!clientInfo) {
      continue;
    }

    /* The callback decides whether to render based on AABB vs frustum. */
    callback(static_cast<KX_ClientObjectInfo *>(clientInfo), userData);
  }
  return true;
}

void JoltPhysicsEnvironment::AddSensor(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return;
  }

  if (joltCtrl->Register()) {
    GetBodyInterface().AddBody(joltCtrl->GetBodyID(), JPH::EActivation::Activate);
    NotifyRigidBodyBodyAdded();
  }
}

void JoltPhysicsEnvironment::RemoveSensor(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return;
  }

  if (joltCtrl->Unregister()) {
    /* If physics is currently updating, defer body removal. */
    if (m_isPhysicsUpdating) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::RemoveBody;
      op.bodyID = joltCtrl->GetBodyID();
      op.controller = joltCtrl;
      QueueDeferredOperation(op);
    }
    else {
      RemoveLogicActiveContactsForBody(joltCtrl->GetBodyID());
      GetBodyInterface().RemoveBody(joltCtrl->GetBodyID());
    }
  }
}

void JoltPhysicsEnvironment::AddCollisionCallback(int response_class,
                                                   PHY_ResponseCallback callback,
                                                   void *user)
{
  if (response_class >= 0 && response_class < PHY_NUM_RESPONSE) {
    m_triggerCallbacks[response_class] = callback;
    m_triggerCallbacksUserPtrs[response_class] = user;
  }
}

bool JoltPhysicsEnvironment::RequestCollisionCallback(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return false;
  }
  /* Register the controller for collision callback notifications.
   * The actual callback invocation happens in CallbackTriggers(). */
  m_collisionCallbackControllers.insert(joltCtrl);
  m_collectContactsForCallbacks.store(true, std::memory_order_relaxed);
  return true;
}

bool JoltPhysicsEnvironment::RemoveCollisionCallback(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return false;
  }
  m_collisionCallbackControllers.erase(joltCtrl);
  const bool collectContacts = !m_collisionCallbackControllers.empty();
  m_collectContactsForCallbacks.store(collectContacts, std::memory_order_relaxed);

  if (!collectContacts &&
      !m_collectContactsForLogicNodes.load(std::memory_order_relaxed))
  {
    /* Drop any pending pairs from the previous frame(s) now that nobody listens. */
    m_contactPairsScratch.clear();
    m_removedContactPairsScratch.clear();
    m_contactEventsScratch.clear();
    m_contactListener.SwapContactEvents(
        m_contactPairsScratch, m_removedContactPairsScratch, m_contactEventsScratch);
    m_contactPairsScratch.clear();
    m_removedContactPairsScratch.clear();
    m_contactEventsScratch.clear();
  }

  return true;
}

void JoltPhysicsEnvironment::SetLogicCollisionContactCacheEnabled(const bool enabled)
{
  SetLogicCollisionContactCacheEnabled(enabled, false);
}

void JoltPhysicsEnvironment::SetLogicCollisionContactCacheEnabled(const bool enabled,
                                                                  const bool collectContactDetails)
{
  m_collectContactsForLogicNodes.store(enabled, std::memory_order_relaxed);
  m_collectContactDetailsForLogicNodes.store(enabled && collectContactDetails,
                                             std::memory_order_relaxed);
  if (!collectContactDetails) {
    m_logicCollisionContactDetailsValid = false;
  }
  if (!enabled) {
    m_logicCollisionContactCacheValid = false;
    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByObject.clear();
    m_logicActiveContactPairs.clear();
    m_removedContactPairsScratch.clear();
    m_contactEventsScratch.clear();
    if (!m_collectContactsForCallbacks.load(std::memory_order_relaxed)) {
      m_contactPairsScratch.clear();
      m_contactListener.SwapContactEvents(
          m_contactPairsScratch, m_removedContactPairsScratch, m_contactEventsScratch);
      m_contactPairsScratch.clear();
      m_removedContactPairsScratch.clear();
      m_contactEventsScratch.clear();
    }
  }
}

bool JoltPhysicsEnvironment::GetCachedCollisionContacts(
    PHY_IPhysicsController *ctrl,
    std::vector<PHY_CachedCollisionContact> &r_contacts,
    const bool include_contacts)
{
  const std::vector<const PHY_CachedCollisionContact *> *contactRefs = nullptr;
  if (!GetCachedCollisionContactRefs(ctrl, contactRefs, include_contacts)) {
    return false;
  }

  for (const PHY_CachedCollisionContact *contact : *contactRefs) {
    if (include_contacts) {
      r_contacts.push_back(*contact);
    }
    else {
      PHY_CachedCollisionContact summary;
      summary.ctrl0 = contact->ctrl0;
      summary.ctrl1 = contact->ctrl1;
      summary.object0 = contact->object0;
      summary.object1 = contact->object1;
      summary.contact_count = contact->contact_count;
      r_contacts.push_back(summary);
    }
  }
  return true;
}

bool JoltPhysicsEnvironment::GetCachedCollisionContactRefs(
    PHY_IPhysicsController *ctrl,
    const std::vector<const PHY_CachedCollisionContact *> *&r_contacts,
    const bool include_contacts)
{
  r_contacts = nullptr;
  if (ctrl == nullptr || !m_collectContactsForLogicNodes.load(std::memory_order_relaxed) ||
      !m_logicCollisionContactCacheValid)
  {
    return false;
  }
  if (include_contacts && !m_logicCollisionContactDetailsValid) {
    return false;
  }

  /* Sensor and parented child shapes are queried on demand. This avoids one narrow-phase
   * query per object per tick when collision nodes target only a subset (or none). */
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!m_isPhysicsUpdating && m_controllers.find(joltCtrl) != m_controllers.end() &&
      joltCtrl->IsLogicCollisionQueryActive())
  {
    CollectLogicObjectQueryOverlaps(joltCtrl, m_logicCollisionContactDetailsValid);
  }
  else if (m_isPhysicsUpdating) {
    return false;
  }

  const auto it = m_logicCollisionContactsByController.find(ctrl);
  r_contacts = (it != m_logicCollisionContactsByController.end()) ?
                   &it->second :
                   &m_emptyLogicCollisionContactRefs;
  return true;
}

bool JoltPhysicsEnvironment::GetCachedCollisionContactRefsForObject(
    KX_GameObject *object,
    const std::vector<const PHY_CachedCollisionContact *> *&r_contacts,
    const bool include_contacts)
{
  r_contacts = nullptr;
  if (!object || !m_collectContactsForLogicNodes.load(std::memory_order_relaxed) ||
      !m_logicCollisionContactCacheValid ||
      (include_contacts && !m_logicCollisionContactDetailsValid))
  {
    return false;
  }

  const auto it = m_logicCollisionContactsByObject.find(object);
  if (it != m_logicCollisionContactsByObject.end()) {
    r_contacts = &it->second;
    return true;
  }

  /* A known compound child needs a valid empty result too, so C++ Logic Nodes can
   * turn last tick's overlap into On Exit. Resolve membership only on demand; do
   * not walk every scene hierarchy during the physics callback. */
  const SG_Node *node = object->GetSGNode();
  for (node = node ? node->GetSGParent() : nullptr; node; node = node->GetSGParent()) {
    KX_GameObject *ancestor = static_cast<KX_GameObject *>(node->GetSGClientObject());
    JoltPhysicsController *ancestorController = ancestor ?
        static_cast<JoltPhysicsController *>(ancestor->GetPhysicsController()) :
        nullptr;
    if (ancestorController &&
        m_controllers.find(ancestorController) != m_controllers.end() &&
        ancestorController->OwnsCompoundChildObject(object))
    {
      r_contacts = &m_emptyLogicCollisionContactRefs;
      return true;
    }
  }
  return false;
}

PHY_CollisionTestResult JoltPhysicsEnvironment::CheckCollision(PHY_IPhysicsController *ctrl0,
                                                               PHY_IPhysicsController *ctrl1)
{
  return CheckCollision(ctrl0, ctrl1, true);
}

PHY_CollisionTestResult JoltPhysicsEnvironment::CheckCollision(PHY_IPhysicsController *ctrl0,
                                                               PHY_IPhysicsController *ctrl1,
                                                               bool collectContacts)
{
  PHY_CollisionTestResult result{false, false, nullptr};

  JoltPhysicsController *jc0 = static_cast<JoltPhysicsController *>(ctrl0);
  JoltPhysicsController *jc1 = static_cast<JoltPhysicsController *>(ctrl1);
  if (!jc0 || !jc1 || jc0->GetBodyID().IsInvalid() || jc1->GetBodyID().IsInvalid()) {
    return result;
  }

  JPH::BodyInterface &bodyInterface = GetBodyInterface();
  KX_ClientObjectInfo *info0 = static_cast<KX_ClientObjectInfo *>(jc0->GetNewClientInfo());
  const bool objectSensor0 = JoltIsObjectSensor(info0);
  const bool logicQuery0 = jc0->IsLogicCollisionQueryActive();
  const bool includeStatic0 = objectSensor0 ? jc0->GetLogicObjectSensorIncludeStatic() : true;
  const bool body0Added = bodyInterface.IsAdded(jc0->GetBodyID());
  const bool body1Added = bodyInterface.IsAdded(jc1->GetBodyID());
  /* The first body may be an intentionally unregistered Sensor query shape. The target must
   * remain in Jolt's broadphase; keeping this exception one-sided prevents solid objects from
   * reporting trigger-only Sensor bodies as ordinary collisions. */
  if (!body1Added || (!body0Added && !logicQuery0) || (objectSensor0 && !logicQuery0))
  {
    return result;
  }

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  JPH::BodyID ids[2] = {jc0->GetBodyID(), jc1->GetBodyID()};
  JPH::RefConst<JPH::Shape> shape0;
  JPH::RMat44 transform0;
  JPH::ObjectLayer layer0 = JPH::ObjectLayer(0);
  JPH::ObjectLayer layer1 = JPH::ObjectLayer(0);
  JPH::CollisionGroup collisionGroup0;
  JPH::CollisionGroup collisionGroup1;
  bool body0IsSensor = false;
  bool body1IsSensor = false;
  {
    JPH::BodyLockMultiRead multiLock(lockInterface, ids, 2);
    const JPH::Body *body0 = multiLock.GetBody(0);
    const JPH::Body *body1 = multiLock.GetBody(1);
    if (!body0 || !body1) {
      return result;
    }

    layer0 = body0->GetObjectLayer();
    layer1 = body1->GetObjectLayer();
    collisionGroup0 = body0->GetCollisionGroup();
    collisionGroup1 = body1->GetCollisionGroup();
    body0IsSensor = body0->IsSensor();
    body1IsSensor = body1->IsSensor();
    shape0 = body0->GetShape();
    transform0 = logicQuery0 ?
                     JoltLogicQueryTransform(*jc0, *shape0, body0->GetCenterOfMassTransform()) :
                     body0->GetCenterOfMassTransform();

    if (logicQuery0) {
      if (!info0 || !info0->m_gameobject || JoltGetPhysicsController(info0) != jc0) {
        return result;
      }
      const JoltLogicObjectSensorBodyFilter sensorFilter(
          jc0->GetBodyID(),
          collisionGroup0,
          *info0,
          includeStatic0,
          m_controllers);
      if (!sensorFilter.ShouldCollideLocked(*body1)) {
        return result;
      }
    }
  }
  if (!shape0) {
    return result;
  }

  if (!m_objectLayerPairFilter.ShouldCollide(layer0, layer1)) {
    return result;
  }

  /* An explicit UPBGE Sensor query must also accept JOLT_BP_SENSOR targets because
   * dynamics suspension can place an ordinary Ghost there. The locked client-info filter
   * above still rejects actual Sensor objects. */
  if (!logicQuery0) {
    const JPH::BroadPhaseLayer bp0 = m_broadPhaseLayerInterface.GetBroadPhaseLayer(layer0);
    const JPH::BroadPhaseLayer bp1 = m_broadPhaseLayerInterface.GetBroadPhaseLayer(layer1);
    if (!m_objectVsBroadPhaseLayerFilter.ShouldCollide(layer0, bp1) ||
        !m_objectVsBroadPhaseLayerFilter.ShouldCollide(layer1, bp0))
    {
      return result;
    }
  }

  if (!collisionGroup0.CanCollide(collisionGroup1)) {
    return result;
  }

  JPH::CollideShapeSettings collideSettings;
  /* Match Jolt's own body-vs-body contact generation rule. Non-sensor bodies can have
   * speculative contacts slightly before/after strict overlap; sensors stay exact. */
  collideSettings.mMaxSeparationDistance = (!body0IsSensor && !body1IsSensor) ?
                                               m_physicsSystem->GetPhysicsSettings()
                                                   .mSpeculativeContactDistance :
                                               0.0f;

  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const JPH::BodyID targetBodyID = jc1->GetBodyID();
  const JoltLogicObjectSensorBroadPhaseLayerFilter sensorBroadPhaseFilter(
      logicQuery0 && includeStatic0);

  class TargetBodyFilter : public JPH::BodyFilter {
   public:
    explicit TargetBodyFilter(const JPH::BodyID &bodyID) : m_bodyID(bodyID)
    {
    }

    bool ShouldCollide(const JPH::BodyID &bodyID) const override
    {
      return bodyID == m_bodyID;
    }

   private:
    JPH::BodyID m_bodyID;
  } targetBodyFilter(targetBodyID);

  if (!collectContacts) {
    JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
    if (logicQuery0) {
      npq.CollideShape(shape0.GetPtr(),
                       JPH::Vec3::sReplicate(1.0f),
                       transform0,
                       collideSettings,
                       JPH::RVec3::sZero(),
                       collector,
                       sensorBroadPhaseFilter,
                       {},
                       targetBodyFilter,
                       {});
    }
    else {
      npq.CollideShape(shape0.GetPtr(),
                       JPH::Vec3::sReplicate(1.0f),
                       transform0,
                       collideSettings,
                       JPH::RVec3::sZero(),
                       collector,
                       {},
                       {},
                       targetBodyFilter,
                       {});
    }
    result.collide = collector.HadHit();
    result.isFirst = result.collide;
    return result;
  }

  if (logicQuery0) {
    JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
    npq.CollideShape(shape0.GetPtr(),
                     JPH::Vec3::sReplicate(1.0f),
                     transform0,
                     collideSettings,
                     JPH::RVec3::sZero(),
                     collector,
                     sensorBroadPhaseFilter,
                     {},
                     targetBodyFilter,
                     {});
    if (collector.HadHit() && collector.mHit.mBodyID2 == targetBodyID) {
      result.collide = true;
      result.isFirst = true;
      JoltCollData *collData = new JoltCollData();
      const MT_Vector3 contactPoint = JoltMath::ToMT(collector.mHit.mContactPointOn2);
      collData->AddContactPoint(contactPoint,
                                contactPoint,
                                contactPoint,
                                JoltLogicContactNormal(collector.mHit.mPenetrationAxis),
                                0.5f,
                                0.0f);
      result.collData = collData;
    }
    return result;
  }

  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
  npq.CollideShape(
      shape0.GetPtr(),
      JPH::Vec3::sReplicate(1.0f),
      transform0,
      collideSettings,
      JPH::RVec3::sZero(),
      collector,
      {},  /* broadphase layer filter */
      {},  /* object layer filter */
      targetBodyFilter,
      {}   /* shape filter */
  );

  JoltCollData *collData = nullptr;
  for (const JPH::CollideShapeResult &hit : collector.mHits) {
    if (hit.mBodyID2 == targetBodyID) {
      result.collide = true;
      result.isFirst = true;
      if (collData == nullptr) {
        collData = new JoltCollData();
      }
      MT_Vector3 contactPt = JoltMath::ToMT(JPH::Vec3(hit.mContactPointOn2));
      MT_Vector3 normal = JoltLogicContactNormal(hit.mPenetrationAxis);
      collData->AddContactPoint(contactPt, contactPt, contactPt, normal, 0.5f, 0.0f);
    }
  }
  result.collData = collData;

  return result;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::CreateSphereController(
    float radius, const MT_Vector3 &position)
{
  /* Create a sensor body with SphereShape for Near/Radar sensor. */
  JPH::SphereShapeSettings shapeSettings(std::max(radius, 0.001f));
  JPH::Shape::ShapeResult result = shapeSettings.Create();
  if (result.HasError()) {
    return nullptr;
  }

  JPH::BodyCreationSettings bodySettings(
      result.Get(),
      JoltMath::ToJolt(position),
      JPH::Quat::sIdentity(),
      JPH::EMotionType::Kinematic,
      JoltMakeObjectLayer(0xFFFF, 0xFFFF, JOLT_BP_SENSOR));
  bodySettings.mIsSensor = true;

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    return nullptr;
  }

  JoltDefaultMotionState *ms = new JoltDefaultMotionState();
  ms->SetWorldPosition(position);

  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(ms);
  ctrl->SetEnvironment(this);
  ctrl->SetSensorFlag(true);
  ctrl->SetOriginalMotionType(JPH::EMotionType::Kinematic);
  ctrl->SetBroadPhaseCategory(JOLT_BP_SENSOR);
  ctrl->SetShape(result.Get(), nullptr);

  AddController(ctrl);
  /* Don't add body yet — it gets added when AddSensor() is called. */

  return ctrl;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::CreateConeController(float coneradius,
                                                                      float coneheight)
{
  /* Create a sensor body with an approximate cone shape (convex hull). */
  JoltShapeBuilder builder;
  builder.SetShapeType(PHY_SHAPE_CONE);
  builder.SetRadius(coneradius);
  builder.SetHeight(coneheight);

  JPH::RefConst<JPH::Shape> shape = builder.Build();
  if (!shape) {
    return nullptr;
  }

  JPH::BodyCreationSettings bodySettings(
      shape,
      JPH::RVec3::sZero(),
      JPH::Quat::sIdentity(),
      JPH::EMotionType::Kinematic,
      JoltMakeObjectLayer(0xFFFF, 0xFFFF, JOLT_BP_SENSOR));
  bodySettings.mIsSensor = true;

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    return nullptr;
  }

  JoltDefaultMotionState *ms = new JoltDefaultMotionState();

  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(ms);
  ctrl->SetEnvironment(this);
  ctrl->SetSensorFlag(true);
  ctrl->SetOriginalMotionType(JPH::EMotionType::Kinematic);
  ctrl->SetBroadPhaseCategory(JOLT_BP_SENSOR);
  ctrl->SetShape(shape, builder.GetShapeQueryData());

  AddController(ctrl);

  return ctrl;
}

void JoltPhysicsEnvironment::MergeEnvironment(PHY_IPhysicsEnvironment *other_env)
{
  JoltPhysicsEnvironment *other = static_cast<JoltPhysicsEnvironment *>(other_env);
  if (!other) {
    return;
  }

  m_rayQueryDetailRequirements |= other->m_rayQueryDetailRequirements;

  FinalizePendingCompoundShapeBuilds();
  other->FinalizePendingCompoundShapeBuilds();

  /* Ensure queued body operations are materialized before transferring state. */
  FlushPendingRigidBodyBodyAdds();
  other->FlushPendingRigidBodyBodyAdds();
  FlushPendingSoftBodyBodyRemoves();
  other->FlushPendingSoftBodyBodyRemoves();

  /* Transfer all controllers from the other environment to this one.
   * In Jolt, bodies cannot be shared between PhysicsSystem instances.
   * We remove from the source system and recreate in the destination using
   * saved body creation settings. */
  JPH::BodyInterface &srcBI = other->GetBodyInterface();
  JPH::BodyInterface &dstBI = this->GetBodyInterface();

  /* Build a mapping from old BodyID → new BodyID for constraint recreation. */
  std::unordered_map<uint32_t, JPH::BodyID> bodyIDMap;

  std::set<JoltPhysicsController *> controllersToMove(other->m_controllers);
  for (JoltPhysicsController *ctrl : controllersToMove) {
    JPH::BodyID oldID = ctrl->GetBodyID();
    if (oldID.IsInvalid()) {
      continue;
    }

    const bool wasAdded = srcBI.IsAdded(oldID);

    /* Lock source body to get its creation settings. */
    const JPH::BodyLockInterface &srcLock = other->m_physicsSystem->GetBodyLockInterface();
    JPH::BodyLockRead lock(srcLock, oldID);
    if (!lock.Succeeded()) {
      continue;
    }

    const JPH::Body &srcBody = lock.GetBody();
    const bool isSoftBody = srcBody.IsSoftBody();
    JPH::BodyCreationSettings bcs;
    JPH::SoftBodyCreationSettings sbcs;
    if (isSoftBody) {
      sbcs = srcBody.GetSoftBodyCreationSettings();
    }
    else {
      bcs = srcBody.GetBodyCreationSettings();
    }
    JPH::uint64 userData = srcBody.GetUserData();
    bool wasActive = wasAdded && srcBody.IsActive();

    /* Release the lock before recreating/removing. */
    lock.ReleaseLock();

    JPH::Body *newBody = isSoftBody ? dstBI.CreateSoftBody(sbcs) : dstBI.CreateBody(bcs);
    if (!newBody) {
      continue;
    }

    /* Remove from source. */
    if (wasAdded) {
      other->RemoveLogicActiveContactsForBody(oldID);
      srcBI.RemoveBody(oldID);
    }
    srcBI.DestroyBody(oldID);
    other->m_controllers.erase(ctrl);
    other->m_controllersIterationCacheDirty = true;
    other->m_fhControllersIterationCacheDirty = true;
    other->m_buoyancyVolumesIterationCacheDirty = true;

    newBody->SetUserData(userData);
    bodyIDMap[oldID.GetIndexAndSequenceNumber()] = newBody->GetID();
    ctrl->SetBodyID(newBody->GetID());
    ctrl->SetEnvironment(this);

    /* Update CollisionGroup to use this environment's constraint group filter
     * and store the new body identity in SubGroupID. */
    JPH::CollisionGroup cg = newBody->GetCollisionGroup();
    cg.SetGroupFilter(m_constraintGroupFilter);
    cg.SetSubGroupID(newBody->GetID().GetIndexAndSequenceNumber());
    newBody->SetCollisionGroup(cg);

    if (wasAdded) {
      dstBI.AddBody(newBody->GetID(),
                    wasActive ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    }
    this->AddController(ctrl);
  }

  for (auto it = other->m_controllerByBlenderObject.begin();
       it != other->m_controllerByBlenderObject.end();) {
    JoltPhysicsController *ctrl = it->second;
    if (ctrl && m_controllers.find(ctrl) != m_controllers.end()) {
      m_controllerByBlenderObject[it->first] = ctrl;
      it = other->m_controllerByBlenderObject.erase(it);
    }
    else {
      ++it;
    }
  }

  /* Transfer constraints: recreate them in this system using the body ID map.
   * Jolt constraints reference Body pointers, which are invalidated when bodies
   * are moved between systems. We use GetConstraintSettings() to extract the
   * configuration and TwoBodyConstraintSettings::Create() with new bodies. */
  for (auto &pair : other->m_constraintById) {
    JoltConstraint *srcCon = pair.second;
    JPH::Constraint *joltCon = srcCon->GetConstraint();
    if (!joltCon) {
      continue;
    }

    /* Get the two body references from the constraint. */
    JPH::TwoBodyConstraint *tbc = static_cast<JPH::TwoBodyConstraint *>(joltCon);
    JPH::BodyID body1Old = tbc->GetBody1()->GetID();
    JPH::BodyID body2Old = tbc->GetBody2()->GetID();

    auto it1 = bodyIDMap.find(body1Old.GetIndexAndSequenceNumber());
    auto it2 = bodyIDMap.find(body2Old.GetIndexAndSequenceNumber());
    if (it1 == bodyIDMap.end() || it2 == bodyIDMap.end()) {
      continue;
    }

    /* Get constraint settings and recreate with the new bodies. */
    JPH::Ref<JPH::ConstraintSettings> settings = joltCon->GetConstraintSettings();
    if (!settings) {
      continue;
    }

    /* Remove from source system. */
    other->m_physicsSystem->RemoveConstraint(joltCon);

    /* Use the locking BodyInterface to safely access the new bodies.
     * TwoBodyConstraintSettings::Create() takes Body references. */
    JPH::TwoBodyConstraintSettings *tbcSettings =
        static_cast<JPH::TwoBodyConstraintSettings *>(settings.GetPtr());

    JPH::Constraint *newCon = nullptr;
    {
      const JPH::BodyLockInterface &dstLock = m_physicsSystem->GetBodyLockInterfaceNoLock();
      JPH::BodyID ids[2] = {it1->second, it2->second};
      JPH::BodyLockMultiWrite multiLock(dstLock, ids, 2);
      JPH::Body *newBody1 = multiLock.GetBody(0);
      JPH::Body *newBody2 = multiLock.GetBody(1);
      if (!newBody1 || !newBody2) {
        continue;
      }

      newCon = tbcSettings->Create(*newBody1, *newBody2);
    }

    m_physicsSystem->AddConstraint(newCon);

    /* Register no-collide pair in this environment's group filter. */
    if (srcCon->GetDisableCollision()) {
      m_constraintGroupFilter->DisableCollision(
          it1->second.GetIndexAndSequenceNumber(),
          it2->second.GetIndexAndSequenceNumber());
    }

    JoltConstraint *newJoltCon = new JoltConstraint(
        newCon, srcCon->GetType(), srcCon->GetIdentifier(),
        srcCon->GetDisableCollision(), this);
    newJoltCon->SetRigidBodyConstraintOwner(srcCon->GetRigidBodyConstraintOwner());
    newJoltCon->SetBreakingThreshold(srcCon->GetBreakingThreshold());
    newJoltCon->SetEnabled(srcCon->GetEnabled());
    m_constraintById[srcCon->GetIdentifier()] = newJoltCon;
    NotifyConstraintTopologyChanged(it1->second, it2->second);

    delete srcCon;
  }
  other->m_constraintById.clear();
  m_breakableConstraintsCacheDirty = true;

  /* Transfer graphic controllers. */
  for (JoltGraphicController *gc : other->m_graphicControllers) {
    gc->SetPhysicsEnvironment(this);
    m_graphicControllers.insert(gc);
  }
  other->m_graphicControllers.clear();

  /* Transfer soft bodies. */
  std::unordered_set<JoltSoftBody *> movedSoftBodies;
  for (auto it = other->m_softBodies.begin(); it != other->m_softBodies.end();) {
    JoltSoftBody *sb = *it;
    if (!sb) {
      it = other->m_softBodies.erase(it);
      continue;
    }

    const JPH::BodyID oldBodyID = sb->GetBodyID();
    auto bodyIt = bodyIDMap.find(oldBodyID.GetIndexAndSequenceNumber());
    if (oldBodyID.IsInvalid() || bodyIt == bodyIDMap.end()) {
      ++it;
      continue;
    }

    sb->SetEnvironment(this);
    sb->SetBodyID(bodyIt->second);
    m_softBodies.push_back(sb);
    movedSoftBodies.insert(sb);
    other->m_softBodyContactListener.UnregisterPlasticityContactBody(oldBodyID);
    if (sb->UsesContactPlasticity() && dstBI.IsAdded(bodyIt->second)) {
      m_softBodyContactListener.RegisterPlasticityContactBody(sb->GetBodyID());
    }

    if (sb->HasPinnedVertices()) {
      m_pinnedSoftBodies.push_back(sb);
      JoltPhysicsController *pinCtrl = sb->GetPinController();
      if (pinCtrl && m_controllers.find(pinCtrl) == m_controllers.end()) {
        pinCtrl = nullptr;
        sb->SetPinController(nullptr);
      }
      if (!pinCtrl && sb->GetPinBlenderObject()) {
        pinCtrl = FindControllerByBlenderObject(sb->GetPinBlenderObject());
        sb->SetPinController(pinCtrl);
      }
      if (pinCtrl && sb->GetNoPinCollision() && !pinCtrl->GetBodyID().IsInvalid()) {
        m_softBodyContactListener.Register(sb->GetBodyID(), pinCtrl->GetBodyID());
      }
    }

    it = other->m_softBodies.erase(it);
  }

  for (auto it = other->m_pinnedSoftBodies.begin(); it != other->m_pinnedSoftBodies.end();) {
    if (movedSoftBodies.find(*it) != movedSoftBodies.end()) {
      it = other->m_pinnedSoftBodies.erase(it);
    }
    else {
      ++it;
    }
  }
  other->m_pendingRigidBodyBodyAdds.clear();
  other->m_pendingCompoundSubShapesByController.clear();
  other->m_pendingSoftBodyBodyAdds.clear();
  other->m_pendingSoftBodyBodyRemoves.clear();
  other->m_pendingRigidBodyAddsForOptimize = 0;
  other->m_pendingSoftBodyAddsForOptimize = 0;
  other->m_pendingSoftBodyBatchAddsForOptimize = 0;
  other->m_rigidBodyAddsSinceLastStep = 0;
  other->m_softBodyAddsSinceLastStep = 0;
  other->m_rigidBodyAddIdleFrames = 0;
  other->m_softBodyAddIdleFrames = 0;

  /* Transfer characters. */
  for (auto it = other->m_characterByObject.begin(); it != other->m_characterByObject.end();) {
    JoltCharacter *character = it->second;
    if (character && m_controllers.find(character->GetController()) != m_controllers.end()) {
      character->SetEnvironment(this);
      m_characterByObject[it->first] = character;
      it = other->m_characterByObject.erase(it);
    }
    else {
      ++it;
    }
  }

  /* Transfer vehicles. */
  for (JoltVehicle *veh : other->m_vehicles) {
    veh->ResetConstraint();
    veh->SetEnvironment(this);
    m_vehicles.push_back(veh);
  }
  other->m_vehicles.clear();

  /* Re-optimize broadphase after bulk insertion. */
  m_needsBroadPhaseOptimize = true;
}

void JoltPhysicsEnvironment::ConvertObject(BL_SceneConverter *converter,
                                            KX_GameObject *gameobj,
                                            RAS_MeshObject *meshobj,
                                            KX_Scene *kxscene,
                                            PHY_IMotionState *motionstate,
                                            int activeLayerBitInfo,
                                            bool isCompoundChild,
                                            bool hasCompoundChildren)
{
  using namespace blender;
  blender::Object *blenderobject = gameobj->GetBlenderObject();

  bool isDyna = (blenderobject->gameflag & OB_DYNAMIC) != 0;
  bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
  bool isCharacter = (blenderobject->gameflag & OB_CHARACTER) != 0;
  bool isSoftBody = (blenderobject->gameflag & OB_SOFT_BODY) != 0;
  bool isRigidBody = (blenderobject->gameflag & OB_RIGID_BODY) != 0;
  bool useGimpact = ((isDyna || isSensor) && !isSoftBody);

  /* Determine bounds type. */
  char boundsType;
  if (!(blenderobject->gameflag & OB_BOUNDS)) {
    /* No explicit bounds configured: use type-based runtime defaults without
     * promoting them into persistent Blender authoring flags. */
    if (blenderobject->gameflag & OB_SOFT_BODY)
      boundsType = OB_BOUND_TRIANGLE_MESH;
    else if (blenderobject->gameflag & OB_CHARACTER)
      boundsType = OB_BOUND_SPHERE;
    else if (isRigidBody && !(blenderobject->gameflag2 & OB_HAS_VEHICLE))
      boundsType = OB_BOUND_BOX;
    else if (isDyna)
      boundsType = OB_BOUND_SPHERE;
    else
      boundsType = OB_BOUND_TRIANGLE_MESH;
  }
  else {
    if (ELEM(blenderobject->collision_boundtype, OB_BOUND_CONVEX_HULL, OB_BOUND_TRIANGLE_MESH) &&
        blenderobject->type != OB_MESH && !meshobj) {
      boundsType = OB_BOUND_SPHERE;
    }
    else {
      boundsType = blenderobject->collision_boundtype;
    }
  }

  /* Get bounds information. */
  float bounds_extends[3] = {1.0f, 1.0f, 1.0f};
  if (const std::optional<Bounds<float3>> bl_bounds = BKE_object_boundbox_eval_cached_get(
          blenderobject)) {
    const std::array<float3, 8> corners = bounds::corners(*bl_bounds);
    bounds_extends[0] = 0.5f * fabsf(corners[0][0] - corners[4][0]);
    bounds_extends[1] = 0.5f * fabsf(corners[0][1] - corners[2][1]);
    bounds_extends[2] = 0.5f * fabsf(corners[0][2] - corners[1][2]);
  }

  /* Build the collision shape. */
  JoltShapeBuilder shapeBuilder;
  float margin = isSoftBody ? 0.1f : blenderobject->margin;
  shapeBuilder.SetMargin(margin);
  if (!isSoftBody) {
    shapeBuilder.SetRayQueryDetailRequirements(m_rayQueryDetailRequirements);
  }

  /* For soft bodies, strip any stale joltSbModifier that survived from a
   * previous game run BEFORE SetMesh() reads vertex positions.  Without
   * this, the depsgraph-cached evaluated mesh still contains the deformation
   * from the previous run and feeds wrong rest-pose coords into Create(). */
  if (isSoftBody) {
    ModifierData *md = (ModifierData *)blenderobject->modifiers.first;
    while (md) {
      ModifierData *next = md->next;
      if (JoltIsSoftBodyModifier(md)) {
        ((blender::SimpleDeformModifierDataBGE *)md)->vertcoos = nullptr;
        BLI_remlink(&blenderobject->modifiers, md);
        BKE_modifier_free(md);
      }
      md = next;
    }
    DEG_id_tag_update(&blenderobject->id, ID_RECALC_GEOMETRY);
  }

  switch (boundsType) {
    case OB_BOUND_SPHERE:
      shapeBuilder.SetShapeType(PHY_SHAPE_SPHERE);
      shapeBuilder.SetRadius(std::max({bounds_extends[0], bounds_extends[1], bounds_extends[2]}));
      break;
    case OB_BOUND_BOX:
      shapeBuilder.SetShapeType(PHY_SHAPE_BOX);
      shapeBuilder.SetHalfExtents(bounds_extends[0], bounds_extends[1], bounds_extends[2]);
      break;
    case OB_BOUND_CYLINDER: {
      float radius = std::max(bounds_extends[0], bounds_extends[1]);
      shapeBuilder.SetShapeType(PHY_SHAPE_CYLINDER);
      shapeBuilder.SetHalfExtents(radius, radius, bounds_extends[2]);
      break;
    }
    case OB_BOUND_CONE:
      shapeBuilder.SetShapeType(PHY_SHAPE_CONE);
      shapeBuilder.SetRadius(std::max(bounds_extends[0], bounds_extends[1]));
      shapeBuilder.SetHeight(2.0f * bounds_extends[2]);
      break;
    case OB_BOUND_CONVEX_HULL:
      shapeBuilder.SetMesh(kxscene, meshobj, true);
      break;
    case OB_BOUND_CAPSULE: {
      float radius = std::max(bounds_extends[0], bounds_extends[1]);
      float height = 2.0f * (bounds_extends[2] - radius);
      if (height < 0.0f) height = 0.0f;
      shapeBuilder.SetShapeType(PHY_SHAPE_CAPSULE);
      shapeBuilder.SetRadius(radius);
      shapeBuilder.SetHeight(height);
      break;
    }
    case OB_BOUND_TRIANGLE_MESH:
      shapeBuilder.SetMesh(kxscene, meshobj, false);
      break;
    case OB_BOUND_EMPTY:
      shapeBuilder.SetShapeType(PHY_SHAPE_EMPTY);
      break;
    default:
      delete motionstate;
      return;
  }

  MT_Vector3 scaling = gameobj->NodeGetWorldScaling();
  MT_Vector3 pos = motionstate->GetWorldPosition();
  MT_Matrix3x3 ori = motionstate->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();
  float mass = isDyna ? blenderobject->mass : 0.0f;

  /* --- Soft body path: create JoltSoftBody instead of a rigid body --- */
  if (isSoftBody && !shapeBuilder.GetVertexArray().empty() &&
      !shapeBuilder.GetTriangleArray().empty())
  {
    unsigned short collGroup = blenderobject->col_group & JOLT_COLLISION_LAYER_MASK;
    unsigned short collMask = blenderobject->col_mask;

    JoltPhysicsController *ctrl = new JoltPhysicsController();
    ctrl->SetMotionState(motionstate);
    ctrl->SetEnvironment(this);
    ctrl->SetDynamic(true);
    ctrl->SetRigidBodyFlag(false);
    ctrl->SetSensorFlag(false);
    ctrl->SetCompoundFlag(false);
    ctrl->SetOriginalMotionType(JPH::EMotionType::Dynamic);
    ctrl->SetBroadPhaseCategory(JOLT_BP_DYNAMIC);
    ctrl->SetMargin(margin);
    ctrl->SetRadius(blenderobject->inertia);
    ctrl->SetCollisionGroup(collGroup);
    ctrl->SetCollisionMask(collMask);
    ctrl->SetNewClientInfo(gameobj->getClientInfo());

    JoltSoftBody *sb = new JoltSoftBody(this, ctrl);

    /* Map all BulletSoftBody settings that have a Jolt equivalent. */
    JoltSoftBodySettings sbSettings;
    sbSettings.mass = mass;
    sbSettings.friction = blenderobject->friction;  /* fallback */
    sbSettings.restitution = blenderobject->reflect;
    sbSettings.damping  = blenderobject->damping;   /* fallback */
    sbSettings.margin   = margin;
    sbSettings.gravityFactor = blenderobject->gravity_factor;

    if (blenderobject->bsoft) {
      const blender::BulletSoftBody *bsoft = blenderobject->bsoft;
      sbSettings.linStiff         = bsoft->linStiff;  /* edge compliance */
      sbSettings.shearStiff       = bsoft->shearStiff; /* shear compliance */
      sbSettings.angStiff         = bsoft->angStiff;  /* bend compliance */
      sbSettings.friction         = bsoft->kDF;        /* dynamic friction */
      sbSettings.damping          = bsoft->kDP;        /* linear damping */
      sbSettings.pressure         = bsoft->kPR;        /* gas pressure */
      sbSettings.margin           = (bsoft->margin > 0.0f) ? bsoft->margin : margin;
      sbSettings.numIterations    = bsoft->piterations;
      sbSettings.bendingConstraints = (bsoft->flag & OB_BSB_BENDING_CONSTRAINTS) != 0;
      sbSettings.lraConstraints   = (bsoft->flag & OB_BSB_LRA_CONSTRAINTS) != 0;
      sbSettings.lraType          = bsoft->lraType;
      sbSettings.facesDoubleSided = (bsoft->flag & OB_BSB_FACES_DOUBLE_SIDED) != 0;
      sbSettings.noPinCollision   = (bsoft->flag & OB_BSB_NO_PIN_COLLISION) != 0;
      sbSettings.pinTransformFollow = (bsoft->flag & OB_BSB_PIN_TRANSFORM_FOLLOW) != 0;
      sbSettings.plasticity       = (bsoft->flag & OB_BSB_PLASTICITY) != 0;
      sbSettings.plasticThreshold = bsoft->plasticThreshold;
      sbSettings.plasticStrength  = bsoft->plasticStrength;
      sbSettings.plasticMaxDeform = bsoft->plasticMaxDeform;
      sbSettings.plasticRepairRate = bsoft->plasticRepairRate;
      sbSettings.pinWeightThreshold = 1.0f - bsoft->pin_weight_threshold;

      /* Build pin vertex weight list from the named vertex group. */
      if (bsoft->pin_vgroup[0] != '\0' && meshobj) {
        blender::Mesh *me = meshobj->GetOrigMesh();
        blender::Object *weightObject = meshobj->GetOriginalObject();

        if (weightObject && weightObject->type == OB_MESH) {
          KX_KetsjiEngine *engine = KX_GetActiveEngine();
          blender::bContext *ctx = engine ? engine->GetContext() : nullptr;
          blender::Depsgraph *depsgraph = ctx ? CTX_data_ensure_evaluated_depsgraph(ctx) :
                                               nullptr;
          blender::Object *ob_eval = depsgraph ? DEG_get_evaluated(depsgraph, weightObject) :
                                                 nullptr;
          if (ob_eval && ob_eval->type == OB_MESH) {
            weightObject = ob_eval;
            me = (blender::Mesh *)ob_eval->data;
          }
        }

        if (me && weightObject) {
          int vgIdx = blender::BKE_object_defgroup_name_index(weightObject, bsoft->pin_vgroup);
          if (vgIdx >= 0) {
            const blender::Span<blender::MDeformVert> dverts = me->deform_verts();
            if (!dverts.is_empty()) {
              /* vertRemap is built after this block — grab it from shapeBuilder directly. */
              const std::unordered_map<int, int> &vr = shapeBuilder.GetVertexRemap();
              for (const auto &[blendIdx, joltIdx] : vr) {
                if (blendIdx < (int)dverts.size()) {
                  float w = blender::BKE_defvert_find_weight(&dverts[blendIdx], vgIdx);
                  if (w > 0.0f) {
                    sbSettings.pinVertexWeights.push_back({joltIdx, w});
                  }
                }
              }
            }
          }
        }
      }

      /* Store the pin object's initial world transform for per-frame following. */
      if (bsoft->pin_object) {
        blender::Object *pinObj = bsoft->pin_object;
        float loc[3], rot[3][3], scale[3];
        /* Use BKE_object_to_mat4 instead of pinObj->object_to_world() because
         * inactive (template) objects have OB_HIDE_VIEWPORT forced on them by
         * BL_DataConversion, which prevents the depsgraph from evaluating their
         * world transform.  On the second game run pinObj->runtime->object_to_world
         * is (0,0,0) because the depsgraph skipped its evaluation.
         * BKE_object_to_mat4 computes the matrix directly from loc/rot/size and
         * the parent chain without touching the depsgraph cache — the same
         * approach used for rigid-body constraints in BL_DataConversion. */
        float pinMat[4][4];
        BKE_object_to_mat4(pinObj, pinMat);
        mat4_to_loc_rot_size(loc, rot, scale, (const float (*)[4])pinMat);
        sbSettings.hasPinObject  = true;
        sbSettings.pinInitialPos = MT_Vector3(loc[0], loc[1], loc[2]);
        sbSettings.pinInitialOri = MT_Matrix3x3(
            rot[0][0], rot[1][0], rot[2][0],
            rot[0][1], rot[1][1], rot[2][1],
            rot[0][2], rot[1][2], rot[2][2]);
      }
    }

    const std::vector<float> &verts = shapeBuilder.GetVertexArray();
    const std::vector<int> &tris = shapeBuilder.GetTriangleArray();
    const std::unordered_map<int, int> &vertRemap = shapeBuilder.GetVertexRemap();

    bool ok = sb->Create(verts.data(),
                         (int)verts.size() / 3,
                         tris.data(),
                         (int)tris.size() / 3,
                         pos,
                         ori,
                         scaling,
                         sbSettings,
                         vertRemap);
    if (!ok) {
      delete sb;
      delete ctrl;
      delete motionstate;
      return;
    }

    /* Template objects (disabled in viewport) must NOT be simulated.
     * Two hazards arise if the template soft body is left active:
     *
     * 1. SGNode drift: SynchronizeMotionStates() writes the Jolt COM back to
     *    the game object's SGNode each frame.  CloneIntoReplica() reads that
     *    node's world position as the clone's spawn origin (replicaPos).  If
     *    the template has been simulating, replicaPos drifts away from the
     *    rest-pose and all spawned clones appear at the wrong location.
     *
     * 2. Modifier corruption: UpdateMesh() writes Jolt particle positions into
     *    the shared Blender modifier buffer (m_sbCoords).  That buffer is used
     *    by every live replica.  The m_isActive guard in UpdateMesh() (set by
     *    SetActive(false) below) prevents this write for the template.
     *
     * Removing the body from the active world keeps m_bodyID valid (the body
     * still exists in the pool so PostProcessReplica can lock+inspect it), but
     * prevents any physics steps from running on it. */
    bool sbVisCheck = (blenderobject->base_flag &
                       (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                        BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT)) != 0;
    if (!sbVisCheck) {
      RemoveLogicActiveContactsForBody(sb->GetBodyID());
      m_physicsSystem->GetBodyInterfaceNoLock().RemoveBody(sb->GetBodyID());
      sb->SetActive(false);
    }

    sb->SetGameObject(gameobj);
    sb->SetMeshObject(meshobj);
    /* Remove any SimpleDeformBGE modifiers left over from a previous game run.
     * Must be called here (once per template, before any clones exist) rather
     * than inside UpdateMesh() where multiple simultaneous clones would
     * accidentally destroy each other's active modifiers. */
    sb->PurgeStaleModifiers();
    if (blenderobject->bsoft && blenderobject->bsoft->pin_object) {
      sb->SetPinBlenderObject(blenderobject->bsoft->pin_object);
    }

    ctrl->SetSoftBody(sb);
    ctrl->SetBodyID(sb->GetBodyID());
    gameobj->SetPhysicsController(ctrl);
    /* Pre-center in Create() places the Jolt body at COM while preserving a
     * stored COM->origin offset in JoltSoftBody. WriteDynamicsToMotionState()
     * subtracts that offset for soft bodies, so the SG node starts at authored
     * object origin from frame 0 with no startup reposition.
     *
     * Client info must be set before this call so parented soft bodies can use
     * game-object world->local conversion during startup sync. */
    ctrl->WriteDynamicsToMotionState();

    m_softBodies.push_back(sb);
    if (sb->HasPinnedVertices()) {
      m_pinnedSoftBodies.push_back(sb);
    }
    if (sbVisCheck && sb->UsesContactPlasticity()) {
      m_softBodyContactListener.RegisterPlasticityContactBody(sb->GetBodyID());
    }
    AddController(ctrl);
    /* Register controller → blender object mapping for soft-body pin resolution. */
    RegisterControllerForObject(blenderobject, ctrl);
    return;
  }

  /* Soft body was requested but the mesh data is empty (no vertices or no
   * triangles). This can happen if the object is not a mesh or has no
   * collider polygons. Warn so the user understands why physics falls back. */
  if (isSoftBody) {
    printf("Jolt: Object '%s' is set to Soft Body but has no triangle mesh data — "
           "falling back to rigid body simulation.\n",
           blenderobject->id.name + 2);
  }

  JPH::RefConst<JPH::Shape> shape = shapeBuilder.Build(useGimpact, scaling);
  if (!shape) {
    delete motionstate;
    return;
  }
  shape = JoltApplyVehicleCenterOfMassOffset(shape, blenderobject);

  /* ---- Compound child: merge shape into the parent's compound shape ---- */
  if (isCompoundChild) {
    /* Walk the parent chain to find the compound root (topmost parent with
     * OB_CHILD flag and a solid physics type — same logic as BL_DataConversion). */
    blender::Object *blenderCompoundRoot = nullptr;
    {
      blender::Object *parentit = blenderobject->parent;
      while (parentit) {
        if ((parentit->gameflag & OB_CHILD) &&
            (parentit->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
            !(parentit->gameflag & OB_SOFT_BODY)) {
          blenderCompoundRoot = parentit;
        }
        parentit = parentit->parent;
      }
    }

    if (!blenderCompoundRoot || !converter) {
      delete motionstate;
      return;
    }

    KX_GameObject *compoundParent = converter->FindGameObject(blenderCompoundRoot);
    if (!compoundParent || !compoundParent->GetPhysicsController()) {
      delete motionstate;
      return;
    }

    JoltPhysicsController *parentCtrl = static_cast<JoltPhysicsController *>(
        compoundParent->GetPhysicsController());
    if (!parentCtrl || !parentCtrl->GetShape()) {
      delete motionstate;
      return;
    }

    /* Compute relative transform (position + rotation) of the child
     * in the parent's local space, matching Bullet's compound child logic. */
    SG_Node *childNode = gameobj->GetSGNode();
    SG_Node *parentNode = compoundParent->GetSGNode();

    MT_Vector3 parentScale = parentNode->GetWorldScaling();
    parentScale[0] = MT_Scalar(1.0f) / parentScale[0];
    parentScale[1] = MT_Scalar(1.0f) / parentScale[1];
    parentScale[2] = MT_Scalar(1.0f) / parentScale[2];

    MT_Matrix3x3 parentInvRot = parentNode->GetWorldOrientation().transposed();
    MT_Vector3 relativePos = parentInvRot *
        ((childNode->GetWorldPosition() - parentNode->GetWorldPosition()) * parentScale);
    MT_Matrix3x3 relativeRot = parentInvRot * childNode->GetWorldOrientation();
    MT_Quaternion relQuat = relativeRot.getRotation();

    if (!m_hasSteppedSimulation) {
      /* Startup conversion path: queue parent shape rebuild for one-pass
       * assembly once conversion-time compound children are known. */
      QueuePendingCompoundChildShape(
          parentCtrl,
          gameobj,
          JoltMath::ToJolt(relativePos),
          JoltMath::ToJolt(relQuat),
          shape,
          shapeBuilder.GetShapeQueryData());

      delete motionstate;
      return;
    }

    /* Runtime path: preserve prior behavior by rebuilding parent compound
     * immediately when a new child is converted. */
    JPH::StaticCompoundShapeSettings compoundSettings;
    JPH::RefConst<JPH::Shape> parentShape = parentCtrl->GetShape();

    JoltAddExistingShapeToCompoundSettings(compoundSettings, parentShape.GetPtr());

    const JPH::uint32 childUserData = parentCtrl->RegisterCompoundChildBinding(
        gameobj, JoltMath::ToJolt(relativePos), JoltMath::ToJolt(relQuat));
    compoundSettings.AddShape(JoltMath::ToJolt(relativePos),
                              JoltMath::ToJolt(relQuat),
                              shape,
                              childUserData);

    JPH::Shape::ShapeResult result = compoundSettings.Create();
    if (result.HasError()) {
      parentCtrl->RemoveCompoundChildBinding(childUserData);
      delete motionstate;
      return;
    }

    JPH::RefConst<JPH::Shape> newCompound = result.Get();
    parentCtrl->SetShapePreservingMassPropertiesAndCenterOfMass(
        newCompound,
        JoltMergeShapeQueryData(parentCtrl->GetShapeQueryData(),
                                shapeBuilder.GetShapeQueryData()));

    delete motionstate;
    return;
  }

  /* Determine Jolt motion type and broadphase category. */
  JPH::EMotionType motionType;
  JoltBroadPhaseLayer bpCategory;
  if (isSensor) {
    motionType = JPH::EMotionType::Static;
    bpCategory = JOLT_BP_SENSOR;
  }
  else if (isCharacter || isDyna) {
    motionType = JPH::EMotionType::Dynamic;
    bpCategory = JOLT_BP_DYNAMIC;
  }
  else {
    motionType = JPH::EMotionType::Static;
    bpCategory = JOLT_BP_STATIC;
  }

  unsigned short col_group = blenderobject->col_group & JOLT_COLLISION_LAYER_MASK;
  unsigned short col_mask = blenderobject->col_mask;
  JPH::ObjectLayer objectLayer = JoltMakeObjectLayer(col_group, col_mask, bpCategory);

  JPH::BodyCreationSettings bodySettings(
      shape,
      JoltMath::ToJolt(pos),
      JoltMath::ToJolt(quat),
      motionType,
      objectLayer);

  /* Set mass for dynamic bodies. */
  if (mass > 0.0f) {
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = mass;
  }

  /* Friction, restitution, damping. */
  bodySettings.mFriction = blenderobject->friction;
  bodySettings.mRestitution = blenderobject->reflect;
  if (blenderobject->damping == 0.04f && blenderobject->rdamping == 0.1f) {
    bodySettings.mLinearDamping = 0.05f;
    bodySettings.mAngularDamping = 0.05f;
  }
  else {
    bodySettings.mLinearDamping = blenderobject->damping;
    bodySettings.mAngularDamping = blenderobject->rdamping;
  }
  bodySettings.mMaxLinearVelocity = JoltLinearVelocityMaxOrDefault(blenderobject->max_vel);
  bodySettings.mMaxAngularVelocity = JoltAngularVelocityMaxOrDefault(blenderobject->max_angvel);

  /* Per-body gravity multiplier. */
  bodySettings.mGravityFactor = blenderobject->gravity_factor;
  if (isDyna && (blenderobject->gameflag2 & OB_JOLT_OVERRIDE_SOLVER_ITERATIONS)) {
    bodySettings.mNumVelocityStepsOverride = (JPH::uint)std::clamp(
        int(blenderobject->jolt_velocity_solver_iterations), 1, 255);
    bodySettings.mNumPositionStepsOverride = (JPH::uint)std::clamp(
        int(blenderobject->jolt_position_solver_iterations), 1, 255);
  }

  /* Inertia multiplier from form factor (Bullet used formfactor / 0.4). */
  if (mass > 0.0f) {
    bodySettings.mInertiaMultiplier = blenderobject->formfactor / 0.4f;
  }

  /* Per-body sleep control: OB_COLLISION_RESPONSE means "No Sleeping" in UI. */
  if (blenderobject->gameflag & OB_COLLISION_RESPONSE) {
    bodySettings.mAllowSleeping = false;
  }

  /* Sensor flag. */
  if (isSensor || (blenderobject->gameflag & OB_GHOST)) {
    bodySettings.mIsSensor = true;
  }

  /* Allow switching dynamic↔static for SuspendDynamics/RestoreDynamics. */
  if (isDyna || isCharacter) {
    bodySettings.mAllowDynamicOrKinematic = true;
  }

  /* CCD for fast-moving dynamic bodies. */
  if ((blenderobject->gameflag2 & OB_CCD_RIGID_BODY) && isDyna) {
    bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
  }

  /* Enhanced internal edge removal for mesh shapes. */
  if (boundsType == OB_BOUND_TRIANGLE_MESH) {
    bodySettings.mEnhancedInternalEdgeRemoval = true;
  }

  /* Axis locking via Jolt AllowedDOFs (matching Bullet linear/angular factor).
   * Coordinate mapping: Blender (X,Y,Z) → Jolt (X,Z,-Y), so:
   *   Blender X → Jolt X
   *   Blender Y → Jolt Z
   *   Blender Z → Jolt Y  */
  if (isDyna) {
    JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationX;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationZ;  /* Blender Y → Jolt Z */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationY;  /* Blender Z → Jolt Y */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationX;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationZ;     /* Blender Y → Jolt Z */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationY;     /* Blender Z → Jolt Y */
    /* Non-rigid dynamic bodies lock all rotation. */
    if (!isRigidBody) {
      dofs &= ~(JPH::EAllowedDOFs::RotationX | JPH::EAllowedDOFs::RotationY |
                JPH::EAllowedDOFs::RotationZ);
    }
    bodySettings.mAllowedDOFs = dofs;
  }

  /* Create the body. */
  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    printf("Jolt: ConvertObject FAILED to create body for '%s'\n",
           gameobj->GetName().c_str());
    delete motionstate;
    return;
  }

  /* Create the controller. */
  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(motionstate);
  ctrl->SetEnvironment(this);
  ctrl->SetDynamic(isDyna);
  ctrl->SetRigidBodyFlag(isDyna && isRigidBody);
  ctrl->SetSensorFlag(bodySettings.mIsSensor);
  ctrl->SetCompoundFlag(hasCompoundChildren);
  ctrl->SetOriginalMotionType(motionType);
  ctrl->SetBroadPhaseCategory(bpCategory);
  ctrl->SetShape(shape, shapeBuilder.GetShapeQueryData());
  if (isDyna) {
    ctrl->SetMassPropertiesTemplate(bodySettings.GetMassProperties());
  }
  ctrl->SetMargin(margin);
  ctrl->SetRadius(blenderobject->inertia);
  ctrl->SetRigidBodyAxisLockState(
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_AXIS) != 0,
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_AXIS) != 0,
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_AXIS) != 0,
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_ROT_AXIS) != 0,
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_ROT_AXIS) != 0,
      (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_ROT_AXIS) != 0);
  ctrl->SetFhEnabled((blenderobject->gameflag & OB_DO_FH) != 0);
  ctrl->SetFhRotEnabled((blenderobject->gameflag & OB_ROT_FH) != 0);
  ctrl->SetFhSpring(blenderobject->fh);
  ctrl->SetFhDamping(blenderobject->xyfrict);
  ctrl->SetFhDistance(blenderobject->fhdist);
  ctrl->SetFhNormal((blenderobject->dynamode & OB_FH_NOR) != 0);
  ctrl->SetBuoyancyVolumeEnabled(
      isSensor && (blenderobject->gameflag2 & OB_JOLT_BUOYANCY_VOLUME) != 0);
  ctrl->SetLogicObjectSensorIncludeStatic(
      isSensor && (blenderobject->gameflag2 & OB_JOLT_SENSOR_INCLUDE_STATIC) != 0);
  ctrl->SetBuoyancy(blenderobject->jolt_buoyancy);
  ctrl->SetBuoyancyLinearDrag(blenderobject->jolt_buoyancy_linear_drag);
  ctrl->SetBuoyancyAngularDrag(blenderobject->jolt_buoyancy_angular_drag);
  ctrl->SetBuoyancyFluidVelocity(JoltMath::ToJolt(blenderobject->jolt_buoyancy_velocity[0],
                                                   blenderobject->jolt_buoyancy_velocity[1],
                                                   blenderobject->jolt_buoyancy_velocity[2]));
  ctrl->SetLinVelocityMin(blenderobject->min_vel);
  ctrl->SetLinVelocityMax(blenderobject->max_vel);
  ctrl->SetAngularVelocityMin(blenderobject->min_angvel);
  ctrl->SetAngularVelocityMax(blenderobject->max_angvel);

  /* Set the shared constraint group filter and store the body's unique ID
   * in SubGroupID for constraint "Disable Collisions" filtering.
   * Primary collection filtering is done by JoltObjectLayerPairFilter
   * via the ObjectLayer set above. */
  body->SetCollisionGroup(JPH::CollisionGroup(m_constraintGroupFilter,
                                               (JPH::CollisionGroup::GroupID)col_group,
                                               body->GetID().GetIndexAndSequenceNumber()));

  /* Store user data on body for fast lookup in ContactListener. */
  body->SetUserData(reinterpret_cast<JPH::uint64>(gameobj->getClientInfo()));

  gameobj->SetPhysicsController(ctrl);
  ctrl->SetNewClientInfo(gameobj->getClientInfo());
  ctrl->SetCollisionGroup(col_group);
  ctrl->SetCollisionMask(col_mask);

  /* --- Character controller: create JoltCharacter alongside the rigid body --- */
  if (isCharacter) {
    float capsuleRadius = std::max(bounds_extends[0], bounds_extends[1]);
    float capsuleHalfHeight = bounds_extends[2] - capsuleRadius;
    if (capsuleHalfHeight < 0.01f) {
      capsuleHalfHeight = 0.01f;
    }
    float stepHeight = blenderobject->step_height;
    JoltCharacter *character = new JoltCharacter(
        ctrl, this, capsuleRadius, capsuleHalfHeight, stepHeight, pos);
    character->SetJumpSpeed(blenderobject->jump_speed);
    character->SetFallSpeed(blenderobject->fall_speed);
    character->SetMaxSlope(blenderobject->max_slope);
    character->SetMaxJumps(blenderobject->max_jumps);
    m_characterByObject[gameobj] = character;
  }

  /* Don't add sensor objects automatically — they are added when a collision sensor registers. */
  bool layCheck = (blenderobject->lay & activeLayerBitInfo) != 0;
  bool visCheck = (blenderobject->base_flag &
                            (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                             BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT)) != 0;
  ctrl->SetLogicObjectSensorActive(isSensor && layCheck && visCheck);
  ctrl->SetLogicCollisionQueryActive(layCheck && visCheck);
  if (!isSensor && layCheck && visCheck)
  {
    if (!m_hasSteppedSimulation) {
      QueueRigidBodyBodyAdd(body->GetID(), JPH::EActivation::Activate);
    }
    else {
      bi.AddBody(body->GetID(), JPH::EActivation::Activate);
      NotifyRigidBodyBodyAdded();
    }
    AddController(ctrl);
  }
  else if (!isSensor) {
    /* Body created but not added yet. */
    AddController(ctrl);
  }
  else {
    /* Sensor: the body is a query shape for C++ Logic Nodes and buoyancy. It enters Jolt's
     * broadphase only if a legacy collision sensor/callback registers it through AddSensor(). */
    AddController(ctrl);
  }

  /* Register blender object → controller mapping so soft-body pin_object lookup works. */
  RegisterControllerForObject(blenderobject, ctrl);

  /* Suspend dynamics for parented objects. */
  blender::Object *blenderRoot = blenderobject->parent;
  while (blenderRoot && blenderRoot->parent) {
    blenderRoot = blenderRoot->parent;
  }
  if (blenderRoot && converter) {
    converter->AddPendingSuspendDynamics(ctrl);
  }
}

void JoltPhysicsEnvironment::SetupObjectConstraints(
    KX_GameObject *obj_src,
    KX_GameObject *obj_dest,
    blender::bRigidBodyJointConstraint *dat,
    bool replicate_dupli)
{
  PHY_IPhysicsController *phy_src = obj_src->GetPhysicsController();
  PHY_IPhysicsController *phy_dest = obj_dest->GetPhysicsController();

  /* Build constraint frame from stored axes. */
  MT_Matrix3x3 localCFrame(MT_Vector3(dat->axX, dat->axY, dat->axZ));
  MT_Vector3 axis0 = localCFrame.getColumn(0);
  MT_Vector3 axis1 = localCFrame.getColumn(1);
  MT_Vector3 axis2 = localCFrame.getColumn(2);
  MT_Vector3 scale = obj_src->NodeGetWorldScaling();

  PHY_IConstraint *constraint = CreateConstraint(phy_src,
                                                  phy_dest,
                                                  (PHY_ConstraintType)dat->type,
                                                  (float)(dat->pivX * scale.x()),
                                                  (float)(dat->pivY * scale.y()),
                                                  (float)(dat->pivZ * scale.z()),
                                                  (float)(axis0.x() * scale.x()),
                                                  (float)(axis0.y() * scale.y()),
                                                  (float)(axis0.z() * scale.z()),
                                                  (float)(axis1.x() * scale.x()),
                                                  (float)(axis1.y() * scale.y()),
                                                  (float)(axis1.z() * scale.z()),
                                                  (float)(axis2.x() * scale.x()),
                                                  (float)(axis2.y() * scale.y()),
                                                  (float)(axis2.z() * scale.z()),
                                                  dat->flag,
                                                  replicate_dupli);

  if (!constraint) {
    return;
  }

  /* Set per-DOF limits based on constraint type. */
  int dof = 0;
  int dof_max = 0;
  int dofbit = 0;

  switch (dat->type) {
    case PHY_GENERIC_6DOF_CONSTRAINT:
      dof_max = 6;
      dofbit = 1;
      break;
    case PHY_CONE_TWIST_CONSTRAINT:
      dof = 3;
      dof_max = 6;
      dofbit = 1 << 3;
      break;
    case PHY_LINEHINGE_CONSTRAINT:
    case PHY_ANGULAR_CONSTRAINT:
      dof = 3;
      dof_max = 4;
      dofbit = 1 << 3;
      break;
    default:
      break;
  }

  for (; dof < dof_max; dof++) {
    if (dat->flag & dofbit) {
      constraint->SetParam(dof, dat->minLimit[dof], dat->maxLimit[dof]);
    }
    else {
      constraint->SetParam(dof, 1.0f, -1.0f);
    }
    dofbit <<= 1;
  }

  if (dat->flag & CONSTRAINT_USE_BREAKING) {
    constraint->SetBreakingThreshold(dat->breaking);
  }
}

int JoltPhysicsEnvironment::CreateRigidBodyConstraint(
    KX_GameObject *gameobj1,
    KX_GameObject *gameobj2,
    const MT_Vector3 &pivotLocal,
    const MT_Matrix3x3 &basisLocal,
    const PHY_RigidBodyConstraintSettings &settings)
{
  if (!gameobj1 || !gameobj2 || gameobj1 == gameobj2) {
    return -1;
  }

  if (!gameobj1->GetSGNode() || !gameobj2->GetSGNode()) {
    return -1;
  }

  PHY_IPhysicsController *ctrl1 = gameobj1->GetPhysicsController();
  PHY_IPhysicsController *ctrl2 = gameobj2 ? gameobj2->GetPhysicsController() : nullptr;

  if (!ctrl1 || !ctrl2 || ctrl1 == ctrl2) {
    return -1;
  }

  MT_Vector3 axis0 = basisLocal.getColumn(0).safe_normalized();
  MT_Vector3 axis1 = basisLocal.getColumn(1).safe_normalized();
  MT_Vector3 axis2 = basisLocal.getColumn(2).safe_normalized();

  /* Build an orthonormal frame for Jolt from the incoming local basis.
   * Jolt is stricter than Bullet about valid constraint frames and may reject or
   * behave badly with scaled/sheared axes from authoring transforms. */
  if (axis0.length2() < 1.0e-8f) {
    axis0 = MT_Vector3(1.0f, 0.0f, 0.0f);
  }

  axis1 = (axis1 - axis0.dot(axis1) * axis0).safe_normalized();
  if (axis1.length2() < 1.0e-8f) {
    axis1 = (axis2 - axis0.dot(axis2) * axis0).safe_normalized();
  }
  if (axis1.length2() < 1.0e-8f) {
    const MT_Vector3 fallback = (std::abs(axis0.z()) < 0.999f) ? MT_Vector3(0.0f, 0.0f, 1.0f) :
                                                              MT_Vector3(0.0f, 1.0f, 0.0f);
    axis1 = (fallback - axis0.dot(fallback) * axis0).safe_normalized();
  }

  axis2 = axis0.cross(axis1).safe_normalized();

  PHY_ConstraintType type = PHY_GENERIC_6DOF_CONSTRAINT;
  bool use_springs = false;
  bool is_slider = false;
  bool is_piston = false;
  bool is_motor = false;

  switch (settings.type) {
    case PHY_RigidBodyConstraintType::Point:
      type = PHY_POINT2POINT_CONSTRAINT;
      break;
    case PHY_RigidBodyConstraintType::Hinge:
      type = PHY_LINEHINGE_CONSTRAINT;
      std::swap(axis0, axis2);
      std::swap(axis1, axis2);
      break;
    case PHY_RigidBodyConstraintType::Slider:
      if ((settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X) == 0 ||
          !std::isfinite(settings.limit_lin_x_lower) ||
          !std::isfinite(settings.limit_lin_x_upper) ||
          settings.limit_lin_x_lower > settings.limit_lin_x_upper ||
          (settings.limit_lin_x_lower < settings.limit_lin_x_upper &&
           settings.limit_lin_x_lower <= 0.0f && settings.limit_lin_x_upper >= 0.0f))
      {
        type = PHY_SLIDER_CONSTRAINT;
      }
      else {
        /* Jolt's native slider requires limits to span zero. SixDOF preserves valid
         * offset and zero-width Blender slider ranges without approximation. */
        type = PHY_GENERIC_6DOF_CONSTRAINT;
        is_slider = true;
      }
      break;
    case PHY_RigidBodyConstraintType::Generic:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      break;
    case PHY_RigidBodyConstraintType::GenericSpring:
      type = PHY_GENERIC_6DOF_SPRING2_CONSTRAINT;
      use_springs = true;
      break;
    case PHY_RigidBodyConstraintType::Fixed:
      type = PHY_FIXED_CONSTRAINT;
      break;
    case PHY_RigidBodyConstraintType::Piston:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_piston = true;
      break;
    case PHY_RigidBodyConstraintType::Motor:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_motor = true;
      break;
    default:
      break;
  }

  int flag = 0;
  if (settings.flags & PHY_RB_CONSTRAINT_DISABLE_COLLISIONS) {
    flag |= CCD_CONSTRAINT_DISABLE_LINKED_COLLISION;
  }

  PHY_IConstraint *constraint = CreateConstraint(ctrl1,
                                                  ctrl2,
                                                  type,
                                                  pivotLocal.x(),
                                                  pivotLocal.y(),
                                                  pivotLocal.z(),
                                                  axis0.x(),
                                                  axis0.y(),
                                                  axis0.z(),
                                                  axis1.x(),
                                                  axis1.y(),
                                                  axis1.z(),
                                                  axis2.x(),
                                                  axis2.y(),
                                                  axis2.z(),
                                                  flag);

  if (!constraint) {
    return -1;
  }

  static_cast<JoltConstraint *>(constraint)->SetRigidBodyConstraintOwner(gameobj1);

  if (settings.flags & PHY_RB_CONSTRAINT_USE_BREAKING) {
    constraint->SetBreakingThreshold(settings.breaking_threshold);
  }

  if (settings.flags & PHY_RB_CONSTRAINT_JOLT_OVERRIDE_SOLVER_ITERATIONS) {
    static_cast<JoltConstraint *>(constraint)->SetSolverIterations(
        settings.jolt_velocity_solver_iterations, settings.jolt_position_solver_iterations);
  }

  auto set_limit = [&](int axis, bool use_flag, float lower, float upper) {
    const bool valid = use_flag && std::isfinite(lower) && std::isfinite(upper) && lower <= upper;
    constraint->SetParam(axis, valid ? lower : 1.0f, valid ? upper : -1.0f);
  };

  if (type == PHY_GENERIC_6DOF_CONSTRAINT || type == PHY_GENERIC_6DOF_SPRING2_CONSTRAINT) {
    if (is_slider) {
      set_limit(0,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X),
                settings.limit_lin_x_lower,
                settings.limit_lin_x_upper);
      constraint->SetParam(1, 0.0f, 0.0f);
      constraint->SetParam(2, 0.0f, 0.0f);
      constraint->SetParam(3, 0.0f, 0.0f);
      constraint->SetParam(4, 0.0f, 0.0f);
      constraint->SetParam(5, 0.0f, 0.0f);
    }
    else if (is_piston) {
      set_limit(0,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X),
                settings.limit_lin_x_lower,
                settings.limit_lin_x_upper);
      constraint->SetParam(1, 0.0f, 0.0f);
      constraint->SetParam(2, 0.0f, 0.0f);
      set_limit(3,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X),
                settings.limit_ang_x_lower,
                settings.limit_ang_x_upper);
      constraint->SetParam(4, 0.0f, 0.0f);
      constraint->SetParam(5, 0.0f, 0.0f);
    }
    else if (is_motor) {
      for (int i = 0; i < 6; ++i) {
        constraint->SetParam(i, 1.0f, -1.0f);
      }
      if (settings.flags & PHY_RB_CONSTRAINT_USE_MOTOR_LIN) {
        constraint->SetParam(
            6, settings.motor_lin_target_velocity, settings.motor_lin_max_impulse);
      }
      if (settings.flags & PHY_RB_CONSTRAINT_USE_MOTOR_ANG) {
        constraint->SetParam(
            9, settings.motor_ang_target_velocity, settings.motor_ang_max_impulse);
      }
    }
    else {
      set_limit(0,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X),
                settings.limit_lin_x_lower,
                settings.limit_lin_x_upper);
      set_limit(1,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Y),
                settings.limit_lin_y_lower,
                settings.limit_lin_y_upper);
      set_limit(2,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Z),
                settings.limit_lin_z_lower,
                settings.limit_lin_z_upper);
      set_limit(3,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X),
                settings.limit_ang_x_lower,
                settings.limit_ang_x_upper);
      set_limit(4,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Y),
                settings.limit_ang_y_lower,
                settings.limit_ang_y_upper);
      set_limit(5,
                (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z),
                settings.limit_ang_z_lower,
                settings.limit_ang_z_upper);

      if (use_springs) {
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_X) {
          constraint->SetParam(12, settings.spring_stiffness_x, settings.spring_damping_x);
        }
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_Y) {
          constraint->SetParam(13, settings.spring_stiffness_y, settings.spring_damping_y);
        }
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_Z) {
          constraint->SetParam(14, settings.spring_stiffness_z, settings.spring_damping_z);
        }
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_ANG_X) {
          constraint->SetParam(
              15, settings.spring_stiffness_ang_x, settings.spring_damping_ang_x);
        }
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_ANG_Y) {
          constraint->SetParam(
              16, settings.spring_stiffness_ang_y, settings.spring_damping_ang_y);
        }
        if (settings.flags & PHY_RB_CONSTRAINT_USE_SPRING_ANG_Z) {
          constraint->SetParam(
              17, settings.spring_stiffness_ang_z, settings.spring_damping_ang_z);
        }
      }
    }
  }
  else if (type == PHY_SLIDER_CONSTRAINT &&
           (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X) &&
           std::isfinite(settings.limit_lin_x_lower) &&
           std::isfinite(settings.limit_lin_x_upper) &&
           settings.limit_lin_x_lower < settings.limit_lin_x_upper)
  {
    constraint->SetParam(
        0, settings.limit_lin_x_lower, settings.limit_lin_x_upper);
  }
  else if (type == PHY_LINEHINGE_CONSTRAINT &&
           (settings.flags & PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z))
  {
    constraint->SetParam(3, settings.limit_ang_z_lower, settings.limit_ang_z_upper);
  }

  if ((settings.flags & PHY_RB_CONSTRAINT_ENABLED) == 0) {
    constraint->SetEnabled(false);
  }

  return constraint->GetIdentifier();
}

void JoltPhysicsEnvironment::SetRigidBodyConstraintEnabled(int constraintid, bool enabled)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end()) {
    return;
  }
  it->second->SetEnabled(enabled);
}

void JoltPhysicsEnvironment::ExportFile(const std::string &filename)
{
  /* Optional: serialize Jolt world state. */
}

bool JoltPhysicsEnvironment::SavePhysicsState(std::vector<uint8_t> &outBuffer)
{
  outBuffer.clear();
  for (JoltVehicle *veh : m_vehicles) {
    veh->Build();
  }

  JPH::StateRecorderImpl recorder;
  constexpr uint64_t logicContactStateMagic = 0x554C4E434F4E5441ull; /* "ULNCONTA" */
  constexpr uint32_t logicContactStateVersion = 1;
  recorder.Write(logicContactStateMagic);
  recorder.Write(logicContactStateVersion);

  if (m_logicActiveContactPairs.size() > UINT32_MAX) {
    return false;
  }
  const uint32_t logicContactCount = uint32_t(m_logicActiveContactPairs.size());
  recorder.Write(logicContactCount);

  std::vector<const JoltContactPair *> sortedLogicContacts;
  sortedLogicContacts.reserve(logicContactCount);
  for (const auto &item : m_logicActiveContactPairs) {
    sortedLogicContacts.push_back(&item.second);
  }
  std::sort(sortedLogicContacts.begin(),
            sortedLogicContacts.end(),
            [](const JoltContactPair *a, const JoltContactPair *b) {
              return a->subShapePair < b->subShapePair;
            });

  for (const JoltContactPair *pair : sortedLogicContacts) {
    recorder.Write(pair->subShapePair);
    recorder.Write(pair->contactNormal);
    recorder.Write(pair->contactPosition);
    for (const JPH::RVec3 &position : pair->contactPositions) {
      recorder.Write(position);
    }
    recorder.Write(pair->penetrationDepth);
    recorder.Write(pair->combinedFriction);
    recorder.Write(pair->combinedRestitution);
    recorder.Write(pair->appliedImpulse);
    recorder.Write(pair->contactCount);
    recorder.Write(pair->storedContactCount);
    const uint8_t flags = uint8_t(pair->isNew) | (uint8_t(pair->retainedWhileDormant) << 1);
    recorder.Write(flags);
  }

  m_physicsSystem->SaveState(recorder);
  if (recorder.IsFailed()) {
    return false;
  }

  std::string data = recorder.GetData();
  outBuffer.assign(data.begin(), data.end());
  return !outBuffer.empty();
}

bool JoltPhysicsEnvironment::RestorePhysicsState(const std::vector<uint8_t> &inBuffer)
{
  if (inBuffer.empty()) {
    return false;
  }

  constexpr uint64_t logicContactStateMagic = 0x554C4E434F4E5441ull; /* "ULNCONTA" */
  constexpr uint32_t logicContactStateVersion = 1;
  uint64_t bufferMagic = 0;
  if (inBuffer.size() >= sizeof(bufferMagic)) {
    std::memcpy(&bufferMagic, inBuffer.data(), sizeof(bufferMagic));
  }

  JPH::StateRecorderImpl recorder;
  /* Write the saved data into the recorder's stream, then rewind for reading. */
  recorder.WriteBytes(inBuffer.data(), inBuffer.size());
  recorder.Rewind();

  std::vector<JoltContactPair> restoredLogicContacts;
  if (bufferMagic == logicContactStateMagic) {
    constexpr size_t headerSize = sizeof(uint64_t) + 2 * sizeof(uint32_t);
    constexpr size_t contactRecordSize =
        sizeof(JPH::SubShapeIDPair) + 3 * sizeof(float) +
        (JOLT_MAX_CACHED_CONTACT_POINTS + 1) * 3 * sizeof(JPH::Real) +
        4 * sizeof(float) + sizeof(uint16_t) + 2 * sizeof(uint8_t);
    uint64_t magic = 0;
    uint32_t version = 0;
    uint32_t contactCount = 0;
    recorder.Read(magic);
    recorder.Read(version);
    recorder.Read(contactCount);
    if (recorder.IsFailed() || magic != logicContactStateMagic ||
        version != logicContactStateVersion || inBuffer.size() < headerSize ||
        contactCount > (inBuffer.size() - headerSize) / contactRecordSize)
    {
      return false;
    }

    restoredLogicContacts.reserve(contactCount);
    for (uint32_t contactIndex = 0; contactIndex < contactCount; contactIndex++) {
      JoltContactPair pair{};
      recorder.Read(pair.subShapePair);
      recorder.Read(pair.contactNormal);
      recorder.Read(pair.contactPosition);
      for (JPH::RVec3 &position : pair.contactPositions) {
        recorder.Read(position);
      }
      recorder.Read(pair.penetrationDepth);
      recorder.Read(pair.combinedFriction);
      recorder.Read(pair.combinedRestitution);
      recorder.Read(pair.appliedImpulse);
      recorder.Read(pair.contactCount);
      recorder.Read(pair.storedContactCount);
      uint8_t flags = 0;
      recorder.Read(flags);
      if (recorder.IsFailed() || pair.storedContactCount > JOLT_MAX_CACHED_CONTACT_POINTS ||
          (flags & ~uint8_t(3)) != 0)
      {
        return false;
      }
      pair.bodyID1 = pair.subShapePair.GetBody1ID();
      pair.bodyID2 = pair.subShapePair.GetBody2ID();
      pair.isNew = (flags & 1) != 0;
      pair.retainedWhileDormant = (flags & 2) != 0;
      restoredLogicContacts.push_back(pair);
    }
  }

  if (!m_physicsSystem->RestoreState(recorder)) {
    return false;
  }

  /* Jolt restores its own contact manager, but queued listener events describe
   * the post-snapshot timeline. Drop those events before installing the saved,
   * pointer-free Logic Nodes contact state. */
  m_contactPairsScratch.clear();
  m_removedContactPairsScratch.clear();
  m_contactEventsScratch.clear();
  m_contactListener.SwapContactEvents(
      m_contactPairsScratch, m_removedContactPairsScratch, m_contactEventsScratch);
  m_contactPairsScratch.clear();
  m_removedContactPairsScratch.clear();
  m_contactEventsScratch.clear();
  m_logicCollisionContactCacheValid = false;
  m_logicCollisionContactDetailsValid = false;
  m_logicActiveContactPairs.clear();
  m_logicCollisionContacts.clear();
  m_logicObjectSensorOverlapsByController.clear();
  m_logicCollisionContactsByController.clear();
  m_logicCollisionContactsByObject.clear();

  if (m_collectContactsForLogicNodes.load(std::memory_order_relaxed)) {
    const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
    for (JoltContactPair &pair : restoredLogicContacts) {
      if (pair.bodyID1.IsInvalid() || pair.bodyID2.IsInvalid()) {
        continue;
      }

      {
        const JPH::BodyID bodyIDs[2] = {pair.bodyID1, pair.bodyID2};
        JPH::BodyLockMultiRead lock(lockInterface, bodyIDs, 2);
        const JPH::Body *body1 = lock.GetBody(0);
        const JPH::Body *body2 = lock.GetBody(1);
        if (!body1 || !body2) {
          continue;
        }

        KX_ClientObjectInfo *info1 = JoltGetClientInfo(*body1);
        KX_ClientObjectInfo *info2 = JoltGetClientInfo(*body2);
        pair.ctrl1 = info1 ? static_cast<JoltPhysicsController *>(
                                 JoltGetPhysicsController(info1)) :
                             nullptr;
        pair.ctrl2 = info2 ? static_cast<JoltPhysicsController *>(
                                 JoltGetPhysicsController(info2)) :
                             nullptr;
        pair.object1 = info1 ? info1->m_gameobject : nullptr;
        pair.object2 = info2 ? info2->m_gameobject : nullptr;
      }

      if (!pair.ctrl1 || !pair.ctrl2 || !pair.object1 || !pair.object2 ||
          m_controllers.find(pair.ctrl1) == m_controllers.end() ||
          m_controllers.find(pair.ctrl2) == m_controllers.end() ||
          pair.ctrl1->GetBodyID() != pair.bodyID1 || pair.ctrl2->GetBodyID() != pair.bodyID2 ||
          !CheckCollision(pair.ctrl1, pair.ctrl2, false).collide)
      {
        continue;
      }
      m_logicActiveContactPairs[pair.subShapePair] = pair;
    }
  }

  for (JoltVehicle *veh : m_vehicles) {
    veh->SyncWheels();
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — Jolt-specific methods
 * \{ */

JPH::BodyInterface &JoltPhysicsEnvironment::GetBodyInterface()
{
  return m_physicsSystem->GetBodyInterface();
}

JPH::BodyInterface &JoltPhysicsEnvironment::GetBodyInterfaceNoLock()
{
  return m_physicsSystem->GetBodyInterfaceNoLock();
}

void JoltPhysicsEnvironment::AddController(JoltPhysicsController *ctrl)
{
  if (!ctrl) {
    return;
  }

  const std::pair<std::set<JoltPhysicsController *>::iterator, bool> insertResult =
      m_controllers.insert(ctrl);
  if (insertResult.second) {
    m_controllersIterationCacheDirty = true;
    m_fhControllersIterationCacheDirty = true;
    m_buoyancyVolumesIterationCacheDirty = true;
  }
}

bool JoltPhysicsEnvironment::RemoveController(JoltPhysicsController *ctrl)
{
  if (!ctrl) {
    return false;
  }

  const bool removed = m_controllers.erase(ctrl) > 0;
  if (!removed) {
    return false;
  }

  m_controllersIterationCacheDirty = true;
  m_fhControllersIterationCacheDirty = true;
  m_buoyancyVolumesIterationCacheDirty = true;
  m_pendingCompoundSubShapesByController.erase(ctrl);
  RemoveLogicActiveContactsForController(ctrl);

  m_collisionCallbackControllers.erase(ctrl);
  m_collectContactsForCallbacks.store(!m_collisionCallbackControllers.empty(),
                                      std::memory_order_relaxed);

  for (auto it = m_controllerByBlenderObject.begin();
       it != m_controllerByBlenderObject.end();) {
    if (it->second == ctrl) {
      it = m_controllerByBlenderObject.erase(it);
    }
    else {
      ++it;
    }
  }

  for (JoltSoftBody *sb : m_softBodies) {
    if (sb && sb->GetPinController() == ctrl) {
      if (!sb->GetBodyID().IsInvalid()) {
        m_softBodyContactListener.Unregister(sb->GetBodyID());
      }
      sb->SetPinController(nullptr);
    }
  }

  for (auto it = m_characterByObject.begin(); it != m_characterByObject.end();) {
    JoltCharacter *character = it->second;
    if (character && character->UsesController(ctrl)) {
      delete character;
      it = m_characterByObject.erase(it);
    }
    else {
      ++it;
    }
  }

  const JPH::BodyID bodyID = ctrl->GetBodyID();
  if (bodyID.IsInvalid()) {
    for (auto it = m_vehicles.begin(); it != m_vehicles.end();) {
      if ((*it)->UsesController(ctrl)) {
        delete *it;
        it = m_vehicles.erase(it);
      }
      else {
        ++it;
      }
    }
    return true;
  }
  RemovePendingRigidBodyBodyAdd(bodyID);

  for (auto it = m_vehicles.begin(); it != m_vehicles.end();) {
    if ((*it)->UsesController(ctrl)) {
      delete *it;
      it = m_vehicles.erase(it);
    }
    else {
      ++it;
    }
  }

  return true;
}

void JoltPhysicsEnvironment::InvalidateFhControllersCache()
{
  m_fhControllersIterationCacheDirty = true;
}

const std::vector<JoltPhysicsController *> &JoltPhysicsEnvironment::GetControllersForIteration()
{
  if (!m_controllersIterationCacheDirty) {
    return m_controllersIterationCache;
  }

  m_controllersIterationCache.clear();
  m_controllersIterationCache.reserve(m_controllers.size());
  for (JoltPhysicsController *ctrl : m_controllers) {
    m_controllersIterationCache.push_back(ctrl);
  }
  m_controllersIterationCacheDirty = false;

  return m_controllersIterationCache;
}

const std::vector<JoltPhysicsController *> &JoltPhysicsEnvironment::GetFhControllersForIteration()
{
  if (!m_fhControllersIterationCacheDirty) {
    return m_fhControllersIterationCache;
  }

  m_fhControllersIterationCache.clear();
  m_fhControllersIterationCache.reserve(m_controllers.size());
  for (JoltPhysicsController *ctrl : m_controllers) {
    if (ctrl && ctrl->UsesFhSpring()) {
      m_fhControllersIterationCache.push_back(ctrl);
    }
  }
  m_fhControllersIterationCacheDirty = false;

  return m_fhControllersIterationCache;
}

void JoltPhysicsEnvironment::InvalidateBuoyancyVolumesCache()
{
  m_buoyancyVolumesIterationCacheDirty = true;
}

const std::vector<JoltPhysicsController *> &
JoltPhysicsEnvironment::GetBuoyancyVolumesForIteration()
{
  if (!m_buoyancyVolumesIterationCacheDirty) {
    return m_buoyancyVolumesIterationCache;
  }

  m_buoyancyVolumesIterationCache.clear();
  m_buoyancyVolumesIterationCache.reserve(m_controllers.size());
  for (JoltPhysicsController *ctrl : m_controllers) {
    if (ctrl && ctrl->UsesBuoyancyVolume()) {
      m_buoyancyVolumesIterationCache.push_back(ctrl);
    }
  }
  m_buoyancyVolumesIterationCacheDirty = false;

  return m_buoyancyVolumesIterationCache;
}

void JoltPhysicsEnvironment::AddGraphicController(JoltGraphicController *ctrl)
{
  m_graphicControllers.insert(ctrl);
}

void JoltPhysicsEnvironment::RemoveGraphicController(JoltGraphicController *ctrl)
{
  m_graphicControllers.erase(ctrl);
}

void JoltPhysicsEnvironment::RemoveLogicActiveContactsForBody(const JPH::BodyID bodyID)
{
  if (bodyID.IsInvalid()) {
    return;
  }

  bool removed = false;
  for (const auto &sensorItem : m_logicObjectSensorOverlapsByController) {
    if ((sensorItem.first && sensorItem.first->GetBodyID() == bodyID) ||
        std::any_of(sensorItem.second.begin(),
                    sensorItem.second.end(),
                    [&](const PHY_CachedCollisionContact &contact) {
                      const JoltPhysicsController *ctrl0 =
                          static_cast<const JoltPhysicsController *>(contact.ctrl0);
                      const JoltPhysicsController *ctrl1 =
                          static_cast<const JoltPhysicsController *>(contact.ctrl1);
                      return (ctrl0 && ctrl0->GetBodyID() == bodyID) ||
                             (ctrl1 && ctrl1->GetBodyID() == bodyID);
                    }))
    {
      removed = true;
      break;
    }
  }
  for (auto it = m_logicActiveContactPairs.begin(); it != m_logicActiveContactPairs.end();) {
    const JoltContactPair &pair = it->second;
    if (pair.bodyID1 == bodyID || pair.bodyID2 == bodyID) {
      removed = true;
      it = m_logicActiveContactPairs.erase(it);
    }
    else {
      ++it;
    }
  }

  if (removed) {
    m_logicCollisionContactCacheValid = false;
    m_logicCollisionContactDetailsValid = false;
    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByObject.clear();
  }
}

void JoltPhysicsEnvironment::RemoveLogicActiveContactsForController(JoltPhysicsController *ctrl)
{
  if (!ctrl) {
    return;
  }

  bool removed = m_logicObjectSensorOverlapsByController.find(ctrl) !=
                 m_logicObjectSensorOverlapsByController.end();
  if (!removed) {
    for (const auto &sensorItem : m_logicObjectSensorOverlapsByController) {
      if (std::any_of(sensorItem.second.begin(),
                      sensorItem.second.end(),
                      [&](const PHY_CachedCollisionContact &contact) {
                        return contact.ctrl0 == ctrl || contact.ctrl1 == ctrl;
                      }))
      {
        removed = true;
        break;
      }
    }
  }
  for (auto it = m_logicActiveContactPairs.begin(); it != m_logicActiveContactPairs.end();) {
    const JoltContactPair &pair = it->second;
    if (pair.ctrl1 == ctrl || pair.ctrl2 == ctrl) {
      removed = true;
      it = m_logicActiveContactPairs.erase(it);
    }
    else {
      ++it;
    }
  }

  if (removed) {
    m_logicCollisionContactCacheValid = false;
    m_logicCollisionContactDetailsValid = false;
    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByObject.clear();
  }
}

void JoltPhysicsEnvironment::CollectLogicObjectQueryOverlaps(
    JoltPhysicsController *queryController, const bool collectContactDetails)
{
  /* Presence in this map, including an empty vector, memoizes this query for the current
   * cache generation. unordered_map rehashing preserves references to value storage. */
  auto [overlapIt, inserted] = m_logicObjectSensorOverlapsByController.try_emplace(
      queryController);
  if (!inserted) {
    return;
  }
  std::vector<PHY_CachedCollisionContact> &sensorOverlaps = overlapIt->second;

  if (!queryController || !queryController->IsLogicCollisionQueryActive() ||
      queryController->GetBodyID().IsInvalid())
  {
    return;
  }

  KX_ClientObjectInfo *sensorInfo = static_cast<KX_ClientObjectInfo *>(
      queryController->GetNewClientInfo());
  if (!sensorInfo || !sensorInfo->m_gameobject ||
      JoltGetPhysicsController(sensorInfo) != queryController)
  {
    return;
  }

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  const JPH::NarrowPhaseQuery &query = m_physicsSystem->GetNarrowPhaseQuery();
  JPH::CollideShapeSettings settings;
  /* Sensor events represent actual overlap, never speculative solid contacts. */
  settings.mMaxSeparationDistance = 0.0f;
  JPH::ClosestHitPerBodyCollisionCollector<JPH::CollideShapeCollector> collector;

  JPH::RefConst<JPH::Shape> sensorShape;
  JPH::RMat44 sensorTransform;
  JPH::ObjectLayer sensorBodyLayer = JPH::ObjectLayer(0);
  JPH::CollisionGroup sensorCollisionGroup;
  {
    JPH::BodyLockRead lock(lockInterface, queryController->GetBodyID());
    if (!lock.Succeeded()) {
      return;
    }
    const JPH::Body &sensorBody = lock.GetBody();
    sensorShape = sensorBody.GetShape();
    sensorTransform = JoltLogicQueryTransform(
        *queryController, *sensorShape, sensorBody.GetCenterOfMassTransform());
    sensorBodyLayer = sensorBody.GetObjectLayer();
    sensorCollisionGroup = sensorBody.GetCollisionGroup();
  }
  if (!sensorShape) {
    return;
  }

  const bool includeStatic = queryController->IsSensor() ?
                                 queryController->GetLogicObjectSensorIncludeStatic() :
                                 true;
  const JoltLogicObjectSensorBroadPhaseLayerFilter broadPhaseFilter(includeStatic);
  const JPH::ObjectLayer sensorQueryLayer = JoltMakeObjectLayer(
      JoltGetCollisionLayers(sensorBodyLayer), 0, JOLT_BP_SENSOR);
  const JPH::DefaultObjectLayerFilter objectLayerFilter(
      m_objectLayerPairFilter, sensorQueryLayer);
  const JoltLogicObjectSensorBodyFilter bodyFilter(queryController->GetBodyID(),
                                                    sensorCollisionGroup,
                                                    *sensorInfo,
                                                    includeStatic,
                                                    m_controllers);

  /* One deepest result per body prevents mesh/compound sub-shapes from multiplying the same
   * logical overlap and bounds both memory traffic and Logic Nodes output size. */
  query.CollideShape(sensorShape.GetPtr(),
                     JPH::Vec3::sReplicate(1.0f),
                     sensorTransform,
                     settings,
                     JPH::RVec3::sZero(),
                     collector,
                     broadPhaseFilter,
                     objectLayerFilter,
                     bodyFilter,
                     {});

  std::sort(collector.mHits.begin(),
            collector.mHits.end(),
            [](const JPH::CollideShapeResult &a, const JPH::CollideShapeResult &b) {
              return a.mBodyID2.GetIndexAndSequenceNumber() <
                     b.mBodyID2.GetIndexAndSequenceNumber();
            });
  sensorOverlaps.reserve(collector.mHits.size());

  for (const JPH::CollideShapeResult &hit : collector.mHits) {
    JPH::BodyLockRead lock(lockInterface, hit.mBodyID2);
    if (!lock.Succeeded()) {
      continue;
    }
    const JPH::Body &otherBody = lock.GetBody();
    if (!bodyFilter.ShouldCollideLocked(otherBody)) {
      continue;
    }
    KX_ClientObjectInfo *otherInfo = JoltGetClientInfo(otherBody);
    JoltPhysicsController *otherController = otherInfo ?
        static_cast<JoltPhysicsController *>(JoltGetPhysicsController(otherInfo)) :
        nullptr;
    if (!otherController || !otherInfo->m_gameobject) {
      continue;
    }

    PHY_CachedCollisionContact contact;
    contact.ctrl0 = queryController;
    contact.ctrl1 = otherController;
    contact.object0 = sensorInfo->m_gameobject;
    contact.object1 = otherInfo->m_gameobject;
    contact.contact_count = 1;
    if (collectContactDetails) {
      contact.points.push_back(JoltMath::ToMT(hit.mContactPointOn2));
      contact.normals.push_back(JoltLogicContactNormal(hit.mPenetrationAxis));
    }
    sensorOverlaps.push_back(std::move(contact));
  }

  /* Sensor overlaps are deliberately one-sided: solid targets do not receive trigger-only
   * collision events. Publish pointers only after vector growth is complete. */
  if (!sensorOverlaps.empty()) {
    std::vector<const PHY_CachedCollisionContact *> &contactRefs =
        m_logicCollisionContactsByController[queryController];
    contactRefs.reserve(contactRefs.size() + sensorOverlaps.size());
    for (const PHY_CachedCollisionContact &contact : sensorOverlaps) {
      contactRefs.push_back(&contact);
    }
  }
}

void JoltPhysicsEnvironment::CallbackTriggers()
{
  const bool collectForCallbacks =
      m_collectContactsForCallbacks.load(std::memory_order_relaxed) &&
      m_triggerCallbacks[PHY_OBJECT_RESPONSE] != nullptr;
  const bool collectForLogicNodes = m_collectContactsForLogicNodes.load(std::memory_order_relaxed);
  const bool collectDetailsForLogicNodes =
      collectForLogicNodes &&
      m_collectContactDetailsForLogicNodes.load(std::memory_order_relaxed);

  if (!collectForCallbacks && !collectForLogicNodes) {
    m_contactPairsScratch.clear();
    m_removedContactPairsScratch.clear();
    m_contactEventsScratch.clear();
    m_contactListener.SwapContactEvents(
        m_contactPairsScratch, m_removedContactPairsScratch, m_contactEventsScratch);
    m_contactPairsScratch.clear();
    m_removedContactPairsScratch.clear();
    m_contactEventsScratch.clear();
    m_logicCollisionContactCacheValid = false;
    m_logicCollisionContactDetailsValid = false;
    m_logicActiveContactPairs.clear();
    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByObject.clear();
    return;
  }

  /* Swap contact pairs from the listener (accumulated during Update()). */
  m_contactPairsScratch.clear();
  m_removedContactPairsScratch.clear();
  m_contactEventsScratch.clear();
  m_contactListener.SwapContactEvents(
      m_contactPairsScratch, m_removedContactPairsScratch, m_contactEventsScratch);
  m_logicCollisionContactCacheValid = collectForLogicNodes;
  m_logicCollisionContactDetailsValid = collectDetailsForLogicNodes;

  if (collectForLogicNodes) {
    JPH::BodyInterface &bodyInterface = GetBodyInterface();
    auto logicPairIsValid = [&](const JoltContactPair &pair) {
      return pair.ctrl1 && pair.ctrl2 && pair.object1 && pair.object2 &&
             !JoltIsObjectSensor(pair.ctrl1) && !JoltIsObjectSensor(pair.ctrl2) &&
             !pair.bodyID1.IsInvalid() && !pair.bodyID2.IsInvalid() &&
             m_controllers.find(pair.ctrl1) != m_controllers.end() &&
             m_controllers.find(pair.ctrl2) != m_controllers.end() &&
             pair.ctrl1->GetBodyID() == pair.bodyID1 && pair.ctrl2->GetBodyID() == pair.bodyID2 &&
             bodyInterface.IsAdded(pair.bodyID1) && bodyInterface.IsAdded(pair.bodyID2);
    };
    auto logicPairLayersCanCollide = [&](const JoltContactPair &pair) {
      const JPH::ObjectLayer layer1 = bodyInterface.GetObjectLayer(pair.bodyID1);
      const JPH::ObjectLayer layer2 = bodyInterface.GetObjectLayer(pair.bodyID2);
      if (!m_objectLayerPairFilter.ShouldCollide(layer1, layer2)) {
        return false;
      }
      const JPH::BroadPhaseLayer bp1 = m_broadPhaseLayerInterface.GetBroadPhaseLayer(layer1);
      const JPH::BroadPhaseLayer bp2 = m_broadPhaseLayerInterface.GetBroadPhaseLayer(layer2);
      return m_objectVsBroadPhaseLayerFilter.ShouldCollide(layer1, bp2) &&
             m_objectVsBroadPhaseLayerFilter.ShouldCollide(layer2, bp1);
    };
    auto logicPairIsDormant = [&](const JoltContactPair &pair) {
      const JPH::EMotionType motionType1 = bodyInterface.GetMotionType(pair.bodyID1);
      const JPH::EMotionType motionType2 = bodyInterface.GetMotionType(pair.bodyID2);
      const bool hasMovableBody = motionType1 != JPH::EMotionType::Static ||
                                  motionType2 != JPH::EMotionType::Static;
      const bool body1Dormant = motionType1 == JPH::EMotionType::Static ||
                                !bodyInterface.IsActive(pair.bodyID1);
      const bool body2Dormant = motionType2 == JPH::EMotionType::Static ||
                                !bodyInterface.IsActive(pair.bodyID2);
      return hasMovableBody && body1Dormant && body2Dormant;
    };

    auto processContact = [&](const JoltContactPair &pair) {
      if (!pair.ctrl1 || !pair.ctrl2 || !pair.object1 || !pair.object2 ||
          pair.bodyID1.IsInvalid() || pair.bodyID2.IsInvalid())
      {
        return;
      }
      m_logicActiveContactPairs[pair.subShapePair] = pair;
    };
    auto processRemoval = [&](const JoltRemovedContactPair &removal) {
      auto it = m_logicActiveContactPairs.find(removal.subShapePair);
      if (it == m_logicActiveContactPairs.end()) {
        return;
      }

      const JoltContactPair &pair = it->second;
      if (!logicPairIsValid(pair)) {
        m_logicActiveContactPairs.erase(it);
        return;
      }

      /* If another sub-shape contact between the same bodies still exists,
       * erase this exact contact and let the remaining contacts keep the
       * high-level body collision alive. */
      if (m_physicsSystem->WereBodiesInContact(removal.bodyID1, removal.bodyID2)) {
        m_logicActiveContactPairs.erase(it);
        return;
      }

      /* Jolt removes all contacts once no movable body in the pair is active.
       * Preserve that logical collision only while the pair is genuinely dormant
       * and a fresh narrow-phase query confirms it is still touching. Active
       * manifold removals must never be retained: box/mesh contacts can change
       * sub-shape IDs while their speculative bounds still overlap. */
      if (!logicPairLayersCanCollide(pair) || !logicPairIsDormant(pair)) {
        m_logicActiveContactPairs.erase(it);
        return;
      }

      const PHY_CollisionTestResult stillTouching = CheckCollision(pair.ctrl1, pair.ctrl2, false);
      if (stillTouching.collide) {
        it->second.retainedWhileDormant = true;
      }
      else {
        m_logicActiveContactPairs.erase(it);
      }
    };

    /* Jolt serializes callbacks for each body pair, and every event for a body pair
     * hashes to the same shard. Replay each shard's compact event sequence so that
     * multiple collision substeps and CCD cannot collapse Added/Removed ordering. */
    for (const JoltContactEvent &event : m_contactEventsScratch) {
      if (event.type == JoltContactEventType::Contact) {
        if (event.index < m_contactPairsScratch.size()) {
          processContact(m_contactPairsScratch[event.index]);
        }
        continue;
      }
      if (event.index < m_removedContactPairsScratch.size()) {
        processRemoval(m_removedContactPairsScratch[event.index]);
      }
    }

    for (auto it = m_logicActiveContactPairs.begin(); it != m_logicActiveContactPairs.end();) {
      const JoltContactPair &pair = it->second;
      if (!logicPairIsValid(pair) ||
          (pair.retainedWhileDormant &&
           (!logicPairLayersCanCollide(pair) || !logicPairIsDormant(pair))))
      {
        it = m_logicActiveContactPairs.erase(it);
      }
      else {
        ++it;
      }
    }

    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    /* At most one root contact and one compound-child contact are emitted per
     * active Jolt sub-shape pair. Reserve first so every published pointer stays stable. */
    m_logicCollisionContacts.reserve(m_logicActiveContactPairs.size() * 2);
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByController.reserve(m_logicActiveContactPairs.size() * 2);
    m_logicCollisionContactsByObject.clear();
    m_logicCollisionContactsByObject.reserve(m_logicActiveContactPairs.size());

    auto appendCachedContact = [&](const JoltContactPair &pair,
                                   KX_GameObject *object0,
                                   KX_GameObject *object1) {
      PHY_CachedCollisionContact cachedContact;
      cachedContact.ctrl0 = pair.ctrl1;
      cachedContact.ctrl1 = pair.ctrl2;
      cachedContact.object0 = object0;
      cachedContact.object1 = object1;
      cachedContact.contact_count = std::max<int>(1, int(pair.contactCount));
      if (collectDetailsForLogicNodes) {
        cachedContact.points.reserve(pair.storedContactCount > 0 ? pair.storedContactCount : 1);
        cachedContact.normals.reserve(pair.storedContactCount > 0 ? pair.storedContactCount : 1);
        if (pair.storedContactCount > 0) {
          for (uint8_t i = 0; i < pair.storedContactCount; i++) {
            cachedContact.points.push_back(JoltMath::ToMT(JPH::Vec3(pair.contactPositions[i])));
            cachedContact.normals.push_back(JoltMath::ToMT(pair.contactNormal));
          }
        }
        else {
          cachedContact.points.push_back(JoltMath::ToMT(JPH::Vec3(pair.contactPosition)));
          cachedContact.normals.push_back(JoltMath::ToMT(pair.contactNormal));
        }
      }
      m_logicCollisionContacts.push_back(std::move(cachedContact));
      return &m_logicCollisionContacts.back();
    };

    for (const auto &activePairItem : m_logicActiveContactPairs) {
      const JoltContactPair &pair = activePairItem.second;
      const PHY_CachedCollisionContact *rootContact = appendCachedContact(
          pair, pair.object1, pair.object2);
      m_logicCollisionContactsByController[pair.ctrl1].push_back(rootContact);
      m_logicCollisionContactsByController[pair.ctrl2].push_back(rootContact);

      KX_GameObject *child1 = pair.ctrl1->ResolveCompoundChildObject(
          pair.subShapePair.GetSubShapeID1());
      KX_GameObject *child2 = pair.ctrl2->ResolveCompoundChildObject(
          pair.subShapePair.GetSubShapeID2());
      if (child1 || child2) {
        const PHY_CachedCollisionContact *childContact = appendCachedContact(
            pair, child1 ? child1 : pair.object1, child2 ? child2 : pair.object2);
        if (child1) {
          m_logicCollisionContactsByObject[child1].push_back(childContact);
        }
        if (child2) {
          m_logicCollisionContactsByObject[child2].push_back(childContact);
        }
      }
    }

    /* Query overlap vectors remain empty until C++ Logic Nodes requests a specific controller. */
  }
  else {
    m_logicActiveContactPairs.clear();
    m_logicCollisionContacts.clear();
    m_logicObjectSensorOverlapsByController.clear();
    m_logicCollisionContactsByController.clear();
    m_logicCollisionContactsByObject.clear();
  }

  for (const JoltContactPair &pair : m_contactPairsScratch) {
    JoltPhysicsController *jctrl1 = pair.ctrl1;
    JoltPhysicsController *jctrl2 = pair.ctrl2;
    KX_GameObject *object1 = pair.object1;
    KX_GameObject *object2 = pair.object2;
    if (!jctrl1 || !jctrl2 || !object1 || !object2) {
      continue;
    }

    PHY_IPhysicsController *ctrl1 = jctrl1;
    PHY_IPhysicsController *ctrl2 = jctrl2;

    if (!collectForCallbacks) {
      continue;
    }

    bool first;
    if (m_collisionCallbackControllers.count(jctrl1)) {
      first = true;
    }
    else if (m_collisionCallbackControllers.count(jctrl2)) {
      first = false;
    }
    else {
      continue;
    }

    JoltCollData *collData = new JoltCollData();
    MT_Vector3 worldPoint = JoltMath::ToMT(JPH::Vec3(pair.contactPosition));
    MT_Vector3 normal = JoltMath::ToMT(pair.contactNormal);
    /* localA and localB are approximated as the world point for now.
     * Full local-space computation requires body transforms. */
    collData->AddContactPoint(worldPoint,
                              worldPoint,
                              worldPoint,
                              normal,
                              pair.combinedFriction,
                              pair.combinedRestitution,
                              pair.appliedImpulse);

    /* Invoke the collision callback. */
    m_triggerCallbacks[PHY_OBJECT_RESPONSE](
        m_triggerCallbacksUserPtrs[PHY_OBJECT_RESPONSE], ctrl1, ctrl2, collData, first);
  }
}

void JoltPhysicsEnvironment::ProcessBuoyancy(float timeStep)
{
  if (timeStep <= 0.0f) {
    return;
  }

  const std::vector<JoltPhysicsController *> &volumes = GetBuoyancyVolumesForIteration();
  if (volumes.empty()) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();
  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const JPH::Vec3 gravity = m_physicsSystem->GetGravity();
  const JoltBuoyancyBroadPhaseLayerFilter broadPhaseFilter;

  JPH::CollideShapeSettings collideSettings;
  collideSettings.mMaxSeparationDistance = 0.0f;

  for (JoltPhysicsController *volumeCtrl : volumes) {
    if (!volumeCtrl || !volumeCtrl->UsesBuoyancyVolume()) {
      continue;
    }

    const float buoyancy = std::max(0.0f, volumeCtrl->GetBuoyancy());
    const float linearDrag = std::max(0.0f, volumeCtrl->GetBuoyancyLinearDrag());
    const float angularDrag = std::max(0.0f, volumeCtrl->GetBuoyancyAngularDrag());
    if (buoyancy <= 0.0f && linearDrag <= 0.0f && angularDrag <= 0.0f) {
      continue;
    }

    const JPH::BodyID volumeID = volumeCtrl->GetBodyID();
    if (volumeID.IsInvalid()) {
      continue;
    }

    JPH::RefConst<JPH::Shape> volumeShape;
    JPH::RMat44 volumeTransform;
    JPH::ObjectLayer volumeLayer = JPH::cObjectLayerInvalid;
    JPH::RVec3 surfacePosition;
    JPH::Vec3 surfaceNormal;
    {
      JPH::BodyLockRead lock(m_physicsSystem->GetBodyLockInterface(), volumeID);
      if (!lock.Succeeded()) {
        continue;
      }

      const JPH::Body &volumeBody = lock.GetBody();
      if (!volumeBody.IsRigidBody() || !volumeBody.IsSensor()) {
        continue;
      }

      volumeShape = volumeBody.GetShape();
      if (!volumeShape) {
        continue;
      }

      const JPH::AABox localBounds = volumeShape->GetLocalBounds();
      if (!localBounds.IsValid()) {
        continue;
      }

      volumeTransform = volumeBody.GetCenterOfMassTransform();
      surfacePosition = volumeTransform * JPH::Vec3(0.0f, localBounds.mMax.GetY(), 0.0f);
      surfaceNormal = volumeTransform.GetAxisY();
      volumeLayer = volumeBody.GetObjectLayer();
    }

    if (surfaceNormal.LengthSq() <= 1.0e-12f ||
        volumeLayer == JPH::cObjectLayerInvalid) {
      continue;
    }
    surfaceNormal = surfaceNormal.Normalized();

    JoltBuoyancyObjectLayerFilter objectLayerFilter(volumeLayer, m_objectLayerPairFilter);
    JoltBuoyancyBodyFilter bodyFilter(volumeID);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    npq.CollideShape(volumeShape.GetPtr(),
                     JPH::Vec3::sReplicate(1.0f),
                     volumeTransform,
                     collideSettings,
                     JPH::RVec3::sZero(),
                     collector,
                     broadPhaseFilter,
                     objectLayerFilter,
                     bodyFilter,
                     {});

    if (collector.mHits.empty()) {
      continue;
    }

    m_buoyancyTargetBodyIDsScratch.clear();
    m_buoyancyTargetBodyKeysScratch.clear();
    m_buoyancyTargetBodyIDsScratch.reserve(collector.mHits.size());
    m_buoyancyTargetBodyKeysScratch.reserve(collector.mHits.size());

    for (const JPH::CollideShapeResult &hit : collector.mHits) {
      const JPH::BodyID targetID = hit.mBodyID2;
      if (targetID.IsInvalid()) {
        continue;
      }

      if (m_buoyancyTargetBodyKeysScratch.insert(targetID.GetIndexAndSequenceNumber()).second) {
        m_buoyancyTargetBodyIDsScratch.push_back(targetID);
      }
    }

    for (const JPH::BodyID targetID : m_buoyancyTargetBodyIDsScratch) {
      bi.ApplyBuoyancyImpulse(targetID,
                              surfacePosition,
                              surfaceNormal,
                              buoyancy,
                              linearDrag,
                              angularDrag,
                              volumeCtrl->GetBuoyancyFluidVelocity(),
                              gravity,
                              timeStep);
    }
  }
}

void JoltPhysicsEnvironment::ProcessFhSprings(float timeStep)
{
  /* FH (Floating Height) springs: cast a ray downward from each dynamic body
   * that has FH enabled, and update its velocity to maintain hover height.
   *
   * Jolt keeps the feature self-contained: the moving object owns the spring,
   * damping, distance, and normal-alignment settings. The hit object only
   * contributes surface normal, velocity, and friction. */
  if (timeStep <= 0.0f) {
    return;
  }

  const std::vector<JoltPhysicsController *> &controllers = GetFhControllersForIteration();
  if (controllers.empty()) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();
  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const float step = timeStep * KX_GetActiveEngine()->GetTicRate();

  for (JoltPhysicsController *ctrl : controllers) {
    if (!ctrl->IsDynamic() || ctrl->IsPhysicsSuspended()) {
      continue;
    }

    if (!ctrl->GetFhEnabled()) {
      continue;
    }
    const bool doRotFh = ctrl->GetFhRotEnabled();

    const float fhDist = ctrl->GetFhDistance();
    const float fhSpring = ctrl->GetFhSpring();
    const float fhDamping = ctrl->GetFhDamping();
    if (fhDist <= 0.0f || fhSpring <= 0.0f) {
      continue;
    }

    JPH::BodyID bodyID = ctrl->GetBodyID();
    if (bodyID.IsInvalid()) {
      continue;
    }

    JPH::RVec3 bodyPos = bi.GetCenterOfMassPosition(bodyID);
    JPH::Vec3 bodyLinVel = bi.GetLinearVelocity(bodyID);
    float bodyInvMass = 0.0f;
    {
      JPH::BodyLockRead bodyLock(m_physicsSystem->GetBodyLockInterface(), bodyID);
      if (!bodyLock.Succeeded()) {
        continue;
      }

      const JPH::Body &body = bodyLock.GetBody();
      if (!body.GetMotionProperties()) {
        continue;
      }
      bodyInvMass = body.GetMotionProperties()->GetInverseMass();
      if (bodyInvMass <= 0.0f) {
        continue;
      }
    }

    const float radius = std::max(0.0f, ctrl->GetRadius());
    const float rayLength = std::max(1.0e-4f, fhDist + radius);
    const JPH::Vec3 rayDir(0.0f, -1.0f, 0.0f); /* Blender -Z. */
    JPH::RRayCast ray(bodyPos, rayDir * rayLength);
    JPH::RayCastSettings raySettings;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    JoltFhBodyFilter bodyFilter(bodyID);

    npq.CastRay(ray, raySettings, collector, {}, {}, bodyFilter);
    if (!collector.HadHit()) {
      continue;
    }

    const JPH::RayCastResult &hit = collector.mHit;
    JPH::Vec3 hitNormal(0.0f, 1.0f, 0.0f);
    JPH::Vec3 hitPointVelocity = JPH::Vec3::sZero();
    float hitFriction = 0.0f;
    {
      JPH::BodyLockRead hitLock(m_physicsSystem->GetBodyLockInterface(), hit.mBodyID);
      if (!hitLock.Succeeded()) {
        continue;
      }

      const JPH::Body &hitBody = hitLock.GetBody();
      if (!JoltGetPhysicsController(JoltGetClientInfo(hitBody))) {
        continue;
      }

      const JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
      hitNormal = hitBody.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);
      if (hitNormal.LengthSq() > 1.0e-12f) {
        hitNormal = hitNormal.Normalized();
      }
      else {
        hitNormal = -rayDir;
      }
      hitPointVelocity = hitBody.GetPointVelocity(hitPoint);
      hitFriction = hitBody.GetFriction();
    }

    const float distance = hit.mFraction * rayLength - radius;
    if (distance >= fhDist) {
      continue;
    }

    const float springExtent = 1.0f - distance / fhDist;
    const float iSpring = springExtent * fhSpring;
    const JPH::Vec3 relativeVelocity = bodyLinVel - hitPointVelocity;
    const float relVelRay = rayDir.Dot(relativeVelocity);

    const JPH::Vec3 forceDir = ctrl->GetFhNormal() ? hitNormal : -rayDir;

    const float iDamp = relVelRay * fhDamping;
    bodyLinVel += (iSpring + iDamp) * forceDir * step;

    const bool contactBetweenSurfaces = distance < ctrl->GetMargin();
    JPH::Vec3 lateral = relativeVelocity - relVelRay * (-forceDir);
    const float relVelLateral = lateral.Length();
    if (contactBetweenSurfaces && relVelLateral > 1.0e-6f) {
      const float maxFriction = hitFriction * std::max(0.0f, iSpring);
      const float relMomLateral = relVelLateral / bodyInvMass;
      const JPH::Vec3 friction = (relMomLateral > maxFriction) ?
                                     -lateral * (maxFriction / relVelLateral) :
                                     -lateral;
      bodyLinVel += friction * bodyInvMass * step;
    }

    bi.SetLinearVelocity(bodyID, bodyLinVel);

    if (doRotFh) {
      JPH::Vec3 bodyAngVel = bi.GetAngularVelocity(bodyID);
      JPH::Vec3 bodyUp = bi.GetRotation(bodyID) * JPH::Vec3(0.0f, 1.0f, 0.0f);
      JPH::Vec3 tiltSpring = bodyUp.Cross(hitNormal) * fhSpring;
      JPH::Vec3 tiltAngVel = bodyAngVel - bodyAngVel.Dot(hitNormal) * hitNormal;
      JPH::Vec3 tiltDamping = tiltAngVel * fhDamping;
      bodyAngVel += (tiltSpring - tiltDamping) * step;
      bi.SetAngularVelocity(bodyID, bodyAngVel);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Deferred Physics Operations
 * \{ */

bool JoltPhysicsEnvironment::QueueDeferredOperation(const JoltDeferredOp &op)
{
  if (!m_isPhysicsUpdating) {
    /* Physics not updating - caller should execute immediately. */
    return false;
  }

  m_deferredOps.push_back(op);
  return true;
}

void JoltPhysicsEnvironment::ProcessDeferredOperations()
{
  /* Swap to local copy to allow new operations to be queued during processing. */
  std::vector<JoltDeferredOp> ops;
  ops.swap(m_deferredOps);

  if (ops.empty()) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();

  auto removePendingRigidAdds = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (bodyIDs.empty() || m_pendingRigidBodyBodyAdds.empty()) {
      return;
    }

    std::unordered_set<JPH::uint32> bodyIDSet;
    bodyIDSet.reserve(bodyIDs.size());
    for (const JPH::BodyID bodyID : bodyIDs) {
      bodyIDSet.insert(bodyID.GetIndexAndSequenceNumber());
    }

    auto newEnd = std::remove_if(
        m_pendingRigidBodyBodyAdds.begin(),
        m_pendingRigidBodyBodyAdds.end(),
        [&](const PendingBodyAddEntry &entry) {
          return bodyIDSet.find(entry.bodyID.GetIndexAndSequenceNumber()) != bodyIDSet.end();
        });
    if (newEnd != m_pendingRigidBodyBodyAdds.end()) {
      m_pendingRigidBodyBodyAdds.erase(newEnd, m_pendingRigidBodyBodyAdds.end());
    }
  };

  auto removeBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (!bodyIDs.empty()) {
      for (const JPH::BodyID bodyID : bodyIDs) {
        RemoveLogicActiveContactsForBody(bodyID);
      }
      std::vector<JPH::BodyID> mutableBodyIDs = bodyIDs;
      bi.RemoveBodies(mutableBodyIDs.data(), (int)mutableBodyIDs.size());
    }
  };

  auto addBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs,
                            JPH::EActivation activation) {
    if (bodyIDs.empty()) {
      return;
    }

    std::vector<JPH::BodyID> addBodyIDs = bodyIDs;
    JPH::BodyID *rawBodyIDs = addBodyIDs.data();
    const int bodyCount = (int)addBodyIDs.size();
    JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(rawBodyIDs, bodyCount);
    if (addState) {
      bi.AddBodiesFinalize(rawBodyIDs, bodyCount, addState, activation);
    }
    else {
      for (int bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        bi.AddBody(rawBodyIDs[bodyIndex], activation);
      }

      /* Fallback used one-by-one insertion. Track this as runtime add pressure
       * for the regular threshold/idle broadphase optimize policy. */
      m_pendingRigidBodyAddsForOptimize += bodyCount;
      m_rigidBodyAddsSinceLastStep += bodyCount;
    }
  };

  auto destroyBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (bodyIDs.empty()) {
      return;
    }

    for (const JPH::BodyID bodyID : bodyIDs) {
      RemoveConstraintsForBody(bodyID);
      RemoveLogicActiveContactsForBody(bodyID);
    }

    std::vector<JPH::BodyID> addedBodyIDs;
    addedBodyIDs.reserve(bodyIDs.size());
    for (const JPH::BodyID bodyID : bodyIDs) {
      if (bi.IsAdded(bodyID)) {
        addedBodyIDs.push_back(bodyID);
      }
    }

    if (!addedBodyIDs.empty()) {
      bi.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
    }
    bi.DestroyBodies(bodyIDs.data(), (int)bodyIDs.size());
  };

  auto collectRunUniqueBodyIDs = [&](size_t &index, JoltDeferredOpType type) {
    std::vector<JPH::BodyID> bodyIDs;
    std::unordered_set<JPH::uint32> bodyIDSet;
    while (index < ops.size() && ops[index].type == type) {
      const JoltDeferredOp &runOp = ops[index];
      if (!runOp.bodyID.IsInvalid() &&
          bodyIDSet.insert(runOp.bodyID.GetIndexAndSequenceNumber()).second) {
        bodyIDs.push_back(runOp.bodyID);
      }
      ++index;
    }
    return bodyIDs;
  };

  size_t i = 0;
  while (i < ops.size()) {
    const JoltDeferredOp &op = ops[i];

    switch (op.type) {
      case JoltDeferredOpType::AddBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::AddBody);
        std::vector<JPH::BodyID> addBodyIDs;
        addBodyIDs.reserve(bodyIDs.size());
        for (const JPH::BodyID bodyID : bodyIDs) {
          if (!bi.IsAdded(bodyID)) {
            addBodyIDs.push_back(bodyID);
          }
        }
        addBodiesBatch(addBodyIDs, JPH::EActivation::Activate);
        break;
      }

      case JoltDeferredOpType::RemoveBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::RemoveBody);
        removePendingRigidAdds(bodyIDs);

        std::vector<JPH::BodyID> addedBodyIDs;
        addedBodyIDs.reserve(bodyIDs.size());
        for (const JPH::BodyID bodyID : bodyIDs) {
          if (bi.IsAdded(bodyID)) {
            addedBodyIDs.push_back(bodyID);
          }
        }
        removeBodiesBatch(addedBodyIDs);
        break;
      }

      case JoltDeferredOpType::DestroyBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::DestroyBody);
        removePendingRigidAdds(bodyIDs);
        destroyBodiesBatch(bodyIDs);
        break;
      }

      case JoltDeferredOpType::SuspendDynamics: {
        ++i;
        if (op.bodyID.IsInvalid()) {
          break;
        }

        RemoveLogicActiveContactsForBody(op.bodyID);
        if (op.dynamicsMode == PHY_DynamicsMode::NoCollision) {
          std::vector<JPH::BodyID> bodyIDs = {op.bodyID};
          removePendingRigidAdds(bodyIDs);
          if (bi.IsAdded(op.bodyID)) {
            bi.RemoveBody(op.bodyID);
          }
          break;
        }

        if (!bi.IsAdded(op.bodyID)) {
          std::vector<JPH::BodyID> addBodyIDs = {op.bodyID};
          addBodiesBatch(addBodyIDs, JPH::EActivation::DontActivate);
        }

        const bool sensor = op.dynamicsMode != PHY_DynamicsMode::StaticCollider;
        const JoltBroadPhaseLayer category = op.dynamicsMode == PHY_DynamicsMode::StaticCollider ?
                                                 JOLT_BP_STATIC :
                                                 JOLT_BP_SENSOR;

        bi.SetMotionType(op.bodyID, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
        NotifyConstraintBodyMotionTypeChanged(op.bodyID);
        {
          JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), op.bodyID);
          if (lock.Succeeded()) {
            lock.GetBody().SetIsSensor(sensor);
          }
        }
        bi.SetObjectLayer(op.bodyID,
                          JoltMakeObjectLayer(op.collisionGroup, op.collisionMask, category));
        break;
      }

      case JoltDeferredOpType::RestoreDynamics: {
        ++i;
        if (op.bodyID.IsInvalid()) {
          break;
        }

        RemoveLogicActiveContactsForBody(op.bodyID);

        /* If body is not in the world (removed on suspend), add it back. */
        if (!bi.IsAdded(op.bodyID)) {
          std::vector<JPH::BodyID> addBodyIDs = {op.bodyID};
          addBodiesBatch(addBodyIDs, JPH::EActivation::Activate);
        }

        /* Restore motion type. */
        bi.SetMotionType(op.bodyID, op.motionType, JPH::EActivation::Activate);
        NotifyConstraintBodyMotionTypeChanged(op.bodyID);

        /* Restore sensor flag. */
        {
          JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), op.bodyID);
          if (lock.Succeeded()) {
            lock.GetBody().SetIsSensor(op.ghost);
          }
        }

        /* Restore layer. */
        if (bi.IsAdded(op.bodyID)) {
          bi.SetObjectLayer(
              op.bodyID, JoltMakeObjectLayer(op.collisionGroup, op.collisionMask, op.bpCategory));
        }
        break;
      }

      case JoltDeferredOpType::SetObjectLayer: {
        ++i;
        if (!op.bodyID.IsInvalid() && bi.IsAdded(op.bodyID)) {
          RemoveLogicActiveContactsForBody(op.bodyID);
          bi.SetObjectLayer(op.bodyID, op.objectLayer);
        }
        break;
      }

      case JoltDeferredOpType::SetMotionType: {
        ++i;
        if (!op.bodyID.IsInvalid()) {
          RemoveLogicActiveContactsForBody(op.bodyID);
          bi.SetMotionType(op.bodyID, op.motionType, JPH::EActivation::Activate);
          NotifyConstraintBodyMotionTypeChanged(op.bodyID);
        }
        break;
      }

      case JoltDeferredOpType::SetSensorFlag: {
        ++i;
        if (op.bodyID.IsInvalid()) {
          break;
        }

        bool changed = false;
        {
          JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), op.bodyID);
          if (lock.Succeeded()) {
            JPH::Body &body = lock.GetBody();
            changed = body.IsSensor() != op.ghost;
            body.SetIsSensor(op.ghost);
          }
        }

        if (changed) {
          RemoveLogicActiveContactsForBody(op.bodyID);
        }
        if (op.ghost && bi.IsAdded(op.bodyID)) {
          bi.ActivateBody(op.bodyID);
        }
        break;
      }
    }
  }
}

/** \} */
