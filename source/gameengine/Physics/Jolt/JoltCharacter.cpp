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

/** \file JoltCharacter.cpp
 *  \ingroup physjolt
 */

#include "JoltCharacter.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "PHY_IMotionState.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

/** Simulate pushes nothing: ApplyStep works the pushes out afterwards, in character order, so
 * characters moving at once (or in any order) read the same world and leave the same one. */
class JoltCharacterNoImpulseListener final : public JPH::CharacterContactListener {
 public:
  void OnContactAdded(const JPH::CharacterVirtual * /*inCharacter*/,
                      const JPH::CharacterContact & /*inContact*/,
                      JPH::CharacterContactSettings &ioSettings) override
  {
    ioSettings.mCanReceiveImpulses = false;
  }
  void OnContactPersisted(const JPH::CharacterVirtual * /*inCharacter*/,
                          const JPH::CharacterContact & /*inContact*/,
                          JPH::CharacterContactSettings &ioSettings) override
  {
    ioSettings.mCanReceiveImpulses = false;
  }

  /** A contact too steep to stand on, judged by where it touches (its contact normal), lifts the
   * character no faster than it rose before it (or than the contact rises: a lift). Jolt judges
   * steepness by the touched face's normal: at a box edge (or a mesh step's edge) that may be the
   * flat top, and the round bottom of a capsule rode up the edge as up a ramp (Step Height 0.25
   * climbed 0.45 m boxes; 0 climbed 0.25 m ones but not 0.2 m ones, by which face the edge's
   * rounding picked). Such a contact acts as the wall it is; Step Height climbs. */
  void OnContactSolve(const JPH::CharacterVirtual *inCharacter,
                      const JPH::BodyID & /*inBodyID2*/,
                      const JPH::SubShapeID & /*inSubShapeID2*/,
                      JPH::RVec3Arg /*inContactPosition*/,
                      JPH::Vec3Arg inContactNormal,
                      JPH::Vec3Arg inContactVelocity,
                      const JPH::PhysicsMaterial * /*inContactMaterial*/,
                      JPH::Vec3Arg inCharacterVelocity,
                      JPH::Vec3 &ioNewCharacterVelocity) override
  {
    const JPH::Vec3 up = inCharacter->GetUp();
    const float normalUp = inContactNormal.Dot(up);
    /* (A wall or a ceiling: Jolt's own planes hold.) */
    if (normalUp <= 1.0e-3f || !inCharacter->IsSlopeTooSteep(inContactNormal)) {
      return;
    }
    const float limit = std::max(
        {inCharacterVelocity.Dot(up), inContactVelocity.Dot(up), 0.0f});
    if (ioNewCharacterVelocity.Dot(up) <= limit) {
      return;
    }
    /* As against a wall facing the same way: no horizontal motion into it, the vertical as it
     * was (Jolt's own vertical plane for a steep face). */
    const JPH::Vec3 wallNormal = (inContactNormal - normalUp * up).Normalized();
    JPH::Vec3 velocity = inCharacterVelocity - inContactVelocity;
    const float into = velocity.Dot(wallNormal);
    if (into < 0.0f) {
      velocity -= into * wallNormal;
    }
    velocity += inContactVelocity;
    const float rising = velocity.Dot(up);
    if (rising > limit) {
      velocity -= (rising - limit) * up;
    }
    ioNewCharacterVelocity = velocity;
  }
};

JoltCharacterNoImpulseListener s_noImpulseListener;

KX_GameObject *GameObjectOf(JoltPhysicsController *ctrl)
{
  KX_ClientObjectInfo *info = ctrl ? static_cast<KX_ClientObjectInfo *>(ctrl->GetNewClientInfo()) :
                                     nullptr;
  return info ? info->m_gameobject : nullptr;
}

JPH::Quat RotationOf(const MT_Matrix3x3 &orientation)
{
  JPH::Quat rotation = JoltMath::ToJolt(orientation.getRotation());
  const float length = rotation.Length();
  if (!std::isfinite(length) || length < 1.0e-6f) {
    return JPH::Quat::sIdentity();
  }
  return rotation.Normalized();
}

bool FiniteVec(const MT_Vector3 &v)
{
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Construction
 * \{ */

JoltCharacter::JoltCharacter(JoltPhysicsController *ctrl,
                             JoltPhysicsEnvironment *env,
                             const MT_Vector3 &position,
                             const MT_Matrix3x3 &orientation,
                             const float stepHeight)
    : m_ctrl(ctrl),
      m_env(env),
      m_wantCrouch(false),
      m_crouchHeight(0.5f),
      m_crouchedShapeHeight(1.0f),
      m_onWall(false),
      m_onCeiling(false),
      m_wallNormal(JPH::Vec3::sZero()),
      m_groundNormal(JPH::Vec3::sZero()),
      m_groundVelocity(JPH::Vec3::sZero()),
      m_walkDirection(0.0f, 0.0f, 0.0f),
      m_gravity(0.0f, 0.0f, -9.81f),
      m_jumpSpeed(10.0f),
      m_fallSpeed(55.0f),
      m_maxSlope(JPH::DegreesToRadians(50.0f)),
      m_stepHeight(std::max(stepHeight, 0.0f)),
      m_maxJumps(1),
      m_jumpCount(0),
      m_wantJump(false),
      m_overrideVelocity(JPH::Vec3::sZero()),
      m_overrideTime(0.0f),
      m_overridePending(false),
      m_knockback(JPH::Vec3::sZero()),
      m_pendingVelocityChange(JPH::Vec3::sZero()),
      m_pendingForce(JPH::Vec3::sZero()),
      m_simulated(false),
      m_requestedVelocity(JPH::Vec3::sZero()),
      m_stepStart(JPH::RVec3::sZero()),
      m_achievedVelocity(JPH::Vec3::sZero()),
      m_writtenPosition(position),
      m_hasWritten(false)
{
  if (env && env->GetPhysicsSystem()) {
    /* The scene's gravity, as Bullet's character starts with. */
    m_gravity = JoltMath::ToMT(env->GetPhysicsSystem()->GetGravity());
    CreateVirtual(JoltMath::ToJolt(position),
                  RotationOf(orientation),
                  JPH::Vec3::sZero(),
                  env->GetPhysicsSystem());
  }
}

JoltCharacter::~JoltCharacter()
{
  m_character = nullptr;
}

void JoltCharacter::CreateVirtual(const JPH::RVec3 &position,
                                  const JPH::Quat &rotation,
                                  const JPH::Vec3 &velocity,
                                  JPH::PhysicsSystem *system)
{
  m_character = nullptr;
  m_shape = m_ctrl ? m_ctrl->GetShape() : nullptr;
  if (!system || !m_shape) {
    return;
  }

  JPH::CharacterVirtualSettings settings;
  settings.mShape = m_shape;
  settings.mUp = UpOf(m_gravity);
  settings.mMaxSlopeAngle = m_maxSlope;
  /* No weight and no pushes inside Jolt's update (ApplyStep does both, in order). */
  settings.mMass = 0.0f;
  settings.mMaxStrength = kMaxStrength;
  settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
  settings.mCharacterPadding = 0.02f;
  settings.mPenetrationRecoverySpeed = 1.0f;
  settings.mPredictiveContactDistance = 0.1f;
  /* Smooth walking over a triangle mesh's inner edges (no bumps at every triangle). */
  settings.mEnhancedInternalEdgeRemoval = true;

  m_character = new JPH::CharacterVirtual(&settings, position, rotation, 0, system);
  m_character->SetLinearVelocity(velocity);
  m_character->SetListener(&s_noImpulseListener);
}

JPH::ObjectLayer JoltCharacter::GetObjectLayer() const
{
  return JoltMakeObjectLayer(m_ctrl->GetCollisionGroup(), m_ctrl->GetCollisionMask(), JOLT_BP_DYNAMIC);
}

JPH::Vec3 JoltCharacter::UpOf(const MT_Vector3 &gravity) const
{
  /* Up is against gravity (a character walks on what its gravity pulls it to), as Bullet's. */
  const JPH::Vec3 g = JoltMath::ToJolt(gravity);
  const float length = g.Length();
  if (std::isfinite(length) && length > 1.0e-6f) {
    return -g / length;
  }
  return m_character ? m_character->GetUp() : JPH::Vec3::sAxisY();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter
 * \{ */

void JoltCharacter::Jump()
{
  /* Whether it may jump is decided at the step (its ground state then). */
  m_wantJump = true;
}

bool JoltCharacter::OnGround()
{
  return m_character &&
         m_character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

MT_Vector3 JoltCharacter::GetGravity()
{
  return m_gravity;
}

void JoltCharacter::SetGravity(const MT_Vector3 &gravity)
{
  if (!FiniteVec(gravity)) {
    return;
  }
  m_gravity = gravity;
  if (m_character) {
    m_character->SetUp(UpOf(gravity));
  }
}

unsigned char JoltCharacter::GetMaxJumps()
{
  return m_maxJumps;
}

void JoltCharacter::SetMaxJumps(unsigned char maxJumps)
{
  m_maxJumps = maxJumps;
}

unsigned char JoltCharacter::GetJumpCount()
{
  return m_jumpCount;
}

void JoltCharacter::SetWalkDirection(const MT_Vector3 &dir)
{
  if (!FiniteVec(dir)) {
    return;
  }
  /* A Set Character Velocity still running keeps its time (a tree sets the walking velocity
   * every step: a knock-back would end at once); walking resumes after it. */
  m_walkDirection = dir;
}

MT_Vector3 JoltCharacter::GetWalkDirection()
{
  return m_walkDirection;
}

float JoltCharacter::GetFallSpeed() const
{
  return m_fallSpeed;
}

void JoltCharacter::SetFallSpeed(float fallSpeed)
{
  if (std::isfinite(fallSpeed)) {
    m_fallSpeed = std::max(fallSpeed, 0.0f);
  }
}

float JoltCharacter::GetMaxSlope() const
{
  return m_maxSlope;
}

void JoltCharacter::SetMaxSlope(float maxSlope)
{
  if (!std::isfinite(maxSlope)) {
    return;
  }
  m_maxSlope = maxSlope;
  if (m_character) {
    m_character->SetMaxSlopeAngle(maxSlope);
  }
}

float JoltCharacter::GetJumpSpeed() const
{
  return m_jumpSpeed;
}

void JoltCharacter::SetJumpSpeed(float jumpSpeed)
{
  if (std::isfinite(jumpSpeed)) {
    m_jumpSpeed = jumpSpeed;
  }
}

void JoltCharacter::SetVelocity(const MT_Vector3 &vel, float time, bool local)
{
  if (!m_character || !FiniteVec(vel) || !std::isfinite(time)) {
    return;
  }
  JPH::Vec3 velocity = JoltMath::ToJolt(vel);
  if (local) {
    velocity = m_character->GetRotation() * velocity;
  }
  const JPH::Vec3 up = m_character->GetUp();
  const JPH::Vec3 vertical = up * velocity.Dot(up);
  /* The vertical part now: the next step keeps it (in the air) and gravity acts on it. */
  JPH::Vec3 current = m_character->GetLinearVelocity();
  current += vertical - up * current.Dot(up);
  m_character->SetLinearVelocity(current);
  m_overrideVelocity = velocity - vertical;
  m_overrideTime = std::max(time, 0.0f);
  m_overridePending = true;
  /* A set velocity is the velocity: no knock-back on top, and the impulses before it (velocity
   * changes, as on a body) are replaced; a force still acts in the step. */
  m_knockback = JPH::Vec3::sZero();
  m_pendingVelocityChange = JPH::Vec3::sZero();
}

void JoltCharacter::Reset()
{
  m_jumpCount = 0;
  m_wantJump = false;
  m_walkDirection = MT_Vector3(0.0f, 0.0f, 0.0f);
  m_overrideVelocity = JPH::Vec3::sZero();
  m_overrideTime = 0.0f;
  m_overridePending = false;
  m_knockback = JPH::Vec3::sZero();
  m_pendingVelocityChange = JPH::Vec3::sZero();
  m_pendingForce = JPH::Vec3::sZero();
  m_achievedVelocity = JPH::Vec3::sZero();
  if (m_character) {
    m_character->SetLinearVelocity(JPH::Vec3::sZero());
  }
}

float JoltCharacter::GetStepHeight() const
{
  return m_stepHeight;
}

void JoltCharacter::SetStepHeight(const float stepHeight)
{
  if (std::isfinite(stepHeight)) {
    m_stepHeight = std::max(stepHeight, 0.0f);
  }
}

void JoltCharacter::SetCrouch(const bool crouch, const float height)
{
  m_wantCrouch = crouch;
  if (std::isfinite(height)) {
    m_crouchHeight = std::clamp(height, kMinCrouchHeight, 1.0f);
  }
}

bool JoltCharacter::IsCrouched() const
{
  /* Its shape is the shorter one; or it wants to crouch at full height (no shorter shape). */
  if (!m_character || !m_shape) {
    return false;
  }
  return m_character->GetShape() != m_shape.GetPtr() ||
         (m_wantCrouch && m_crouchedShape == m_shape);
}

bool JoltCharacter::OnWall() const
{
  return m_onWall;
}

bool JoltCharacter::OnCeiling() const
{
  return m_onCeiling;
}

MT_Vector3 JoltCharacter::GetWallNormal() const
{
  return JoltMath::ToMT(m_wallNormal);
}

MT_Vector3 JoltCharacter::GetGroundNormal() const
{
  return JoltMath::ToMT(m_groundNormal);
}

MT_Vector3 JoltCharacter::GetGroundVelocity() const
{
  return JoltMath::ToMT(m_groundVelocity);
}

KX_GameObject *JoltCharacter::GetGroundObject() const
{
  if (m_groundBody.IsInvalid() || !m_env || !m_env->GetPhysicsSystem()) {
    return nullptr;
  }
  /* Asked between steps (Logic Nodes' snapshots, on any thread while trees record: nothing adds
   * or removes bodies then), so without a lock: an index and a sequence check. A body removed
   * since (its object removed) is no longer found; a new body never takes its ID (the sequence
   * number differs). */
  JPH::BodyLockRead lock(m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock(), m_groundBody);
  if (!lock.Succeeded()) {
    return nullptr;
  }
  KX_ClientObjectInfo *info = reinterpret_cast<KX_ClientObjectInfo *>(
      lock.GetBody().GetUserData());
  return info ? info->m_gameobject : nullptr;
}

JPH::RefConst<JPH::Shape> JoltCharacter::CrouchedShapeOf(const JPH::Shape &standing,
                                                        const float height)
{
  const float part = std::isfinite(height) ? std::clamp(height, kMinCrouchHeight, 1.0f) : 1.0f;
  /* (Jolt's local bounds are around the center of mass: the shape's own space adds it.) */
  const JPH::AABox bounds = standing.GetLocalBounds();
  const float bottom = bounds.mMin.GetY() + standing.GetCenterOfMass().GetY();
  const float tall = bounds.mMax.GetY() - bounds.mMin.GetY();
  if (part >= 1.0f || !(tall > 1.0e-6f) || !std::isfinite(tall)) {
    return &standing;
  }
  const float target = tall * part;

  /* (A scaled capsule or sphere: the scale is uniform, Jolt's rule for them.) */
  const JPH::Shape *inner = &standing;
  JPH::Vec3 scale = JPH::Vec3::sOne();
  if (standing.GetSubType() == JPH::EShapeSubType::Scaled) {
    const JPH::ScaledShape &scaled = static_cast<const JPH::ScaledShape &>(standing);
    inner = scaled.GetInnerShape();
    scale = scaled.GetScale();
  }
  const float uniform = std::abs(scale.GetX());

  JPH::Shape::ShapeResult result;
  float centerY = 0.0f;
  switch (inner->GetSubType()) {
    case JPH::EShapeSubType::Capsule: {
      /* The same radius (a character keeps its width), a shorter middle; never less than a
       * sphere's height. */
      const JPH::CapsuleShape &capsule = static_cast<const JPH::CapsuleShape &>(*inner);
      const float radius = capsule.GetRadius() * uniform;
      const float halfHeight = std::max(0.5f * target - radius, 0.001f);
      result = JPH::CapsuleShapeSettings(halfHeight, radius).Create();
      centerY = bottom + halfHeight + radius;
      break;
    }
    case JPH::EShapeSubType::Sphere: {
      const float radius = std::max(0.5f * target, 0.001f);
      result = JPH::SphereShapeSettings(radius).Create();
      centerY = bottom + radius;
      break;
    }
    default:
      /* Scaled along up about the origin: its bottom moves to bottom x part, put back. */
      result = inner->ScaleShape(scale * JPH::Vec3(1.0f, part, 1.0f));
      centerY = bottom - bottom * part;
      break;
  }
  if (result.HasError() || !result.Get()) {
    return &standing;
  }
  if (std::abs(centerY) < 1.0e-7f) {
    return result.Get();
  }
  return new JPH::RotatedTranslatedShape(
      JPH::Vec3(0.0f, centerY, 0.0f), JPH::Quat::sIdentity(), result.Get());
}

void JoltCharacter::AddImpulse(const MT_Vector3 &impulse, const bool velocityChange)
{
  if (!m_character || !FiniteVec(impulse)) {
    return;
  }
  const JPH::Vec3 change = JoltMath::ToJolt(impulse) * (velocityChange ? 1.0f : 1.0f / kMass);
  const JPH::Vec3 total = m_pendingVelocityChange + change;
  if (FiniteVec(JoltMath::ToMT(total))) {
    m_pendingVelocityChange = total;
  }
}

void JoltCharacter::AddForce(const MT_Vector3 &force)
{
  if (!m_character || !FiniteVec(force)) {
    return;
  }
  const JPH::Vec3 total = m_pendingForce + JoltMath::ToJolt(force);
  if (FiniteVec(JoltMath::ToMT(total))) {
    m_pendingForce = total;
  }
}

MT_Vector3 JoltCharacter::GetKnockback() const
{
  return JoltMath::ToMT(m_knockback);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The step
 * \{ */

bool JoltCharacter::PrepareStep(const float timeStep)
{
  m_simulated = false;
  if (!m_character || !m_ctrl || !m_env || !(timeStep > 0.0f)) {
    return false;
  }
  PHY_IMotionState *motionState = m_ctrl->GetMotionState();
  if (!motionState) {
    return false;
  }

  const MT_Vector3 position = motionState->GetWorldPosition();
  const MT_Matrix3x3 orientation = motionState->GetWorldOrientation();
  if (!FiniteVec(position)) {
    return false;
  }
  m_character->SetRotation(RotationOf(orientation));

  KX_GameObject *gameobj = GameObjectOf(m_ctrl);
  const JPH::BodyID bodyID = m_ctrl->GetBodyID();
  const bool bodyInWorld = !bodyID.IsInvalid() &&
                           m_env->GetBodyInterfaceNoLock().IsAdded(bodyID) &&
                           !m_ctrl->IsPhysicsSuspended() && !m_ctrl->IsDynamicsSuspended();
  const bool parented = gameobj && gameobj->GetParent() != nullptr;
  if (!bodyInWorld || parented) {
    /* It does not move itself: it is where its object is (its body follows as any Kinematic
     * body's does, in the step's kinematic write). */
    m_ctrl->SetCharacterDrivesBody(false);
    m_character->SetPosition(JoltMath::ToJolt(position));
    m_character->SetLinearVelocity(JPH::Vec3::sZero());
    m_achievedVelocity = JPH::Vec3::sZero();
    /* (Pushes meet nothing that moves: none waits for a later step.) */
    m_knockback = JPH::Vec3::sZero();
    m_pendingVelocityChange = JPH::Vec3::sZero();
    m_pendingForce = JPH::Vec3::sZero();
    m_hasWritten = false;
    /* It touches nothing it knows of; its stance still follows (a character seated in a car
     * crouches). */
    ClearSurfaces();
    if (bodyInWorld) {
      /* (A new scale reaches the body in the step's kinematic write, after this: the crouched
       * shape follows at the next step.) */
      const JPH::RefConst<JPH::Shape> standing = m_ctrl->GetShape();
      const bool standingChanged = standing && standing != m_shape;
      if (standingChanged) {
        m_shape = standing;
        m_crouchedShape = nullptr;
      }
      UpdateStance(standingChanged);
    }
    return false;
  }

  /* Its object moved by something else than the character (logic, a restored game): a
   * teleport. */
  if (!m_hasWritten || !(position == m_writtenPosition)) {
    m_character->SetPosition(JoltMath::ToJolt(position));
  }

  /* The object's scale changed (logic, a parent before): the body's shape follows as a
   * Kinematic body's does, the character's with it (crouched: the crouched shape of the new
   * standing one). */
  m_ctrl->SyncKinematicScaling();
  const JPH::RefConst<JPH::Shape> shape = m_ctrl->GetShape();
  bool standingChanged = false;
  if (shape && shape != m_shape) {
    m_shape = shape;
    m_crouchedShape = nullptr;
    standingChanged = true;
  }
  UpdateStance(standingChanged);

  /* The velocity of this step, as Jolt's CharacterVirtual samples work it out: on the ground
   * (and not moving away from it) the ground's velocity (a moving platform carries it), in the
   * air its vertical speed; then a jump, gravity, the walking velocity. */
  const JPH::Vec3 up = m_character->GetUp();
  const JPH::Vec3 gravity = JoltMath::ToJolt(m_gravity);
  const JPH::Vec3 current = m_character->GetLinearVelocity();
  const JPH::Vec3 ground = m_character->GetGroundVelocity();
  const bool onGround = m_character->GetGroundState() ==
                        JPH::CharacterVirtual::EGroundState::OnGround;
  const bool grounded = onGround && (current - ground).Dot(up) < 0.1f;
  if (grounded) {
    m_jumpCount = 0;
  }

  JPH::Vec3 velocity = grounded ? ground : up * current.Dot(up);
  if (m_wantJump) {
    m_wantJump = false;
    if (m_jumpCount < m_maxJumps) {
      const float base = grounded ? ground.Dot(up) : 0.0f;
      velocity += up * (base + m_jumpSpeed - velocity.Dot(up));
      m_jumpCount++;
    }
  }
  /* Pushes since the last step (2026-11-17): along up as a jump's (off the ground, then gravity
   * acts), sideways the knock-back (kept, fading, in ApplyStep). */
  const JPH::Vec3 pushed = m_pendingVelocityChange + m_pendingForce * (timeStep / kMass);
  m_pendingVelocityChange = JPH::Vec3::sZero();
  m_pendingForce = JPH::Vec3::sZero();
  if (FiniteVec(JoltMath::ToMT(pushed))) {
    const float pushedUp = pushed.Dot(up);
    velocity += up * pushedUp;
    m_knockback += pushed - up * pushedUp;
  }
  /* (Sideways to the character's up now: gravity may have turned.) */
  m_knockback -= up * m_knockback.Dot(up);
  velocity += gravity * timeStep;

  if (m_overridePending || m_overrideTime > 0.0f) {
    velocity += m_overrideVelocity;
    m_overrideTime -= timeStep;
    m_overridePending = false;
    if (m_overrideTime <= 0.0f) {
      m_overrideTime = 0.0f;
    }
  }
  else {
    JPH::Vec3 walk = JoltMath::ToJolt(m_walkDirection);
    walk -= up * walk.Dot(up);
    velocity += walk;
  }
  velocity += m_knockback;

  /* Fall Speed: the fastest it falls. */
  const float vertical = velocity.Dot(up);
  if (vertical < -m_fallSpeed) {
    velocity += up * (-m_fallSpeed - vertical);
  }

  m_character->SetLinearVelocity(velocity);
  m_requestedVelocity = velocity;
  m_stepStart = m_character->GetPosition();
  m_ctrl->SetCharacterDrivesBody(true);
  m_simulated = true;
  return true;
}

void JoltCharacter::Simulate(const float timeStep, JPH::TempAllocator &allocator)
{
  if (!m_simulated || !m_character) {
    return;
  }
  JPH::CharacterVirtual &character = *m_character;
  JPH::PhysicsSystem *system = m_env->GetPhysicsSystem();
  const JPH::ObjectLayer layer = GetObjectLayer();
  const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter = system->GetDefaultBroadPhaseLayerFilter(
      layer);
  const JPH::DefaultObjectLayerFilter objectLayerFilter = system->GetDefaultLayerFilter(layer);
  /* Its own body is the character in the world for everything else: never in its way. */
  const JPH::IgnoreSingleBodyFilter bodyFilter(m_ctrl->GetBodyID());
  const JPH::ShapeFilter shapeFilter;
  const JPH::Vec3 up = character.GetUp();
  const JPH::Vec3 gravity = JoltMath::ToJolt(m_gravity);
  /* Stairs up to Step Height, and down as far when walking off a step (it sticks to the floor
   * instead of hopping down). */
  const JPH::Vec3 stepUp = up * m_stepHeight;
  const JPH::Vec3 stepDown = -up * m_stepHeight;
  /* ExtendedUpdateSettings' defaults. */
  constexpr float walkStairsMinStepForward = 0.02f;
  constexpr float walkStairsStepForwardTest = 0.15f;
  const float walkStairsCosAngleForwardContact = std::cos(JPH::DegreesToRadians(75.0f));

  /* CharacterVirtual::ExtendedUpdate of Jolt 5.7.0, line for line, with one change before the
   * stairs (MarkSteepEdges below). A Jolt update: compare with its ExtendedUpdate. */
  character.StartTrackingContactChanges();

  const JPH::Vec3 desiredVelocity = character.GetLinearVelocity();
  character.SetLinearVelocity(character.CancelVelocityTowardsSteepSlopes(desiredVelocity));
  const JPH::RVec3 oldPosition = character.GetPosition();
  bool groundToAir = character.IsSupported();

  character.Update(
      timeStep, gravity, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, allocator);

  if (character.IsSupported()) {
    groundToAir = false;
  }
  if (groundToAir && !stepDown.IsNearZero()) {
    const float velocity = JPH::Vec3(character.GetPosition() - oldPosition).Dot(up) / timeStep;
    if (velocity <= 1.0e-6f) {
      character.StickToFloor(
          stepDown, broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, allocator);
    }
  }

  if (!stepUp.IsNearZero()) {
    JPH::Vec3 desiredStep = desiredVelocity * timeStep;
    desiredStep -= desiredStep.Dot(up) * up;
    const float desiredStepLength = desiredStep.Length();
    if (desiredStepLength > 0.0f) {
      JPH::Vec3 achievedStep = JPH::Vec3(character.GetPosition() - oldPosition);
      achievedStep -= achievedStep.Dot(up) * up;
      const JPH::Vec3 forward = desiredStep / desiredStepLength;
      achievedStep = std::max(0.0f, achievedStep.Dot(forward)) * forward;
      const float achievedStepLength = achievedStep.Length();

      /* The change: an edge the character pushes against that is too steep to stand on where it
       * touches (its contact normal) counts as steep for the stairs too. Jolt judges by the
       * touched face's normal, which at a box (or mesh step) edge may be the flat top: the
       * stair walk then never started and a 0.25 m box stopped a Step Height 0.4 character
       * (the OnContactSolve rule above keeps it from riding up such an edge instead). */
      for (const JPH::CharacterContact &contact : character.GetActiveContacts()) {
        const float normalUp = contact.mContactNormal.Dot(up);
        if (normalUp > 1.0e-3f && character.IsSlopeTooSteep(contact.mContactNormal) &&
            !character.IsSlopeTooSteep(contact.mSurfaceNormal))
        {
          const_cast<JPH::CharacterContact &>(contact).mSurfaceNormal = contact.mContactNormal;
        }
      }

      if (achievedStepLength + 1.0e-4f < desiredStepLength &&
          character.CanWalkStairs(desiredVelocity))
      {
        JPH::Vec3 walkStairsDirection = forward;
        float maxDot = walkStairsCosAngleForwardContact;
        for (const JPH::CharacterContact &contact : character.GetActiveContacts()) {
          if (contact.mHadCollision && !contact.mWasDiscarded &&
              contact.mSurfaceNormal.Dot(desiredVelocity - contact.mLinearVelocity) < 0.0f &&
              character.IsSlopeTooSteep(contact.mSurfaceNormal))
          {
            JPH::Vec3 test = contact.mSurfaceNormal.Dot(up) * up - contact.mSurfaceNormal;
            const float testLength = test.Length();
            if (testLength > 1.0e-6f) {
              test /= testLength;
              const float dot = test.Dot(forward);
              if (dot > maxDot) {
                walkStairsDirection = test;
                maxDot = dot;
              }
            }
          }
        }
        const JPH::Vec3 stepForward = walkStairsDirection *
                                      std::max(walkStairsMinStepForward,
                                               desiredStepLength - achievedStepLength);
        const JPH::Vec3 stepForwardTest = walkStairsDirection * walkStairsStepForwardTest;
        const JPH::RVec3 beforeStairs = character.GetPosition();
        const float groundBefore = JPH::Vec3(character.GetGroundPosition()).Dot(up);
        if (character.WalkStairs(timeStep,
                                 stepUp,
                                 stepForward,
                                 stepForwardTest,
                                 JPH::Vec3::sZero(),
                                 broadPhaseFilter,
                                 objectLayerFilter,
                                 bodyFilter,
                                 shapeFilter,
                                 allocator) &&
            !StairLandingHolds(groundBefore))
        {
          character.SetPosition(beforeStairs);
          character.RefreshContacts(
              broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, allocator);
        }
      }
    }
  }

  character.FinishTrackingContactChanges();
}

void JoltCharacter::UpdateStance(const bool standingChanged)
{
  if (!m_character || !m_shape || !m_env || !m_env->GetPhysicsSystem()) {
    return;
  }
  JPH::RefConst<JPH::Shape> wanted = m_shape;
  if (m_wantCrouch) {
    if (!m_crouchedShape || m_crouchedShapeHeight != m_crouchHeight) {
      m_crouchedShape = CrouchedShapeOf(*m_shape, m_crouchHeight);
      m_crouchedShapeHeight = m_crouchHeight;
    }
    wanted = m_crouchedShape;
  }
  const JPH::Shape *current = m_character->GetShape();
  if (current != nullptr && wanted.GetPtr() != current) {
    /* Growing (standing up, a taller crouch) needs room; shrinking always fits, and a new scale
     * is taken as the object has it. */
    const auto top = [](const JPH::Shape &shape) {
      return shape.GetLocalBounds().mMax.GetY() + shape.GetCenterOfMass().GetY();
    };
    const bool grows = top(*wanted) > top(*current) + 1.0e-5f;
    SwitchShape(wanted, grows && !standingChanged);
  }
  SyncBodyShape();
}

void JoltCharacter::SyncBodyShape()
{
  const JPH::BodyID bodyID = m_ctrl->GetBodyID();
  const JPH::Shape *shape = m_character ? m_character->GetShape() : nullptr;
  if (bodyID.IsInvalid() || shape == nullptr) {
    return;
  }
  const JPH::RefConst<JPH::Shape> standing = m_ctrl->GetShape();
  JPH::RefConst<JPH::Shape> wanted;
  if (shape == standing.GetPtr()) {
    if (!m_bodyShape) {
      return; /* (Its controller's: what the body has.) */
    }
    wanted = standing;
  }
  else {
    if (m_bodyShape.GetPtr() == shape) {
      return;
    }
    wanted = shape;
  }
  /* Its body is the character for everything else (rays, other bodies): the same shape. The
   * controller keeps the standing one (its scale code works on that). */
  m_env->RemoveLogicActiveContactsForBody(bodyID);
  m_env->GetBodyInterface().SetShape(bodyID, wanted, false, JPH::EActivation::DontActivate);
  m_bodyShape = shape == standing.GetPtr() ? JPH::RefConst<JPH::Shape>() : wanted;
}

bool JoltCharacter::SwitchShape(const JPH::RefConst<JPH::Shape> &shape, const bool check)
{
  JPH::PhysicsSystem *system = m_env->GetPhysicsSystem();
  const JPH::BodyID bodyID = m_ctrl->GetBodyID();
  const JPH::ObjectLayer layer = GetObjectLayer();
  const JPH::IgnoreSingleBodyFilter bodyFilter(bodyID);
  /* Jolt's crouch sample: a shape that would sink deeper than 1.5 x the penetration slop into
   * anything (a ceiling, a low tunnel) is refused. */
  const float maxPenetration = check ? 1.5f * system->GetPhysicsSettings().mPenetrationSlop :
                                       FLT_MAX;
  if (!m_character->SetShape(shape,
                             maxPenetration,
                             system->GetDefaultBroadPhaseLayerFilter(layer),
                             system->GetDefaultLayerFilter(layer),
                             bodyFilter,
                             JPH::ShapeFilter(),
                             *m_env->GetTempAllocator()))
  {
    return false;
  }
  return true;
}

void JoltCharacter::ClearSurfaces()
{
  m_onWall = false;
  m_onCeiling = false;
  m_wallNormal = JPH::Vec3::sZero();
  m_groundNormal = JPH::Vec3::sZero();
  m_groundVelocity = JPH::Vec3::sZero();
  m_groundBody = JPH::BodyID();
}

void JoltCharacter::UpdateSurfaces()
{
  ClearSurfaces();
  if (!m_simulated || !m_character) {
    return;
  }
  if (m_character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
    m_groundNormal = m_character->GetGroundNormal();
    m_groundVelocity = m_character->GetGroundVelocity();
    m_groundBody = m_character->GetGroundBodyID();
  }
  const JPH::Vec3 up = m_character->GetUp();
  const float touching = m_character->GetCharacterPadding() + kTouchTolerance;
  JPH::Vec3 walls = JPH::Vec3::sZero();
  for (const JPH::CharacterContact &contact : m_character->GetActiveContacts()) {
    if (contact.mIsSensorB || contact.mWasDiscarded ||
        (!contact.mHadCollision && contact.mDistance > touching))
    {
      continue;
    }
    const JPH::Vec3 normal = contact.mContactNormal;
    const float normalUp = normal.Dot(up);
    if (normalUp > kWallFlatness && !m_character->IsSlopeTooSteep(normal)) {
      continue; /* (Ground, or a slope it walks.) */
    }
    if (normalUp < -kWallFlatness && !m_character->IsSlopeTooSteep(-normal)) {
      m_onCeiling = true;
      continue;
    }
    m_onWall = true;
    walls += normal;
  }
  if (m_onWall) {
    const float length = walls.Length();
    /* (Two walls facing each other: no way out, no normal.) */
    m_wallNormal = length > 1.0e-6f && std::isfinite(length) ? walls / length :
                                                                JPH::Vec3::sZero();
  }
}

bool JoltCharacter::StairLandingHolds(const float groundBefore) const
{
  /* Jolt lands a stair step on whatever its face normal calls flat: at a box (or mesh step) edge
   * that is the top face, while the round bottom rests on the edge with its side. That is the
   * way up a stair (the character walks on over the edge), and the way a Step Height 0.1
   * character hung 0.1 m up against a 0.35 m box, stair-stepping onto its edge every step.
   * A landing holds when it stands (its contact walkable where it touches) or the edge is no
   * higher above the ground it stepped from than Step Height (a stair the next steps walk on
   * over). */
  const JPH::BodyID groundBody = m_character->GetGroundBodyID();
  const JPH::RVec3 groundPosition = m_character->GetGroundPosition();
  const JPH::CharacterContact *landing = nullptr;
  float nearest = FLT_MAX;
  for (const JPH::CharacterContact &contact : m_character->GetActiveContacts()) {
    if (contact.mBodyB != groundBody) {
      continue;
    }
    const float distance = JPH::Vec3(contact.mPosition - groundPosition).LengthSq();
    if (distance < nearest) {
      nearest = distance;
      landing = &contact;
    }
  }
  if (landing == nullptr || !m_character->IsSlopeTooSteep(landing->mContactNormal)) {
    return true;
  }
  const float rise = JPH::Vec3(groundPosition).Dot(m_character->GetUp()) - groundBefore;
  return rise <= m_stepHeight + kStairTolerance;
}

void JoltCharacter::ApplyStep(const float timeStep)
{
  if (!m_simulated || !m_character) {
    return;
  }
  const JPH::RVec3 position = m_character->GetPosition();
  const JPH::Vec3 up = m_character->GetUp();
  m_achievedVelocity = JPH::Vec3(position - m_stepStart) / timeStep;

  /* Something above (a ceiling) stopped its rise: it rises no faster than it did (Jolt keeps the
   * asked velocity; Bullet's character stops rising there too). */
  const float requestedUp = m_requestedVelocity.Dot(up);
  const float achievedUp = m_achievedVelocity.Dot(up);
  if (requestedUp > 0.0f && achievedUp < requestedUp) {
    JPH::Vec3 velocity = m_character->GetLinearVelocity();
    velocity += up * (std::max(achievedUp, 0.0f) - velocity.Dot(up));
    m_character->SetLinearVelocity(velocity);
  }

  /* Knock-back: what it ran into stops the part going into it (a wall, a box it pushes), then
   * it fades (fast on the ground, slowly in the air). */
  if (!m_knockback.IsNearZero(1.0e-12f)) {
    for (const JPH::CharacterContact &contact : m_character->GetActiveContacts()) {
      if (!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB) {
        continue;
      }
      const float into = m_knockback.Dot(contact.mContactNormal);
      if (into < 0.0f) {
        m_knockback -= into * contact.mContactNormal;
      }
    }
    m_knockback -= up * m_knockback.Dot(up);
    float speed = m_knockback.Length();
    if (m_character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
      speed = speed * std::max(0.0f, 1.0f - kKnockbackGroundFriction * timeStep) -
              kKnockbackGroundBraking * timeStep;
    }
    else {
      speed *= std::max(0.0f, 1.0f - kKnockbackAirDrag * timeStep);
    }
    const float length = m_knockback.Length();
    m_knockback = speed > 1.0e-3f && length > 1.0e-6f && std::isfinite(speed) ?
                      m_knockback * (speed / length) :
                      JPH::Vec3::sZero();
  }

  const MT_Vector3 worldPosition = JoltMath::ToMT(JPH::Vec3(position));
  PHY_IMotionState *motionState = m_ctrl->GetMotionState();
  motionState->SetWorldPosition(worldPosition);
  m_writtenPosition = worldPosition;
  m_hasWritten = true;
  m_ctrl->MoveCharacterBody(worldPosition, motionState->GetWorldOrientation(), timeStep);

  ApplyPushes(timeStep);
  UpdateSurfaces();
}

void JoltCharacter::ApplyPushes(const float timeStep)
{
  /* Jolt's CharacterVirtual::HandleContact and the end of its Update, with the step's asked
   * velocity: a dynamic body it ran into starts moving at its speed at the contact (at most
   * kMaxStrength newtons), and what it stands on carries its weight. */
  JPH::PhysicsSystem *system = m_env->GetPhysicsSystem();
  JPH::BodyInterface &bodyInterface = system->GetBodyInterface();
  const JPH::BodyLockInterface &lockInterface = system->GetBodyLockInterface();
  const JPH::Vec3 up = m_character->GetUp();
  constexpr float cDamping = 0.9f;
  constexpr float cPenetrationResolution = 0.4f;

  for (const JPH::CharacterContact &contact : m_character->GetActiveContacts()) {
    if (!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
        contact.mMotionTypeB != JPH::EMotionType::Dynamic || contact.mBodyB.IsInvalid())
    {
      continue;
    }
    const JPH::Vec3 relative = m_requestedVelocity - contact.mLinearVelocity;
    const float projected = relative.Dot(contact.mContactNormal);
    const float deltaVelocity = -projected * cDamping -
                                std::min(contact.mDistance, 0.0f) * cPenetrationResolution /
                                    timeStep;
    if (deltaVelocity < 0.0f) {
      continue;
    }
    float inverseEffectiveMass = 0.0f;
    {
      JPH::BodyLockRead lock(lockInterface, contact.mBodyB);
      if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsDynamic()) {
        continue;
      }
      const JPH::Body &body = lock.GetBody();
      const JPH::Vec3 jacobian = JPH::Vec3(contact.mPosition - body.GetCenterOfMassPosition())
                                     .Cross(contact.mContactNormal);
      inverseEffectiveMass = body.GetInverseInertia().Multiply3x3(jacobian).Dot(jacobian) +
                             body.GetMotionProperties()->GetInverseMass();
    }
    if (!(inverseEffectiveMass > 0.0f)) {
      continue;
    }
    const float impulse = std::min(deltaVelocity / inverseEffectiveMass, kMaxStrength * timeStep);
    JPH::Vec3 worldImpulse = -impulse * contact.mContactNormal;
    /* No push down (its weight is below). */
    const float down = worldImpulse.Dot(up);
    if (down < 0.0f) {
      worldImpulse -= down * up;
    }
    bodyInterface.AddImpulse(contact.mBodyB, worldImpulse, contact.mPosition);
  }

  const JPH::BodyID groundBody = m_character->GetGroundBodyID();
  const JPH::Vec3 gravity = JoltMath::ToJolt(m_gravity);
  const float gravityLength = gravity.Length();
  if (!groundBody.IsInvalid() && gravityLength > 1.0e-6f) {
    const float normalDotGravity = m_character->GetGroundNormal().Dot(gravity);
    if (normalDotGravity < 0.0f) {
      const JPH::Vec3 weight = -(kMass * normalDotGravity / gravityLength * timeStep) * gravity;
      /* (AddImpulse leaves bodies that are not dynamic alone.) */
      bodyInterface.AddImpulse(groundBody, weight, m_character->GetGroundPosition());
    }
  }
}

void JoltCharacter::GetTouchingContacts(std::vector<JoltCharacterContact> &r_contacts) const
{
  r_contacts.clear();
  if (!m_simulated || !m_character) {
    return;
  }
  const float touching = m_character->GetCharacterPadding() + kTouchTolerance;
  for (const JPH::CharacterContact &contact : m_character->GetActiveContacts()) {
    if (contact.mBodyB.IsInvalid() || contact.mIsSensorB || contact.mWasDiscarded) {
      continue;
    }
    if (!contact.mHadCollision && contact.mDistance > touching) {
      continue;
    }
    JoltCharacterContact out;
    out.body = contact.mBodyB;
    out.subShape = contact.mSubShapeIDB;
    out.position = contact.mPosition;
    out.normal = -contact.mContactNormal;
    out.penetration = std::max(-contact.mDistance, 0.0f);
    r_contacts.push_back(out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The object's physics
 * \{ */

MT_Vector3 JoltCharacter::GetPosition() const
{
  if (!m_character) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  return JoltMath::ToMT(JPH::Vec3(m_character->GetPosition()));
}

MT_Vector3 JoltCharacter::GetLinearVelocity() const
{
  return JoltMath::ToMT(m_achievedVelocity);
}

void JoltCharacter::SetLinearVelocity(const MT_Vector3 &velocity)
{
  SetVelocity(velocity, 0.0f, false);
}

void JoltCharacter::MoveToEnvironment(JoltPhysicsEnvironment *env)
{
  /* A CharacterVirtual queries the physics system it was made in: rebuilt in the new one. */
  m_env = env;
  if (!m_character || !env) {
    return;
  }
  const JPH::RVec3 position = m_character->GetPosition();
  const JPH::Quat rotation = m_character->GetRotation();
  const JPH::Vec3 velocity = m_character->GetLinearVelocity();
  /* (Crouched: it stays so, its body has the crouched shape still.) */
  const JPH::RefConst<JPH::Shape> stance = m_character->GetShape();
  CreateVirtual(position, rotation, velocity, env->GetPhysicsSystem());
  if (m_character && stance && stance != m_shape) {
    JPH::PhysicsSystem *system = env->GetPhysicsSystem();
    const JPH::ObjectLayer layer = GetObjectLayer();
    m_character->SetShape(stance,
                          FLT_MAX,
                          system->GetDefaultBroadPhaseLayerFilter(layer),
                          system->GetDefaultLayerFilter(layer),
                          JPH::IgnoreSingleBodyFilter(m_ctrl->GetBodyID()),
                          JPH::ShapeFilter(),
                          *env->GetTempAllocator());
  }
}

JoltCharacter *JoltCharacter::CreateReplica(JoltPhysicsController *replicaCtrl) const
{
  PHY_IMotionState *motionState = replicaCtrl ? replicaCtrl->GetMotionState() : nullptr;
  if (!motionState || !m_env) {
    return nullptr;
  }
  JoltCharacter *replica = new JoltCharacter(replicaCtrl,
                                             m_env,
                                             motionState->GetWorldPosition(),
                                             motionState->GetWorldOrientation(),
                                             m_stepHeight);
  replica->SetGravity(m_gravity);
  replica->SetJumpSpeed(m_jumpSpeed);
  replica->SetFallSpeed(m_fallSpeed);
  replica->SetMaxSlope(m_maxSlope);
  replica->SetMaxJumps(m_maxJumps);
  replica->SetStepHeight(m_stepHeight);
  /* (It stands: a copy starts with none of its source's motion; the height is a setting. Its
   * body was made from its source body's settings: a crouched source's shape goes back at its
   * first step.) */
  replica->SetCrouch(false, m_crouchHeight);
  replica->m_bodyShape = m_bodyShape;
  return replica;
}

/** \} */
