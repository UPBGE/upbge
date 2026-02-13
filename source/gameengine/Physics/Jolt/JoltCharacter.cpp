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

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"

#include <cmath>

/* -------------------------------------------------------------------- */
/** \name JoltCharacter — Construction / Destruction
 * \{ */

JoltCharacter::JoltCharacter(JoltPhysicsController *ctrl,
                             JoltPhysicsEnvironment *env,
                             float capsuleRadius,
                             float capsuleHalfHeight,
                             float stepHeight,
                             const MT_Vector3 &startPos)
    : m_ctrl(ctrl),
      m_env(env),
      m_walkDirection(0, 0, 0),
      m_gravity(0, 0, -9.81f),
      m_jumpSpeed(10.0f),
      m_fallSpeed(55.0f),
      m_maxSlope(JPH::DegreesToRadians(50.0f)),
      m_stepHeight(stepHeight),
      m_maxJumps(1),
      m_jumpCount(0),
      m_wantJump(false)
{
  JPH::PhysicsSystem *physSystem = env->GetPhysicsSystem();
  if (!physSystem) {
    return;
  }

  /* Create capsule shape standing upright in Jolt Y-up space.
   * Jolt CapsuleShape is along Y axis by default — perfect for characters. */
  JPH::RefConst<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleHalfHeight, capsuleRadius);

  /* Create a slightly smaller inner body shape so that CastRay and sensors
   * can detect the CharacterVirtual. Without this, CharacterVirtual is
   * invisible to the physics system's queries. */
  float innerRadius = capsuleRadius * 0.9f;
  float innerHalfHeight = capsuleHalfHeight * 0.9f;
  if (innerRadius < 0.001f) innerRadius = 0.001f;
  if (innerHalfHeight < 0.001f) innerHalfHeight = 0.001f;
  JPH::RefConst<JPH::Shape> innerShape = new JPH::CapsuleShape(innerHalfHeight, innerRadius);

  /* Create CharacterVirtual settings. */
  JPH::CharacterVirtualSettings settings;
  settings.mShape = capsuleShape;
  settings.mInnerBodyShape = innerShape;
  /* Use the controller's collision group/mask for the inner body layer. */
  settings.mInnerBodyLayer = JoltMakeObjectLayer(
      m_ctrl->GetCollisionGroup(), m_ctrl->GetCollisionMask(), JOLT_BP_DYNAMIC);
  settings.mMaxSlopeAngle = m_maxSlope;
  settings.mMaxStrength = 100.0f;
  settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
  settings.mCharacterPadding = 0.02f;
  settings.mPenetrationRecoverySpeed = 1.0f;
  settings.mPredictiveContactDistance = 0.1f;

  /* Convert start position from Blender Z-up to Jolt Y-up. */
  JPH::RVec3 startPosJolt = JoltMath::ToJolt(startPos);

  m_character = new JPH::CharacterVirtual(
      &settings, startPosJolt, JPH::Quat::sIdentity(), 0, physSystem);
}

JoltCharacter::~JoltCharacter()
{
  m_character = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Jump / Ground
 * \{ */

void JoltCharacter::Jump()
{
  if (m_jumpCount < m_maxJumps) {
    m_wantJump = true;
  }
}

bool JoltCharacter::OnGround()
{
  if (!m_character) {
    return false;
  }
  return m_character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Gravity
 * \{ */

MT_Vector3 JoltCharacter::GetGravity()
{
  return m_gravity;
}

void JoltCharacter::SetGravity(const MT_Vector3 &gravity)
{
  m_gravity = gravity;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Jumps
 * \{ */

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Walk Direction
 * \{ */

void JoltCharacter::SetWalkDirection(const MT_Vector3 &dir)
{
  m_walkDirection = dir;
}

MT_Vector3 JoltCharacter::GetWalkDirection()
{
  return m_walkDirection;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Fall / Slope / Jump Speed
 * \{ */

float JoltCharacter::GetFallSpeed() const
{
  return m_fallSpeed;
}

void JoltCharacter::SetFallSpeed(float fallSpeed)
{
  m_fallSpeed = fallSpeed;
}

float JoltCharacter::GetMaxSlope() const
{
  return m_maxSlope;
}

void JoltCharacter::SetMaxSlope(float maxSlope)
{
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
  m_jumpSpeed = jumpSpeed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_ICharacter — Velocity / Reset
 * \{ */

void JoltCharacter::SetVelocity(const MT_Vector3 &vel, float time, bool local)
{
  if (!m_character) {
    return;
  }

  JPH::Vec3 joltVel = JoltMath::ToJolt(vel);

  if (local) {
    /* Transform velocity from character-local to world space. */
    JPH::Quat charRot = m_character->GetRotation();
    joltVel = charRot * joltVel;
  }

  m_character->SetLinearVelocity(joltVel);
}

void JoltCharacter::Reset()
{
  m_jumpCount = 0;
  m_wantJump = false;
  m_walkDirection = MT_Vector3(0, 0, 0);

  if (m_character) {
    m_character->SetLinearVelocity(JPH::Vec3::sZero());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltCharacter — Update (per-frame step)
 * \{ */

void JoltCharacter::Update(float deltaTime)
{
  if (!m_character || !m_env || deltaTime <= 0.0f) {
    return;
  }

  JPH::PhysicsSystem *physSystem = m_env->GetPhysicsSystem();

  /* Convert gravity from Blender Z-up to Jolt Y-up. */
  JPH::Vec3 gravityJolt = JoltMath::ToJolt(m_gravity);

  /* Get current velocity. */
  JPH::Vec3 currentVel = m_character->GetLinearVelocity();

  /* Apply gravity. */
  JPH::Vec3 newVel = currentVel + gravityJolt * deltaTime;

  /* Clamp fall speed. */
  if (newVel.GetY() < -m_fallSpeed) {
    newVel.SetY(-m_fallSpeed);
  }

  /* Reset jump count when on ground. */
  if (OnGround()) {
    m_jumpCount = 0;
    /* Remove downward velocity when grounded. */
    if (newVel.GetY() < 0.0f) {
      newVel.SetY(0.0f);
    }
  }

  /* Handle jump. */
  if (m_wantJump) {
    /* Jump velocity in Jolt Y-up space (upward). */
    newVel.SetY(m_jumpSpeed);
    m_jumpCount++;
    m_wantJump = false;
  }

  /* Apply walk direction (convert from Blender Z-up to Jolt Y-up). */
  JPH::Vec3 walkJolt = JoltMath::ToJolt(m_walkDirection);
  newVel.SetX(walkJolt.GetX());
  newVel.SetZ(walkJolt.GetZ());

  m_character->SetLinearVelocity(newVel);

  /* Step the character using the broadphase and object layer filter from the physics system. */
  JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
  updateSettings.mStickToFloorStepDown = JPH::Vec3(0, -m_stepHeight, 0);
  updateSettings.mWalkStairsStepUp = JPH::Vec3(0, m_stepHeight, 0);

  m_character->ExtendedUpdate(deltaTime,
                              gravityJolt,
                              updateSettings,
                              physSystem->GetDefaultBroadPhaseLayerFilter(
                                  JoltMakeObjectLayer(m_ctrl->GetCollisionGroup(),
                                                      m_ctrl->GetCollisionMask(),
                                                      JOLT_BP_DYNAMIC)),
                              physSystem->GetDefaultLayerFilter(
                                  JoltMakeObjectLayer(m_ctrl->GetCollisionGroup(),
                                                      m_ctrl->GetCollisionMask(),
                                                      JOLT_BP_DYNAMIC)),
                              {},
                              {},
                              *m_env->GetTempAllocator());
}

MT_Vector3 JoltCharacter::GetPosition() const
{
  if (!m_character) {
    return MT_Vector3(0, 0, 0);
  }
  return JoltMath::ToMT(JPH::Vec3(m_character->GetPosition()));
}

/** \} */
