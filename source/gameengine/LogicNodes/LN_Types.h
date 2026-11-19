/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Types.h
 *  \ingroup logicnodes
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../Physics/Common/PHY_RigidBodyConstraintSettings.h"
#include "LN_Rotation.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

class LN_CommandBuffer;
struct LN_Value;

/**
 * Compiled Logic Nodes compatibility policy:
 * - Bump #LN_PROGRAM_VERSION for opcode layout changes, opcode semantic changes, bytecode
 *   encoding changes, or runtime-state layout changes.
 * - Bump #LN_PROGRAM_SCHEMA_VERSION for node id, socket id, or schema changes that alter
 *   compiled output.
 * - Compiled programs are reusable only when version, schema, build flags, source tree name,
 *   source library path, and source checksum all match the current runtime/source graph.
 * - Project policy: do not bump either value unless explicitly requested.
 */
static constexpr uint32_t LN_PROGRAM_VERSION = 42;
static constexpr uint32_t LN_PROGRAM_SCHEMA_VERSION = 26;
static constexpr uint32_t LN_INVALID_INDEX = UINT32_MAX;

/**
 * On Collision's Events setting (bNode::custom1): Any Object (0, every node saved before
 * 2026-11-09) fires On Enter when the owner goes from touching nothing to touching something and
 * On Exit when the last object leaves; Each Object (1, new nodes) runs each event's route once for
 * every object that started touching, still touches or stopped touching in that step.
 */
enum class LN_CollisionEvents : int16_t {
  AnyObject = 0,
  EachObject = 1,
};
/** Which event of an Each Object On Collision node a route belongs to (the repeat key's low bits). */
enum class LN_CollisionEventKind : uint32_t {
  Enter = 1,
  Stay = 2,
  Exit = 3,
};
/** A repeat key (LN_Instruction::ui_event_state_index) of an Each Object On Collision event:
 * bit 31 set, the node's index in the program (LN_Program::AcquireCollisionEventNode) shifted by 2,
 * the event kind in the low 2 bits. Never LN_INVALID_INDEX (the low bits are never 0). */
inline constexpr uint32_t LN_COLLISION_REPEAT_ROUTE_BIT = 1u << 31;
/* (1 << 29) - 1 would make the last node's Exit key LN_INVALID_INDEX. */
inline constexpr uint32_t LN_MAX_COLLISION_EVENT_NODES = (1u << 29) - 2;
inline constexpr uint32_t LN_CollisionRepeatRoute(const uint32_t node_index,
                                                  const LN_CollisionEventKind kind)
{
  return LN_COLLISION_REPEAT_ROUTE_BIT | (node_index << 2) | uint32_t(kind);
}
inline constexpr bool LN_IsCollisionRepeatRoute(const uint32_t repeat_key)
{
  return repeat_key != LN_INVALID_INDEX && (repeat_key & LN_COLLISION_REPEAT_ROUTE_BIT) != 0;
}
inline constexpr uint32_t LN_CollisionRepeatRouteNode(const uint32_t repeat_key)
{
  return (repeat_key & ~LN_COLLISION_REPEAT_ROUTE_BIT) >> 2;
}
inline constexpr LN_CollisionEventKind LN_CollisionRepeatRouteKind(const uint32_t repeat_key)
{
  return LN_CollisionEventKind(repeat_key & 3u);
}
static_assert(LN_CollisionRepeatRoute(LN_MAX_COLLISION_EVENT_NODES, LN_CollisionEventKind::Exit) !=
              LN_INVALID_INDEX);
static_assert(LN_CollisionRepeatRouteNode(LN_CollisionRepeatRoute(
                  LN_MAX_COLLISION_EVENT_NODES, LN_CollisionEventKind::Stay)) ==
              LN_MAX_COLLISION_EVENT_NODES);

/** Shared upper bound for compiler-validated dynamic Logic Node sockets. */
inline constexpr uint32_t LN_MAX_DYNAMIC_SOCKET_COUNT = 64;

struct LN_StringId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

enum class LN_Event : uint8_t {
  OnInit = 0,
  OnUpdate,
  OnFixedUpdate,
};

/** Objects whose render-frame Logic Nodes and visual animations remain active during a global
 * game pause. Targets are authored on the While Paused event node and resolved before scheduling,
 * so a paused tree never has to execute in order to make itself runnable. */
enum class LN_PauseProcessingScope : uint8_t {
  Owner = 0,
  ObjectHierarchy,
  Collection,
  Scene,
};

/** On Logic + Physics Step's "Every N Steps": its routes run only on every `interval`-th step.
 * With `spread`, each object using the tree runs on its own step of the N (trees of one source
 * tree are numbered as they start), so N objects' work is spread evenly; without, all run on the
 * steps divisible by N. Step nodes with the same setting share one gate (same due steps). */
struct LN_StepGate {
  uint32_t interval = 1;
  bool spread = true;
};

/** On Logic + Physics Step's node setting (custom2 bit): all objects run on the same steps. */
inline constexpr int16_t LN_STEP_GATE_SAME_STEP = 1 << 0;
/** Largest Every N Steps (the node's setting is clamped to it). */
inline constexpr uint32_t LN_STEP_GATE_MAX_INTERVAL = 1000;

struct LN_PauseProcessingRequest {
  LN_PauseProcessingScope scope = LN_PauseProcessingScope::Owner;
  uint32_t target_value_expr_index = LN_INVALID_INDEX;
  uint32_t source_ref_index = LN_INVALID_INDEX;
};

enum class LN_OpCode : uint8_t {
  Nop = 0,
  SetWorldPosition,
  SetLocalPosition,
  SetWorldOrientation,
  SetLocalOrientation,
  SetWorldScale,
  SetLocalScale,
  SetLinearVelocity,
  SetLocalLinearVelocity,
  SetAngularVelocity,
  SetLocalAngularVelocity,
  /** Apply Impulse and Apply Angular Impulse (2026-11-11): value_expr_index Object (none: the
   * owner), vector_expr_index Impulse / Angular Impulse, secondary_vector_expr_index Point (the
   * socket "Attach"), int_value the LN_ImpulseMode, bool_expr_index Local,
   * secondary_bool_expr_index Ignore Mass, tertiary_bool_expr_index Visualize. Read in that order:
   * Impulse, Point, Local, Ignore Mass, Visualize. */
  ApplyImpulse,
  SetVisibility,
  SetObjectColor,
  MakeLightUnique,
  SetLightColor,
  SetLightPower,
  SetLightShadow,
  ApplyMovement,
  ApplyRotation,
  ApplyForce,
  ApplyForceToTarget,
  ApplyTorque,
  SetGameProperty,
  AddObject,
  SetParent,
  RemoveParent,
  RemoveObject,
  SetGravity,
  SetTimeScale,
  SetGamePaused,
  SetActiveCamera,
  SetCameraFov,
  SetCameraOrthoScale,
  SetWindowSize,
  SetFullscreen,
  SetVSync,
  SetShowFramerate,
  SetShowProfile,
  SetCollisionGroup,
  SetPhysics,
  SetPhysicsPaused,
  SetDynamics,
  RebuildCollisionShape,
  SetRigidBodyAttribute,
  SetTreeProperty,
  SetCursorVisibility,
  SetCursorPosition,
  SetGamepadVibration,
  GamepadLook,
  MouseLook,
  CharacterJump,
  SetCharacterGravity,
  SetCharacterJumpSpeed,
  SetCharacterMaxJumps,
  SetCharacterWalkDirection,
  SetCharacterVelocity,
  VehicleControl,
  VehicleApplyEngineForce,
  VehicleApplyBraking,
  VehicleApplySteering,
  SetVehicleSuspensionCompression,
  SetVehicleSuspensionStiffness,
  SetVehicleSuspensionDamping,
  SetVehicleWheelFriction,
  Print,
  QuitGame,
  RestartGame,
  LoadBlendFile,
  SetLogicTreeEnabled,
  RunLogicTreeOnce,
  InstallLogicTree,
  PlayAction,
  StopAction,
  SetActionFrame,
  SetShapeKeyValue,
  StopAllSounds,
  /** Play Sound and Play Sound 3D: int_value bits 0-3 Play Sound 3D's listener, bits 4 and up
   * the node's sound slot + 1 (2026-11-17, LN_Program::AcquireSoundNode; 0: none, every older
   * program). Play Sound 3D: tertiary_bool_expr_index At Position, vector_expr_index Position
   * (read last). */
  PlaySound,
  PlaySound3D,
  PauseSound,
  ResumeSound,
  StopSound,
  SendEvent,
  SetGlobalProperty,
  SaveVariable,
  SaveVariableDict,
  ClearVariables,
  RemoveVariable,
  Translate,
  Navigate,
  FollowPath,
  SetCollectionVisibility,
  SetOverlayCollection,
  RemoveOverlayCollection,
  OpenObjectMenu,
  CloseObjectMenu,
  SetUIFocus,
  ScreenAnchor,
  MoveToward,
  SlowFollow,
  LoadScene,
  SetScene,
  SaveGame,
  SetSaveExistence,
  LoadGame,
  AlignAxisToVector,
  RotateToward,
  ReplaceMesh,
  CopyProperty,
  SetBonePoseLocation,
  SetBonePoseRotation,
  SetBonePoseScale,
  SetBoneAttribute,
  SetBoneConstraintInfluence,
  SetBoneConstraintTarget,
  SetBoneConstraintAttribute,
  SetMaterialSlot,
  SetMaterialParameter,
  SetMaterialNodeSocketValue,
  SetGeometryNodesInput,
  SetGeometryNodeSocketValue,
  SetCompositorNodeSocketValue,
  MakeNodeTreeUnique,
  SetNodeMute,
  EnableDisableModifier,
  AssignGeometryNodesModifier,
  DrawLine,
  DrawArrow,
  DrawPath,
  DrawBox,
  DrawMesh,
  DrawAxis,
  AddPhysicsConstraint,
  RemovePhysicsConstraint,
  SetTransformVector,
  SetVelocityVector,
  ApplyTransformVector,
  ApplyPhysicsVector,
  /** Timer and Delay (Restart / Queue): int_value the time flow state, float_expr_index the
   * seconds, secondary_bool_expr_index Ignore Timescale. secondary_int_value (2026-11-17,
   * LN_TIME_FLOW_*): 0 arms a countdown (every older op); Stop, Pause and Resume act on the
   * node's countdowns and read no input. */
  ArmTimer,
  ArmDelay,
  ArmNextFrame,
  UpdatePulsify,
  UpdateBarrier,
  BranchRoute,
  SetFlowGate,
  TryFlowGate,
  SetBonePoseTransform,
  TryOnce,
  ResetOnce,
  TryCooldown,
  ResetCooldown,
  TryFlowRouter,
  ResetFlowRouter,
  TryRepeatLimit,
  ResetRepeatLimit,
  SampleMotion,
  ResetMotion,
  BreakLoop,
  /** Change a UI Element's Interactable state at runtime. */
  SetUIInteractable,
  /** Set Object Attribute's Teleport: when bool_expr_index is true, the object's move is a jump
   * Smooth Motion draws at once (KX_GameObject::ResetSmoothing). */
  ResetSmoothing,
  /** Rebind Input Action and Input Map: change the game's Input Map (LN_InputMapOperation). */
  InputMap,
  /** Inventory node: run its operation on an object's inventory game property and write it back
   * at once, so the next inventory change in the same tick sees it. */
  InventoryEdit,
  /** Drop Item: put a copy of a library item in the world, now in a Logic + Physics step, else at
   * the next one. */
  DropItem,
  /** Scroll Object View: ask a Scroll or Grid View to scroll (LN_UIViewScrollMode in int_value);
   * the view applies it when it next lays out. */
  ScrollObjectView,
  /** Set Split Screen: bool_expr_index Enabled, int_expr_index Divider, secondary_bool_expr_index
   * Keep Mouse in View, int_value the node's packed choices (LN_SPLIT_SCREEN_*). */
  SetSplitScreen,
  /** Set Camera Viewport: value_expr_index camera, bool_expr_index Enabled, float_expr_index to
   * quaternary_float_expr_index Left, Top, Width, Height, secondary_bool_expr_index On Top,
   * secondary_int_value the area (0 the window, 1 a player's view) and int_expr_index User. */
  SetCameraViewport,
  /** Set Player Gamepad: int_expr_index User, secondary_int_expr_index Gamepad (-1 none). */
  SetPlayerGamepad,
  /** Set Camera Visibility: value_expr_index camera, secondary_value_expr_index object (owner if
   * none), bool_expr_index Visible, secondary_bool_expr_index Keep Shadow,
   * tertiary_bool_expr_index Include Children. */
  SetCameraVisibility,
  /** Set Player Pointer: int_expr_index user, bool_expr_index Enabled, value_expr_index surface
   * (owner if none), secondary_value_expr_index cursor (none if none), string_expr_index Move
   * Action, float_expr_index Speed. */
  SetPlayerPointer,
  /** Take UI Control: value_expr_index object (owner if none), int_expr_index user,
   * bool_expr_index Take, property_ref_index its UI Command Completed expression. */
  TakeUIControl,
  /** Set Collection Member: value_expr_index collection, secondary_value_expr_index object (owner
   * if none), bool_expr_index Member, secondary_bool_expr_index Include Children. */
  SetCollectionMember,
  /** Show Message: value_expr_index object (owner if none), string_expr_index Text,
   * float_expr_index Seconds, int_expr_index Lines, secondary_int_expr_index User,
   * tertiary_int_expr_index Count. */
  ShowMessage,
  /** Set Volume: int_value the LN_SoundGroup (Master too), float_expr_index Volume,
   * secondary_float_expr_index Fade (seconds). */
  SetSoundVolume,
  /** Set Random Seed: int_value 0 = the Seed input (int_expr_index), 1 = a new seed from the
   * clock. Applied after the tick (LN_RequestRandomGameSeed). */
  SetRandomSeed,
  /** A list or dictionary node in Game Property mode (2026-11-08): changes the list or dictionary
   * a game property holds, at the flush (CommandType::EditGameProperty). int_value the
   * LN_PropertyEditOp, value_expr_index Object (none: the owner), string_expr_index Property,
   * int_expr_index Index, secondary_string_expr_index Key, secondary_value_expr_index Value,
   * tertiary_value_expr_index Amount. */
  EditGameProperty,
  /** Explode or Implode Impulse (2026-11-11; idname LogicNativeExplosionImpulse):
   * secondary_int_value the center (LN_EXPLOSION_CENTER_*):
   * Object reads tertiary_value_expr_index (none: the owner; its pending moves applied first,
   * the object itself left alone), then vector_expr_index its Offset (2026-11-17: in the
   * object's own space, its transform x the Offset as ShapeCastPlace; none or zero: its
   * position), Position reads vector_expr_index Origin; then
   * float_expr_index Radius, secondary_float_expr_index Strength, tertiary_float_expr_index
   * Upward Lift, bool_expr_index Ignore Mass, secondary_bool_expr_index Visualize,
   * secondary_value_expr_index Ignore Object (none: nothing more ignored),
   * quaternary_value_expr_index Ignore Objects (a list; none in a node without it),
   * string_expr_index Property, int_expr_index Mask, int_value the falloff and Implode
   * bits (LN_EXPLOSION_FALLOFF_LINEAR, LN_EXPLOSION_IMPLODE); read in that
   * order. It reads the scene in the instruction (its center and its overlap): never on the batch
   * lists or the bridge for that. Finds the bodies touching the sphere
   * (LN_RuntimeTree::ShapeCast, as Shape Cast All with X-Ray: at most 256, nearest first) and
   * records one ApplyImpulse command (LN_ImpulseMode::Explosion) for each; each push is worked
   * out at the flush from the body's center of mass then. The node's Objects output is a Shape
   * Cast All query of the same sphere. Main thread only (it reads the physics world). */
  ExplosionImpulse,
  /** Keep Distance (2026-11-12, Pull and Hold the same day; idname LogicNativeKeepDistance):
   * int_value the node's state slot
   * (LN_Program::AcquireKeepDistanceNode), secondary_int_value bit 0 the Smooth Falloff, bit 1
   * Measure From Sphere (LN_KEEP_DISTANCE_BIT_*); tertiary_value_expr_index Object (none: the
   * owner), tertiary_int_expr_index Mode (2026-11-15, an input: LN_KEEP_DISTANCE_MODE_*, below 0
   * Push, above 2 Hold), float_expr_index Distance, value_expr_index Reach (read in Hold only: a
   * generic value, every float field being taken; read after Distance),
   * secondary_float_expr_index Strength, tertiary_float_expr_index Damping,
   * quaternary_float_expr_index Radius, int_expr_index Check
   * Every, bool_expr_index Ignore Mass, secondary_bool_expr_index Apply,
   * tertiary_bool_expr_index Visualize, string_expr_index Property, secondary_int_expr_index
   * Mask, secondary_value_expr_index Ignore Object, quaternary_value_expr_index Ignore Objects,
   * quaternary_bool_expr_index Include Sensors; read in that order. It records a request
   * (LN_CommandBuffer::AppendKeepDistance); the flush answers every request of the step at its
   * end, the physics queries on the physics threads, and pushes (LN_KeepDistance.h). Main thread
   * only (its requests are the scene buffer's). */
  KeepDistance,
  /** Tween Value's Forward / Back (2026-11-17): a pulse reaching the input. int_value the
   * tween's Bool::TweenValue expression, secondary_int_value LN_TWEEN_REQUEST_FORWARD or _BACK.
   * It only stores the request in the tween's state (the latest of a step wins); the tween takes
   * it at its next update (LN_RuntimeTree::UpdateTweenValue): in the same step when it is read
   * after the op, else in the next. A sensor's or Boolean Edge's pulse gets the op where the tween
   * compiles, in the sensor's event behind its condition, so it starts the tween in the step it
   * fires. Reads nothing; a native lean op and a column node (STweenRequest). */
  TweenRequest,
  /* Set Navigation Attribute (2026-11-18): int_value the LN_NavigationAttribute; a request handed
   * to the scene's KX_Navigation at the flush's end, as Navigate's. */
  NavigationAttribute,
  Count,
};

/** ArmTimer / ArmDelay's secondary_int_value (2026-11-17): Arm (0, every older op) starts a
 * countdown; Stop forgets every countdown still waiting, and a pause (a Done already due this
 * step still runs); Pause holds them where they are (a Flow arriving meanwhile waits paused too);
 * Resume lets them run on. */
inline constexpr int32_t LN_TIME_FLOW_ARM = 0;
inline constexpr int32_t LN_TIME_FLOW_STOP = 1;
inline constexpr int32_t LN_TIME_FLOW_PAUSE = 2;
inline constexpr int32_t LN_TIME_FLOW_RESUME = 3;

/** How an ApplyImpulse instruction and command push (2026-11-11; the instruction's int_value, the
 * command's int_value). Nodes made before 2026-11-11 hold WorldOffset (custom1 0). */
enum class LN_ImpulseMode : int32_t {
  /** Point is an offset from the body's position along the world axes (the old Apply Impulse:
   * it does not turn with the object). */
  WorldOffset = 0,
  /** Point is a world position (a Raycast's Hit Point). */
  WorldPoint = 1,
  /** Point is in the object's own axes from its origin: it turns with the object. */
  LocalPoint = 2,
  /** Apply Angular Impulse: the vector is an angular impulse; no point. */
  Angular = 3,
  /** Explosion Impulse's push of one reached object (commands only): through the center of
   * mass, away from the blast. */
  Explosion = 4,
};
inline constexpr int32_t LN_IMPULSE_MODE_LAST = int32_t(LN_ImpulseMode::Explosion);

/** Explode or Implode Impulse's options (the node's custom1 bits, the instruction's int_value):
 * bit 0 the Falloff (Linear), bit 1 Implode (pull toward the center instead of pushing away). */
inline constexpr int32_t LN_EXPLOSION_FALLOFF_CONSTANT = 0;
/** Set Rigid Body Constraint (2026-11-17): Op::AddPhysicsConstraint with secondary_int_value
 * LN_RIGID_BODY_CONSTRAINT_SET + the setting (custom1, LN_RigidBodyConstraintSetting) changes
 * the named constraint of the Object (empty: the owner) instead of adding one; its payload holds
 * the setting's inputs (the Add node's names). 0: Add. */
inline constexpr int32_t LN_RIGID_BODY_CONSTRAINT_SET = 1;
enum class LN_RigidBodyConstraintSetting : int32_t {
  Enabled = 0,
  Limits,
  Motor,
  Springs,
  Breaking,
};
/** Save Slot's Delete (2026-11-17): Op::SaveGame's secondary_int_value, a Save Game command's
 * bool_value: delete the slot's file, in order with the save service's other requests. */
inline constexpr int32_t LN_SAVE_SLOT_DELETE = 1;
inline constexpr int32_t LN_EXPLOSION_FALLOFF_LINEAR = 1;
/** Explode or Implode's falloff at `distance` from the center (the push at the flush and the
 * Strengths output, 2026-11-17): Linear: 1 at the center to 0 at `radius` (0 without a radius);
 * else 1. */
inline float LN_ExplosionFalloff(const float distance, const float radius, const bool linear)
{
  if (!linear) {
    return 1.0f;
  }
  if (!(radius > 0.0f)) {
    return 0.0f;
  }
  const float factor = 1.0f - distance / radius;
  return factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
}
inline constexpr int32_t LN_EXPLOSION_IMPLODE = 2;
/** Explosion Impulse's Center (the node's custom2, the instruction's secondary_int_value); nodes
 * made before the dropdown hold Position. */
inline constexpr int32_t LN_EXPLOSION_CENTER_POSITION = 0;
inline constexpr int32_t LN_EXPLOSION_CENTER_OBJECT = 1;
/** The most objects one Explosion Impulse reaches (the nearest ones; Shape Cast All's limit). */
inline constexpr uint32_t LN_EXPLOSION_MAX_OBJECTS = 256;

/** Keep Distance's node settings: custom1 bit 0 the Falloff (Smooth); custom2 Measure From. Its
 * Mode is an input (2026-11-15; the node's Mode dropdown before, custom1 bits 1-2, moved into
 * the input when a file is read): 0 Push, 1 Pull, 2 Hold. */
inline constexpr int32_t LN_KEEP_DISTANCE_FALLOFF_LINEAR = 0;
inline constexpr int32_t LN_KEEP_DISTANCE_FALLOFF_SMOOTH = 1;
inline constexpr int32_t LN_KEEP_DISTANCE_MODE_PUSH = 0;
inline constexpr int32_t LN_KEEP_DISTANCE_MODE_PULL = 1;
inline constexpr int32_t LN_KEEP_DISTANCE_MODE_HOLD = 2;
inline constexpr int32_t LN_KEEP_DISTANCE_MEASURE_OBJECT_SHAPE = 0;
inline constexpr int32_t LN_KEEP_DISTANCE_MEASURE_SPHERE = 1;

/** Math's and Vector Math's own operations (custom1, 2026-11-16): Blender's NODE_MATH_* and
 * NODE_VECTOR_MATH_* values stay below 100; these have no Blender value. rna_nodetree.cc and the
 * node files use the same numbers. */
inline constexpr int16_t LN_MATH_ANGLE_DIFFERENCE = 100;
inline constexpr int16_t LN_VECTOR_MATH_ANGLE = 100;
inline constexpr int16_t LN_VECTOR_MATH_SIGNED_ANGLE = 101;
inline constexpr int16_t LN_VECTOR_MATH_LIMIT_LENGTH = 102;
inline constexpr int16_t LN_VECTOR_MATH_MOVE_TOWARDS = 103;
/** The bits of its instruction's secondary_int_value. */
inline constexpr int32_t LN_KEEP_DISTANCE_BIT_SMOOTH = 1 << 0;
inline constexpr int32_t LN_KEEP_DISTANCE_BIT_SPHERE = 1 << 1;
/** The most surfaces (patches) one check keeps, the nearest ones. */
inline constexpr uint32_t LN_KEEP_DISTANCE_MAX_PATCHES = 32;

/** Play Sound 3D's Listener (the node's custom1, LN_Instruction::int_value): whose camera hears
 * the sound. */
enum class LN_SoundListener : int32_t {
  NearestPlayer = 0,
  Player = 1,
  ActiveCamera = 2,
};

/** A sound's volume group (Play Sound's custom2, LN_Instruction::secondary_int_value): a group's
 * volume scales every sound of it, also those already playing. Effects is 0, so every Play Sound
 * saved before the groups is an effect. UI is for menu sounds (On UI Sound -> Play Sound). Master
 * (Set Volume only) scales the whole audio device: every sound of the game, also from Python or
 * logic bricks. */
enum class LN_SoundGroup : int32_t {
  Effects = 0,
  Music = 1,
  Voice = 2,
  Ambience = 3,
  UI = 4,
  Master = 5,
};
inline constexpr int32_t LN_SOUND_GROUP_COUNT = 6;

/** A menu's sound moments, On UI Sound's outputs: the highlight moves to another UI Element, a
 * click, Back closes a menu, a press that does nothing. A player's frame tells the most telling of
 * them (Back, Denied, Accept, Move). */
enum class LN_UISound : uint8_t { Move = 0, Accept, Back, Denied, Count, None = Count };

/** Set Split Screen's choices in LN_Instruction::int_value: the node's custom1 (two players in
 * bits 0-1, three players in bit 2) and its custom2 (field of view) shifted by 8. Values match
 * KX_PlayerViews::TwoPlayers, ThreePlayers and FieldOfView. The command also sets FREE_MOUSE when
 * the Keep Mouse in View input is off. */
constexpr int32_t LN_SPLIT_SCREEN_TWO_PLAYERS_MASK = 0x3;
constexpr int32_t LN_SPLIT_SCREEN_THREE_PLAYERS_SHIFT = 2;
constexpr int32_t LN_SPLIT_SCREEN_FREE_MOUSE = 1 << 3;
constexpr int32_t LN_SPLIT_SCREEN_FIELD_OF_VIEW_SHIFT = 8;

/** How Scroll Object View scrolls (stable values, the node's mode). */
enum class LN_UIViewScrollMode : uint8_t {
  /** Grid View: scroll the least so the item at the index is in view. */
  ShowItem = 0,
  Top,
  Bottom,
  NextPage,
  PreviousPage,
  /** By whole rows (columns of a horizontal grid); positive toward the end. */
  Lines,
};

/** LN_OpCode::InputMap's int_value. */
enum class LN_InputMapOperation : uint8_t {
  Save = 0,
  Load,
  Reset,
  Rebind,
};

/** Get / Set Rigid Body Attribute's Attribute (custom1). */
enum class LN_RigidBodyAttribute : uint8_t {
  Mass = 0,
  Friction,
  Restitution,
  /** Both dampings: Get's Damping (two outputs); Set nodes saved with it are Linear Damping since
   * 2026-11-15 (the editor's node_update), the engine still sets both for one. */
  Damping,
  MinLinearVelocity,
  MaxLinearVelocity,
  MinAngularVelocity,
  MaxAngularVelocity,
  GravityFactor,
  Ccd,
  Sleeping,
  AxisLocks,
  AllowPhysicsRotation,
  /** Set only (2026-11-15, the user's call): one damping, the other left as it is. */
  LinearDamping,
  AngularDamping,
  /** Set only (2026-11-18, the user's request): the Compound checkbox while playing
   * (KX_GameObject::SetCompoundEnabled; Enabled, as CCD's). */
  Compound,
};

enum class LN_RigidBodyConstraintMatchMode : uint8_t {
  Exact = 0,
  Contains,
  All,
};

enum class LN_FlowGateControl : int32_t {
  Toggle = 0,
  Open,
  Close,
};

/** Tween Value's Forward / Back (Op::TweenRequest's secondary_int_value, and the request a
 * tween's state holds until its next update: BoolExpressionRuntimeState::tween_request). */
inline constexpr int32_t LN_TWEEN_REQUEST_NONE = 0;
inline constexpr int32_t LN_TWEEN_REQUEST_FORWARD = 1;
inline constexpr int32_t LN_TWEEN_REQUEST_BACK = 2;

/** Set Character Attribute's settings (2026-11-17): Op::SetCharacterJumpSpeed and its command
 * carry every one, the setting in the instruction's and the command's int_value (Jump Force 0:
 * every older program). The value is the instruction's Float (float_expr_index, fallback
 * vector_value.x); Crouch also reads its Bool (bool_expr_index, fallback bool_value), the Float
 * then its Height. Read order: the Float, then the Bool. */
inline constexpr int32_t LN_CHARACTER_SETTING_JUMP_FORCE = 0;
inline constexpr int32_t LN_CHARACTER_SETTING_MAX_SLOPE = 1;
inline constexpr int32_t LN_CHARACTER_SETTING_FALL_SPEED = 2;
inline constexpr int32_t LN_CHARACTER_SETTING_STEP_HEIGHT = 3;
inline constexpr int32_t LN_CHARACTER_SETTING_CROUCH = 4;

/** Get Character Info's Bool::SnapshotCharacterState (int_value). */
inline constexpr int32_t LN_CHARACTER_STATE_ON_WALL = 0;
inline constexpr int32_t LN_CHARACTER_STATE_ON_CEILING = 1;
inline constexpr int32_t LN_CHARACTER_STATE_CROUCHED = 2;

/** Get Navigation Info's Bool::NavigationState (int_value), On Navigation Link's event. */
inline constexpr int32_t LN_NAV_STATE_MOVING = 0;
inline constexpr int32_t LN_NAV_STATE_ARRIVED = 1;
inline constexpr int32_t LN_NAV_STATE_HAS_PATH = 2;
inline constexpr int32_t LN_NAV_STATE_PATH_COMPLETE = 3;
inline constexpr int32_t LN_NAV_STATE_NO_PATH = 4;
inline constexpr int32_t LN_NAV_STATE_ON_LINK = 5;
inline constexpr int32_t LN_NAV_STATE_LINK_STARTED = 6;
inline constexpr int32_t LN_NAV_STATE_ACTIVE = 7;
/** Navigate's When Reached (2026-11-18): it arrived in the last step (KX_NavAgentInfo::reached). */
inline constexpr int32_t LN_NAV_STATE_REACHED = 8;
/** Vector::NavigationState (int_value). */
inline constexpr int32_t LN_NAV_VECTOR_NEXT_CORNER = 0;
inline constexpr int32_t LN_NAV_VECTOR_VELOCITY = 1;
inline constexpr int32_t LN_NAV_VECTOR_GOAL = 2;
/** Float::NavigationState (int_value). */
inline constexpr int32_t LN_NAV_FLOAT_REMAINING = 0;
inline constexpr int32_t LN_NAV_FLOAT_SPEED = 1;

/** Navigate's custom2 bits (2026-11-18). Until Arrived: one pulse is enough, the order holds until
 * the NPC arrives, a new order replaces it or Stop ends it. */
inline constexpr int16_t LN_NAVIGATE_UNTIL_ARRIVED = 1 << 0;

/** Navmesh Query's mode (custom1). */
enum class LN_NavmeshQueryMode : uint8_t {
  FindPath = 0,
  NearestPoint = 1,
  RandomPoint = 2,
  IsReachable = 3,
  WalkableLine = 4,
};

/** Set Navigation Attribute's attribute (custom1). */
enum class LN_NavigationAttribute : uint8_t {
  Obstacle = 0,
  AreaCost = 1,
  AreaClosed = 2,
  RebuildArea = 3,
  /** Navigate's Stop input (2026-11-18; not in the node's dropdown): the object's order ends. */
  Stop = 4,
};

/** Get Character Info's Vector::SnapshotCharacterSurface (int_value). */
inline constexpr int32_t LN_CHARACTER_SURFACE_GROUND_NORMAL = 0;
inline constexpr int32_t LN_CHARACTER_SURFACE_GROUND_VELOCITY = 1;
inline constexpr int32_t LN_CHARACTER_SURFACE_WALL_NORMAL = 2;

enum class LN_DistanceEndpointMode : uint8_t {
  Object = 0,
  Position,
};

enum class LN_AverageCenterSource : uint8_t {
  VectorList = 0,
  ObjectList,
  Collection,
};

enum class LN_MotionSourceMode : int32_t {
  Position = 0,
  OwnerObject,
  Object,
};

enum class LN_VectorOperationMode : uint8_t {
  World = 0,
  Local,
  LocalFromBool,
};

enum class LN_BonePoseLocationSpace : uint8_t {
  ArmatureOffset = 0,
  BoneLocalOffset,
  Armature,
  World,
  PoseChannel,
};

enum class LN_BonePosePositionSpace : uint8_t {
  World = 0,
  Armature,
};

enum class LN_BonePoseRotationSpace : uint8_t {
  /** Native Blender pose-channel rotation in the bone's local basis. */
  PoseChannel = 0,
  /** Evaluated native bone orientation in armature-object space. */
  Armature,
  /** Evaluated native bone orientation after the armature object's world orientation. */
  World,
};

static constexpr uint32_t LN_BONE_ATTRIBUTE_INDEX_MASK = 0xFFFFu;
static constexpr uint32_t LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG = 1u << 16;
/** Get Bone Attribute's Location: its Space (an `LN_BonePoseRotationSpace`, 2 bits) from here. */
static constexpr uint32_t LN_BONE_ATTRIBUTE_LOCATION_SPACE_SHIFT = 17;

enum class LN_VectorOperationChannel : uint8_t {
  Position = 0,
  Orientation,
  Scale,
  LinearVelocity,
  AngularVelocity,
  Movement,
  Rotation,
  Force,
  Torque,
};

static constexpr uint8_t LN_VECTOR_OPERATION_MASK_X = 1 << 0;
static constexpr uint8_t LN_VECTOR_OPERATION_MASK_Y = 1 << 1;
static constexpr uint8_t LN_VECTOR_OPERATION_MASK_Z = 1 << 2;
static constexpr uint8_t LN_VECTOR_OPERATION_MASK_ALL = LN_VECTOR_OPERATION_MASK_X |
                                                        LN_VECTOR_OPERATION_MASK_Y |
                                                        LN_VECTOR_OPERATION_MASK_Z;

enum class LN_MainThreadOnlyReason : uint8_t {
  None = 0,
  QueryExpression,
  CollisionOrPhysicsQuery,
  MouseOverQuery,
  ImmediateCommandHelper,
  ObjectLifecycleCommand,
  GlobalPropertyPersistence,
  FilePersistence,
  PhysicsConstraint,
  CollectionVisibility,
  OverlayCollection,
  InputDeviceState,
  Navigation,
  DebugDraw,
};

enum class LN_SchedulerPurity : uint8_t {
  Pure = 0,
  ReadsSnapshot,
  ReadsInput,
  ReadsQueryCache,
  WritesCommands,
  MainThreadOnly,
};

enum LN_SchedulerResourceAccess : uint32_t {
  LN_SCHEDULER_RESOURCE_NONE = 0u,
  LN_SCHEDULER_RESOURCE_SNAPSHOT = 1u << 0,
  LN_SCHEDULER_RESOURCE_INPUT = 1u << 1,
  LN_SCHEDULER_RESOURCE_QUERY_CACHE = 1u << 2,
  LN_SCHEDULER_RESOURCE_COMMAND_BUFFER = 1u << 3,
  LN_SCHEDULER_RESOURCE_WORLD = 1u << 4,
  LN_SCHEDULER_RESOURCE_SCENE = 1u << 5,
  LN_SCHEDULER_RESOURCE_PHYSICS = 1u << 6,
  LN_SCHEDULER_RESOURCE_RENDER = 1u << 7,
  LN_SCHEDULER_RESOURCE_AUDIO = 1u << 8,
  LN_SCHEDULER_RESOURCE_FILE = 1u << 9,
  LN_SCHEDULER_RESOURCE_GLOBAL_STATE = 1u << 10,
  LN_SCHEDULER_RESOURCE_LOGIC_MANAGER = 1u << 11,
  LN_SCHEDULER_RESOURCE_DATABLOCK = 1u << 12,
};

enum class LN_EstimatedWorkClass : uint8_t {
  Trivial = 0,
  Small,
  Medium,
  Heavy,
};

enum class LN_SchedulerRequiredPhase : uint8_t {
  None = 0,
  OnInit,
  OnUpdate,
  OnFixedUpdate,
};

struct LN_SchedulerSummary {
  LN_SchedulerPurity purity = LN_SchedulerPurity::Pure;
  LN_SchedulerRequiredPhase required_phase = LN_SchedulerRequiredPhase::None;
  uint32_t resource_access = LN_SCHEDULER_RESOURCE_NONE;
  LN_MainThreadOnlyReason main_thread_only_reason = LN_MainThreadOnlyReason::None;
  LN_EstimatedWorkClass estimated_work_class = LN_EstimatedWorkClass::Trivial;
  bool emits_commands = false;
  bool uses_jolt_queries_or_contacts = false;
  bool reads_snapshot_only = true;
  bool worker_lane_eligible = true;
};

enum LN_DependencySnapshotChannel : uint32_t {
  LN_DEP_SNAPSHOT_NONE = 0u,
  LN_DEP_SNAPSHOT_TRANSFORM = 1u << 0,
  LN_DEP_SNAPSHOT_VELOCITY = 1u << 1,
  LN_DEP_SNAPSHOT_COLOR = 1u << 2,
  LN_DEP_SNAPSHOT_LIGHT = 1u << 3,
  LN_DEP_SNAPSHOT_VISIBILITY = 1u << 4,
  LN_DEP_SNAPSHOT_COLLISION = 1u << 5,
  LN_DEP_SNAPSHOT_CHARACTER = 1u << 6,
  LN_DEP_SNAPSHOT_GRAVITY = 1u << 7,
  LN_DEP_SNAPSHOT_TIMING = 1u << 8,
  LN_DEP_SNAPSHOT_GAME_PROPERTY = 1u << 9,
  LN_DEP_SNAPSHOT_TREE_PROPERTY = 1u << 10,
  LN_DEP_SNAPSHOT_OBJECT_GRAPH = 1u << 11,
};

enum LN_DependencyInputChannel : uint32_t {
  LN_DEP_INPUT_NONE = 0u,
  LN_DEP_INPUT_KEYBOARD = 1u << 0,
  LN_DEP_INPUT_MOUSE = 1u << 1,
  LN_DEP_INPUT_WHEEL = 1u << 2,
  LN_DEP_INPUT_TEXT = 1u << 3,
  LN_DEP_INPUT_GAMEPAD_BUTTON = 1u << 4,
  LN_DEP_INPUT_GAMEPAD_AXIS = 1u << 5,
  LN_DEP_INPUT_WINDOW = 1u << 6,
  /** Input Map actions: the manager samples them before the tick's trees run. */
  LN_DEP_INPUT_ACTION = 1u << 7,
  /** Input including what the UI used ("Also when the UI uses it"). */
  LN_DEP_INPUT_RAW = 1u << 8,
};

enum LN_DependencyEventChannel : uint32_t {
  LN_DEP_EVENT_NONE = 0u,
  LN_DEP_EVENT_SUBJECT_READ = 1u << 0,
  LN_DEP_EVENT_CONTENT_READ = 1u << 1,
  LN_DEP_EVENT_MESSENGER_READ = 1u << 2,
  LN_DEP_EVENT_TARGETED_RECEIVE = 1u << 3,
  LN_DEP_EVENT_SUBJECT_WRITE = 1u << 4,
  LN_DEP_EVENT_TARGETED_SEND = 1u << 5,
};

enum LN_DependencyQueryChannel : uint32_t {
  LN_DEP_QUERY_NONE = 0u,
  LN_DEP_QUERY_RAY = 1u << 0,
  LN_DEP_QUERY_PROJECTILE = 1u << 1,
  LN_DEP_QUERY_MOUSE_OVER = 1u << 2,
  LN_DEP_QUERY_COLLISION_CONTACT = 1u << 3,
  LN_DEP_QUERY_RESULT_FIELDS = 1u << 4,
  LN_DEP_QUERY_COLLISION_CONTACT_DETAILS = 1u << 5,
  /** Object UI queries the scene UI service collects once per frame. */
  LN_DEP_QUERY_OBJECT_UI = 1u << 6,
};

enum LN_DependencyCommandClass : uint32_t {
  LN_DEP_COMMAND_NONE = 0u,
  LN_DEP_COMMAND_TRANSFORM = 1u << 0,
  LN_DEP_COMMAND_PHYSICS = 1u << 1,
  LN_DEP_COMMAND_PROPERTY = 1u << 2,
  LN_DEP_COMMAND_EVENT = 1u << 3,
  LN_DEP_COMMAND_AUDIO = 1u << 4,
  LN_DEP_COMMAND_RENDER_DATABLOCK = 1u << 5,
  LN_DEP_COMMAND_SCENE_LIFECYCLE = 1u << 6,
  LN_DEP_COMMAND_FILE_GLOBAL = 1u << 7,
  LN_DEP_COMMAND_RUNTIME_TREE = 1u << 8,
};

enum LN_DependencyAccessClass : uint32_t {
  LN_DEP_ACCESS_NONE = 0u,
  LN_DEP_ACCESS_IMMUTABLE_READ = 1u << 0,
  LN_DEP_ACCESS_SNAPSHOT_READ = 1u << 1,
  LN_DEP_ACCESS_QUERY_CACHE_READ = 1u << 2,
  LN_DEP_ACCESS_COMMAND_WRITE = 1u << 3,
  LN_DEP_ACCESS_IMMEDIATE_MAIN_THREAD_SIDE_EFFECT = 1u << 4,
  LN_DEP_ACCESS_RUNTIME_STATE = 1u << 5,
  LN_DEP_ACCESS_DYNAMIC_UNKNOWN = 1u << 6,
};

enum LN_DependencyDynamicFlag : uint32_t {
  LN_DEP_DYNAMIC_NONE = 0u,
  LN_DEP_DYNAMIC_PROPERTY_NAME = 1u << 0,
  LN_DEP_DYNAMIC_EVENT_SUBJECT = 1u << 1,
  LN_DEP_DYNAMIC_OBJECT_REF = 1u << 2,
  LN_DEP_DYNAMIC_DATABLOCK_REF = 1u << 3,
  LN_DEP_DYNAMIC_FILE_GLOBAL_STATE = 1u << 4,
  LN_DEP_DYNAMIC_GENERIC_VALUE = 1u << 5,
  LN_DEP_DYNAMIC_AMBIGUOUS_SIDE_EFFECT = 1u << 6,
};

enum LN_DependencyStatePreservation : uint32_t {
  LN_DEP_STATE_NONE = 0u,
  LN_DEP_STATE_BOOL_EXPRESSION = 1u << 0,
  LN_DEP_STATE_FLOAT_EXPRESSION = 1u << 1,
  LN_DEP_STATE_TIME_FLOW = 1u << 2,
  LN_DEP_STATE_QUERY_CACHE = 1u << 3,
  LN_DEP_STATE_TREE_PROPERTY = 1u << 4,
  LN_DEP_STATE_LOOP = 1u << 6,
  LN_DEP_STATE_FLOW_CONTROL = 1u << 7,
};

struct LN_ProgramDependencySummary {
  uint32_t snapshot_channels = LN_DEP_SNAPSHOT_NONE;
  uint32_t input_channels = LN_DEP_INPUT_NONE;
  uint32_t event_channels = LN_DEP_EVENT_NONE;
  uint32_t query_channels = LN_DEP_QUERY_NONE;
  uint32_t command_classes = LN_DEP_COMMAND_NONE;
  uint32_t access_classes = LN_DEP_ACCESS_NONE;
  uint32_t dynamic_flags = LN_DEP_DYNAMIC_NONE;
  uint32_t state_preservation = LN_DEP_STATE_NONE;
  uint32_t main_thread_reason_mask = 0;
  uint8_t ray_query_detail_requirements = 0;
  bool worker_lane_eligible = true;
};

enum class LN_ExitAction : uint8_t {
  Quit = 0,
  Restart,
  LoadBlendFile,
};

enum class LN_VehicleAxis : uint8_t {
  Rear = 0,
  Front,
  All,
};

enum class LN_BoolExpressionKind : uint8_t {
  Constant = 0,
  /** Immutable pause state supplied by the scheduler for this tick. */
  GamePaused,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  Not,
  And,
  Or,
  FloatCompare,
  StringContains,
  StringStartsWith,
  StringEndsWith,
  SnapshotVisibility,
  WindowFullscreen,
  NextFrameDone,
  Once,
  ValueChanged,
  ValueChangedTo,
  SnapshotGamePropertyExists,
  InputStatus,
  ValueIsNone,
  KeyboardActive,
  MouseMoved,
  MouseWheelMoved,
  GamepadActive,
  GamepadButton,
  PhysicsQueryDone,
  PhysicsQueryHit,
  PhysicsQueryBlocked,
  PhysicsQueryHasUV,
  PhysicsQueryStartedOverlapping,
  PhysicsQueryAllHit,
  MouseOverEnter,
  MouseOverOver,
  MouseOverExit,
  UIScrolled,
  UIScrollAtStart,
  UIScrollAtEnd,
  UIItemsRebound,
  UICommandCompleted,
  UICommandSucceeded,
  UICommandFailed,
  ActionLifecycleEvent,
  ActionStarted,
  ActionCompleted,
  ActionInterrupted,
  ActionFailed,
  SnapshotCharacterOnGround,
  LogicTreeRunning,
  LogicTreeStopped,
  ActionDone,
  ObjectsColliding,
  EventReceived,
  CollisionDetected,
  CollisionEnter,
  CollisionStay,
  CollisionExit,
  ConstraintBreak,
  AnimationPlaying,
  LoopActive,
  DictHasKey,
  StoreValueDone,
  ListContains,
  InstructionExecuted,
  InstructionReached,
  KeyLoggerPressed,
  FromGenericValue,
  RigidBodyAttribute,
  ValueCompare,
  TweenValue,
  TweenReached,
  TimerElapsed,
  DelayDone,
  PulsifyPulse,
  BarrierPassed,
  MaterialSlotFound,
  MaterialNodeValueFound,
  EditorNodeValueFound,
  RigidBodyConstraintFound,
  BooleanEdge,
  BooleanEdgeFalling,
  CooldownAccepted,
  CooldownBlocked,
  CooldownCompleted,
  CooldownReady,
  PersistenceCompleted,
  PersistenceSucceeded,
  GameLoadPulse,
  GameLoadSucceeded,
  FromInt,
  FromFloat,
  FromString,
  FromVector,
  FromColor,
  FlowGateOpen,
  FlowRouterSelected,
  FlowRouterLastRoute,
  RepeatLimitAccepted,
  RepeatLimitBlocked,
  RepeatLimitExhausted,
  QueryValid,
  MotionValid,
  /** On UI Event: the node's selected event happened to a UI Element this frame. */
  UIElementEventFired,
  /** On UI Event: Drop Enter/Exit, the element takes the dragged item; Drop, a UI Element took
   * it; Drop Received, always. input0 = query. */
  UIElementEventAccepted,
  /** Input Action: input0 user, input1 action name, secondary_int_value input status,
   * bool_value also what the UI used. */
  InputActionState,
  /** Open Object Menu with Toggle: the menu was open, so it closed. input0 = UICommandCompleted. */
  UICommandClosed,
  /** Get Player View: the player's split screen view is on screen. input0 = user (int). */
  PlayerViewShown,
  /** On Player Join: a player joined this frame (LN_Manager). */
  PlayerJoinPulse,
  /** Get Player Gamepad: the player's gamepad is plugged in / the player joined. input0 = user. */
  PlayerGamepadConnected,
  PlayerGamepadJoined,
  /** On UI Sound: the UI's sound this frame is the one of int_value (LN_UIService::UISound).
   * input0 = the node's query (a UIElementEvent query of LN_UIElementEvent::UISound). */
  UISoundFired,
  /** Random Value (Boolean): true with float_expr_index Probability; int_expr_index Seed,
   * int_value the node's identity with bit 0 = Same for All (LN_Random.h). */
  Random,
  /** Get Game Property's Exists of another object (and of its Any option): the object has the
   * property, whatever it holds. input0 = the object (a value; none: the owner), input1 = the
   * name (a string). An Object property holding nothing counts (2026-11-08: Not(ValueIsNone)
   * of the value called it missing, while the owner's typed Exists, the snapshot's, did not). */
  ObjectGamePropertyExists,
  /** Aim for Arc's Can Reach (LN_Ballistics.h): input0 Start, input1 Target, input2 Gravity
   * (vectors), float_expr_index Speed or Arc Height, int_value the mode (LN_ArcSolveMode). */
  ArcCanReach,
  /** Compare's Object type (2026-11-16): input0 A, input1 B (object values, read A then B), each
   * resolved by ResolveObjectValue (a removed object is none, an empty socket is none);
   * float_compare_operation Equal: the same object (none equals none), NotEqual the opposite,
   * any other operation false. */
  ObjectsEqual,
  /** On Action Frame (2026-11-16): the layer's action passed a frame since the step before
   * (LN_ActionFrameCrossed, LN_ActionFrames.h). Keeps its read per tree (int_value the state slot,
   * LN_Program::AcquireActionFrameNode), worked out once a tick. Read order: input0 the object
   * (none: the tree's own), int_expr_index the Layer, float_expr_index the Frame, input1 the
   * Action (a string; empty: any). */
  ActionFrameReached,
  /** Play Sound / Play Sound 3D's Playing (2026-11-17): the sound the node started last plays
   * (LN_GetSoundTicketStatus of the tree's ticket of sound slot int_value). */
  SoundPlaying,
  /** Their Finished: true in the step it is first seen ended (the tree's sound slot int_value;
   * LN_SoundSlotState). */
  SoundFinished,
  /** An input node's Input Type Tap, Double Tap or Hold (2026-11-17, LN_InputGestureStep):
   * input0 the input held, input1 pressed, input2 released (Bool), float_expr_index Time;
   * int_value the tree's input slot (LN_Program::AcquireInputGestureNode), secondary_int_value
   * the LN_InputGesture. Worked out once a tick. Read order: Time, held, pressed, released. */
  InputGesture,
  /** Input Combo's Triggered (2026-11-17, LN_InputComboStep): input0, input1, input2 Steps 1-3,
   * int_expr_index Step 4 (an Int::FromBool: not 0 held), float_expr_index Time; int_value the
   * input slot, secondary_int_value the mode | the linked steps << 4 | Strict << 8. Read order:
   * Time, Steps 1 to 4. */
  InputCombo,
  /** Save Slot's Exists (2026-11-17, LN_GameStateService::SlotExists): int_expr_index Slot,
   * input0 Path (String); whether the slot holds a saved game, a Save Game or a delete asked
   * for it counting at once. Main thread (the save service, the file system). Read order: Slot,
   * Path. */
  SaveSlotExists,
  /** On Removed's Out (2026-11-17): the tree's object was removed and this is its last step
   * (LN_RuntimeTree::IsBeingRemoved, set by LN_Manager's hold before the tick). */
  BeingRemoved,
  /** Get Character Info's On Wall, On Ceiling and Crouched (2026-11-17): int_value the
   * LN_CHARACTER_STATE_*; input0 the object (none: the owner), read from its character channel
   * as On Ground (the character's end of its last step: PHY_ICharacter::OnWall & co). */
  SnapshotCharacterState,
  /* Get Navigation Info / On Navigation Link (2026-11-18): input0 the object (the owner when
   * unset), int_value the LN_NAV_STATE_*; the navigation's last step (KX_NavAgentInfo). */
  NavigationState,
  Count,
};

enum class LN_ThresholdOperation : uint8_t {
  Greater = 0,
  Less,
};

enum class LN_RangeOperation : uint8_t {
  Inside = 0,
  Outside,
};

/** Within Range and Ranged Threshold (2026-11-08): Min and Max in either order (lo = std::min,
 * hi = std::max, as the Minimum / Maximum kinds Within Range's route is built from), both ends
 * inside; Outside is the rest (a NaN value is neither). Before, both ends were neither inside nor
 * outside and a Max below Min was never inside. Folding, the VM and the columns all call this. */
inline bool LN_RangeHolds(const LN_RangeOperation operation,
                          const float value,
                          const float min_value,
                          const float max_value)
{
  const float lo = std::min(min_value, max_value);
  const float hi = std::max(min_value, max_value);
  switch (operation) {
    case LN_RangeOperation::Inside:
      return lo <= value && value <= hi;
    case LN_RangeOperation::Outside:
      return value < lo || value > hi;
  }
  return false;
}

/** A factor applied once a Logic + Physics step (Slow Follow, Rotate To, Align Axis To Vector:
 * "the fraction of the way each fixed step"), for a run covering `steps` steps (a route behind
 * Every N Steps: N): as if applied once each step, 1 - (1 - f)^steps (2026-11-08: such a route
 * followed N times slower, while Translate and Move To keep their speed through Time Passed).
 * One step: the factor as it is. The VM's handlers and the columns' FStepFactor call this. */
inline float LN_StepFactor(const float factor, const uint32_t steps)
{
  if (steps <= 1) {
    return factor;
  }
  const float f = std::clamp(factor, 0.0f, 1.0f);
  return 1.0f - std::pow(1.0f - f, float(steps));
}

enum class LN_FloatCompareOperation : uint8_t {
  Equal = 0,
  NotEqual,
  GreaterThan,
  LessThan,
  GreaterEqual,
  LessEqual,
};

enum class LN_FloatToIntMode : uint8_t {
  Truncate = 0,
  Round,
  Floor,
  Ceil,
};

enum class LN_FloatExpressionKind : uint8_t {
  Constant = 0,
  /** Wall-clock delta represented by the current render-frame update. */
  UnscaledDeltaTime,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  Add,
  Subtract,
  Multiply,
  Divide,
  /** Blender-compatible division: return zero only when the divisor is exactly zero. */
  SafeDivide,
  Power,
  Minimum,
  Maximum,
  Absolute,
  Sign,
  Round,
  Floor,
  Ceil,
  Truncate,
  Fraction,
  Modulo,
  Sine,
  Cosine,
  Radians,
  Degrees,
  Negate,
  Clamp,
  Threshold,
  RangedThreshold,
  VectorComponent,
  ColorComponent,
  /** Random Value (Float): input0 Min, input1 Max, input2 Seed (int), int_value the node's
   * identity with bit 0 = Same for All (LN_Random.h). */
  Random,
  Select,
  StoreValue,
  SnapshotTimeScale,
  SnapshotLightPower,
  SnapshotElapsedTime,
  SnapshotFrameDelta,
  SnapshotFPS,
  SnapshotDeltaFactor,
  AnimationFrame,
  /** Rest-pose bone length from armature data (`Bone::length`). */
  BoneLength,
  /** Coerce a generic value expression to float at evaluation time. */
  FromGenericValue,
  /** Analog trigger strength (LT/RT) for gamepad button nodes. */
  GamepadButtonStrength,
  Formula,
  FloatCurve,
  TweenFactor,
  TweenFloatResult,
  ObjectDistance,
  PhysicsQueryDistance,
  PhysicsQueryFraction,
  PhysicsQueryPenetrationDepth,
  RigidBodyAttribute,
  ConstraintBreakLinearImpulse,
  ConstraintBreakAngularImpulse,
  ConstraintBreakAppliedImpulse,
  CooldownRemaining,
  CooldownProgress,
  QueryDistance,
  QuerySquaredDistance,
  MotionSpeed,
  FromBool,
  FromInt,
  FromString,
  /** Input Action's Value: input0 user, string_expr_index action name, bool_value also what the
   * UI used. */
  InputActionValue,
  /** Get Player View: where the player's view is, in window fractions. input0 = user (int),
   * component_index 0 Left, 1 Top, 2 Width, 3 Height. */
  PlayerViewArea,
  /** On Logic + Physics Step's Time Passed: the game time since its route last ran. int_value =
   * the node's step gate (LN_StepGate), -1 for a node that runs every step. */
  StepTimePassed,
  /** Projectile Ray's Flight Time: the throw's time to the hit, or to where the path ended. */
  PhysicsQueryFlightTime,
  /** Line of Sight's Angle (degrees) between Forward and the direction to the target. */
  PhysicsQueryAngle,
  /** Ray to Plane: the distance along the ray to the plane, -1 when it never meets it. input0 the
   * ray's origin minus a point of the plane, input1 the direction, input2 the plane's normal
   * (vectors; neither needs to be unit length). */
  RayPlaneDistance,
  /** Aim for Arc's Flight Time: as Bool::ArcCanReach, input_indices[0] Speed or Arc Height,
   * bool_expr_index High Arc. */
  ArcFlightTime,
  /** The length of vector input0 (Get Object Attribute's Speed, Velocity at Point's Speed). */
  VectorLength,
  /** Math's newer operations (2026-11-16): int_value the LN_MathFunction, input0 A, input1 B,
   * input2 C (floats; read A, B, C), `LN_MathFunctionValue` (LN_MathOps.h). */
  MathFunction,
  /** Vector Math's number results (2026-11-16): int_value the LN_VectorMeasure, input0 A,
   * input1 B, input2 the Axis (vectors; read A, B, Axis), `LN_VectorMeasureValue`. */
  VectorMeasure,
  /** Smooth Value's number (2026-11-16): keeps its value per tree (int_value the state slot,
   * LN_Program::AcquireSmoothValueNode), component_index the LN_SmoothMode. Read order:
   * bool_expr_index Reset, input0 Target, input1 Speed (Smooth Damp: Smooth Time), input2 Max
   * Speed (Smooth Damp only), input_indices[0] Ignore Timescale; read only on the first
   * evaluation of a tick. `LN_SmoothValueAdvance` (LN_MathOps.h). */
  SmoothValue,
  /** Noise (2026-11-16): `LN_NoiseValue`; int_value LN_NoiseSettings (dimensions, octaves),
   * component_index the channel (a Vector output's axis). Read order: input0 Vector, input1 W,
   * input2 Scale, input_indices[0] Roughness (floats), input_indices[1] Seed (int). */
  Noise,
  /** The game's time (2026-11-17): the scaled time of the Logic + Physics steps run before this
   * tick (LN_TickContext::game_time), the same on the frames between two steps, still while
   * paused: a step's time a step whatever the frames (the frame clock, SnapshotElapsedTime, is
   * not). State Machine's Time in State. */
  GameTime,
  /** List Operation's Min, Max, Sum and Average (2026-11-17, LN_ListStatistic): input0 the List,
   * int_value the LN_ListStatistic; over the numbers in it (whole or decimal), others skipped;
   * none: 0. */
  ListStatistic,
  /** Oscillate's place in its cycle, 0..1 (2026-11-17, LN_OscillateCycle): input0 the time
   * (Float::GameTime), input1 Frequency, input2 Phase, bool_expr_index Random Phase, int_value
   * the node's random identity (the object's own phase: its tree's stream, tick 0). Read order:
   * the time, Frequency, Phase, Random Phase. */
  OscillateCycle,
  /** Oscillate's Value (LN_OscillateWaveValue): input0 the cycle (OscillateCycle), input1
   * Amplitude, input2 Center, int_value the LN_OscillateWave. Center + Amplitude x the wave. Read
   * order: the cycle, Amplitude, Center. */
  Oscillate,
  /** Date and Time's Day Fraction (2026-11-17, LN_DateTime.h): how much of the day has passed
   * at the tick's wall clock (LN_Manager::WallClockSeconds); int_value 1: UTC, 0: local time. */
  DayFraction,
  /* Get Navigation Info's Remaining Distance (2026-11-18): input0 the object, int_value
   * LN_NAV_FLOAT_*. */
  NavigationState,
  Count,
};

/** Blender CurveMapping uses 256 uniform intervals; retain its 257 samples without resampling. */
inline constexpr int LN_CURVE_SAMPLE_COUNT = 257;

/** Plain game-runtime representation of one compiled CurveMapping channel. */
struct LN_CurveTable {
  std::array<float, LN_CURVE_SAMPLE_COUNT> samples{};
  float domain_min = 0.0f;
  float domain_max = 1.0f;
  float sample_scale = float(LN_CURVE_SAMPLE_COUNT - 1);
  float table_min_x = 0.0f;
  float table_max_x = 1.0f;
  float extend_in_x = 1.0f;
  float extend_in_y = 1.0f;
  float extend_out_x = 1.0f;
  float extend_out_y = 1.0f;
  float clip_min = 0.0f;
  float clip_max = 1.0f;
  bool extrapolate = true;
  bool clip_output = true;
};

enum class LN_IntExpressionKind : uint8_t {
  Constant = 0,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  StringCount,
  SnapshotCollisionGroup,
  MouseWheelDelta,
  WindowResolutionWidth,
  WindowResolutionHeight,
  WindowVSyncMode,
  SnapshotCharacterMaxJumps,
  SnapshotCharacterJumpCount,
  ListLength,
  LoopIndex,
  DictLength,
  FromGenericValue,
  /** Random Value (Integer): input0 Min, input1 Max (both included), input2 Seed, int_value the
   * node's identity with bit 0 = Same for All (LN_Random.h). */
  Random,
  MaterialSlotCount,
  CollisionContactCount,
  PhysicsQueryFaceIndex,
  PhysicsQueryHitCount,
  PhysicsQueryRayCount,
  FromBool,
  FromFloat,
  FromString,
  ConstraintBreakCount,
  RepeatLimitRemaining,
  QueryVectorCount,
  UICommandResult,
  UICommandHandle,
  UIFirstVisibleIndex,
  UILastVisibleIndex,
  /** On UI Event: the grid index of the element and of its target (-1: not a grid slot), and the
   * player it happened for. input0 = query. */
  UIElementEventIndex,
  UIElementEventTargetIndex,
  UIElementEventUser,
  /** On UI Event: how many of the item (ElementEvent::amount). input0 = query. */
  UIElementEventAmount,
  /** Object Grid View: the page in view, counting from 1, and how many pages there are (at least
   * 1). input0 = query. */
  UIViewPage,
  UIViewPageCount,
  /** Get Player View: how many player views are on screen. */
  PlayerViewCount,
  /** On Player Join: the player who joined and their gamepad (LN_Manager). */
  PlayerJoinUser,
  PlayerJoinGamepad,
  /** Get Player Gamepad: the player's gamepad number, -1 for none. input0 = user. */
  PlayerGamepad,
  /** Get Menu Player: the player whose menu the tree's object is in (0 outside menus). */
  MenuPlayerUser,
  /** Set Random Seed's Seed output: the game's random seed now (LN_GetRandomGameSeed). */
  GameRandomSeed,
  /** Get Random List Item's Index (2026-11-16): the place Value::ListRandomItem of the same
   * fields picks (input0 List, input1 Seed, input2 Weights, int_value the identity), -1 when it
   * picks none. */
  ListRandomIndex,
  /** String Operation's Length (2026-11-17, LN_TextLength): input0 the text (String). */
  TextLength,
  /** String Operation's Find (2026-11-17, LN_TextFind): input0 the text, input1 the search
   * (String), -1 when absent. Read order: the search, the text. */
  TextFind,
  /** List Operation's Index for Min and Max (2026-11-17): where the first smallest (largest)
   * number of input0 is, -1 without numbers; int_value the LN_ListStatistic. */
  ListStatisticIndex,
  /** Input Combo's Progress (2026-11-17): input0 its Bool::InputCombo (worked out first), the
   * steps matched (Sequence) or held (Chord). */
  InputComboProgress,
  /** Date and Time's whole numbers (2026-11-17, LN_DateTime.h) at the tick's wall clock
   * (LN_Manager::WallClockSeconds): int_value the LN_DateTimePart | UTC << 8. */
  DateTimePart,
  Count,
};

enum class LN_StringExpressionKind : uint8_t {
  Constant = 0,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  Join,
  Replace,
  ToUppercase,
  ToLowercase,
  ZeroFill,
  Format,
  AnimationActionName,
  KeyLoggerCharacter,
  KeyLoggerKeycode,
  FromGenericValue,
  MasterFolder,
  ObjectID,
  MaterialName,
  RigidBodyConstraintName,
  ConstraintBreakName,
  FromBool,
  FromInt,
  FromFloat,
  FromVector,
  FromColor,
  /** On UI Event: the Action name of the UI Element the event happened to. */
  UIElementEventAction,
  /** On UI Event: the Action name of the target when it is a UI Element (empty otherwise). */
  UIElementEventTargetAction,
  /** On UI Event: what the Grid View's Inventory did with the event (empty: nothing). */
  UIElementEventOutcome,
  /** Get Input Binding: input0 action name, input1 device class and input2 direction (constant
   * int expressions). */
  InputBindingText,
  /** On UI Sound: the name of the menu (its collection) the sound came from, empty outside
   * menus. input0 = the node's query. */
  UIElementEventMenu,
  /** Hit Material: the name of the material on the hit face (the slot on the Hit Part's object;
   * slot 0 for shapes without triangles), empty without a hit or a material. input0 = query. */
  PhysicsQueryMaterial,
  /** String Operation's Substring (2026-11-17, LN_TextSubstring): input0 the text (String),
   * input1 Start and input2 Length (Int). Read order: Length, Start, the text. */
  TextSubstring,
  /** String Operation's Format Time (2026-11-17, LN_FormatTime): input0 Seconds (Float), input1
   * Round Up (Bool), input2 the LN_TimeFormat (a constant Int). Read order: the format, Round Up,
   * Seconds. */
  TextFormatTime,
  /* Get Navigation Info / On Navigation Link's Link Kind (2026-11-18): input0 the object. */
  NavigationLinkKind,
  Count,
};

enum class LN_VectorExpressionKind : uint8_t {
  Constant = 0,
  SnapshotGravity,
  WindowResolution,
  WorldToScreen,
  /** Rest-pose bone `arm_head` in world space (KX object transform × armature-space head). */
  BoneHeadWorld,
  /** Rest-pose bone `arm_tail` in world space. */
  BoneTailWorld,
  /** Rest-pose bone center (midpoint of head/tail) in world space. */
  BoneCenterWorld,
  /** Posed bone head in world space (after BKE_pose_where_is). */
  BoneHeadPoseWorld,
  /** Posed bone tail in world space (after BKE_pose_where_is). */
  BoneTailPoseWorld,
  /** Posed bone center (midpoint of pose head/tail) in world space. */
  BoneCenterPoseWorld,
  /** Pose bone location channel or evaluated pose head in the selected transform space. */
  BonePoseLocation,
  /** Native Blender pose-channel scale in the bone's local basis. */
  BonePoseScale,
  ScreenToWorld,
  Add,
  Subtract,
  Multiply,
  Divide,
  Absolute,
  Minimum,
  Maximum,
  Scale,
  Normalize,
  Resize,
  RotateAroundAxis,
  VectorToRotation,
  /** Convert native Blender bone orientation (+Y along the bone) to object +Z along the bone. */
  BoneRotationToObjectRotation,
  AxisVector,
  Combine,
  RuntimeTreeProperty,
  FromGenericValue,
  CursorPosition,
  CursorMovement,
  GamepadStick,
  PhysicsQueryPoint,
  PhysicsQueryNormal,
  PhysicsQueryCastPosition,
  PhysicsQueryDirection,
  PhysicsQueryEndPoint,
  PhysicsQueryUV,
  UIScrollOffset,
  UIScrollNormalized,
  SnapshotCharacterGravity,
  SnapshotCharacterWalkDirection,
  SnapshotCharacterLocalWalkDirection,
  CollisionHitPoint,
  CollisionHitNormal,
  ConstraintBreakAnchorPosition,
  EvaluateCurveAtFactor,
  InstructionNextPoint,
  /** Random Value (Vector): input0 Min, input1 Max (vectors), input2 Seed (int), int_value the
   * node's identity with bit 0 = Same for All (LN_Random.h). */
  Random,
  TweenVectorResult,
  TweenRotationResult,
  QueryVectorAverage,
  MotionPosition,
  MotionVelocity,
  MotionDirection,
  MotionPredictedPosition,
  RigidBodyAttribute,
  BroadcastFloat,
  BroadcastInt,
  FromColor,
  /** Input Action's Vector: input0 user, input1 action name, bool_value also what the UI
   * used. */
  InputActionVector,
  /** Mouse Ray / Camera Ray's Ray Origin: where the cast started (input0 = query). */
  PhysicsQueryRayOrigin,
  /** Projectile Ray's Impact Velocity: the throw's velocity at the hit, or where the path ended. */
  PhysicsQueryImpactVelocity,
  /** Ray to Plane's Point: input0 the ray's origin, input1 its direction, float_expr_index the
   * distance (Float::RayPlaneDistance); zero without a hit. */
  RayPlanePoint,
  /** Ray to Plane's Normal: input0 the ray's direction, input1 the plane's normal,
   * float_expr_index the distance; the unit normal facing the ray's start, zero without a hit. */
  RayPlaneNormal,
  /** Aim for Arc's Launch Velocity: as Bool::ArcCanReach, bool_expr_index High Arc. */
  ArcLaunchVelocity,
  /** Get Game Property Vector of the owner (2026-11-15): the snapshot's kept vector of the
   * game property ref `property_ref_index` (LN_Snapshot::ReadGameProperty), as a read of another
   * object converts it (LN_GamePropertyAsVector: a Color's red, green and blue, a number in all
   * three); the ref's default (zero) when the object has no such property or it gives no
   * vector. */
  SnapshotGameProperty,
  /** Vector Math's newer vector results (2026-11-16): int_value the LN_VectorFunction, input0 A,
   * input1 B (vectors), float_expr_index Scale with float_value its fallback (read A, B, Scale),
   * `LN_VectorFunctionValue` (LN_MathOps.h). */
  VectorFunction,
  /** Rotation Math and Transform Vector (2026-11-16): int_value the LN_RotationFunction. The
   * rotation operations (output_type Rotation): input0 A, input1 B (rotations, read A then B),
   * `LN_RotationFunctionRotation`; the vector ones: input0 the vector, input1 the rotation,
   * input2 the scale (read in that order), `LN_RotationFunctionVector` (LN_MathOps.h). */
  RotationFunction,
  /** Smooth Value's vector (2026-11-16): as Float::SmoothValue, property_ref_index the
   * LN_SmoothMode (set explicitly). Read order: bool_expr_index Reset, input0 Target (vector),
   * float_expr_index Speed (Smooth Damp: Smooth Time), input1 Max Speed (a float, Smooth Damp
   * only), input2 Ignore Timescale (a bool). */
  SmoothValue,
  /** Random Value's shapes (2026-11-16): property_ref_index the LN_RandomShape (set
   * explicitly), int_value the node's identity (LN_Random.h, as Random). Read order: input0 Axis
   * (In Cone), float_expr_index Radius or Angle, input2 Seed (an int).
   * `LN_RandomShapeVector`. */
  RandomShape,
  /** Oscillate's Bob and Orbit (2026-11-17, LN_OscillateBob / LN_OscillateOrbit): input0 the
   * cycle (Float::OscillateCycle), input1 Axis, float_expr_index Amplitude; int_value the
   * LN_OscillateWave, LN_OSCILLATE_ORBIT for Orbit. Read order: the cycle, Axis, Amplitude. */
  Oscillate,
  /** Get Character Info's Ground Normal, Ground Velocity and Wall Normal (2026-11-17), world
   * space: int_value the LN_CHARACTER_SURFACE_*; input0 the object (none: the owner), read from
   * its character channel. */
  SnapshotCharacterSurface,
  /* Get Navigation Info's vectors (2026-11-18): input0 the object, int_value LN_NAV_VECTOR_*. */
  NavigationState,
  Count,
};

enum class LN_ColorExpressionKind : uint8_t {
  Constant = 0,
  SnapshotObjectColor,
  SnapshotLightColor,
  RuntimeTreeProperty,
  Combine,
  FromGenericValue,
  /** Get Game Property Color of the owner (2026-11-15): the snapshot's kept color of the game
   * property ref `property_ref_index`, as a read of another object converts it
   * (LN_GamePropertyAsColor: a Vector with alpha 1); the ref's default when the object has no
   * such property or it gives no color. */
  SnapshotGameProperty,
  Count,
};

enum class LN_ValueExpressionKind : uint8_t {
  Constant = 0,
  RuntimeTreeProperty,
  Mix,
  FromBool,
  FromInt,
  FromFloat,
  FromString,
  FromVector,
  FromColor,
  FromRotation,
  CombineVector4,
  ResizeVectorValue,
  EulerToMatrix,
  MatrixToEuler,
  ActiveCamera,
  OwnerObject,
  ObjectByName,
  ObjectParent,
  ObjectChild,
  ObjectChildByName,
  PhysicsQueryObject,
  PhysicsQueryObjects,
  PhysicsQueryPoints,
  PhysicsQueryNormals,
  PhysicsQueryDistances,
  PhysicsQueryCastPositions,
  PhysicsQueryFractions,
  PhysicsQueryPenetrationDepths,
  PhysicsQueryStartedOverlappingList,
  PhysicsQueryFaceIndices,
  PhysicsQueryHasUVs,
  PhysicsQueryUVs,
  PhysicsQueryHits,
  PhysicsQueryBlockedList,
  PhysicsQueryRayOrigins,
  PhysicsQueryRayDirections,
  PhysicsQueryEndPoints,
  EventContent,
  EventMessenger,
  CollisionHitObject,
  CollisionHitObjects,
  CollisionHitPoints,
  CollisionHitNormals,
  ConstraintBreakTargetObject,
  ConstraintBreakEvents,
  ObjectAttribute,
  BoneAttribute,
  MaterialSlot,
  MaterialNodeValue,
  EditorNodeValue,
  ObjectGameProperty,
  LoopCurrentValue,
  ListElement,
  MakeList,
  ListExtend,
  DictGetKey,
  MakeDict,
  DictGetKeys,
  EmptyList,
  EmptyDict,
  ListDuplicate,
  DictMerge,
  ListAppend,
  ListRemoveIndex,
  ListRemoveValue,
  ListSetIndex,
  DictSetKey,
  DictRemoveKey,
  DictRemoveKeyValue,
  ListFromItems,
  GetGlobalProperty,
  ListGlobalProperties,
  LoadVariable,
  LoadVariableDict,
  ListSavedVariables,
  CurrentScene,
  CollectionObjects,
  CollectionObjectNames,
  RigidBodyConstraintNames,
  ValueChangedOld,
  ValueChangedNew,
  /** Get Random List Item: input0 List, input1 Seed (int), value.int_value the node's identity
   * with bit 0 = Same for All (LN_Random.h). input2 Weights (a list, 2026-11-16; none: every
   * item equally likely): item i's chance is weight i over the positive weights' sum
   * (LN_RandomWeightedIndex), an item past the weights' end never comes out. */
  ListRandomItem,
  ValueSwitchList,
  ValueSwitchListCompare,
  ProjectileParabola,
  BonePoseRotation,
  UIFocusObject,
  /** On UI Event: the UI Element, and a Drop's target (UI Element or game-world object). */
  UIElementEventObject,
  UIElementEventTarget,
  /** On UI Event: the grid item the element shows, and the one its target shows. */
  UIElementEventItem,
  UIElementEventTargetItem,
  /** List Sort: input0 list, input1 key (string), input2 descending. */
  ListSort,
  /** List Find Index: input0 list, input1 value. */
  ListFindIndex,
  /** Add to Dictionary Value: input0 dictionary, input1 key (string), input2 amount. */
  DictAddValue,
  /** Inventory node: input0 inventory, input_indices LN_InventoryInput, value.int_value the
   * LN_InventoryOperation. InventoryEdit gives the new list, InventoryAmount the leftover or
   * removed amount (InventoryChanged: the Removed output). */
  InventoryEdit,
  InventoryAmount,
  /** Item Count: input0 inventory, input1 item. */
  InventoryItemCount,
  /** Get Item Info: input0 library collection, input1 item, value.int_value LN_ItemInfoField. */
  ItemLibraryInfo,
  /** Inventory Move To Slot / Craft: the item a recipe made, and what happened (strings). */
  InventoryMade,
  InventoryOutcome,
  /** Get Recipe Info: input0 recipe text, input1 inventory, value.int_value LN_RecipeInfoField. */
  RecipeInfo,
  /** Find Recipe: input0 recipes, input1 items, input2 station, value.int_value
   * LN_RecipeFindField. */
  RecipeFind,
  /** Get Player View: the camera the player sees through. input0 = user (int). */
  PlayerViewCamera,
  /** Get Menu Player: whose menu the tree's object is in (LN_UIService::MenuOwnerOfObject). */
  MenuPlayerOwner,
  /** Inventory node's Removed output (as InventoryAmount): how many it removed or crafted, or,
   * Add Item, how many went in (its label Added). */
  InventoryChanged,
  /** Hit Part: the compound child whose shape was hit, else the Hit Object (input0 = query). */
  PhysicsQueryPart,
  /** Hit Parts: as PhysicsQueryPart for every hit of a list query. */
  PhysicsQueryParts,
  /** Hit Materials: as String::PhysicsQueryMaterial for every hit of a list query. */
  PhysicsQueryMaterials,
  /** Line of Sight's Blocker: the object a query's `blocked` result names (none otherwise). */
  PhysicsQueryBlocker,
  /** Velocity at Point's Velocities: the `impact_velocity` of every result of a list query (as
   * PhysicsQueryPoints lists their `hit_position`). */
  PhysicsQueryImpactVelocities,
  /** String Operation's Split (2026-11-17, LN_TextSplit): a List of Strings; input0 the text,
   * input1 the separator (String). Read order: the separator, the text. */
  TextSplit,
  /** Insert's Output Only (2026-11-17): input0 the List, input1 the Index (Int), input2 the
   * Value; the list with the value inserted (LN_ListInsertPlace). Read order: the list, the
   * index, the value (as Set List Index). */
  ListInsert,
  /** List Operation's Reverse (2026-11-17): input0 the List. */
  ListReverse,
  /** List Operation's Shuffle (2026-11-17): input0 the List, input1 the Seed (Int), value.int_value
   * the node's random identity (Same for All bit); a new order every step (frame), repeatable as
   * every random node (game seed, node, step). Read order: the seed, the list. */
  ListShuffle,
  /** List Operation's Slice (2026-11-17): input0 the List, input1 Start, input2 Length (Int), as
   * Substring (LN_ClampSlice). Read order: Length, Start, the list. */
  ListSlice,
  /** Get Children (2026-11-17): input0 the parent (none: the owner), input1 All Descendants
   * (Bool); the children in the scene graph's order, all descendants depth first. Read order:
   * All Descendants, the parent. */
  ObjectChildren,
  /** Explode or Implode Impulse's Strengths (2026-11-17, LN_ExplosionFalloff): input0 its Objects
   * query, input1 Radius, input2 Strength (Float); value.bool_value the Linear falloff. For each
   * of the query's objects: Strength x the falloff from the blast's center (each result's
   * cast_position: the cast has no length) to the object's center of mass, as the push at the
   * flush. Read order: Strength, Radius, the query. */
  ExplosionStrengths,
  /** Get Character Info's Ground Object (2026-11-17): what the character stands on while On
   * Ground (its character channel's character_ground_object), else None; input0 the object
   * (none: the owner). */
  SnapshotCharacterGroundObject,
  /* Get Navigation Info / On Navigation Link's Link Object (2026-11-18): input0 the object. */
  NavigationLinkObject,
  Count,
};

enum class LN_InventoryOperation : int32_t {
  AddItem = 0,
  RemoveItem,
  RemoveFromSlot,
  SwapSlots,
  SplitStack,
  Sort,
  MoveToSlot,
  Craft,
};

/** Order of LN_ValueExpression::input_indices for the Inventory node. */
enum LN_InventoryInput : uint32_t {
  LN_INVENTORY_INPUT_ITEM = 0,
  LN_INVENTORY_INPUT_AMOUNT,
  LN_INVENTORY_INPUT_MAX_STACK,
  LN_INVENTORY_INPUT_STACK,
  LN_INVENTORY_INPUT_CAPACITY,
  LN_INVENTORY_INPUT_SLOT,
  LN_INVENTORY_INPUT_OTHER_SLOT,
  LN_INVENTORY_INPUT_BY_COUNT,
  LN_INVENTORY_INPUT_DESCENDING,
  LN_INVENTORY_INPUT_RECIPES,
  LN_INVENTORY_INPUT_RECIPE,
  LN_INVENTORY_INPUT_LIBRARY,
  LN_INVENTORY_INPUT_SWAP,
  /** Move To Slot into another inventory: its object and game property. */
  LN_INVENTORY_INPUT_TO_OBJECT,
  LN_INVENTORY_INPUT_TO_PROPERTY,
  /** An emptied slot stays an empty place (LN_InventoryLayout::KeepPlaces). */
  LN_INVENTORY_INPUT_KEEP_PLACES,
  /** Override: the node's Max Stack, Stack, Capacity, Recipes, Library and Keep Places instead of
   * the list's Inventory Rules. */
  LN_INVENTORY_INPUT_OVERRIDE,
  LN_INVENTORY_INPUT_COUNT,
};

/** InventoryEdit `input1`: an empty Library means the scene's Item Library. */
constexpr uint32_t LN_INVENTORY_SCENE_LIBRARY = 1u;

enum class LN_ItemInfoField : int32_t {
  Found = 0,
  Object,
  Name,
  MaxStack,
  Category,
  Description,
};

enum class LN_RecipeInfoField : int32_t {
  Valid = 0,
  Result,
  Count,
  Ingredients,
  Station,
  CanCraft,
  Results,
  Craftable,
};

enum class LN_RecipeFindField : int32_t {
  Found = 0,
  Recipe,
  Index,
};

enum class LN_QueryExpressionKind : uint8_t {
  Raycast = 0,
  RaycastAll,
  RaycastFromMesh,
  ShapeCast,
  ShapeCastAll,
  MouseOver,
  ObjectScrollView,
  ObjectGridView,
  MouseRay,
  CameraRay,
  ProjectileRay,
  VectorAverage,
  DistanceBetween,
  /** On UI Event: one UI Element event selected by type and optional Action name. */
  UIElementEvent,
  /** Line of Sight: whether an eye sees a target (LN_RuntimeTree::LineOfSightKind). */
  LineOfSight,
  /** Sweep Object: the object's own collision shape moved to a destination (SweepObjectKind). */
  SweepObject,
  /** Keep Distance's outputs: what its last check found (int_value the node's state slot), read
   * from the tree; no physics query of its own. */
  KeepDistance,
  /** Velocity at Point (2026-11-14, LN_RuntimeTree::PointVelocityKind): input0 the object (owner
   * when unset), int_value the LN_PointVelocitySource, Position: input1 the world point; Vertex
   * Group: string_expr_index the group, secondary_int_value the LN_PointVelocityPick. Result:
   * physics_query.hit_position the point, impact_velocity its velocity (hit: a body moves it);
   * Each Vertex: one result per vertex in physics_query_results. */
  PointVelocity,
  /** Find Objects (2026-11-16, LN_FindObjects.h): the tick's LN_FindIndex searched around a
   * center. int_value LN_FindSettings (source, pick, the program's find source), secondary_int_value
   * Random's identity (LN_Random.h), bool_value: Count is read. Read order: input0 the center
   * object (none: the tree's own), input1 the List, float_expr_index Radius,
   * secondary_float_expr_index the View Angle (radians, the cone's whole width), bool_expr_index
   * Exclude Self. Result: physics_query hit / object_ref / hit_position / hit_distance the pick,
   * vector_count the count, physics_query_results the list (All). */
  FindObjects,
  /* Navmesh Query (2026-11-18): input0 the navmesh object (unset: the scene's own), input1 From /
   * Position, input2 To, float_expr_index Radius; int_value the LN_NavmeshQueryMode,
   * secondary_int_value the node's random identity. Result: hit (found, reachable, clear),
   * hit_position the point, hit_normal a wall's normal, hit_distance the length; Find Path's
   * corners one result each (hit_position). */
  NavmeshQuery,
  Count,
};

enum class LN_PointVelocitySource : uint8_t {
  Position = 0,
  VertexGroup,
};

enum class LN_PointVelocityPick : uint8_t {
  Center = 0,
  Fastest,
  EachVertex,
};

enum class LN_MeshRayDomain : uint8_t {
  Vertices = 0,
  Faces,
};

enum class LN_MeshRayDirection : uint8_t {
  Normals = 0,
  InvertedNormals,
};

enum class LN_ShapeCastType : uint8_t {
  Sphere = 0,
  Box,
  Capsule,
};

enum LN_RayQueryDetail : uint8_t {
  LN_RAY_QUERY_DETAIL_NONE = 0,
  LN_RAY_QUERY_DETAIL_FACE_INDEX = 1 << 0,
  LN_RAY_QUERY_DETAIL_UV = 1 << 1,
  /** Hit Part: the compound child hit (PHY_RAY_QUERY_DETAIL_PART; no data kept). */
  LN_RAY_QUERY_DETAIL_PART = 1 << 2,
  /** Hit Material: the hit triangle's slot (PHY_RAY_QUERY_DETAIL_MATERIAL; triangle meshes keep
   * their slots when some program asks). */
  LN_RAY_QUERY_DETAIL_MATERIAL = 1 << 3,
};

enum class LN_RayInputMode : uint8_t {
  EndPoint = 0,
  DirectionDistance,
};

/** Render/interaction view used by cursor-driven queries. */
enum class LN_InteractionView : uint8_t {
  Automatic = 0,
  Main,
  Overlay,
  CustomCamera,
};

/** Runtime object set eligible to block a pointer query. */
enum class LN_InteractionDomain : uint8_t {
  Any = 0,
  Main,
  Overlay,
};

/** How an object menu affects interaction with layers beneath it. */
enum class LN_UIMenuRouting : uint8_t {
  Modal = 0,
  PassThrough,
};

/** How a new object menu is inserted into the active navigation stack. */
enum class LN_UIMenuOpenMode : uint8_t {
  Push = 0,
  ReplaceTop,
};

/** Whether a covering menu hides or keeps the preceding session visible. */
enum class LN_UIMenuPresentation : uint8_t {
  Screen = 0,
  Overlay,
};

/** What semantic Cancel/Back does while this menu is topmost. */
enum class LN_UIMenuBackBehavior : uint8_t {
  CloseMenu = 0,
  Ignore,
  Custom,
};

/** Which menu sessions Close Object Menu affects. */
enum class LN_UIMenuCloseOperation : uint8_t {
  Top = 0,
  Specific,
  All,
};

/** Open Object Menu options, next to the open mode in the packed settings' first byte (and in
 * the node's custom2): pause the game while the menu is open; close the menu when it is open. */
inline constexpr int32_t LN_UI_MENU_PAUSES_GAME = 1 << 4;
inline constexpr int32_t LN_UI_MENU_TOGGLES = 1 << 5;
/** Area Player's View: shown in the menu's player's split screen view (rna_nodetree.cc uses the
 * same node bit). */
inline constexpr int32_t LN_UI_MENU_PLAYER_VIEW = 1 << 6;
inline constexpr int16_t LN_UI_MENU_NODE_PAUSES_GAME = 1 << 6;
inline constexpr int16_t LN_UI_MENU_NODE_TOGGLES = 1 << 7;
inline constexpr int16_t LN_UI_MENU_NODE_PLAYER_VIEW = 1 << 8;

inline constexpr int32_t LN_PackUIMenuOpenSettings(const LN_UIMenuOpenMode mode,
                                                   const LN_UIMenuPresentation presentation,
                                                   const LN_UIMenuBackBehavior back,
                                                   const bool pauses_game = false,
                                                   const bool toggles = false,
                                                   const bool in_player_view = false)
{
  return int32_t(uint32_t(mode) & 0x0Fu) | (pauses_game ? LN_UI_MENU_PAUSES_GAME : 0) |
         (toggles ? LN_UI_MENU_TOGGLES : 0) | (in_player_view ? LN_UI_MENU_PLAYER_VIEW : 0) |
         (int32_t(uint32_t(presentation) & 0xFFu) << 8) |
         (int32_t(uint32_t(back) & 0xFFu) << 16);
}

inline constexpr bool LN_UnpackUIMenuInPlayerView(const int32_t packed)
{
  return (packed & LN_UI_MENU_PLAYER_VIEW) != 0;
}

inline constexpr LN_UIMenuOpenMode LN_UnpackUIMenuOpenMode(const int32_t packed)
{
  return LN_UIMenuOpenMode(packed & 0x0F);
}

inline constexpr bool LN_UnpackUIMenuPausesGame(const int32_t packed)
{
  return (packed & LN_UI_MENU_PAUSES_GAME) != 0;
}

inline constexpr bool LN_UnpackUIMenuToggles(const int32_t packed)
{
  return (packed & LN_UI_MENU_TOGGLES) != 0;
}

inline constexpr LN_UIMenuPresentation LN_UnpackUIMenuPresentation(const int32_t packed)
{
  return LN_UIMenuPresentation((packed >> 8) & 0xFF);
}

inline constexpr LN_UIMenuBackBehavior LN_UnpackUIMenuBackBehavior(const int32_t packed)
{
  return LN_UIMenuBackBehavior((packed >> 16) & 0xFF);
}

/** The player a menu command is for, in the top byte of the packed open settings (Open Object
 * Menu) or above the close operation (Close Object Menu). */
inline constexpr int32_t LN_PackUIMenuUser(const int32_t packed, const int32_t user, const int shift)
{
  return packed | (int32_t(uint32_t(user) & 0xFFu) << shift);
}

inline constexpr int32_t LN_UnpackUIMenuUser(const int32_t packed, const int shift)
{
  return (packed >> shift) & 0xFF;
}

inline constexpr int16_t LN_PackUIMenuNodeSettings(const LN_UIMenuOpenMode mode,
                                                   const LN_UIMenuPresentation presentation,
                                                   const LN_UIMenuBackBehavior back)
{
  return int16_t((int(mode) & 3) | ((int(presentation) & 3) << 2) | ((int(back) & 3) << 4));
}

inline constexpr LN_UIMenuOpenMode LN_UnpackUIMenuNodeOpenMode(const int16_t packed)
{
  return LN_UIMenuOpenMode(packed & 3);
}

inline constexpr LN_UIMenuPresentation LN_UnpackUIMenuNodePresentation(const int16_t packed)
{
  return LN_UIMenuPresentation((packed >> 2) & 3);
}

inline constexpr LN_UIMenuBackBehavior LN_UnpackUIMenuNodeBackBehavior(const int16_t packed)
{
  return LN_UIMenuBackBehavior((packed >> 4) & 3);
}

/** How one interactable affects candidates behind it in the same pointer view. */
enum class LN_UIPropagation : uint8_t {
  Block = 0,
  PassThrough,
  Ignore,
};

/** Semantic role of one registered object in the centralized UI router. */
enum class LN_UIInteractableRole : uint8_t {
  Control = 0,
  DragSource,
};

/** What happened to a UI Element, as selected on the On UI Event node (stable values). */
enum class LN_UIElementEvent : uint8_t {
  Click = 0,
  Press,
  Release,
  Enter,
  Exit,
  Focus,
  FocusLost,
  DragStart,
  Drag,
  Drop,
  DragCancel,
  /** Secondary: a right click, or its key or gamepad control on the focused element. */
  SecondaryClick,
  /** On a Drop Target: an element was dropped on it. The event's target is the dragged one. */
  DropReceived,
  /** On the dragged element: let go over the game world, where no element takes it (also sent
   * as Drop). Let go over other UI or over nothing, it is a Drag Cancel instead. */
  DropOutside,
  /** A player's highlight moved to it: under player 0's mouse, or the focus a keyboard or gamepad
   * player sees; also when the grid item it shows changed. Sent with an empty object when the
   * highlight leaves the elements of an Action. */
  Highlight,
  /** A widget (Toggle, Slider, Selector) changed its game property; the event's item is the new
   * value. */
  Changed,
  /** On a Drop Target: a dragged element came over it (also one it refuses: see Accepted). The
   * event's target is the dragged one. */
  DropEnter,
  /** On a Drop Target: the dragged element is no longer over it, for whatever reason: it moved
   * away, was dropped (also on this one), was put back or cancelled. */
  DropExit,
  /** It became highlighted, as its Highlight Object shows it (some player's highlight: under player
   * 0's mouse or the focus a keyboard or gamepad player sees; not while it refuses a mouse drag),
   * and it stopped being so: once each, to start and end an effect such as an animation. */
  HighlightStart,
  HighlightEnd,
  /** A dragged item over it is refused by its Accept Categories, as its Refused Object shows it,
   * and it no longer is. The event's target is the dragged element. */
  RefusedStart,
  RefusedEnd,
  /** A Grid View slot holding an item is used: the grid's Use Action pressed while it is the
   * player's highlight, or one of its Use Keys (the slot's place in the grid). */
  Use,
  /** A Grid View slot now shows another item, or its item changed (its count or own values), or
   * it emptied: the event's item is what it shows. At most kMaxEventsPerNodeFrame a frame, the
   * rest the next frames. */
  ItemShown,
  /** Not an element's event: an On UI Sound query, which reads the frame's UI sounds
   * (LN_UIService::FindSoundEvent) instead of its element events. */
  UISound,
};

/** UI Element panel Widget setting (matches DNA eObjectGameUIWidget). */
enum class LN_UIWidget : uint8_t {
  None = 0,
  Toggle,
  Slider,
  Selector,
  Text,
  /** Shows a number game property as a fill that grows along the element; takes no input. */
  Bar,
};

/** UI Element tooltip Placement (matches DNA eObjectGameUITooltipMode). */
enum class LN_UITooltipMode : uint8_t {
  /** Shown where it is while pointed at. */
  InPlace = 0,
  /** Moved next to the element by its offset, shown while pointed at. */
  Beside,
  /** Only its text changes, emptied when nothing is pointed at. */
  TextOnly,
};

/** UI Element panel Focus setting (matches DNA eObjectGameUIFocus). */
enum class LN_UIElementFocus : uint8_t {
  All = 0,
  Click,
  None,
};

/** UI Element panel Screen Anchor setting (matches DNA eObjectGameUIAnchor). */
enum class LN_UIElementAnchor : uint8_t {
  None = 0,
  TopLeft,
  Top,
  TopRight,
  Left,
  Center,
  Right,
  BottomLeft,
  Bottom,
  BottomRight,
};

/** Device-independent actions consumed by object menus. */
/** The menus' UI controls, in Input Map order (GAME_INPUT_BUILTIN_* - 1). */
enum class LN_UIAction : uint8_t {
  NavigateUp = 0,
  NavigateDown,
  NavigateLeft,
  NavigateRight,
  Accept,
  /** Back. */
  Cancel,
  Next,
  Previous,
  Secondary,
  ScrollUp,
  ScrollDown,
  /** Picks up the focused draggable element and puts it down on the focused drop target. */
  Grab,
};
inline constexpr int LN_UI_ACTION_COUNT = int(LN_UIAction::Grab) + 1;

enum class LN_UIActionState : uint8_t {
  Triggered = 0,
  Active,
  Released,
};

/** Stable result codes returned by deferred object-menu commands. */
enum class LN_UIResult : int32_t {
  Success = 0,
  NoChange,
  InvalidHandle,
  InvalidOwner,
  InvalidCamera,
  InvalidCollection,
  WrongScene,
  CameraUnavailable,
  CameraConflict,
  CollectionUnavailable,
  CollectionConflict,
  RuntimeUnavailable,
  InstantiationFailed,
  InvalidTarget,
  SessionConflict,
  AlreadyOpen,
  HandleExhausted,
  StaleOwner,
  /** Open Object Menu with Toggle: the menu was open, so it was closed instead. */
  Closed,
  /** Open Object Menu with Toggle: the menu is open for another player, who closes it. */
  OpenForOtherPlayer,
  /** Take UI Control: another player has the UI. */
  ControlledByOtherPlayer,
};

const char *LN_UIResultMessage(LN_UIResult result);

/** One event emitted by the exact action request started by a Play Action instruction. */
enum class LN_ActionResult : uint8_t {
  Started = 0,
  Completed,
  Interrupted,
  Failed,
};

enum LN_ActionOutputMask : uint8_t {
  LN_ACTION_OUTPUT_NONE = 0u,
  LN_ACTION_OUTPUT_STARTED = 1u << 0u,
  LN_ACTION_OUTPUT_COMPLETED = 1u << 1u,
  LN_ACTION_OUTPUT_INTERRUPTED = 1u << 2u,
  LN_ACTION_OUTPUT_FAILED = 1u << 3u,
  LN_ACTION_OUTPUT_ALL = LN_ACTION_OUTPUT_STARTED | LN_ACTION_OUTPUT_COMPLETED |
                         LN_ACTION_OUTPUT_INTERRUPTED | LN_ACTION_OUTPUT_FAILED,
};

/** What Set Action Frame's instruction sets (its secondary_int_value; the command's
 * animation_flags): the frame (every Set Action Frame), or Set Action Layer's speed or layer
 * weight (2026-11-16). */
enum : int32_t {
  LN_ACTION_LAYER_SET_FRAME = 0,
  LN_ACTION_LAYER_SET_SPEED = 1,
  LN_ACTION_LAYER_SET_WEIGHT = 2,
};

enum class LN_ValueType : uint8_t {
  None = 0,
  Bool,
  Int,
  Float,
  Vector,
  Vector4,
  Matrix,
  Color,
  Rotation,
  String,
  ObjectRef,
  SceneRef,
  CollectionRef,
  DatablockRef,
  List,
  Dict,
  Generic,
};

enum class LN_RuntimeRefKind : uint8_t {
  None = 0,
  Object,
  Scene,
  Collection,
  Datablock,
  PhysicsController,
};

struct LN_RuntimeRef {
  LN_RuntimeRefKind kind = LN_RuntimeRefKind::None;
  uint32_t slot = LN_INVALID_INDEX;
  uint32_t generation = 0;
  /** Zero: `slot` indexes the issuing runtime tree's table. Nonzero: `slot`/`generation` are a
   * scene-wide object handle of the LN_Manager with this id, used for values stored where other
   * trees read them (global properties, event content, UI payloads). */
  uint32_t shared_domain = 0;
  std::string debug_name;

  bool IsValid() const
  {
    return kind != LN_RuntimeRefKind::None && slot != LN_INVALID_INDEX && generation != 0;
  }
};

enum class LN_QueryDiagnosticStatus : uint8_t {
  NotEvaluated = 0,
  Hit,
  NoHit,
  Disabled,
  InvalidTarget,
  InvalidFilter,
  MissingPhysicsWorld,
  UnsupportedPhysicsBackend,
  UnavailableUnsnapshottedData,
};

struct LN_PhysicsQueryResult {
  bool hit = false;
  LN_QueryDiagnosticStatus diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
  LN_RuntimeRef object_ref;
  MT_Vector3 hit_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 cast_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 ray_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 ray_origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 end_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector2 hit_uv = MT_Vector2(0.0f, 0.0f);
  float hit_fraction = 0.0f;
  float hit_distance = 0.0f;
  float penetration_depth = 0.0f;
  int32_t polygon_index = -1;
  /** Hit Part: the compound child hit (LN_RAY_QUERY_DETAIL_PART), invalid when the hit object
   * itself is the part (PhysicsQueryPart then gives object_ref). */
  LN_RuntimeRef part_ref;
  /** Projectile Ray: the throw's velocity and time at the hit (or where its path ended). */
  MT_Vector3 impact_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  float flight_time = 0.0f;
  /** Line of Sight: the angle (degrees) between the eye's forward and the target. */
  float angle = 0.0f;
  /** Hit Material: the hit triangle's slot on the part's mesh (LN_RAY_QUERY_DETAIL_MATERIAL),
   * -1 for shapes without triangles (slot 0 then). */
  int16_t material_slot = -1;
  bool has_uv = false;
  bool blocked = false;
  bool started_overlapping = false;
  std::vector<MT_Vector3> parabola_points;
};

struct LN_QueryResult {
  LN_PhysicsQueryResult physics_query;
  std::vector<LN_PhysicsQueryResult> physics_query_results;
  MT_Vector3 vector_average = MT_Vector3(0.0f, 0.0f, 0.0f);
  float distance = 0.0f;
  float squared_distance = 0.0f;
  int32_t vector_count = 0;
  bool valid = false;
  bool over = false;
  bool entered = false;
  bool exited = false;
  bool ui_scrolled = false;
  bool ui_scroll_at_start = true;
  bool ui_scroll_at_end = true;
  bool ui_items_rebound = false;
  int32_t ui_first_visible_index = -1;
  int32_t ui_last_visible_index = -1;
  MT_Vector3 ui_scroll_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 ui_scroll_normalized = MT_Vector3(0.0f, 0.0f, 0.0f);
  LN_RuntimeRef ui_source_object;
  LN_RuntimeRef ui_target_object;
  /** On UI Event: Action name of the element the event happened to, and of its target. */
  std::string ui_action;
  std::string ui_target_action;
  /** On UI Event: what the Grid View's Inventory did with the event (empty: nothing). */
  std::string ui_outcome;
  /** On UI Event: the grid items of the element and of its target, their indices (-1: not a grid
   * slot) and the player. */
  std::shared_ptr<LN_Value> ui_item;
  std::shared_ptr<LN_Value> ui_target_item;
  int32_t ui_index = -1;
  int32_t ui_target_index = -1;
  int32_t ui_user = 0;
  int32_t ui_amount = 0;
  /** On UI Event: see LN_BoolExpressionKind::UIElementEventAccepted. */
  bool ui_accepted = false;
  /** On UI Event: how many events this frame match the node (its route runs once for each). */
  uint32_t ui_event_count = 0;
  /** On UI Sound: which sound (LN_UIService::UISound), and the menu it came from. */
  int32_t ui_sound = -1;
  std::string ui_menu;
  /** Object Grid View: the page in view (from 1) and the page count. */
  int32_t ui_page = 1;
  int32_t ui_page_count = 1;
};

enum class LN_CompileSeverity : uint8_t {
  Info = 0,
  Warning,
  Error,
};

struct LN_SourceRef {
  int32_t source_node_identifier = -1;
  std::string node_idname;
  std::string node_name;
  std::string socket_name;
  std::string source_tree_name;
  std::string source_tree_library_path;
};

struct LN_GroupCallFrame {
  std::string group_name;
  std::string source_tree_name;
  std::string interface_key;
  uint32_t input_value_count = 0;
  uint32_t output_value_count = 0;
  bool owns_state = false;
};

enum class LN_LoopKind : uint8_t {
  Count = 0,
  FromList,
};

struct LN_LoopFrame {
  LN_LoopKind kind = LN_LoopKind::Count;
  uint32_t trigger_bool_expr_index = LN_INVALID_INDEX;
  uint32_t count_int_expr_index = LN_INVALID_INDEX;
  uint32_t list_value_expr_index = LN_INVALID_INDEX;
  uint32_t ticks_to_complete_int_expr_index = LN_INVALID_INDEX;
  uint32_t loop_active_bool_expr_index = LN_INVALID_INDEX;
  uint32_t loop_index_int_expr_index = LN_INVALID_INDEX;
  uint32_t loop_current_value_expr_index = LN_INVALID_INDEX;
  /** Bit per LN_Event whose incoming flow starts the frame. Body routes mirrored into another
   * event (On Init into Fixed Update) only resume an active frame there. */
  uint8_t start_event_mask = 0;
};

enum class LN_InstructionPayloadKind : uint8_t {
  None = 0,
  Transform,
  Velocity,
  Physics,
  Property,
  Scene,
  Audio,
  Event,
  Persistence,
  Navigation,
  Render,
  Generic,
};

/** Instruction flags. A UI event route starts at a UI event (On UI Event, a Grid or Scroll View's
 * events): it keeps running while the game is paused, like UI does in other engines, while the
 * rest of a paused tree waits. */
constexpr uint16_t LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE = 1u << 0;
/** The first instruction of a Loop body copy (one per incoming path): it starts its own block. */
constexpr uint16_t LN_INSTRUCTION_FLAG_LOOP_BODY_START = 1u << 1;

struct LN_InstructionHeader {
  LN_OpCode opcode = LN_OpCode::Nop;
  uint32_t source_ref_index = 0;
  uint32_t payload_index = LN_INVALID_INDEX;
  uint16_t flags = 0;
};

struct LN_Instruction {
  LN_OpCode opcode = LN_OpCode::Nop;
  uint32_t source_ref_index = 0;
  LN_InstructionPayloadKind payload_kind = LN_InstructionPayloadKind::None;
  uint32_t command_payload_index = LN_INVALID_INDEX;
  uint16_t flags = 0;
  uint8_t vector_operation_mode = uint8_t(LN_VectorOperationMode::World);
  uint8_t vector_operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  uint8_t vector_operation_mask = LN_VECTOR_OPERATION_MASK_ALL;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  uint32_t vector_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_vector_expr_index = LN_INVALID_INDEX;
  uint32_t bool_guard_expr_index = LN_INVALID_INDEX;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_int_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_int_expr_index = LN_INVALID_INDEX;
  uint32_t float_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_float_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_float_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_float_expr_index = LN_INVALID_INDEX;
  uint32_t string_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_string_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_string_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  uint32_t color_expr_index = LN_INVALID_INDEX;
  uint32_t value_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_value_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_value_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_value_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t loop_frame_index = LN_INVALID_INDEX;
  /**
   * A prerequisite Loop frame that must complete in the current outer execution scope before
   * this instruction can run. This keeps a Loop's Completed route ordered after every body
   * iteration without turning completion into a lossy boolean pulse.
   */
  uint32_t completed_loop_frame_index = LN_INVALID_INDEX;
  /**
   * The route's repeat key: its instructions run again, inside the same tick, for each further
   * event of the node that started it. Either an On UI Event's (or On UI Sound's) query runtime
   * state, or, with LN_COLLISION_REPEAT_ROUTE_BIT set, an On Collision node's Each Object event
   * (LN_CollisionRepeatRoute: one run per object that entered, stayed or left). Every user of
   * repeat keys (the walk's repeats, Loop restarts, the refusals of compiled routes, wide runs and
   * tree batches) treats both alike; only the walk's repeat count and cursor differ.
   */
  uint32_t ui_event_state_index = LN_INVALID_INDEX;
  /** Part of a route of On Logic + Physics Step's Every N Steps: the program's LN_StepGate it runs
   * behind (only on that gate's steps, with the time passed since its last run). Continuations
   * (a latent node's Done...) start fresh routes that run whenever they fire. */
  uint32_t step_gate_index = LN_INVALID_INDEX;
  LN_ValueType property_value_type = LN_ValueType::None;
  int32_t int_value = 0;
  int32_t secondary_int_value = 0;
  bool bool_value = false;
};

struct LN_VectorCommandPayload {
  uint32_t object_value_expr_index = LN_INVALID_INDEX;
  uint32_t vector_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_vector_expr_index = LN_INVALID_INDEX;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  uint8_t operation_mode = uint8_t(LN_VectorOperationMode::World);
  uint8_t operation_channel = uint8_t(LN_VectorOperationChannel::Position);
  uint8_t operation_mask = LN_VECTOR_OPERATION_MASK_ALL;
  bool bool_value = false;
};

/** Modify Property operation. It is applied to the live property value when commands flush, so
 * repeated modifications in one tick (loops, several trees) all take effect. */
enum class LN_PropertyModifyOp : uint8_t {
  None = 0,
  Add,
  Subtract,
  Multiply,
  Divide,
  Power,
  Modulo,
  FloorDivide,
  /** Toggle Property: negate the live boolean value (no operand). */
  Toggle,
};

struct LN_PropertyModify {
  LN_PropertyModifyOp op = LN_PropertyModifyOp::None;
  bool clamped = false;
  float operand = 0.0f;
  float min_value = 0.0f;
  float max_value = 0.0f;
};

/** The list and dictionary nodes' changes (node custom1 LN_COLLECTION_STORE_GAME_PROPERTY):
 * applied to the list or dictionary a game property holds when commands flush, in the order
 * they ran, so two changes in one step both take effect (LN_ApplyGamePropertyEdit). */
enum class LN_PropertyEditOp : uint8_t {
  ListAppend = 0,
  ListRemoveIndex,
  ListRemoveValue,
  ListSetIndex,
  DictSetKey,
  DictAddValue,
  DictRemoveKey,
  /** Insert (2026-11-17): the value at the index (below 0 counts from the end; past the end:
   * at the end), as Python's list.insert. */
  ListInsert,
};

/** The list and dictionary nodes' custom1: 0 gives the changed copy at the output (every node
 * saved before 2026-11-08), 1 changes the game property (new nodes). */
inline constexpr int16_t LN_COLLECTION_STORE_OUTPUT_ONLY = 0;
inline constexpr int16_t LN_COLLECTION_STORE_GAME_PROPERTY = 1;

inline constexpr bool LN_PropertyEditIsList(const LN_PropertyEditOp op)
{
  return op == LN_PropertyEditOp::ListAppend || op == LN_PropertyEditOp::ListRemoveIndex ||
         op == LN_PropertyEditOp::ListRemoveValue || op == LN_PropertyEditOp::ListSetIndex ||
         op == LN_PropertyEditOp::ListInsert;
}

struct LN_GamePropertyCommandPayload {
  uint32_t object_value_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  uint32_t value_expr_index = LN_INVALID_INDEX;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t float_expr_index = LN_INVALID_INDEX;
  uint32_t string_expr_index = LN_INVALID_INDEX;
  uint32_t vector_expr_index = LN_INVALID_INDEX;
  uint32_t color_expr_index = LN_INVALID_INDEX;
  LN_ValueType value_type = LN_ValueType::None;
  /** Modify/Toggle Property: `float_expr_index` is the operand; min/max are set when clamped. */
  LN_PropertyModifyOp modify_op = LN_PropertyModifyOp::None;
  uint32_t modify_min_expr_index = LN_INVALID_INDEX;
  uint32_t modify_max_expr_index = LN_INVALID_INDEX;
};

enum class LN_RigidBodyConstraintBoolInput : uint8_t {
  UseWorldSpace = 0,
  Enabled,
  DisableCollisions,
  Breakable,
  OverrideIterations,
  UseLimitLinX,
  UseLimitLinY,
  UseLimitLinZ,
  UseLimitAngX,
  UseLimitAngY,
  UseLimitAngZ,
  UseSpringX,
  UseSpringY,
  UseSpringZ,
  UseSpringAngX,
  UseSpringAngY,
  UseSpringAngZ,
  UseMotorLin,
  UseMotorAng,
  Count,
};

enum class LN_RigidBodyConstraintVectorInput : uint8_t {
  Pivot = 0,
  Rotation,
  LinearLower,
  LinearUpper,
  AngularLower,
  AngularUpper,
  SpringStiffness,
  SpringDamping,
  AngularSpringStiffness,
  AngularSpringDamping,
  Count,
};

enum class LN_RigidBodyConstraintFloatInput : uint8_t {
  BreakingThreshold = 0,
  MotorLinTargetVelocity,
  MotorLinMaxImpulse,
  MotorAngTargetVelocity,
  MotorAngMaxImpulse,
  Count,
};

static constexpr size_t LN_RIGID_BODY_CONSTRAINT_BOOL_INPUT_COUNT = size_t(
    LN_RigidBodyConstraintBoolInput::Count);
static constexpr size_t LN_RIGID_BODY_CONSTRAINT_VECTOR_INPUT_COUNT = size_t(
    LN_RigidBodyConstraintVectorInput::Count);
static constexpr size_t LN_RIGID_BODY_CONSTRAINT_FLOAT_INPUT_COUNT = size_t(
    LN_RigidBodyConstraintFloatInput::Count);

struct LN_RigidBodyConstraintCommandPayload {
  uint32_t constraint_object_value_expr_index = LN_INVALID_INDEX;
  uint32_t object_value_expr_index = LN_INVALID_INDEX;
  uint32_t target_value_expr_index = LN_INVALID_INDEX;
  uint32_t name_string_expr_index = LN_INVALID_INDEX;
  uint32_t velocity_solver_iterations_int_expr_index = LN_INVALID_INDEX;
  uint32_t position_solver_iterations_int_expr_index = LN_INVALID_INDEX;
  std::array<uint32_t, LN_RIGID_BODY_CONSTRAINT_BOOL_INPUT_COUNT> bool_expr_indices;
  std::array<uint32_t, LN_RIGID_BODY_CONSTRAINT_VECTOR_INPUT_COUNT> vector_expr_indices;
  std::array<uint32_t, LN_RIGID_BODY_CONSTRAINT_FLOAT_INPUT_COUNT> float_expr_indices;
  PHY_RigidBodyConstraintType type = PHY_RigidBodyConstraintType::Point;
  PHY_RigidBodyConstraintSpringType spring_type = PHY_RigidBodyConstraintSpringType::Spring2;

  LN_RigidBodyConstraintCommandPayload()
  {
    bool_expr_indices.fill(LN_INVALID_INDEX);
    vector_expr_indices.fill(LN_INVALID_INDEX);
    float_expr_indices.fill(LN_INVALID_INDEX);
  }
};

struct LN_BoolExpression {
  LN_BoolExpressionKind kind = LN_BoolExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t float_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_float_expr_index = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  LN_FloatCompareOperation float_compare_operation = LN_FloatCompareOperation::Equal;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  float float_value = 0.0f;
  float secondary_float_value = 0.0f;
  int32_t int_value = 0;
  int32_t secondary_int_value = 0;
  std::string string_value;
  bool bool_value = false;
};

struct LN_FloatExpression {
  LN_FloatExpressionKind kind = LN_FloatExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  std::vector<uint32_t> input_indices;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t string_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  int32_t int_value = 0;
  LN_ThresholdOperation threshold_operation = LN_ThresholdOperation::Greater;
  LN_RangeOperation range_operation = LN_RangeOperation::Inside;
  float float_value = 0.0f;
  bool bool_value = false;
  uint8_t component_index = 0;
};

struct LN_IntExpression {
  LN_IntExpressionKind kind = LN_IntExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  int32_t int_value = 0;
  LN_FloatToIntMode float_to_int_mode = LN_FloatToIntMode::Truncate;
};

struct LN_StringExpression {
  LN_StringExpressionKind kind = LN_StringExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t input3 = LN_INVALID_INDEX;
  uint32_t input4 = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  uint32_t value_expr_index = LN_INVALID_INDEX;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  LN_StringId string_id;
  std::string string_value;
};

struct LN_VectorExpression {
  LN_VectorExpressionKind kind = LN_VectorExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t float_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  LN_Rotation rotation_value;
  LN_ValueType output_type = LN_ValueType::Vector;
  float float_value = 0.0f;
  /** Random: the node's identity (LN_Random.h). */
  int32_t int_value = 0;
  bool bool_value = false;
};

struct LN_QueryExpression {
  LN_QueryExpressionKind kind = LN_QueryExpressionKind::Raycast;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t input3 = LN_INVALID_INDEX;
  uint32_t input4 = LN_INVALID_INDEX;
  uint32_t input5 = LN_INVALID_INDEX;
  uint32_t input6 = LN_INVALID_INDEX;
  uint32_t input7 = LN_INVALID_INDEX;
  uint32_t input8 = LN_INVALID_INDEX;
  uint32_t input9 = LN_INVALID_INDEX;
  uint32_t ignore_list_expr_index = LN_INVALID_INDEX;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t quinary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t senary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t condition_bool_expr_index = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_int_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_int_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_int_expr_index = LN_INVALID_INDEX;
  uint32_t float_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_float_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_float_expr_index = LN_INVALID_INDEX;
  uint32_t string_expr_index = LN_INVALID_INDEX;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 tertiary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 quaternary_vector_value = MT_Vector3(0.5f, 0.5f, 0.5f);
  float float_value = 0.0f;
  float secondary_float_value = 0.0f;
  float tertiary_float_value = 0.0f;
  int32_t int_value = 0;
  int32_t secondary_int_value = 0;
  LN_RayInputMode ray_input_mode = LN_RayInputMode::EndPoint;
  LN_InteractionView interaction_view = LN_InteractionView::Automatic;
  LN_InteractionDomain interaction_domain = LN_InteractionDomain::Any;
  /** Object Grid View: items fill columns and the grid scrolls by columns. */
  bool grid_horizontal = false;
  LN_UIPropagation ui_propagation = LN_UIPropagation::Block;
  uint8_t ray_query_detail_flags = LN_RAY_QUERY_DETAIL_NONE;
  LN_ShapeCastType shape_cast_type = LN_ShapeCastType::Sphere;
  LN_MeshRayDomain mesh_ray_domain = LN_MeshRayDomain::Vertices;
  LN_MeshRayDirection mesh_ray_direction = LN_MeshRayDirection::Normals;
  LN_DistanceEndpointMode distance_endpoint_a = LN_DistanceEndpointMode::Object;
  LN_DistanceEndpointMode distance_endpoint_b = LN_DistanceEndpointMode::Object;
  LN_AverageCenterSource average_center_source = LN_AverageCenterSource::VectorList;
  int32_t cache_key = -1;
  uint32_t runtime_state_index = LN_INVALID_INDEX;
  bool bool_value = false;
  /** Object Grid View: its list and Inventory inputs, in LN_GridViewInput order (each of the
   * kind LN_GridViewInputKind gives); empty in programs of other nodes. */
  std::vector<uint32_t> input_indices;
};

/** Order of LN_QueryExpression::input_indices for Object Grid View. */
enum LN_GridViewInput : uint32_t {
  /** The list: a game property of an object (empty Object: whose menu it is). */
  LN_GRID_INPUT_OBJECT = 0,
  LN_GRID_INPUT_PROPERTY,
  /** Moving Items: the grid moves, stacks, combines and drops its items itself, by the list's
   * Inventory Rules. */
  LN_GRID_INPUT_HANDLE_MOVES,
  LN_GRID_INPUT_SPLIT_ACTION,
  LN_GRID_INPUT_SPLIT_AMOUNT,
  LN_GRID_INPUT_SWAP,
  LN_GRID_INPUT_DROP_INTO_WORLD,
  LN_GRID_INPUT_DROP_AT_POINTER,
  LN_GRID_INPUT_DROP_IN_FRONT,
  LN_GRID_INPUT_DISTANCE,
  LN_GRID_INPUT_HOVER_HEIGHT,
  LN_GRID_INPUT_HINT_TEXT,
  LN_GRID_INPUT_ACCEPT_HINT,
  LN_GRID_INPUT_REFUSE_HINT,
  LN_GRID_INPUT_SHOW_CATEGORIES,
  LN_GRID_INPUT_SEARCH,
  LN_GRID_INPUT_USE_ACTION,
  LN_GRID_INPUT_USE_KEYS,
  LN_GRID_INPUT_COUNT,
};

/** Which expression list an LN_GridViewInput indexes. */
enum class LN_GridViewInputKind : uint8_t { Value, Bool, Int, Float, String };
constexpr LN_GridViewInputKind LN_GridViewInputKindOf(const uint32_t input)
{
  switch (input) {
    case LN_GRID_INPUT_OBJECT:
    case LN_GRID_INPUT_HINT_TEXT:
      return LN_GridViewInputKind::Value;
    case LN_GRID_INPUT_HANDLE_MOVES:
    case LN_GRID_INPUT_SWAP:
    case LN_GRID_INPUT_DROP_INTO_WORLD:
    case LN_GRID_INPUT_DROP_AT_POINTER:
    case LN_GRID_INPUT_DROP_IN_FRONT:
      return LN_GridViewInputKind::Bool;
    case LN_GRID_INPUT_SPLIT_AMOUNT:
      return LN_GridViewInputKind::Int;
    case LN_GRID_INPUT_DISTANCE:
    case LN_GRID_INPUT_HOVER_HEIGHT:
      return LN_GridViewInputKind::Float;
    default:
      return LN_GridViewInputKind::String;
  }
}

struct LN_ColorExpression {
  LN_ColorExpressionKind kind = LN_ColorExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  uint32_t input3 = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
};

struct LN_Value {
  LN_ValueType type = LN_ValueType::None;
  bool exists = false;
  bool bool_value = false;
  /**
   * `vector_value` is also the optional authored-Euler compatibility hint for Rotation values.
   * The quaternion remains authoritative for all orientation math. This flag lets generic graph
   * boundaries preserve legacy radians (including full turns) without enlarging the hot
   * LN_Rotation type or converting back from a quaternion unnecessarily.
   */
  bool rotation_euler_hint_valid = false;
  int32_t int_value = 0;
  float float_value = 0.0f;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector4 vector4_value = MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 matrix_value = MT_Matrix3x3::Identity();
  MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  LN_Rotation rotation_value;
  LN_RuntimeRef runtime_ref;
  /* No physics controller handle or query result here (2026-10-31): nothing ever read or wrote
   * them, and they were 240 of the value's 544 bytes, paid by every value made or copied. Query
   * results live in LN_QueryResult. */
  std::vector<LN_Value> list_value;
  std::map<std::string, LN_Value> dict_value;
  std::string reference_name;
  std::string string_value;

  MT_Vector3 RotationEulerRadians() const
  {
    return rotation_euler_hint_valid ? vector_value : rotation_value.ToEulerXYZ();
  }

  MT_Vector3 RotationEulerDegrees() const
  {
    /* Degrees are the user-facing representation used by the Print node and editor-style
     * diagnostics. Runtime math and graph conversion boundaries remain quaternion/radian based. */
    constexpr float radians_to_degrees = 57.295779513082320876f;
    return RotationEulerRadians() * radians_to_degrees;
  }

  void SetRotation(const LN_Rotation &rotation)
  {
    rotation_value = rotation;
    vector_value = rotation.ToEulerXYZ();
    rotation_euler_hint_valid = true;
  }

  void SetRotation(const LN_Rotation &rotation, const MT_Vector3 &euler_radians)
  {
    rotation_value = rotation;
    vector_value = euler_radians;
    rotation_euler_hint_valid = true;
  }
};

/**
 * A number written to a property without a whole LN_Value (Set Game Property Bool, Int or
 * Float): `type` and the matching field. Its value (ToValue) is the one those nodes made before:
 * the default value with `exists`, `type` and that field set. None: no number.
 */
struct LN_PropertyNumber {
  LN_ValueType type = LN_ValueType::None;
  bool bool_value = false;
  int32_t int_value = 0;
  float float_value = 0.0f;

  bool IsNumber() const
  {
    return type == LN_ValueType::Bool || type == LN_ValueType::Int || type == LN_ValueType::Float;
  }

  LN_Value ToValue() const
  {
    LN_Value value;
    if (!IsNumber()) {
      return value;
    }
    value.exists = true;
    value.type = type;
    switch (type) {
      case LN_ValueType::Bool:
        value.bool_value = bool_value;
        break;
      case LN_ValueType::Int:
        value.int_value = int_value;
        break;
      default:
        value.float_value = float_value;
        break;
    }
    return value;
  }
};

/** The attributes an ObjectAttribute value expression reads by name (plan "Attribute and
 * Retirement", step G2), as numbers. Worked out once per program from the expression's constant
 * text (LN_RegisterExpressionProgram::value_object_attribute); None: not an object attribute, a
 * text that is not constant, or one that names no attribute (a game property). */
enum class LN_ObjectAttributeId : uint8_t {
  None = 0,
  WorldPosition,
  LocalPosition,
  WorldOrientation,
  LocalOrientation,
  WorldScale,
  LocalScale,
  WorldLinearVelocity,
  LocalLinearVelocity,
  WorldAngularVelocity,
  LocalAngularVelocity,
  Color,
  Visible,
  Name,
};

/** The attribute a name stands for, exactly the names the ObjectAttribute kind knows ("position"
 * is World Position); None for any other text. */
LN_ObjectAttributeId LN_ObjectAttributeIdFromName(const std::string &name);

struct LN_ValueExpression {
  LN_ValueExpressionKind kind = LN_ValueExpressionKind::Constant;
  uint32_t input0 = LN_INVALID_INDEX;
  uint32_t input1 = LN_INVALID_INDEX;
  uint32_t input2 = LN_INVALID_INDEX;
  std::vector<uint32_t> input_indices;
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t string_expr_index = LN_INVALID_INDEX;
  uint32_t property_ref_index = LN_INVALID_INDEX;
  LN_RigidBodyConstraintMatchMode rigid_body_constraint_match_mode =
      LN_RigidBodyConstraintMatchMode::Exact;
  LN_Value value;
};

struct LN_GamePropertyRef {
  LN_StringId name_id;
  std::string name;
  LN_ValueType value_type = LN_ValueType::None;
  LN_Value default_value;
};

/** What a program's game property refs start as (plan A10): each ref's default value with its type
 * set, and the place of each String ref's live text in a snapshot. Made once for a program and
 * shared by the snapshots of all its trees (LN_Program::GetGamePropertyLayout). */
struct LN_GamePropertyLayout {
  std::vector<LN_Value> defaults;
  std::vector<uint32_t> string_slot;
  uint32_t string_slots = 0;
  /** A Vector or Color ref's place in the snapshot's kept vectors (2026-11-15), else
   * LN_INVALID_INDEX: the cells stay small for every other ref. */
  std::vector<uint32_t> vector_slot;
  uint32_t vector_slots = 0;
};

struct LN_TreePropertyRef {
  LN_StringId name_id;
  std::string name;
  LN_ValueType value_type = LN_ValueType::None;
  LN_Value default_value;
};

struct LN_Constant {
  uint32_t source_ref_index = 0;
  LN_Value value;
};

class LN_FindIndex;

struct LN_TickContext {
  LN_Event event = LN_Event::OnFixedUpdate;
  double current_time = 0.0;
  /** Scaled delta for the current phase: render-frame delta on Update, fixed step otherwise. */
  double delta_time = 0.0;
  /** Wall-clock delta represented by the current phase, used by unscaled time-flow nodes. */
  double unscaled_dt = 0.0;
  bool use_fixed_timestep = false;
  /** Global game-pause state captured before scheduling this update. */
  bool game_paused = false;
  /** The players whose keys, gamepad or mouse the UI uses, one bit each, as the last UI frame
   * left them (LN_UIService::UIUsesPlayerInput): Mouse Look and Gamepad Look wait for them. */
  uint8_t ui_input_users = 0;
  /** Player 0's mouse is not the game's now: their UI uses it, or the window has no focus. */
  bool mouse_look_waits = false;
  /** Counts the times Mouse Look had to start waiting or the game paused: Mouse Look starts over
   * (no jump) when it changed since its last run. */
  uint64_t mouse_ui_serial = 0;
  LN_CommandBuffer *command_buffer = nullptr;
  uint64_t tick_index = 0;
  uint64_t phase_tick_index = 0;
  /** Logic + Physics steps run before this tick (in a step tick, its phase_tick_index). On Init's
   * random numbers count steps with it, whether the tree's first tick is a frame or a step: a
   * copy added in step N rolls the same numbers at any frame rate (2026-11-07). */
  uint64_t steps_done = 0;
  /** Steps between two runs of the route being executed: 1, or N inside a route of On Logic +
   * Physics Step's Every N Steps. Nodes that need their flow on consecutive runs (Pulsify,
   * Barrier, an on-demand Tween) count a run this many steps later as the next one. */
  uint32_t phase_tick_stride = 1;
  /** Monotonic command-execution point used by command-ordered query caches. */
  uint64_t command_execution_key = UINT64_MAX;
  uint64_t execution_scope_serial = 0;
  /** Find Objects' poses and grids of this tick (2026-11-16; LN_Manager and the test rigs set
   * it before the trees record; null: Find Objects finds nothing). */
  const LN_FindIndex *find_index = nullptr;
  /** The scaled time of the Logic + Physics steps run before this tick (2026-11-17; the sum of
   * their delta_time): Float::GameTime. */
  double game_time = 0.0;
};
