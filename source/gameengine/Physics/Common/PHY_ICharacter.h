
/** \file PHY_ICharacter.h
 *  \ingroup phys
 */

#pragma once

#include "MT_Vector3.h"

class KX_GameObject;

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

  /* ---- 2026-11-17: what Get Character Info reads and Set Character Attribute sets beyond the
   * above. Jolt's character has them; another one (Bullet's) answers as one touching nothing. */

  /** The highest step it climbs (m). */
  virtual float GetStepHeight() const
  {
    return 0.0f;
  }
  virtual void SetStepHeight(float /*stepHeight*/) {}

  /** Crouching: a shape \a height (a part of the standing one, 0..1) tall, its bottom where the
   * standing one's is. It stands up again only where the standing shape has room. */
  virtual void SetCrouch(bool /*crouch*/, float /*height*/) {}
  /** Whether its shape is the crouched one (it may stay so after standing up was asked: no room
   * above). */
  virtual bool IsCrouched() const
  {
    return false;
  }

  /** What it touched at the end of its last step (none when it did not move itself): a surface
   * too steep to stand on (a wall), one facing down that it could stand on upside down (a
   * ceiling). */
  virtual bool OnWall() const
  {
    return false;
  }
  virtual bool OnCeiling() const
  {
    return false;
  }
  /** Away from the walls it touches (their normals' average, unit length); zero off walls. */
  virtual MT_Vector3 GetWallNormal() const
  {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  /** What it stands on, while OnGround: its normal, its velocity where the character stands, its
   * object; zero and null otherwise. */
  virtual MT_Vector3 GetGroundNormal() const
  {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  virtual MT_Vector3 GetGroundVelocity() const
  {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  virtual KX_GameObject *GetGroundObject() const
  {
    return nullptr;
  }
};
