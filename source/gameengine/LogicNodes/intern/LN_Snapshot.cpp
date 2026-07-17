/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Snapshot.cpp
 *  \ingroup logicnodes
 */

#include "LN_Snapshot.h"

#include <cstdlib>

#include "DNA_light_types.h"

#include "DEV_Joystick.h"
#include "EXP_Value.h"
#include "KX_Globals.h"
#include "KX_GameObject.h"
#include "KX_KetsjiEngine.h"
#include "KX_Light.h"
#include "KX_Scene.h"
#include "LN_Program.h"
#include "MT_Matrix3x3.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_ICanvas.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

namespace {

constexpr LN_SnapshotChannelMask SNAPSHOT_OBJECT_STATE_CHANNELS =
    LN_SNAPSHOT_CHANNEL_TRANSFORM | LN_SNAPSHOT_CHANNEL_VELOCITY |
    LN_SNAPSHOT_CHANNEL_COLOR | LN_SNAPSHOT_CHANNEL_LIGHT |
    LN_SNAPSHOT_CHANNEL_VISIBILITY | LN_SNAPSHOT_CHANNEL_COLLISION |
    LN_SNAPSHOT_CHANNEL_CHARACTER;

MT_Vector3 OrientationToEuler(const MT_Matrix3x3 &orientation)
{
  MT_Scalar yaw = 0.0f;
  MT_Scalar pitch = 0.0f;
  MT_Scalar roll = 0.0f;
  orientation.getEuler(yaw, pitch, roll);
  return MT_Vector3(yaw, pitch, roll);
}

bool HasSnapshotChannel(const LN_SnapshotChannelMask mask, const LN_SnapshotChannelMask channel)
{
  return (mask & channel) != 0u;
}

int32_t WindowVSyncModeFromSwapInterval(const int interval)
{
  if (interval < 0) {
    return VSYNC_ADAPTIVE;
  }
  return interval > 0 ? VSYNC_ON : VSYNC_OFF;
}

int32_t DominantFrameMouseDelta(const SCA_InputEvent &event)
{
  if (event.m_values.size() <= 1) {
    return 0;
  }

  const int32_t origin = event.m_values.front();
  int32_t dominant_delta = event.m_values.back() - origin;
  for (size_t index = 1; index < event.m_values.size(); index++) {
    const int32_t delta = event.m_values[index] - origin;
    if (std::abs(delta) > std::abs(dominant_delta)) {
      dominant_delta = delta;
    }
  }
  return dominant_delta;
}

void ResetObjectSnapshot(LN_ObjectSnapshot &object)
{
  KX_GameObject *owner = object.object;
  object = LN_ObjectSnapshot();
  object.object = owner;
}

}  // namespace

LN_SnapshotChannelMask LN_SnapshotChannelMaskFromDependencies(const uint32_t dependency_channels)
{
  LN_SnapshotChannelMask mask = LN_SNAPSHOT_CHANNEL_NONE;
  if ((dependency_channels & LN_DEP_SNAPSHOT_OBJECT_GRAPH) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_TRANSFORM) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_TRANSFORM;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_VELOCITY) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_VELOCITY;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_COLOR) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_COLOR;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_LIGHT) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_LIGHT;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_VISIBILITY) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_VISIBILITY;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_COLLISION) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_COLLISION;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_CHARACTER) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_CHARACTER;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_GRAVITY) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_GRAVITY;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_TIMING) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_TIMING;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_GAME_PROPERTY) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_GAME_PROPERTY;
  }
  if ((dependency_channels & LN_DEP_SNAPSHOT_TREE_PROPERTY) != 0u) {
    mask |= LN_SNAPSHOT_CHANNEL_TREE_PROPERTY;
  }
  return mask;
}

LN_SnapshotChannelMask LN_SnapshotChannelMaskForProgram(const LN_Program *program)
{
  if (program == nullptr) {
    return LN_SNAPSHOT_CHANNEL_ALL;
  }
  return LN_SnapshotChannelMaskFromDependencies(program->GetDependencySummary().snapshot_channels);
}

void LN_InputSnapshot::Capture(SCA_IInputDevice *input_device, RAS_ICanvas *canvas)
{
  m_entries.clear();
  m_textInput.clear();
  m_mouse = LN_MouseSnapshot();
  m_hasCanvas = canvas != nullptr;
  m_canvasWidth = canvas != nullptr ? canvas->GetWidth() : 0;
  m_canvasHeight = canvas != nullptr ? canvas->GetHeight() : 0;
  m_canvasFullscreen = canvas != nullptr ? canvas->GetFullScreen() : false;
  m_hasCanvasVSyncMode = false;
  m_canvasVSyncMode = VSYNC_OFF;
  if (canvas != nullptr) {
    int swap_interval = 0;
    if (canvas->GetSwapInterval(swap_interval)) {
      m_hasCanvasVSyncMode = true;
      m_canvasVSyncMode = WindowVSyncModeFromSwapInterval(swap_interval);
    }
  }
  m_hasDevice = input_device != nullptr;
  if (input_device == nullptr) {
    for (LN_GamepadSnapshot &gamepad : m_gamepads) {
      gamepad = LN_GamepadSnapshot();
    }
    return;
  }

  m_textInput = input_device->GetText();

  m_entries.resize(SCA_IInputDevice::MAX_KEYS);
  for (int32_t input_code = 0; input_code < SCA_IInputDevice::MAX_KEYS; input_code++) {
    SCA_InputEvent &event = input_device->GetInput(
        SCA_IInputDevice::SCA_EnumInputs(input_code));
    LN_InputSnapshotEntry &entry = m_entries[input_code];
    for (int32_t status = int32_t(SCA_InputEvent::NONE);
         status <= int32_t(SCA_InputEvent::JUSTRELEASED);
         status++)
    {
      if (event.Find(SCA_InputEvent::SCA_EnumInputs(status))) {
        entry.status_mask |= uint8_t(1u << status);
      }
    }
    if (!event.m_values.empty()) {
      entry.value = event.m_values.back();
    }
    entry.unicode = event.m_unicode;
  }

  m_mouse.x = GetValue(SCA_IInputDevice::MOUSEX);
  m_mouse.y = GetValue(SCA_IInputDevice::MOUSEY);
  m_mouse.has_position = m_entries.size() > SCA_IInputDevice::MOUSEY;
  if (m_hasPreviousMousePosition && m_mouse.has_position) {
    m_mouse.delta_x = m_mouse.x - m_previousMouseX;
    m_mouse.delta_y = m_mouse.y - m_previousMouseY;
  }
  const SCA_InputEvent &mouse_x_event = input_device->GetInput(SCA_IInputDevice::MOUSEX);
  const SCA_InputEvent &mouse_y_event = input_device->GetInput(SCA_IInputDevice::MOUSEY);
  const int32_t frame_delta_x = DominantFrameMouseDelta(mouse_x_event);
  const int32_t frame_delta_y = DominantFrameMouseDelta(mouse_y_event);
  if (std::abs(frame_delta_x) > std::abs(m_mouse.delta_x)) {
    m_mouse.delta_x = frame_delta_x;
  }
  if (std::abs(frame_delta_y) > std::abs(m_mouse.delta_y)) {
    m_mouse.delta_y = frame_delta_y;
  }
  if (m_mouse.has_position) {
    m_previousMouseX = m_mouse.x;
    m_previousMouseY = m_mouse.y;
    m_hasPreviousMousePosition = true;
  }
  if (canvas != nullptr && m_mouse.has_position) {
    m_mouse.normalized_x = canvas->GetMouseNormalizedX(m_mouse.x);
    m_mouse.normalized_y = canvas->GetMouseNormalizedY(m_mouse.y);
  }

  if (GetStatus(SCA_IInputDevice::WHEELUPMOUSE, SCA_InputEvent::JUSTACTIVATED) ||
      GetStatus(SCA_IInputDevice::WHEELUPMOUSE, SCA_InputEvent::ACTIVE))
  {
    m_mouse.wheel_delta++;
  }
  if (GetStatus(SCA_IInputDevice::WHEELDOWNMOUSE, SCA_InputEvent::JUSTACTIVATED) ||
      GetStatus(SCA_IInputDevice::WHEELDOWNMOUSE, SCA_InputEvent::ACTIVE))
  {
    m_mouse.wheel_delta--;
  }

  for (int32_t gamepad_index = 0; gamepad_index < LN_MAX_GAMEPADS; gamepad_index++) {
    LN_GamepadSnapshot &gamepad = m_gamepads[gamepad_index];
    gamepad = LN_GamepadSnapshot();
    DEV_Joystick *joystick = DEV_Joystick::GetInstance(short(gamepad_index));
    if (joystick == nullptr || !joystick->Connected()) {
      m_previousButtons[gamepad_index].fill(0);
      continue;
    }

    gamepad.connected = true;
    gamepad.name = joystick->GetName();
    for (int32_t axis_index = 0; axis_index < LN_MAX_GAMEPAD_AXES; axis_index++) {
      gamepad.axes[axis_index] = joystick->GetAxisPosition(axis_index);
    }
    for (int32_t button_index = 0; button_index < LN_MAX_GAMEPAD_BUTTONS; button_index++) {
      const uint8_t active = joystick->aButtonPressIsPositive(button_index) ? 1 : 0;
      const uint8_t previous = m_previousButtons[gamepad_index][button_index];
      gamepad.buttons[button_index] = active;
      gamepad.pressed[button_index] = (active && !previous) ? 1 : 0;
      gamepad.released[button_index] = (!active && previous) ? 1 : 0;
      m_previousButtons[gamepad_index][button_index] = active;
    }
  }
}

void LN_InputSnapshot::CopyFrom(const LN_InputSnapshot &other)
{
  m_entries = other.m_entries;
  m_gamepads = other.m_gamepads;
  m_previousButtons = other.m_previousButtons;
  m_mouse = other.m_mouse;
  m_textInput = other.m_textInput;
  m_hasDevice = other.m_hasDevice;
  m_hasCanvas = other.m_hasCanvas;
  m_canvasWidth = other.m_canvasWidth;
  m_canvasHeight = other.m_canvasHeight;
  m_canvasFullscreen = other.m_canvasFullscreen;
  m_hasCanvasVSyncMode = other.m_hasCanvasVSyncMode;
  m_canvasVSyncMode = other.m_canvasVSyncMode;
  m_hasPreviousMousePosition = other.m_hasPreviousMousePosition;
  m_previousMouseX = other.m_previousMouseX;
  m_previousMouseY = other.m_previousMouseY;
}

bool LN_InputSnapshot::GetStatus(const int32_t input_code, const int32_t status) const
{
  if (!m_hasDevice || input_code < 0 || status < int32_t(SCA_InputEvent::NONE) ||
      status > int32_t(SCA_InputEvent::JUSTRELEASED) ||
      input_code >= int32_t(m_entries.size()))
  {
    return false;
  }
  return (m_entries[input_code].status_mask & uint8_t(1u << status)) != 0;
}

int32_t LN_InputSnapshot::GetValue(const int32_t input_code) const
{
  if (!m_hasDevice || input_code < 0 || input_code >= int32_t(m_entries.size())) {
    return 0;
  }
  return m_entries[input_code].value;
}

uint32_t LN_InputSnapshot::GetUnicode(const int32_t input_code) const
{
  if (!m_hasDevice || input_code < 0 || input_code >= int32_t(m_entries.size())) {
    return 0;
  }
  return m_entries[input_code].unicode;
}

const LN_GamepadSnapshot *LN_InputSnapshot::GetGamepad(const int32_t gamepad_index) const
{
  if (gamepad_index < 0 || gamepad_index >= LN_MAX_GAMEPADS) {
    return nullptr;
  }
  return &m_gamepads[gamepad_index];
}

void LN_Snapshot::Capture(KX_GameObject *gameobj,
                          const LN_Program *program,
                          const LN_TickReadContext *tick_context,
                          const float fixed_delta)
{
  const LN_SnapshotChannelMask declared_mask = LN_SnapshotChannelMaskForProgram(program);
  const size_t previous_property_capacity = m_gameProperties.capacity();

  m_tickContext = tick_context;
  m_tickIndex = tick_context != nullptr ? tick_context->tick_index : UINT64_MAX;
  m_captureStats = LN_SnapshotCaptureStats();
  m_captureStats.declared_channels = declared_mask;
  m_captureStats.used_shared_input = tick_context != nullptr && tick_context->has_input &&
                                     tick_context->input != nullptr;

  m_object.object = gameobj;
  ResetObjectSnapshot(m_object);
  m_gravity = MT_Vector3(0.0f, 0.0f, -9.8f);
  m_timeScale = 1.0f;
  m_elapsedTime = 0.0f;
  m_frameDelta = 0.0f;
  m_fps = 0.0f;
  m_deltaFactor = 1.0f;
  m_gameProperties.clear();

  if (tick_context != nullptr && tick_context->has_timing) {
    m_timeScale = tick_context->time_scale;
    m_elapsedTime = tick_context->elapsed_time;
    m_frameDelta = tick_context->frame_delta;
    m_fps = tick_context->fps;
    m_deltaFactor = tick_context->delta_factor;
  }
  else if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_TIMING)) {
    if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
      m_timeScale = float(engine->GetTimeScale());
      m_elapsedTime = float(engine->GetFrameTime());
      m_fps = float(engine->GetAverageFrameRate());
      m_frameDelta = fixed_delta >= 0.0f ? fixed_delta : ((m_fps > 0.0f) ? (1.0f / m_fps) : 0.0f);
      m_deltaFactor = m_frameDelta * 60.0f;
    }
  }

  if (tick_context != nullptr && tick_context->has_gravity) {
    m_gravity = tick_context->gravity;
  }
  else if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_GRAVITY) && gameobj != nullptr) {
    if (KX_Scene *scene = gameobj->GetScene()) {
      m_gravity = scene->GetGravity();
    }
  }

  if (tick_context == nullptr || (!tick_context->has_input && tick_context->input_channels !=
                                                               LN_DEP_INPUT_NONE))
  {
    if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
      m_input.Capture(engine->GetInputDevice(), engine->GetCanvas());
    }
    else {
      m_input.Capture(nullptr, nullptr);
    }
  }

  if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_TIMING)) {
    m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_TIMING;
  }
  if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_GRAVITY)) {
    m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_GRAVITY;
  }
  if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY)) {
    m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY;
  }

  if (gameobj == nullptr) {
    m_captureStats.captured_channels |= declared_mask & SNAPSHOT_OBJECT_STATE_CHANNELS;
  }
  else {
    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_TRANSFORM)) {
      m_object.world_position = gameobj->NodeGetWorldPosition();
      m_object.local_position = gameobj->NodeGetLocalPosition();
      m_object.world_orientation = OrientationToEuler(gameobj->NodeGetWorldOrientation());
      m_object.local_orientation = OrientationToEuler(gameobj->NodeGetLocalOrientation());
      m_object.world_scale = gameobj->NodeGetWorldScaling();
      m_object.local_scale = gameobj->NodeGetLocalScaling();
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_TRANSFORM;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_VELOCITY)) {
      m_object.linear_velocity = gameobj->GetLinearVelocity(false);
      m_object.local_linear_velocity = gameobj->GetLinearVelocity(true);
      m_object.angular_velocity = gameobj->GetAngularVelocity(false);
      m_object.local_angular_velocity = gameobj->GetAngularVelocity(true);
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_VELOCITY;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_COLOR)) {
      m_object.object_color = gameobj->GetObjectColor();
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_COLOR;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_LIGHT)) {
      if (KX_LightObject *light_object = dynamic_cast<KX_LightObject *>(gameobj)) {
        if (blender::Light *light = light_object->GetLight()) {
          m_object.light_color = MT_Vector4(light->r, light->g, light->b, 1.0f);
          m_object.light_power = light->energy;
          m_object.light_shadow = (light->mode & blender::LA_SHADOW) != 0;
        }
      }
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_LIGHT;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_VISIBILITY)) {
      m_object.visible = gameobj->GetVisible();
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_VISIBILITY;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_COLLISION)) {
      m_object.collision_group = gameobj->GetCollisionGroup();
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_COLLISION;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_CHARACTER)) {
      m_object.character_gravity = m_gravity;
      if (KX_Scene *scene = gameobj->GetScene()) {
        if (PHY_IPhysicsEnvironment *environment = scene->GetPhysicsEnvironment()) {
          if (PHY_ICharacter *character = environment->GetCharacterController(gameobj)) {
            const MT_Vector3 walk_direction = character->GetWalkDirection();
            m_object.has_character = true;
            m_object.character_on_ground = character->OnGround();
            m_object.character_max_jumps = character->GetMaxJumps();
            m_object.character_jump_count = character->GetJumpCount();
            m_object.character_gravity = character->GetGravity();
            m_object.character_walk_direction_world = walk_direction;
            m_object.character_walk_direction_local =
                gameobj->NodeGetWorldOrientation().transposed() * walk_direction;
          }
        }
      }
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_CHARACTER;
    }

    if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_GAME_PROPERTY) &&
        program != nullptr)
    {
      const std::vector<LN_GamePropertyRef> &property_refs = program->GetGamePropertyRefs();
      m_gameProperties.reserve(property_refs.size());
      for (const LN_GamePropertyRef &property_ref : property_refs) {
        LN_Value value = property_ref.default_value;
        value.type = property_ref.value_type;

        if (!property_ref.name.empty()) {
          EXP_Value *property = gameobj->GetProperty(property_ref.name);
          value.exists = property != nullptr;
          if (property != nullptr) {
            switch (property_ref.value_type) {
              case LN_ValueType::Bool:
                value.bool_value = gameobj->GetPropertyNumber(
                                       property_ref.name,
                                       property_ref.default_value.bool_value ? 1.0f : 0.0f) !=
                                   0.0f;
                break;
              case LN_ValueType::Int:
                value.int_value = int32_t(gameobj->GetPropertyNumber(
                    property_ref.name, property_ref.default_value.int_value));
                break;
              case LN_ValueType::Float:
                value.float_value = gameobj->GetPropertyNumber(
                    property_ref.name, property_ref.default_value.float_value);
                break;
              case LN_ValueType::String:
                value.string_value = property->GetText();
                break;
              case LN_ValueType::Vector:
              case LN_ValueType::Vector4:
              case LN_ValueType::Matrix:
              case LN_ValueType::Color:
              case LN_ValueType::Rotation:
              case LN_ValueType::ObjectRef:
              case LN_ValueType::SceneRef:
              case LN_ValueType::CollectionRef:
              case LN_ValueType::DatablockRef:
              case LN_ValueType::List:
              case LN_ValueType::Dict:
              case LN_ValueType::Generic:
              case LN_ValueType::None:
                break;
            }
          }
        }

        m_gameProperties.push_back(value);
      }
      m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_GAME_PROPERTY;
    }
  }

  if (HasSnapshotChannel(declared_mask, LN_SNAPSHOT_CHANNEL_TREE_PROPERTY)) {
    m_captureStats.captured_channels |= LN_SNAPSHOT_CHANNEL_TREE_PROPERTY;
  }

  m_captureStats.skipped_channels = LN_SNAPSHOT_CHANNEL_ALL & ~m_captureStats.captured_channels;
  m_captureStats.reused_property_storage =
      m_gameProperties.capacity() == previous_property_capacity;
}

const LN_Value *LN_Snapshot::GetGamePropertyValue(uint32_t property_ref_index) const
{
  RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_GAME_PROPERTY, "game property");
  if (property_ref_index >= m_gameProperties.size()) {
    return nullptr;
  }
  return &m_gameProperties[property_ref_index];
}
