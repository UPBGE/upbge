/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Snapshot.h
 *  \ingroup logicnodes
 *
 * Collision design note: `LN_ObjectSnapshot` captures per-object kinematics and collision
 * group/mask metadata, but not contact manifolds. Pairwise overlap conditions (`ObjectsColliding`)
 * query the active `PHY_IPhysicsEnvironment` at evaluation time. A future touch/collision sensor with
 * hit lists, material filters, or pulse modes would likely add a per-frame contact cache fed from the
 * physics step rather than expanding the snapshot struct for every object every frame.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "BLI_assert.h"
#include "LN_Types.h"

#include "MT_Vector4.h"
#include "MT_Vector3.h"

class KX_GameObject;
class LN_Program;
class RAS_ICanvas;
class SCA_IInputDevice;

static constexpr int32_t LN_MAX_GAMEPADS = 8;
static constexpr int32_t LN_MAX_GAMEPAD_AXES = 6;
static constexpr int32_t LN_MAX_GAMEPAD_BUTTONS = 22;

struct LN_InputSnapshotEntry {
  uint8_t status_mask = 0;
  int32_t value = 0;
  uint32_t unicode = 0;
};

struct LN_MouseSnapshot {
  bool has_position = false;
  int32_t x = 0;
  int32_t y = 0;
  int32_t delta_x = 0;
  int32_t delta_y = 0;
  int32_t wheel_delta = 0;
  float normalized_x = 0.0f;
  float normalized_y = 0.0f;
};

struct LN_GamepadSnapshot {
  bool connected = false;
  std::array<int32_t, LN_MAX_GAMEPAD_AXES> axes = {};
  std::array<uint8_t, LN_MAX_GAMEPAD_BUTTONS> buttons = {};
  std::array<uint8_t, LN_MAX_GAMEPAD_BUTTONS> pressed = {};
  std::array<uint8_t, LN_MAX_GAMEPAD_BUTTONS> released = {};
  std::string name;
};

class LN_InputSnapshot {
 public:
  void Capture(SCA_IInputDevice *input_device, RAS_ICanvas *canvas);
  void CopyFrom(const LN_InputSnapshot &other);

  bool HasDevice() const
  {
    return m_hasDevice;
  }

  bool GetStatus(int32_t input_code, int32_t status) const;
  int32_t GetValue(int32_t input_code) const;
  uint32_t GetUnicode(int32_t input_code) const;

  bool HasCanvas() const
  {
    return m_hasCanvas;
  }

  int32_t GetCanvasWidth() const
  {
    return m_canvasWidth;
  }

  int32_t GetCanvasHeight() const
  {
    return m_canvasHeight;
  }

  bool GetCanvasFullscreen() const
  {
    return m_canvasFullscreen;
  }

  bool HasCanvasVSyncMode() const
  {
    return m_hasCanvasVSyncMode;
  }

  int32_t GetCanvasVSyncMode() const
  {
    return m_canvasVSyncMode;
  }

  const LN_MouseSnapshot &GetMouse() const
  {
    return m_mouse;
  }

  const LN_GamepadSnapshot *GetGamepad(int32_t gamepad_index) const;

  const std::wstring &GetTextInput() const
  {
    return m_textInput;
  }

 private:
  std::vector<LN_InputSnapshotEntry> m_entries;
  std::array<LN_GamepadSnapshot, LN_MAX_GAMEPADS> m_gamepads = {};
  std::array<std::array<uint8_t, LN_MAX_GAMEPAD_BUTTONS>, LN_MAX_GAMEPADS> m_previousButtons = {};
  LN_MouseSnapshot m_mouse;
  std::wstring m_textInput;
  bool m_hasDevice = false;
  bool m_hasCanvas = false;
  int32_t m_canvasWidth = 0;
  int32_t m_canvasHeight = 0;
  bool m_canvasFullscreen = false;
  bool m_hasCanvasVSyncMode = false;
  int32_t m_canvasVSyncMode = 0;
  bool m_hasPreviousMousePosition = false;
  int32_t m_previousMouseX = 0;
  int32_t m_previousMouseY = 0;
};

using LN_SnapshotChannelMask = uint32_t;

enum LN_SnapshotChannel : uint32_t {
  LN_SNAPSHOT_CHANNEL_NONE = 0u,
  LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY = 1u << 0,
  LN_SNAPSHOT_CHANNEL_TRANSFORM = 1u << 1,
  LN_SNAPSHOT_CHANNEL_VELOCITY = 1u << 2,
  LN_SNAPSHOT_CHANNEL_COLOR = 1u << 3,
  LN_SNAPSHOT_CHANNEL_LIGHT = 1u << 4,
  LN_SNAPSHOT_CHANNEL_VISIBILITY = 1u << 5,
  LN_SNAPSHOT_CHANNEL_COLLISION = 1u << 6,
  LN_SNAPSHOT_CHANNEL_CHARACTER = 1u << 7,
  LN_SNAPSHOT_CHANNEL_GRAVITY = 1u << 8,
  LN_SNAPSHOT_CHANNEL_TIMING = 1u << 9,
  LN_SNAPSHOT_CHANNEL_GAME_PROPERTY = 1u << 10,
  LN_SNAPSHOT_CHANNEL_TREE_PROPERTY = 1u << 11,
};

inline constexpr LN_SnapshotChannelMask LN_SNAPSHOT_CHANNEL_ALL =
    LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY | LN_SNAPSHOT_CHANNEL_TRANSFORM |
    LN_SNAPSHOT_CHANNEL_VELOCITY | LN_SNAPSHOT_CHANNEL_COLOR | LN_SNAPSHOT_CHANNEL_LIGHT |
    LN_SNAPSHOT_CHANNEL_VISIBILITY | LN_SNAPSHOT_CHANNEL_COLLISION |
    LN_SNAPSHOT_CHANNEL_CHARACTER | LN_SNAPSHOT_CHANNEL_GRAVITY |
    LN_SNAPSHOT_CHANNEL_TIMING | LN_SNAPSHOT_CHANNEL_GAME_PROPERTY |
    LN_SNAPSHOT_CHANNEL_TREE_PROPERTY;

struct LN_TickReadContext {
  const LN_InputSnapshot *input = nullptr;
  uint32_t input_channels = LN_DEP_INPUT_NONE;
  MT_Vector3 gravity = MT_Vector3(0.0f, 0.0f, -9.8f);
  float time_scale = 1.0f;
  float elapsed_time = 0.0f;
  float frame_delta = 0.0f;
  float fps = 0.0f;
  float delta_factor = 1.0f;
  uint64_t tick_index = UINT64_MAX;
  bool has_input = false;
  bool has_timing = false;
  bool has_gravity = false;
};

struct LN_SnapshotCaptureStats {
  LN_SnapshotChannelMask declared_channels = LN_SNAPSHOT_CHANNEL_NONE;
  LN_SnapshotChannelMask captured_channels = LN_SNAPSHOT_CHANNEL_NONE;
  LN_SnapshotChannelMask skipped_channels = LN_SNAPSHOT_CHANNEL_NONE;
  bool used_shared_input = false;
  bool reused_property_storage = true;
};

struct LN_ObjectSnapshot {
  KX_GameObject *object = nullptr;
  MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 local_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 world_orientation = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 local_orientation = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 local_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 linear_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 local_linear_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 angular_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 local_angular_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector4 object_color = MT_Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  MT_Vector4 light_color = MT_Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  float light_power = 1.0f;
  bool light_shadow = false;
  bool visible = false;
  int32_t collision_group = 0;
  bool has_character = false;
  bool character_on_ground = false;
  int32_t character_max_jumps = 0;
  int32_t character_jump_count = 0;
  MT_Vector3 character_gravity = MT_Vector3(0.0f, 0.0f, -9.8f);
  MT_Vector3 character_walk_direction_world = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 character_walk_direction_local = MT_Vector3(0.0f, 0.0f, 0.0f);
};

class LN_Snapshot {
 public:
  void Capture(KX_GameObject *gameobj,
               const LN_Program *program,
               const LN_TickReadContext *tick_context = nullptr,
               float fixed_delta = -1.0f);

  const LN_ObjectSnapshot &GetObjectSnapshot() const
  {
    RequireAnyCapturedChannel(LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY |
                                  LN_SNAPSHOT_CHANNEL_TRANSFORM |
                                  LN_SNAPSHOT_CHANNEL_VELOCITY |
                                  LN_SNAPSHOT_CHANNEL_COLOR | LN_SNAPSHOT_CHANNEL_LIGHT |
                                  LN_SNAPSHOT_CHANNEL_VISIBILITY |
                                  LN_SNAPSHOT_CHANNEL_COLLISION |
                                  LN_SNAPSHOT_CHANNEL_CHARACTER,
                              "object snapshot");
    return m_object;
  }

  const MT_Vector3 &GetGravity() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_GRAVITY, "gravity");
    if (m_tickContext != nullptr && m_tickContext->has_gravity) {
      return m_tickContext->gravity;
    }
    return m_gravity;
  }

  float GetTimeScale() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING, "time scale");
    if (m_tickContext != nullptr && m_tickContext->has_timing) {
      return m_tickContext->time_scale;
    }
    return m_timeScale;
  }

  float GetElapsedTime() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING, "elapsed time");
    if (m_tickContext != nullptr && m_tickContext->has_timing) {
      return m_tickContext->elapsed_time;
    }
    return m_elapsedTime;
  }

  float GetFrameDelta() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING, "frame delta");
    if (m_tickContext != nullptr && m_tickContext->has_timing) {
      return m_tickContext->frame_delta;
    }
    return m_frameDelta;
  }

  float GetFPS() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING, "fps");
    if (m_tickContext != nullptr && m_tickContext->has_timing) {
      return m_tickContext->fps;
    }
    return m_fps;
  }

  float GetDeltaFactor() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING, "delta factor");
    if (m_tickContext != nullptr && m_tickContext->has_timing) {
      return m_tickContext->delta_factor;
    }
    return m_deltaFactor;
  }

  int32_t GetCollisionGroup() const
  {
    RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_COLLISION, "collision group");
    return m_object.collision_group;
  }

  const LN_Value *GetGamePropertyValue(uint32_t property_ref_index) const;

  const LN_InputSnapshot &GetInputSnapshot() const
  {
    BLI_assert_msg(m_tickContext == nullptr || m_tickContext->has_input,
                   "Logic Nodes input read without declared/captured input channel");
    if (m_tickContext != nullptr && m_tickContext->has_input && m_tickContext->input != nullptr) {
      return *m_tickContext->input;
    }
    return m_input;
  }

  bool GetInputStatus(int32_t input_code, int32_t status) const
  {
    return GetInputSnapshot().GetStatus(input_code, status);
  }

  bool HasCapturedChannel(LN_SnapshotChannelMask channel) const
  {
    return (m_captureStats.captured_channels & channel) == channel;
  }

  uint64_t GetTickIndex() const
  {
    return m_tickIndex;
  }

  const LN_SnapshotCaptureStats &GetCaptureStats() const
  {
    return m_captureStats;
  }

 private:
  void RequireCapturedChannel(LN_SnapshotChannelMask channel, const char *description) const
  {
    BLI_assert_msg((m_captureStats.captured_channels & channel) != 0u, description);
  }

  void RequireAnyCapturedChannel(LN_SnapshotChannelMask channels, const char *description) const
  {
    BLI_assert_msg((m_captureStats.captured_channels & channels) != 0u, description);
  }

  LN_ObjectSnapshot m_object;
  MT_Vector3 m_gravity = MT_Vector3(0.0f, 0.0f, -9.8f);
  float m_timeScale = 1.0f;
  float m_elapsedTime = 0.0f;
  float m_frameDelta = 0.0f;
  float m_fps = 0.0f;
  float m_deltaFactor = 1.0f;
  std::vector<LN_Value> m_gameProperties;
  LN_InputSnapshot m_input;
  const LN_TickReadContext *m_tickContext = nullptr;
  uint64_t m_tickIndex = UINT64_MAX;
  LN_SnapshotCaptureStats m_captureStats;
};

LN_SnapshotChannelMask LN_SnapshotChannelMaskFromDependencies(uint32_t dependency_channels);
LN_SnapshotChannelMask LN_SnapshotChannelMaskForProgram(const LN_Program *program);
