
/** \file PHY_ICharacter.h
 *  \ingroup phys
 */

#pragma once

// PHY_ICharacter provides a generic interface for "character" controllers

class PHY_ICharacter {
 public:
  virtual ~PHY_ICharacter()
  {
  }

  virtual void Jump() = 0;
  virtual bool OnGround() = 0;

  virtual MT_Vector3 GetGravity() = 0;
  virtual void SetGravity(const MT_Vector3 &gravity) = 0;

  virtual unsigned char GetMaxJumps() = 0;
  virtual void SetMaxJumps(unsigned char maxJumps) = 0;

  virtual unsigned char GetJumpCount() = 0;

  virtual void SetWalkDirection(const class MT_Vector3 &dir) = 0;
  virtual MT_Vector3 GetWalkDirection() = 0;

  virtual float GetFallSpeed() const = 0;
  virtual void SetFallSpeed(float fallSpeed) = 0;

  virtual float GetMaxSlope() const = 0;
  virtual void SetMaxSlope(float maxSlope) = 0;

  virtual float GetJumpSpeed() const = 0;
  virtual void SetJumpSpeed(float jumpSpeed) = 0;

  virtual void SetVelocity(const MT_Vector3 &vel, float time, bool local) = 0;

  virtual void Reset() = 0;
};
