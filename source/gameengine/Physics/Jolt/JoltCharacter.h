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

#include "JoltPhysicsConfig.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <vector>

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

#include "PHY_ICharacter.h"

class JoltPhysicsController;
class JoltPhysicsEnvironment;

/** A surface a character touches after its step: what On Collision reports for it. */
struct JoltCharacterContact {
  JPH::BodyID body;
  JPH::SubShapeID subShape;
  JPH::RVec3 position;
  /** From the character toward the touched body (a contact manifold's convention with the
   * character as body 1). */
  JPH::Vec3 normal;
  /** Below zero: how deep the character's shape is in it. */
  float penetration;
};

/**
 * A Character physics type object (2026-11-16): Jolt's CharacterVirtual moves it, and its own
 * controller body follows the character as Jolt's "inner body" does.
 *
 * - The character's shape is its controller's (the object's Collision Bounds, scaled; a fitted
 *   capsule by default), posed by the object's rotation.
 * - Its body is a Kinematic body of the same shape that never sleeps, moved with MoveKinematic to
 *   where the character went: rays, sensors, other characters and dynamic bodies meet the
 *   character where it is drawn. The character's own sweeps ignore it.
 * - Each step (`JoltPhysicsEnvironment::UpdateCharacters`, before Jolt's update):
 *   PrepareStep (main thread: follow a teleport, the turn and scale of the object, work out the
 *   velocity), Simulate (moves through the world, reads it only: the characters may run on
 *   Jolt's threads), ApplyStep (main thread, in character order: the object's position, the body,
 *   the pushes on dynamic bodies and the character's weight on what it stands on, worked out as
 *   Jolt's CharacterVirtual does; Simulate pushes nothing). So the world comes out the same
 *   whatever order or thread the characters moved in.
 * - A parented character, one whose body is not in the world (an inactive template, Suspend
 *   Physics) or whose dynamics are suspended does not move itself: it follows its object.
 */
class JoltCharacter : public PHY_ICharacter {
 public:
  /** A character for \a ctrl's object at its pose, with \a ctrl's shape. */
  JoltCharacter(JoltPhysicsController *ctrl,
                JoltPhysicsEnvironment *env,
                const MT_Vector3 &position,
                const MT_Matrix3x3 &orientation,
                float stepHeight);
  virtual ~JoltCharacter();

  /* ---- PHY_ICharacter interface ---- */

  virtual void Jump() override;
  virtual bool OnGround() override;

  virtual MT_Vector3 GetGravity() override;
  virtual void SetGravity(const MT_Vector3 &gravity) override;

  virtual unsigned char GetMaxJumps() override;
  virtual void SetMaxJumps(unsigned char maxJumps) override;

  virtual unsigned char GetJumpCount() override;

  /** The walking velocity in m/s; its part along the character's up is ignored. */
  virtual void SetWalkDirection(const MT_Vector3 &dir) override;
  virtual MT_Vector3 GetWalkDirection() override;

  virtual float GetFallSpeed() const override;
  virtual void SetFallSpeed(float fallSpeed) override;

  virtual float GetMaxSlope() const override;
  virtual void SetMaxSlope(float maxSlope) override;

  virtual float GetJumpSpeed() const override;
  virtual void SetJumpSpeed(float jumpSpeed) override;

  /** Its part along up becomes the vertical speed now (a launch, gravity then acts); its
   * horizontal part replaces the walking velocity for \a time seconds (at least one step). A set
   * velocity ends the knock-back and the impulses not taken yet (forces still act). */
  virtual void SetVelocity(const MT_Vector3 &vel, float time, bool local) override;

  virtual void Reset() override;

  virtual float GetStepHeight() const override;
  virtual void SetStepHeight(float stepHeight) override;

  /** Crouching (2026-11-17): the shape changes at its next step. Down at once; up only where the
   * standing shape has room (Jolt's CharacterVirtual::SetShape with a penetration check, as
   * Jolt's crouch sample), else it stays crouched and tries again every step. Its body takes the
   * same shape (rays and other bodies meet the shorter character). */
  virtual void SetCrouch(bool crouch, float height) override;
  virtual bool IsCrouched() const override;
  /** The crouched shape's height as a part of the standing one. */
  float GetCrouchHeight() const
  {
    return m_crouchHeight;
  }

  /** Worked out at the end of its step (ApplyStep) from its contacts there: touching (collided,
   * or within the padding + kTouchTolerance), not a sensor. By the contact normal n (toward the
   * character) and up: a ceiling when -n is a slope it could walk (upside down), a wall when n
   * is too steep to walk and no ceiling (Godot's rule, by Max Slope; Jolt's "no limit" Max Slope
   * of 0 leaves only vertical ones walls). */
  virtual bool OnWall() const override;
  virtual bool OnCeiling() const override;
  virtual MT_Vector3 GetWallNormal() const override;
  virtual MT_Vector3 GetGroundNormal() const override;
  virtual MT_Vector3 GetGroundVelocity() const override;
  /** Found by its body at the call (a removed object's body is gone: null). */
  virtual KX_GameObject *GetGroundObject() const override;

  /** The crouched shape of \a standing: \a height of its height (along the shape's up, Jolt's Y:
   * the object's Z), its bottom kept. A capsule stays one of the same radius (at least a sphere
   * tall), a sphere becomes a smaller one, any other shape is scaled along up. Height 1 (or a
   * shape with no height): \a standing itself. */
  static JPH::RefConst<JPH::Shape> CrouchedShapeOf(const JPH::Shape &standing, float height);
  /** The smallest crouched height (a part of the standing one). */
  static constexpr float kMinCrouchHeight = 0.05f;
  /** Contacts whose normal is this close to horizontal are walls whatever Max Slope says. */
  static constexpr float kWallFlatness = 1.0e-3f;

  /* ---- Pushes (2026-11-17): forces and impulses, as Unreal's character movement takes them ---- */

  /** A push (Apply Impulse, an explosion): a velocity change of impulse / kMass (with
   * \a velocityChange the impulse is the velocity change), taken at its next step: the part along
   * up as a jump's (off the ground, gravity then acts), the rest as knock-back. */
  void AddImpulse(const MT_Vector3 &impulse, bool velocityChange);
  /** A force (Apply Force, Keep Distance): force / kMass x the step, at its next step (the
   * forces of a step add up, as a body's). */
  void AddForce(const MT_Vector3 &force);
  /** The sideways velocity pushes gave it (m/s), on top of walking. */
  MT_Vector3 GetKnockback() const;
  /** How knock-back fades: on the ground it loses kKnockbackGroundFriction of itself a second
   * and kKnockbackGroundBraking m/s a second (Unreal's walking friction: a 5 m/s push slides
   * about 0.55 m in 0.4 s), in the air kKnockbackAirDrag of itself a second (it flies on). What
   * it runs into stops its part into the surface. */
  static constexpr float kKnockbackGroundFriction = 8.0f;
  static constexpr float kKnockbackGroundBraking = 2.0f;
  static constexpr float kKnockbackAirDrag = 0.5f;

  /* ---- The step ---- */

  /** Main thread, before Simulate. Follows its object (a move by logic is a teleport; its
   * rotation and scale), counts jumps and sets this step's velocity. False: no move this step
   * (it follows its object only). */
  bool PrepareStep(float timeStep);
  /** Moves through the world. Reads the world only and writes only this character: any thread,
   * many characters at once. */
  void Simulate(float timeStep, JPH::TempAllocator &allocator);
  /** Main thread, in character order: the object's position, the body, pushes and weight. */
  void ApplyStep(float timeStep);

  /** Whether the last step moved it (PrepareStep returned true). */
  bool SimulatedThisStep() const
  {
    return m_simulated;
  }
  /** The surfaces it touches after its last step (none when it did not move itself). */
  void GetTouchingContacts(std::vector<JoltCharacterContact> &r_contacts) const;

  /* ---- Reads and writes of the object's physics ---- */

  MT_Vector3 GetPosition() const;
  /** How fast it moved in its last step (m/s), the push of what it stands on included. */
  MT_Vector3 GetLinearVelocity() const;
  /** World linear velocity set on the object: as SetVelocity(velocity, 0). */
  void SetLinearVelocity(const MT_Vector3 &velocity);

  JPH::CharacterVirtual *GetCharacterVirtual()
  {
    return m_character.GetPtr();
  }
  JoltPhysicsController *GetController() const
  {
    return m_ctrl;
  }
  bool UsesController(const JoltPhysicsController *ctrl) const
  {
    return m_ctrl == ctrl;
  }

  /** A merged scene's character in \a env: rebuilt in its physics system, everything kept. */
  void MoveToEnvironment(JoltPhysicsEnvironment *env);

  /** A copy's character (Add Object) for \a replicaCtrl at its object's pose: this one's
   * settings (gravity, speeds, jumps, slope, step), none of its motion. */
  JoltCharacter *CreateReplica(JoltPhysicsController *replicaCtrl) const;

  /** Jolt's CharacterVirtual defaults: the weight pressed on what it stands on, the most force it
   * pushes dynamic bodies with. */
  static constexpr float kMass = 70.0f;
  static constexpr float kMaxStrength = 100.0f;
  /** A contact counts as touching for On Collision within the padding plus this (m). */
  static constexpr float kTouchTolerance = 0.01f;
  /** How much higher than Step Height a stair edge may be (m). */
  static constexpr float kStairTolerance = 0.02f;

 private:
  /** Whether Jolt's stair step just made lands where the character may stay (see the .cpp). */
  bool StairLandingHolds(float groundBefore) const;
  void CreateVirtual(const JPH::RVec3 &position,
                     const JPH::Quat &rotation,
                     const JPH::Vec3 &velocity,
                     JPH::PhysicsSystem *system);
  JPH::ObjectLayer GetObjectLayer() const;
  JPH::Vec3 UpOf(const MT_Vector3 &gravity) const;
  void ApplyPushes(float timeStep);
  /** Main thread, PrepareStep: the shape it wants (standing or crouched) when it fits;
   * \a standingChanged (a new scale) switches without the room check. */
  void UpdateStance(bool standingChanged);
  /** Sets \a shape on the CharacterVirtual (\a check: only with room). */
  bool SwitchShape(const JPH::RefConst<JPH::Shape> &shape, bool check);
  /** Gives its body the CharacterVirtual's shape where they differ (crouched, a copy of a
   * crouched one, a new scale written by the controller). */
  void SyncBodyShape();
  /** End of ApplyStep: On Wall, On Ceiling, the wall and ground answers. */
  void UpdateSurfaces();
  void ClearSurfaces();

  JPH::Ref<JPH::CharacterVirtual> m_character;
  JoltPhysicsController *m_ctrl;
  JoltPhysicsEnvironment *m_env;
  /** The standing shape: its controller's (followed when the object's scale changes). */
  JPH::RefConst<JPH::Shape> m_shape;
  /** Crouching: whether it wants to, how tall (a part of standing), the shape made for that
   * (null until needed; made again when m_shape or the height changes). */
  bool m_wantCrouch;
  float m_crouchHeight;
  JPH::RefConst<JPH::Shape> m_crouchedShape;
  float m_crouchedShapeHeight;
  /** The shape SyncBodyShape gave its body (a crouched one); null: its controller's. */
  JPH::RefConst<JPH::Shape> m_bodyShape;

  /** What it touched at the end of its last step (UpdateSurfaces). */
  bool m_onWall;
  bool m_onCeiling;
  JPH::Vec3 m_wallNormal;
  JPH::Vec3 m_groundNormal;
  JPH::Vec3 m_groundVelocity;
  JPH::BodyID m_groundBody;

  MT_Vector3 m_walkDirection;
  MT_Vector3 m_gravity;
  float m_jumpSpeed;
  float m_fallSpeed;
  float m_maxSlope;
  float m_stepHeight;
  unsigned char m_maxJumps;
  unsigned char m_jumpCount;
  bool m_wantJump;

  /** Set Character Velocity: the horizontal velocity in place of walking, and for how long. */
  JPH::Vec3 m_overrideVelocity;
  float m_overrideTime;
  bool m_overridePending;

  /** Knock-back: the sideways velocity pushes gave it, added to walking (or to a Set Character
   * Velocity), fading out (kKnockback*). */
  JPH::Vec3 m_knockback;
  /** Pushes since its last step: velocity changes (impulses) and forces (N). */
  JPH::Vec3 m_pendingVelocityChange;
  JPH::Vec3 m_pendingForce;

  /** This step: whether it moves, its velocity, where it started. */
  bool m_simulated;
  JPH::Vec3 m_requestedVelocity;
  JPH::RVec3 m_stepStart;
  /** How fast it moved in its last step. */
  JPH::Vec3 m_achievedVelocity;

  /** The position it last wrote into its object: another one there is a teleport. */
  MT_Vector3 m_writtenPosition;
  bool m_hasWritten;
};
