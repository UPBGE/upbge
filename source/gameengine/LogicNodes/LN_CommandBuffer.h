/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CommandBuffer.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstddef>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LN_DenseIds.h"
#include "LN_Performance.h"
#include "LN_Types.h"

class KX_GameObject;
class KX_Scene;
class LN_Manager;
class LN_RuntimeTree;

namespace blender {
struct Main;
struct Material;
struct bNode;
struct bNodeSocket;
struct bNodeTree;
}

class LN_CommandBuffer {
 public:
  enum class CommandType : uint8_t {
    SetWorldPosition = 0,
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
    ApplyTorque,
    SetGameProperty,
    AddObject,
    AddObjectFromRef,
    SetParent,
    SetParentFromRef,
    RemoveParent,
    RemoveObject,
    SetGravity,
    SetTimeScale,
    SetActiveCamera,
    SetCameraFov,
    SetCameraOrthoScale,
    SetCollisionGroup,
    SetPhysics,
    SetDynamics,
    RebuildCollisionShape,
    SetRigidBodyAttribute,
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
    SetTreeProperty,
    SetCursorVisibility,
    SetCursorPosition,
    SetGamepadVibration,
    SetWindowSize,
    SetFullscreen,
    SetVSync,
    SetShowFramerate,
    SetShowProfile,
    SubsystemCommand,
    Print,
    QuitGame,
    RestartGame,
    LoadBlendFile,
    PlayAction,
    StopAction,
    SetActionFrame,
    StopAllSounds,
    PlaySound,
    PlaySound3D,
    PauseSound,
    ResumeSound,
    StopSound,
    SetLogicTreeEnabled,
    InstallLogicTree,
    SendEvent,
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
    SetBonePoseTransform,
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
  };

  enum class ModifierTarget : int32_t {
    Name = 0,
    First = 1,
    Last = 2,
    Index = 3,
    PersistentId = 4,
  };

  enum class ModifierAssignmentOperation : int32_t {
    Append = 0,
    Insert = 1,
    Replace = 2,
  };

  enum class CommandSubsystem : uint8_t {
    ObjectTransform = 0,
    ObjectPhysics,
    ObjectState,
    ObjectProperty,
    ObjectLifecycle,
    SceneState,
    Parenting,
    Animation,
    /** Global AUD device; native logic tracks a bounded list of `AUD_Handle` for stop-by-sound. */
    Sound,
    Camera,
    Light,
    Render,
    Window,
    Datablock,
    Collection,
    Geometry,
    Group,
    Input,
    TreeState,
    GameLifecycle,
    Events,
  };

  enum class CommandThreadPolicy : uint8_t {
    MainThreadFlush = 0,
    SnapshotReadOnly,
    MainThreadQuery,
    Unsupported,
  };

  enum class CommandClass : uint8_t {
    LegacyFallback = 0,
    AbsoluteObjectTransform,
    AbsoluteObjectVelocity,
    RelativeObjectVector,
    GamePropertyWrite,
    TreePropertyWrite,
    EventSend,
    SimpleAudioSideEffect,
    ObjectLifecycleMutation,
    ObjectServiceMutation,
    PhysicsServiceMutation,
  };

  enum class CommandBarrier : uint8_t {
    None = 0,
    AbsoluteSetter,
    RelativeMutation,
    ObjectLifecycle,
    SceneDatablockMutation,
    EventMessage,
    ExternalSideEffect,
    Diagnostic,
  };

  enum class CoalescingPolicy : uint8_t {
    Forbidden = 0,
    LastWriteWinsWithinBarrierSegment,
  };

  struct CommandSubsystemPolicy {
    CommandThreadPolicy read_policy = CommandThreadPolicy::Unsupported;
    CommandThreadPolicy write_policy = CommandThreadPolicy::Unsupported;
    const char *runtime_service = nullptr;
    const char *notes = nullptr;
  };

  struct CommandStreamStats {
    uint32_t legacy_command_count = 0;
    uint32_t typed_transform_count = 0;
    uint32_t typed_velocity_count = 0;
    uint32_t typed_relative_vector_count = 0;
    uint32_t typed_motion_count = 0;
    uint32_t typed_property_count = 0;
    uint32_t typed_event_count = 0;
    uint32_t typed_audio_count = 0;
    uint32_t typed_lifecycle_count = 0;
    uint32_t typed_object_service_count = 0;
    uint32_t typed_runtime_service_count = 0;
    uint32_t typed_animation_count = 0;
    uint32_t typed_armature_count = 0;
    uint32_t typed_material_count = 0;
    uint32_t typed_physics_count = 0;
    uint32_t coalesced_command_count = 0;
    uint32_t direct_ordered_stream_count = 0;
    uint32_t direct_ordered_command_count = 0;
  };

  struct CommandHeader {
    uint64_t sort_key = 0;
    uint64_t record_sequence = 0;
    uint32_t source_ref_index = 0;
    CommandClass command_class = CommandClass::LegacyFallback;
    CommandBarrier barrier = CommandBarrier::None;
    CoalescingPolicy coalescing = CoalescingPolicy::Forbidden;
  };

  struct Command {
    CommandType type = CommandType::SetWorldPosition;
    CommandSubsystem subsystem = CommandSubsystem::ObjectTransform;
    KX_GameObject *object = nullptr;
    LN_RuntimeTree *runtime_tree = nullptr;
    KX_GameObject *event_target_object = nullptr;
    LN_RuntimeRef runtime_ref;
    LN_ObjectHandle object_handle;
    LN_ObjectHandle event_target_handle;
    LN_GamePropertyId game_property_id;
    LN_EventSubjectId event_subject_id;
    const std::string *property_name_ptr = nullptr;
    MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    LN_Value property_value;
    LN_StringId property_name_id;
    std::string property_name;
    std::string secondary_property_name;
    std::string tertiary_property_name;
    std::string quaternary_property_name;
    float scalar_value = 0.0f;
    int32_t int_value = 0;
    int32_t secondary_int_value = 0;
    uint32_t property_ref_index = LN_INVALID_INDEX;
    bool bool_value = false;
    bool secondary_bool_value = false;
    uint64_t sort_key = 0;
    uint64_t record_sequence = 0;
    uint32_t source_ref_index = 0;
    /** Packed play/blend/ipo flags for #CommandType::PlayAction (see `BL_Action` enums). */
    uint32_t animation_flags = 0;
  };

  struct RecordedCommandList {
    size_t runtime_tree_index = 0;
    std::vector<Command> commands;
    LN_RuntimeProfileCounters profile_counters;
    CommandStreamStats command_stream_stats;
  };

  static void SortCommands(std::vector<Command> &commands);
  static CommandSubsystem GetCommandSubsystem(CommandType type);
  static CommandSubsystem GetCommandSubsystem(const Command &command);
  static CommandThreadPolicy GetCommandThreadPolicy(CommandType type);
  static CommandSubsystemPolicy GetCommandSubsystemPolicy(CommandSubsystem subsystem);
  static CommandClass GetCommandClass(CommandType type);
  static CommandBarrier GetCommandBarrier(CommandType type);
  static CoalescingPolicy GetCoalescingPolicy(CommandType type);
  static bool AllowsObjectlessFlush(CommandType type);

  void SetMainThreadId(std::thread::id main_thread_id);
  void SetLogicManager(LN_Manager *logic_manager);
  void SetTypedCommandStreamsEnabled(bool enabled);
  /** When true, recording may occur on worker threads; flush remains main-thread only. */
  void SetAllowWorkerRecording(bool allow_worker_recording);
  bool CanFlushPendingObjectTransformCommands() const;
  void BeginRecording();
  void EndRecording();
  std::vector<Command> TakeRecordedCommands();
  std::vector<Command> TakeFlushPlannedCommands();
  void MergeRecordedCommands(std::vector<Command> commands);
  void MergeRecordedCommandLists(std::vector<RecordedCommandList> command_lists);
  void AppendSetWorldPosition(KX_GameObject *gameobj,
                              const MT_Vector3 &position,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendSetLocalPosition(KX_GameObject *gameobj,
                              const MT_Vector3 &position,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendSetWorldOrientation(KX_GameObject *gameobj,
                                 const MT_Vector3 &rotation,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendSetLocalOrientation(KX_GameObject *gameobj,
                                 const MT_Vector3 &rotation,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendSetWorldScale(KX_GameObject *gameobj,
                           const MT_Vector3 &scale,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetLocalScale(KX_GameObject *gameobj,
                           const MT_Vector3 &scale,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetLinearVelocity(KX_GameObject *gameobj,
                               const MT_Vector3 &velocity,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendSetLocalLinearVelocity(KX_GameObject *gameobj,
                                    const MT_Vector3 &velocity,
                                    uint64_t sort_key,
                                    uint32_t source_ref_index);
  void AppendSetAngularVelocity(KX_GameObject *gameobj,
                                const MT_Vector3 &velocity,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  void AppendSetLocalAngularVelocity(KX_GameObject *gameobj,
                                     const MT_Vector3 &velocity,
                                     uint64_t sort_key,
                                     uint32_t source_ref_index);
  void AppendApplyImpulse(KX_GameObject *gameobj,
                          const MT_Vector3 &attach,
                          const MT_Vector3 &impulse,
                          uint64_t sort_key,
                          uint32_t source_ref_index);
  void AppendSetVisibility(KX_GameObject *gameobj,
                           bool visible,
                           bool recursive,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetObjectColor(KX_GameObject *gameobj,
                            const MT_Vector4 &color,
                            uint64_t sort_key,
                            uint32_t source_ref_index);
  void AppendMakeLightUnique(KX_GameObject *gameobj,
                             uint64_t sort_key,
                             uint32_t source_ref_index);
  void AppendSetLightColor(KX_GameObject *gameobj,
                           const MT_Vector4 &color,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetLightPower(KX_GameObject *gameobj,
                           float power,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetLightShadow(KX_GameObject *gameobj,
                            bool use_shadow,
                            uint64_t sort_key,
                            uint32_t source_ref_index);
  void AppendApplyMovement(KX_GameObject *gameobj,
                           const MT_Vector3 &movement,
                           bool local,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendApplyRotation(KX_GameObject *gameobj,
                           const MT_Vector3 &rotation,
                           bool local,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void FlushPendingObjectTransformCommands(KX_GameObject *gameobj, uint64_t max_sort_key);
  void FlushPendingObjectHierarchyTransformCommands(KX_GameObject *gameobj,
                                                    uint64_t max_sort_key);
  void AppendApplyForce(KX_GameObject *gameobj,
                        const MT_Vector3 &force,
                        bool local,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendApplyTorque(KX_GameObject *gameobj,
                         const MT_Vector3 &torque,
                         bool local,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendSetGameProperty(KX_GameObject *gameobj,
                             const std::string &property_name,
                             const LN_Value &value,
                             uint64_t sort_key,
                             uint32_t source_ref_index);
  void AppendSetGamePropertyRef(LN_RuntimeTree *runtime_tree,
                                KX_GameObject *gameobj,
                                uint32_t property_ref_index,
                                const LN_Value &value,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  void AppendSetTreeProperty(LN_RuntimeTree *runtime_tree,
                             uint32_t property_ref_index,
                             const LN_Value &value,
                             uint64_t sort_key,
                             uint32_t source_ref_index);
  void AppendAddObject(KX_GameObject *gameobj,
                       const std::string &object_name,
                       float life_time,
          bool full_copy,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendAddObjectFromRef(LN_RuntimeTree *runtime_tree,
                              KX_GameObject *gameobj,
                              const LN_RuntimeRef &source_ref,
                              float life_time,
            bool full_copy,
                              uint32_t property_ref_index,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  KX_GameObject *ExecuteAddObjectFromRefImmediate(LN_RuntimeTree *runtime_tree,
                                                  KX_GameObject *gameobj,
                                                  const LN_RuntimeRef &source_ref,
                                                  float life_time,
                                                  bool full_copy,
                                                  uint32_t property_ref_index,
                                                  uint32_t source_ref_index);
  void AppendSetParent(KX_GameObject *gameobj,
                       const std::string &parent_name,
          bool compound,
          bool ghost,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendSetParentFromRef(LN_RuntimeTree *runtime_tree,
                              KX_GameObject *gameobj,
                              const LN_RuntimeRef &parent_ref,
            bool compound,
            bool ghost,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendRemoveParent(KX_GameObject *gameobj, uint64_t sort_key, uint32_t source_ref_index);
  void AppendRemoveObject(KX_GameObject *gameobj, uint64_t sort_key, uint32_t source_ref_index);
  void AppendSetGravity(KX_GameObject *gameobj,
                        const MT_Vector3 &gravity,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendSetTimeScale(KX_GameObject *gameobj,
                          float timescale,
                          uint64_t sort_key,
                          uint32_t source_ref_index);
  void AppendSetActiveCamera(KX_GameObject *camera,
                             uint64_t sort_key,
                             uint32_t source_ref_index);
  void AppendSetCameraFov(KX_GameObject *camera,
                          float fov_degrees,
                          uint64_t sort_key,
                          uint32_t source_ref_index);
  void AppendSetCameraOrthoScale(KX_GameObject *camera,
                                 float scale,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendSetCollisionGroup(KX_GameObject *gameobj,
                               int32_t group,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendSetPhysics(KX_GameObject *gameobj,
                        bool active,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendSetDynamics(KX_GameObject *gameobj,
                         int32_t mode,
                         bool enabled,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendRebuildCollisionShape(KX_GameObject *gameobj,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  void AppendSetRigidBodyAttribute(KX_GameObject *gameobj,
                                   LN_RigidBodyAttribute attribute,
                                   const MT_Vector3 &value,
                                   const MT_Vector3 &secondary_value,
                                   float scalar_value,
                                   bool bool_value,
                                   bool secondary_bool_value,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  void AppendCharacterJump(KX_GameObject *gameobj,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetCharacterGravity(KX_GameObject *gameobj,
                                 const MT_Vector3 &gravity,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendSetCharacterJumpSpeed(KX_GameObject *gameobj,
                                   float jump_speed,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  void AppendSetCharacterMaxJumps(KX_GameObject *gameobj,
                                  int32_t max_jumps,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendSetCharacterWalkDirection(KX_GameObject *gameobj,
                                       const MT_Vector3 &walk_direction,
                                       bool local,
                                       uint64_t sort_key,
                                       uint32_t source_ref_index);
  void AppendSetCharacterVelocity(KX_GameObject *gameobj,
                                  const MT_Vector3 &velocity,
                                  float time,
                                  bool local,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendVehicleControl(KX_GameObject *gameobj,
                            const MT_Vector3 &control,
                            float steering,
                            uint64_t sort_key,
                            uint32_t source_ref_index);
  void AppendVehicleApplyEngineForce(KX_GameObject *gameobj,
                                     float power,
                                     int32_t wheel_count,
                                     int32_t axis,
                                     uint64_t sort_key,
                                     uint32_t source_ref_index);
  void AppendVehicleApplyBraking(KX_GameObject *gameobj,
                                 float power,
                                 int32_t wheel_count,
                                 int32_t axis,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendVehicleApplySteering(KX_GameObject *gameobj,
                                  float steering,
                                  int32_t wheel_count,
                                  int32_t axis,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendSetVehicleSuspensionCompression(KX_GameObject *gameobj,
                                             float value,
                                             int32_t wheel_count,
                                             int32_t axis,
                                             uint64_t sort_key,
                                             uint32_t source_ref_index);
  void AppendSetVehicleSuspensionStiffness(KX_GameObject *gameobj,
                                           float value,
                                           int32_t wheel_count,
                                           int32_t axis,
                                           uint64_t sort_key,
                                           uint32_t source_ref_index);
  void AppendSetVehicleSuspensionDamping(KX_GameObject *gameobj,
                                         float value,
                                         int32_t wheel_count,
                                         int32_t axis,
                                         uint64_t sort_key,
                                         uint32_t source_ref_index);
  void AppendSetVehicleWheelFriction(KX_GameObject *gameobj,
                                     float value,
                                     int32_t wheel_count,
                                     int32_t axis,
                                     uint64_t sort_key,
                                     uint32_t source_ref_index);
  void AppendSetCursorVisibility(bool visible, uint64_t sort_key, uint32_t source_ref_index);
  void AppendSetCursorPosition(int32_t x,
                               int32_t y,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendSetGamepadVibration(int32_t gamepad_index,
                                 float strength_left,
                                 float strength_right,
                                 uint32_t duration_ms,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendSetWindowSize(int32_t width,
                           int32_t height,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetFullscreen(bool fullscreen, uint64_t sort_key, uint32_t source_ref_index);
  void AppendSetVSync(int32_t mode, uint64_t sort_key, uint32_t source_ref_index);
  void AppendSetShowFramerate(bool show, uint64_t sort_key, uint32_t source_ref_index);
  void AppendSetShowProfile(bool show, uint64_t sort_key, uint32_t source_ref_index);
  void AppendPrint(const std::string &message, uint64_t sort_key, uint32_t source_ref_index);
  void AppendPrintTreeProperty(LN_RuntimeTree *runtime_tree,
                               uint32_t property_ref_index,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendSubsystemCommand(CommandSubsystem subsystem,
                              const std::string &action_name,
                              KX_GameObject *gameobj,
                              const LN_RuntimeRef &runtime_ref,
                              const LN_Value &payload,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendQuitGame(uint64_t sort_key, uint32_t source_ref_index);
  void AppendRestartGame(uint64_t sort_key, uint32_t source_ref_index);
  void AppendPlayAction(KX_GameObject *gameobj,
                        const std::string &action_name,
                        float start_frame,
                        float end_frame,
                        int32_t layer,
                        int32_t priority,
                        float blendin,
                        float layer_weight,
                        uint32_t animation_flags,
                        float playback_speed,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendStopAction(KX_GameObject *gameobj,
                        int32_t layer,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendSetActionFrame(KX_GameObject *gameobj,
                            int32_t layer,
                            float frame,
                            uint64_t sort_key,
                            uint32_t source_ref_index);
  void AppendStopAllSounds(uint64_t sort_key, uint32_t source_ref_index);
  void AppendPlaySound(const std::string &sound_name,
                       float volume,
                       float pitch,
                       bool loop,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendStopSound(const std::string &sound_name,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendPlaySound3D(KX_GameObject *speaker_object,
                         const std::string &sound_name,
                         float volume,
                         float pitch,
                         bool loop,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendPauseSound(const std::string &sound_name,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendResumeSound(const std::string &sound_name,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendLoadBlendFile(const std::string &filepath,
                           uint64_t sort_key,
                           uint32_t source_ref_index);
  void AppendSetLogicTreeEnabled(KX_GameObject *target_object,
                                 const std::string &tree_name,
                                 bool enabled,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendInstallLogicTree(KX_GameObject *target_object,
                               const std::string &tree_name,
                               bool initial_enabled,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendSendEvent(const std::string &subject,
                       const LN_Value &content,
                       KX_GameObject *messenger,
                       KX_GameObject *target,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendSendEvent(LN_RuntimeTree *runtime_tree,
                       LN_StringId subject_id,
                       const LN_Value &content,
                       KX_GameObject *messenger,
                       KX_GameObject *target,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendMoveToward(KX_GameObject *gameobj,
                        const MT_Vector3 &target_position,
                        float speed,
                        float stop_distance,
                        bool dynamic,
                        bool use_frame_delta,
                        float frame_delta,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendSlowFollow(KX_GameObject *gameobj,
                        KX_GameObject *target,
                        float factor,
                        uint8_t attribute,
                        uint64_t sort_key,
                        uint32_t source_ref_index);
  void AppendLoadScene(const std::string &scene_name,
                       uint64_t sort_key,
                       uint32_t source_ref_index);
  void AppendSetScene(const std::string &scene_name,
                      uint64_t sort_key,
                      uint32_t source_ref_index);
  void AppendSaveGame(KX_GameObject *gameobj,
                      int32_t slot,
                      const std::string &path,
                      uint64_t sort_key,
                      uint32_t source_ref_index);
  void AppendLoadGame(KX_GameObject *gameobj,
                      int32_t slot,
                      const std::string &path,
                      uint64_t sort_key,
                      uint32_t source_ref_index);
  void AppendAlignAxisToVector(KX_GameObject *gameobj,
                               const MT_Vector3 &vector,
                               int32_t axis,
                               float factor,
                               uint64_t sort_key,
                               uint32_t source_ref_index);
  void AppendRotateToward(KX_GameObject *gameobj,
                          const MT_Vector3 &target,
                          float factor,
                          int32_t rot_axis,
                          int32_t front_axis,
                          uint64_t sort_key,
                          uint32_t source_ref_index);
  void AppendReplaceMesh(KX_GameObject *gameobj,
                         KX_GameObject *mesh_object,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendCopyProperty(KX_GameObject *source,
                          KX_GameObject *target,
                          const std::string &property_name,
                          uint64_t sort_key,
                          uint32_t source_ref_index);
  void AppendSetBonePoseLocation(KX_GameObject *armature_object,
                                 const std::string &bone_name,
                                 const MT_Vector3 &location,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index,
                                 int32_t location_space = int32_t(
                                     LN_BonePoseLocationSpace::ArmatureOffset),
                                 bool use_center = false);
  void AppendSetBonePoseRotation(KX_GameObject *armature_object,
                                 const std::string &bone_name,
                                 const MT_Vector3 &rotation_euler,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index,
                                 bool use_center = false,
                                 int32_t rotation_space = int32_t(
                                     LN_BonePoseRotationSpace::PoseChannel));
  void AppendSetBonePoseScale(KX_GameObject *armature_object,
                              const std::string &bone_name,
                              const MT_Vector3 &scale,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendSetBonePoseTransform(KX_GameObject *armature_object,
                                  const std::string &bone_name,
                                  const MT_Vector3 &location,
                                  const MT_Vector3 &rotation_euler,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index,
                                  int32_t location_space = int32_t(
                                      LN_BonePoseLocationSpace::ArmatureOffset),
                                  bool use_center = false);
  void AppendSetBoneAttribute(KX_GameObject *armature_object,
                              const std::string &bone_name,
                              int32_t attribute,
                              int32_t scale_mode,
                              const LN_Value &value,
                              uint64_t sort_key,
                              uint32_t source_ref_index);
  void AppendSetBoneConstraintInfluence(KX_GameObject *armature_object,
                                        const std::string &bone_name,
                                        const std::string &constraint_name,
                                        float influence,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index);
  void AppendSetBoneConstraintTarget(LN_RuntimeTree *runtime_tree,
                                     KX_GameObject *armature_object,
                                     const std::string &bone_name,
                                     const std::string &constraint_name,
                                     const LN_Value &target_value,
                                     uint64_t sort_key,
                                     uint32_t source_ref_index);
  void AppendSetBoneConstraintAttribute(LN_RuntimeTree *runtime_tree,
                                        KX_GameObject *armature_object,
                                        const std::string &bone_name,
                                        const std::string &constraint_name,
                                        const std::string &attribute_name,
                                        const LN_Value &value,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index);
  void AppendAssignMaterialToSlot(LN_RuntimeTree *runtime_tree,
                                  KX_GameObject *gameobj,
                                  const LN_Value &material_value,
                                  int32_t slot,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendSetMaterialNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                        const LN_Value &material_value,
                                        const std::string &node_name,
                                        const std::string &socket_name,
                                        const LN_Value &value,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index);
  void AppendSetMaterialNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                        KX_GameObject *gameobj,
                                        const LN_Value &material_value,
                                        int32_t slot,
                                        const std::string &node_name,
                                        const std::string &socket_name,
                                        const LN_Value &value,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index,
                                        bool make_unique = true);
  void AppendSetMaterialParameter(LN_RuntimeTree *runtime_tree,
                                  const LN_Value &material_value,
                                  const std::string &node_name,
                                  const std::string &socket_name,
                                  const LN_Value &value,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendSetMaterialParameter(LN_RuntimeTree *runtime_tree,
                                  KX_GameObject *gameobj,
                                  const LN_Value &material_value,
                                  int32_t slot,
                                  const std::string &node_name,
                                  const std::string &socket_name,
                                  const LN_Value &value,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendSetGeometryNodesInput(LN_RuntimeTree *runtime_tree,
                                   KX_GameObject *gameobj,
                                   const std::string &modifier_name,
                                   const std::string &input_identifier,
                                   const LN_Value &value,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  void AppendSetGeometryNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                        KX_GameObject *gameobj,
                                        const std::string &modifier_name,
                                        const std::string &node_name,
                                        const std::string &socket_identifier,
                                        const LN_Value &value,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index);
  void AppendSetCompositorNodeSocketValue(LN_RuntimeTree *runtime_tree,
                                          int16_t target_id_code,
                                          const std::string &target_name,
                                          const std::string &node_name,
                                          const std::string &socket_identifier,
                                          const LN_Value &value,
                                          uint64_t sort_key,
                                          uint32_t source_ref_index);
  void AppendMakeNodeTreeUnique(KX_GameObject *gameobj,
                                int32_t editor_type,
                                int32_t slot,
                                const std::string &target_name,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  void AppendSetNodeMute(LN_RuntimeTree *runtime_tree,
                         int16_t target_id_code,
                         const std::string &target_name,
                         const std::string &node_name,
                         bool muted,
                         uint64_t sort_key,
                         uint32_t source_ref_index);
  void AppendEnableDisableModifier(KX_GameObject *gameobj,
                                   ModifierTarget target,
                                   const std::string &modifier_name,
                                   int32_t index,
                                   bool enabled,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  int32_t AssignGeometryNodesModifierNow(LN_RuntimeTree *runtime_tree,
                                         KX_GameObject *gameobj,
                                         const LN_Value &node_group_value,
                                         ModifierAssignmentOperation operation,
                                         ModifierTarget replace_target,
                                         const std::string &name,
                                         int32_t index_or_id);
  void RemoveCommandsForGameObject(KX_GameObject *gameobj);
  void Flush();
  void Clear();
  void ClearMaterialParameterDefaultState();
  void PrewarmMaterialParameterBindings(LN_RuntimeTree &runtime_tree);
  const CommandStreamStats &GetLastCommandStreamStats() const
  {
    return m_lastCommandStreamStats;
  }

  size_t Size() const
  {
    return m_commands.size() + m_transformCommands.size() + m_velocityCommands.size() +
           m_relativeVectorCommands.size() + m_motionCommands.size() +
           m_propertyCommands.size() + m_eventCommands.size() +
           m_audioCommands.size() + m_lifecycleCommands.size() +
           m_objectServiceCommands.size() + m_runtimeServiceCommands.size() +
           m_animationCommands.size() + m_armatureCommands.size() + m_materialCommands.size() +
           m_physicsCommands.size();
  }

  const std::vector<Command> &GetCommandsForTests() const;

 private:
  struct AddObjectSourceStateKey {
    KX_Scene *scene = nullptr;
    KX_GameObject *source = nullptr;

    bool operator==(const AddObjectSourceStateKey &other) const
    {
      return scene == other.scene && source == other.source;
    }
  };

  struct AddObjectSourceStateKeyHash {
    size_t operator()(const AddObjectSourceStateKey &key) const
    {
      const size_t scene_hash = std::hash<KX_Scene *>{}(key.scene);
      const size_t source_hash = std::hash<KX_GameObject *>{}(key.source);
      return scene_hash ^ (source_hash + 0x9e3779b97f4a7c15ULL + (scene_hash << 6u) +
                           (scene_hash >> 2u));
    }
  };

  struct MaterialParameterDefaultStateKey {
    blender::Material *material = nullptr;
    std::string attribute_name;

    bool operator==(const MaterialParameterDefaultStateKey &other) const
    {
      return material == other.material && attribute_name == other.attribute_name;
    }
  };

  struct MaterialParameterDefaultStateKeyHash {
    size_t operator()(const MaterialParameterDefaultStateKey &key) const
    {
      const size_t material_hash = std::hash<blender::Material *>{}(key.material);
      const size_t attribute_hash = std::hash<std::string>{}(key.attribute_name);
      return material_hash ^ (attribute_hash + 0x9e3779b97f4a7c15ULL + (material_hash << 6u) +
                              (material_hash >> 2u));
    }
  };

  struct TypedVectorCommand {
    CommandHeader header;
    CommandType type = CommandType::SetWorldPosition;
    KX_GameObject *object = nullptr;
    MT_Vector3 value = MT_Vector3(0.0f, 0.0f, 0.0f);
    bool bool_value = false;
  };

  struct TypedPropertyCommand {
    CommandHeader header;
    CommandType type = CommandType::SetGameProperty;
    KX_GameObject *object = nullptr;
    LN_RuntimeTree *runtime_tree = nullptr;
    LN_ObjectHandle object_handle;
    LN_GamePropertyId game_property_id;
    LN_StringId property_name_id;
    const std::string *property_name_ptr = nullptr;
    uint32_t property_ref_index = LN_INVALID_INDEX;
    std::string property_name;
    LN_Value value;
  };

  struct TypedEventCommand {
    CommandHeader header;
    LN_RuntimeTree *runtime_tree = nullptr;
    KX_GameObject *messenger = nullptr;
    KX_GameObject *target = nullptr;
    LN_ObjectHandle messenger_handle;
    LN_ObjectHandle target_handle;
    LN_EventSubjectId event_subject_id;
    LN_StringId subject_name_id;
    std::string dynamic_subject;
    LN_Value content;
  };

  struct TypedAudioCommand {
    CommandHeader header;
    CommandType type = CommandType::PlaySound;
    KX_GameObject *speaker = nullptr;
    LN_SoundId sound_id;
    std::string sound_name;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
  };

  struct TypedLifecycleCommand {
    CommandHeader header;
    CommandType type = CommandType::AddObject;
    KX_GameObject *object = nullptr;
    LN_RuntimeTree *runtime_tree = nullptr;
    LN_RuntimeRef runtime_ref;
    std::string name;
    float scalar_value = 0.0f;
    uint32_t property_ref_index = LN_INVALID_INDEX;
    bool bool_value = false;
    bool secondary_bool_value = false;
  };

  struct TypedObjectServiceCommand {
    CommandHeader header;
    CommandType type = CommandType::SetVisibility;
    KX_GameObject *object = nullptr;
    MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector4 color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    float scalar_value = 0.0f;
    bool bool_value = false;
    bool secondary_bool_value = false;
  };

  struct TypedRuntimeServiceCommand {
    CommandHeader header;
    CommandType type = CommandType::SetCursorVisibility;
    int32_t int_value = 0;
    int32_t secondary_int_value = 0;
    uint32_t uint_value = 0;
    float scalar_value = 0.0f;
    float secondary_scalar_value = 0.0f;
    bool bool_value = false;
  };

  struct TypedAnimationCommand {
    CommandHeader header;
    CommandType type = CommandType::PlayAction;
    KX_GameObject *object = nullptr;
    std::string action_name;
    float start_frame = 0.0f;
    float end_frame = 0.0f;
    float blendin = 0.0f;
    float layer_weight = 1.0f;
    float playback_speed = 1.0f;
    float frame = 0.0f;
    int32_t layer = 0;
    int32_t priority = 0;
    uint32_t animation_flags = 0;
  };

  struct TypedArmatureCommand {
    CommandHeader header;
    CommandType type = CommandType::SetBonePoseLocation;
    KX_GameObject *object = nullptr;
    LN_RuntimeTree *runtime_tree = nullptr;
    std::string bone_name;
    std::string constraint_name;
    std::string attribute_name;
    MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    LN_Value value;
    float scalar_value = 0.0f;
    int32_t int_value = 0;
    int32_t secondary_int_value = 0;
  };

  struct TypedMaterialCommand {
    CommandHeader header;
    CommandType type = CommandType::SetMaterialSlot;
    KX_GameObject *object = nullptr;
    LN_RuntimeTree *runtime_tree = nullptr;
    LN_RuntimeRef runtime_ref;
    std::string material_name;
    std::string node_name;
    std::string internal_name;
    std::string attribute_name;
    LN_Value value;
    float scalar_value = 0.0f;
    int32_t int_value = 0;
  };

  struct TypedPhysicsCommand {
    CommandHeader header;
    CommandType type = CommandType::ApplyImpulse;
    KX_GameObject *object = nullptr;
    MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    float scalar_value = 0.0f;
    int32_t int_value = 0;
    int32_t secondary_int_value = 0;
    bool bool_value = false;
    bool secondary_bool_value = false;
  };

  struct TypedMotionCommand {
    CommandHeader header;
    CommandType type = CommandType::MoveToward;
    KX_GameObject *object = nullptr;
    KX_GameObject *target_object = nullptr;
    MT_Vector3 vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 secondary_vector_value = MT_Vector3(0.0f, 0.0f, 0.0f);
    float scalar_value = 0.0f;
    int32_t int_value = 0;
    int32_t secondary_int_value = 0;
    bool bool_value = false;
    bool secondary_bool_value = false;
  };

  void AssertMainThread() const;
  void FlushPendingObjectTransformCommandsInternal(KX_GameObject *gameobj,
                                                   uint64_t max_sort_key,
                                                   bool include_parents);
  CommandHeader MakeCommandHeader(CommandType type,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendLegacyCommand(Command command);
  template<typename TypedCommand>
  void AppendTypedCommand(std::vector<TypedCommand> &stream, TypedCommand command);
  void AppendTypedVectorCommand(CommandType type,
                                KX_GameObject *gameobj,
                                const MT_Vector3 &value,
                                bool bool_value,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  void AppendTypedVectorCommand(CommandType type,
                                KX_GameObject *gameobj,
                                const MT_Vector3 &value,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  void AppendTypedObjectServiceCommand(CommandType type,
                                       KX_GameObject *gameobj,
                                       const MT_Vector4 &color_value,
                                       float scalar_value,
                                       bool bool_value,
                                       bool secondary_bool_value,
                                       uint64_t sort_key,
                                       uint32_t source_ref_index,
                                       const MT_Vector3 &vector_value = MT_Vector3(0.0f,
                                                                                  0.0f,
                                                                                  0.0f));
  void AppendTypedRuntimeServiceCommand(CommandType type,
                                        int32_t int_value,
                                        int32_t secondary_int_value,
                                        uint32_t uint_value,
                                        float scalar_value,
                                        float secondary_scalar_value,
                                        bool bool_value,
                                        uint64_t sort_key,
                                        uint32_t source_ref_index);
  void AppendTypedAnimationCommand(CommandType type,
                                   KX_GameObject *gameobj,
                                   const std::string &action_name,
                                   float start_frame,
                                   float end_frame,
                                   float blendin,
                                   float layer_weight,
                                   float playback_speed,
                                   float frame,
                                   int32_t layer,
                                   int32_t priority,
                                   uint32_t animation_flags,
                                   uint64_t sort_key,
                                   uint32_t source_ref_index);
  void AppendTypedArmatureCommand(CommandType type,
                                  KX_GameObject *gameobj,
                                  LN_RuntimeTree *runtime_tree,
                                  const std::string &bone_name,
                                  const std::string &constraint_name,
                                  const std::string &attribute_name,
                                  const MT_Vector3 &vector_value,
                                  const MT_Vector3 &secondary_vector_value,
                                  const LN_Value &value,
                                  float scalar_value,
                                  int32_t int_value,
                                  int32_t secondary_int_value,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendTypedMaterialCommand(CommandType type,
                                  KX_GameObject *gameobj,
                                  LN_RuntimeTree *runtime_tree,
                                  const LN_RuntimeRef &runtime_ref,
                                  const std::string &material_name,
                                  const std::string &node_name,
                                  const std::string &internal_name,
                                  const std::string &attribute_name,
                                  const LN_Value &value,
                                  float scalar_value,
                                  int32_t int_value,
                                  uint64_t sort_key,
                                  uint32_t source_ref_index);
  void AppendTypedPhysicsCommand(CommandType type,
                                 KX_GameObject *gameobj,
                                 const MT_Vector3 &vector_value,
                                 const MT_Vector3 &secondary_vector_value,
                                 float scalar_value,
                                 int32_t int_value,
                                 int32_t secondary_int_value,
                                 bool bool_value,
                                 bool secondary_bool_value,
                                 uint64_t sort_key,
                                 uint32_t source_ref_index);
  void AppendTypedMotionCommand(CommandType type,
                                KX_GameObject *gameobj,
                                KX_GameObject *target_object,
                                const MT_Vector3 &vector_value,
                                const MT_Vector3 &secondary_vector_value,
                                float scalar_value,
                                int32_t int_value,
                                int32_t secondary_int_value,
                                bool bool_value,
                                bool secondary_bool_value,
                                uint64_t sort_key,
                                uint32_t source_ref_index);
  uint32_t CountCoalescibleCommands() const;
  std::vector<Command> BuildPlannedCommands(bool enable_coalescing,
                                            bool sort_commands,
                                            CommandStreamStats *r_stats) const;
  bool IsAddObjectSourceInactiveCached(KX_Scene *scene, KX_GameObject *source);
  KX_GameObject *ExecuteAddObjectFromRefCommand(const Command &command, LN_Value &r_added_object);
  bool EnsureMaterialParameterBinding(blender::Main *bmain,
                                      blender::Material *material,
                                      blender::bNodeTree *ntree,
                                      blender::bNode *node,
                                      blender::bNodeSocket *socket,
                                      std::string *r_attribute_name);
  void RefreshCommandStreamTracking();
  void ClearTypedStreams();

  std::vector<Command> m_commands;
  std::vector<TypedVectorCommand> m_transformCommands;
  std::vector<TypedVectorCommand> m_velocityCommands;
  std::vector<TypedVectorCommand> m_relativeVectorCommands;
  std::vector<TypedPropertyCommand> m_propertyCommands;
  std::vector<TypedEventCommand> m_eventCommands;
  std::vector<TypedAudioCommand> m_audioCommands;
  std::vector<TypedLifecycleCommand> m_lifecycleCommands;
  std::vector<TypedObjectServiceCommand> m_objectServiceCommands;
  std::vector<TypedRuntimeServiceCommand> m_runtimeServiceCommands;
  std::vector<TypedAnimationCommand> m_animationCommands;
  std::vector<TypedArmatureCommand> m_armatureCommands;
  std::vector<TypedMaterialCommand> m_materialCommands;
  std::vector<TypedPhysicsCommand> m_physicsCommands;
  std::vector<TypedMotionCommand> m_motionCommands;
  mutable std::vector<Command> m_testPlannedCommands;
  CommandStreamStats m_lastCommandStreamStats;
  std::unordered_set<KX_GameObject *> m_removedObjectsDuringFlush;
  std::unordered_map<AddObjectSourceStateKey, bool, AddObjectSourceStateKeyHash>
      m_addObjectSourceInactiveCache;
  std::unordered_set<MaterialParameterDefaultStateKey, MaterialParameterDefaultStateKeyHash>
      m_initializedMaterialParameterDefaults;
  std::thread::id m_mainThreadId;
  LN_Manager *m_logicManager = nullptr;
  uint64_t m_nextRecordSequence = 1;
  bool m_isRecording = false;
  bool m_isFlushing = false;
  bool m_allowWorkerRecording = false;
  bool m_typedCommandStreamsEnabled = true;
  bool m_commandStreamsSorted = true;
  uint32_t m_coalescibleCommandCount = 0;
};
