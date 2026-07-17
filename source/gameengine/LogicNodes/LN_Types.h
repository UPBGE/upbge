/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Types.h
 *  \ingroup logicnodes
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "MT_Matrix3x3.h"
#include "MT_Vector4.h"
#include "MT_Vector3.h"
#include "../Physics/Common/PHY_RigidBodyConstraintSettings.h"

class LN_CommandBuffer;

/**
 * Compiled Logic Nodes compatibility policy:
 * - Bump #LN_PROGRAM_VERSION for opcode layout changes, opcode semantic changes, bytecode
 *   encoding changes, or runtime-state layout changes.
 * - Bump #LN_PROGRAM_SCHEMA_VERSION for node id, socket id, or schema changes that alter
 *   compiled output.
 * - Compiled programs are reusable only when version, schema, build flags, source tree name,
 *   source library path, and source checksum all match the current runtime/source graph.
 */
static constexpr uint32_t LN_PROGRAM_VERSION = 30;
static constexpr uint32_t LN_PROGRAM_SCHEMA_VERSION = 21;
static constexpr uint32_t LN_INVALID_INDEX = UINT32_MAX;

struct LN_StringId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

enum class LN_Event : uint8_t {
  OnInit = 0,
  OnFixedUpdate,
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
  StopAllSounds,
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
  MoveToward,
  SlowFollow,
  LoadScene,
  SetScene,
  SaveGame,
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
  SpawnPoolCreate,
  SpawnPoolSpawn,
  SetTransformVector,
  SetVelocityVector,
  ApplyTransformVector,
  ApplyPhysicsVector,
  ArmTimer,
  ArmDelay,
  UpdatePulsify,
  UpdateBarrier,
  BranchRoute,
  SetBonePoseTransform,
  TryOnce,
  ResetOnce,
  TryCooldown,
  ResetCooldown,
  Count,
};

enum class LN_RigidBodyAttribute : uint8_t {
  Mass = 0,
  Friction,
  Restitution,
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
};

enum class LN_RigidBodyConstraintMatchMode : uint8_t {
  Exact = 0,
  Contains,
  All,
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
  PoseChannel = 0,
  Armature,
  World,
};

static constexpr uint32_t LN_BONE_ATTRIBUTE_INDEX_MASK = 0xFFFFu;
static constexpr uint32_t LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG = 1u << 16;

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
  SpawnPool,
  CollectionVisibility,
  OverlayCollection,
  InputDeviceState,
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
  LN_DEP_STATE_SPAWN_POOL = 1u << 5,
  LN_DEP_STATE_LOOP = 1u << 6,
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
  OnNextTick,
  Once,
  Timer,
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
  MouseOverEnter,
  MouseOverOver,
  MouseOverExit,
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
  SpawnPoolSpawnedPulse,
  SpawnPoolHitPulse,
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

enum class LN_FloatCompareOperation : uint8_t {
  Equal = 0,
  NotEqual,
  GreaterThan,
  LessThan,
  GreaterEqual,
  LessEqual,
};

enum class LN_FloatExpressionKind : uint8_t {
  Constant = 0,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  Add,
  Subtract,
  Multiply,
  Divide,
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
  TweenFactor,
  TweenFloatResult,
  ObjectDistance,
  PhysicsQueryDistance,
  PhysicsQueryFraction,
  PhysicsQueryPenetrationDepth,
  RigidBodyAttribute,
  CooldownRemaining,
  CooldownProgress,
  Count,
};

inline constexpr int LN_TWEEN_CURVE_SAMPLE_COUNT = 64;

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
  Random,
  MaterialSlotCount,
  CollisionContactCount,
  PhysicsQueryFaceIndex,
  PhysicsQueryHitCount,
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
  Count,
};

enum class LN_VectorExpressionKind : uint8_t {
  Constant = 0,
  SnapshotWorldPosition,
  SnapshotLocalPosition,
  SnapshotWorldOrientation,
  SnapshotLocalOrientation,
  SnapshotWorldScale,
  SnapshotLocalScale,
  SnapshotLinearVelocity,
  SnapshotLocalLinearVelocity,
  SnapshotAngularVelocity,
  SnapshotLocalAngularVelocity,
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
  /** Pose bone scale remapped to normal object-style axes. */
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
  SnapshotCharacterGravity,
  SnapshotCharacterWalkDirection,
  SnapshotCharacterLocalWalkDirection,
  CollisionHitPoint,
  CollisionHitNormal,
  EvaluateCurveAtFactor,
  InstructionNextPoint,
  Random,
  TweenVectorResult,
  TweenRotationResult,
  SpawnPoolHitPoint,
  SpawnPoolHitNormal,
  SpawnPoolHitDirection,
  GroupCenterPosition,
  RigidBodyAttribute,
  Count,
};

enum class LN_ColorExpressionKind : uint8_t {
  Constant = 0,
  SnapshotObjectColor,
  SnapshotLightColor,
  RuntimeTreeProperty,
  Combine,
  FromGenericValue,
  Count,
};

enum class LN_ValueExpressionKind : uint8_t {
  Constant = 0,
  SnapshotGameProperty,
  RuntimeTreeProperty,
  Select,
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
  EventContent,
  EventMessenger,
  CollisionHitObject,
  CollisionHitObjects,
  CollisionHitPoints,
  CollisionHitNormals,
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
  ListRandomItem,
  ValueSwitchList,
  ValueSwitchListCompare,
  ProjectileParabola,
  SpawnPoolHitObject,
  BonePoseRotation,
  Count,
};

enum class LN_QueryExpressionKind : uint8_t {
  Raycast = 0,
  RaycastAll,
  ShapeCast,
  ShapeCastAll,
  MouseOver,
  MouseRay,
  CameraRay,
  ProjectileRay,
  Count,
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
};

enum class LN_RayInputMode : uint8_t {
  EndPoint = 0,
  DirectionDistance,
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
  std::string debug_name;

  bool IsValid() const
  {
    return kind != LN_RuntimeRefKind::None && slot != LN_INVALID_INDEX && generation != 0;
  }
};

struct LN_PhysicsControllerHandle {
  LN_RuntimeRef object_ref;
  uint32_t controller_generation = 0;

  bool IsValid() const
  {
    return object_ref.IsValid() && controller_generation != 0;
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
  MT_Vector3 hit_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  MT_Vector3 cast_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 ray_direction = MT_Vector3(0.0f, 0.0f, 1.0f);
  MT_Vector3 end_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector2 hit_uv = MT_Vector2(0.0f, 0.0f);
  float hit_fraction = 0.0f;
  float hit_distance = 0.0f;
  float penetration_depth = 0.0f;
  int32_t polygon_index = -1;
  bool has_uv = false;
  bool blocked = false;
  bool started_overlapping = false;
  std::vector<MT_Vector3> parabola_points;
};

struct LN_QueryResult {
  LN_PhysicsQueryResult physics_query;
  std::vector<LN_PhysicsQueryResult> physics_query_results;
  bool over = false;
  bool entered = false;
  bool exited = false;
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
  uint32_t loop_active_bool_expr_index = LN_INVALID_INDEX;
  uint32_t loop_index_int_expr_index = LN_INVALID_INDEX;
  uint32_t loop_current_value_expr_index = LN_INVALID_INDEX;
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

static constexpr size_t LN_RIGID_BODY_CONSTRAINT_BOOL_INPUT_COUNT =
    size_t(LN_RigidBodyConstraintBoolInput::Count);
static constexpr size_t LN_RIGID_BODY_CONSTRAINT_VECTOR_INPUT_COUNT =
    size_t(LN_RigidBodyConstraintVectorInput::Count);
static constexpr size_t LN_RIGID_BODY_CONSTRAINT_FLOAT_INPUT_COUNT =
    size_t(LN_RigidBodyConstraintFloatInput::Count);

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
  int32_t int_value = 0;
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
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  float float_value = 0.0f;
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
  uint32_t bool_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t tertiary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t quaternary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t quinary_bool_expr_index = LN_INVALID_INDEX;
  uint32_t condition_bool_expr_index = LN_INVALID_INDEX;
  uint32_t int_expr_index = LN_INVALID_INDEX;
  uint32_t secondary_int_expr_index = LN_INVALID_INDEX;
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
  uint8_t ray_query_detail_flags = LN_RAY_QUERY_DETAIL_NONE;
  LN_ShapeCastType shape_cast_type = LN_ShapeCastType::Sphere;
  int32_t cache_key = -1;
  uint32_t runtime_state_index = LN_INVALID_INDEX;
  bool bool_value = false;
};

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
  int32_t int_value = 0;
  float float_value = 0.0f;
  MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector4 vector4_value = MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 matrix_value = MT_Matrix3x3::Identity();
  MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  MT_Vector3 rotation_euler_value = MT_Vector3(0.0f, 0.0f, 0.0f);
  LN_RuntimeRef runtime_ref;
  LN_PhysicsControllerHandle physics_controller;
  LN_PhysicsQueryResult physics_query_result;
  std::vector<LN_Value> list_value;
  std::map<std::string, LN_Value> dict_value;
  std::string reference_name;
  std::string string_value;
};

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

struct LN_TickContext {
  LN_Event event = LN_Event::OnFixedUpdate;
  double current_time = 0.0;
  double fixed_dt = 0.0;
  double unscaled_dt = 0.0;
  bool use_fixed_timestep = false;
  bool use_register_expression_evaluator = false;
  bool use_typed_command_streams = true;
  LN_CommandBuffer *command_buffer = nullptr;
  uint64_t tick_index = 0;
  uint64_t command_sort_key = UINT64_MAX;
  uint64_t execution_scope_serial = 0;
};
