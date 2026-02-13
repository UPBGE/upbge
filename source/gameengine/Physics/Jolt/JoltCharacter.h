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

/** \file JoltCharacter.h
 *  \ingroup physjolt
 *  \brief Jolt Physics character controller implementing PHY_ICharacter.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include "MT_Vector3.h"

#include "PHY_ICharacter.h"

class JoltPhysicsController;
class JoltPhysicsEnvironment;

/**
 * JoltCharacter wraps Jolt's CharacterVirtual and implements UPBGE's PHY_ICharacter interface.
 *
 * CharacterVirtual is preferred over Character because:
 *   - No Body needed in the physics system (no extra body overhead)
 *   - Better control over collision response
 *   - Suitable for game engine character controllers
 */
class JoltCharacter : public PHY_ICharacter {
 public:
  JoltCharacter(JoltPhysicsController *ctrl,
                JoltPhysicsEnvironment *env,
                float capsuleRadius,
                float capsuleHalfHeight,
                float stepHeight,
                const MT_Vector3 &startPos);
  virtual ~JoltCharacter();

  /* ---- PHY_ICharacter interface ---- */

  virtual void Jump() override;
  virtual bool OnGround() override;

  virtual MT_Vector3 GetGravity() override;
  virtual void SetGravity(const MT_Vector3 &gravity) override;

  virtual unsigned char GetMaxJumps() override;
  virtual void SetMaxJumps(unsigned char maxJumps) override;

  virtual unsigned char GetJumpCount() override;

  virtual void SetWalkDirection(const MT_Vector3 &dir) override;
  virtual MT_Vector3 GetWalkDirection() override;

  virtual float GetFallSpeed() const override;
  virtual void SetFallSpeed(float fallSpeed) override;

  virtual float GetMaxSlope() const override;
  virtual void SetMaxSlope(float maxSlope) override;

  virtual float GetJumpSpeed() const override;
  virtual void SetJumpSpeed(float jumpSpeed) override;

  virtual void SetVelocity(const MT_Vector3 &vel, float time, bool local) override;

  virtual void Reset() override;

  /* ---- Jolt-specific methods ---- */

  /** Step the character controller. Called each frame from JoltPhysicsEnvironment. */
  void Update(float deltaTime);

  /** Get the character's current world position. */
  MT_Vector3 GetPosition() const;

  /** Get the underlying CharacterVirtual. */
  JPH::CharacterVirtual *GetCharacterVirtual() { return m_character.GetPtr(); }

 private:
  JPH::Ref<JPH::CharacterVirtual> m_character;
  JoltPhysicsController *m_ctrl;
  JoltPhysicsEnvironment *m_env;

  MT_Vector3 m_walkDirection;
  MT_Vector3 m_gravity;
  float m_jumpSpeed;
  float m_fallSpeed;
  float m_maxSlope;
  float m_stepHeight;
  unsigned char m_maxJumps;
  unsigned char m_jumpCount;
  bool m_wantJump;
};
