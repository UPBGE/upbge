/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Snapshot.cpp
 *  \ingroup logicnodes
 */

#include "LN_Snapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "DNA_light_types.h"

#include "DEV_Joystick.h"
#include "EXP_Value.h"
#include "KX_Globals.h"
#include "KX_GameObject.h"
#include "KX_KetsjiEngine.h"
#include "KX_Light.h"
#include "KX_Scene.h"
#include "LN_InputMap.h"
#include "LN_Program.h"
#include "MT_Matrix3x3.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_ICanvas.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

namespace {
bool is_number_or_text(EXP_Value *property)
{
  const int type = property->GetValueType();
  return type == VALUE_BOOL_TYPE || type == VALUE_INT_TYPE || type == VALUE_FLOAT_TYPE ||
         type == VALUE_STRING_TYPE;
}

static_assert(LN_MAX_GAMEPADS <= JOYINDEX_MAX);
static_assert(LN_MAX_GAMEPAD_AXES <= JOYAXIS_MAX);
static_assert(LN_MAX_GAMEPAD_BUTTONS <= JOYBUT_MAX);

constexpr LN_SnapshotChannelMask SNAPSHOT_OBJECT_STATE_CHANNELS =
    LN_SNAPSHOT_CHANNEL_TRANSFORM | LN_SNAPSHOT_CHANNEL_VELOCITY |
    LN_SNAPSHOT_CHANNEL_COLOR | LN_SNAPSHOT_CHANNEL_LIGHT |
    LN_SNAPSHOT_CHANNEL_VISIBILITY | LN_SNAPSHOT_CHANNEL_COLLISION |
    LN_SNAPSHOT_CHANNEL_CHARACTER;

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

int32_t SaturatingAdd(const int32_t a, const int32_t b)
{
  const int64_t sum = int64_t(a) + int64_t(b);
  return int32_t(std::clamp(sum,
                            int64_t(std::numeric_limits<int32_t>::min()),
                            int64_t(std::numeric_limits<int32_t>::max())));
}

int32_t SaturatingSubtract(const int32_t a, const int32_t b)
{
  const int64_t difference = int64_t(a) - int64_t(b);
  return int32_t(std::clamp(difference,
                            int64_t(std::numeric_limits<int32_t>::min()),
                            int64_t(std::numeric_limits<int32_t>::max())));
}

uint64_t UnsignedMagnitude(const int32_t value)
{
  return value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value);
}

int32_t DominantMouseDeltaSince(const SCA_InputEvent &event,
                                size_t first_new_value,
                                const bool has_previous_position,
                                const int32_t previous_position)
{
  if (first_new_value >= event.m_values.size()) {
    return 0;
  }

  /* A known absolute position is the exact origin for newly appended events. On the first sample,
   * the first new event establishes that origin; later events in the same sample still represent
   * real relative motion. */
  int32_t origin = previous_position;
  if (!has_previous_position) {
    origin = event.m_values[first_new_value++];
    if (first_new_value >= event.m_values.size()) {
      return 0;
    }
  }

  int32_t dominant_delta = 0;
  for (size_t index = first_new_value; index < event.m_values.size(); index++) {
    const int32_t delta = SaturatingSubtract(event.m_values[index], origin);
    if (UnsignedMagnitude(delta) > UnsignedMagnitude(dominant_delta)) {
      dominant_delta = delta;
    }
  }
  return dominant_delta;
}

int32_t WheelDeltaSince(const SCA_InputEvent &event, const size_t first_new_value)
{
  int32_t delta = 0;
  for (size_t index = first_new_value; index < event.m_values.size(); index++) {
    delta = SaturatingAdd(delta, event.m_values[index]);
  }
  return delta;
}

float NormalizedSignedGamepadAxisMagnitude(const int32_t axis_value)
{
  return std::fabs(std::clamp(float(axis_value) / 32767.0f, -1.0f, 1.0f));
}

void ResetObjectSnapshot(LN_ObjectSnapshot &object)
{
  KX_GameObject *owner = object.object;
  object = LN_ObjectSnapshot();
  object.object = owner;
}

}  // namespace

float LN_NormalizeGamepadTriggerValue(const int32_t axis_value)
{
  return std::clamp(float(axis_value) / 32767.0f, 0.0f, 1.0f);
}

bool LN_GamepadHasActiveInput(const LN_GamepadSnapshot &gamepad, const float threshold)
{
  if (!gamepad.connected) {
    return false;
  }
  if (std::any_of(gamepad.buttons.begin(), gamepad.buttons.end(), [](const uint8_t button) {
        return button != 0;
      }))
  {
    return true;
  }
  const float clamped_threshold = std::max(0.0f, threshold);
  for (size_t axis_index = 0; axis_index < gamepad.axes.size(); axis_index++) {
    const float value = axis_index == 4 || axis_index == 5 ?
                            LN_NormalizeGamepadTriggerValue(gamepad.axes[axis_index]) :
                            NormalizedSignedGamepadAxisMagnitude(gamepad.axes[axis_index]);
    if (value > 0.0f && value >= clamped_threshold) {
      return true;
    }
  }
  return false;
}

bool LN_GetGamepadControlState(const LN_GamepadSnapshot &gamepad,
                               const int32_t control_index,
                               const bool is_trigger,
                               const int32_t input_status)
{
  if (control_index < 0) {
    return false;
  }

  if (is_trigger) {
    if (control_index >= int32_t(gamepad.axes_active.size())) {
      return false;
    }
    const size_t index = size_t(control_index);
    if (input_status == SCA_InputEvent::JUSTACTIVATED) {
      return gamepad.axes_pressed[index] != 0;
    }
    if (input_status == SCA_InputEvent::JUSTRELEASED) {
      return gamepad.axes_released[index] != 0;
    }
    return gamepad.connected && gamepad.axes_active[index] != 0;
  }

  if (control_index >= int32_t(gamepad.buttons.size())) {
    return false;
  }
  const size_t index = size_t(control_index);
  if (input_status == SCA_InputEvent::JUSTACTIVATED) {
    return gamepad.pressed[index] != 0;
  }
  if (input_status == SCA_InputEvent::JUSTRELEASED) {
    return gamepad.released[index] != 0;
  }
  return gamepad.connected && gamepad.buttons[index] != 0;
}

bool LN_TryGetGamepadTriggerStrength(const LN_GamepadSnapshot &gamepad,
                                     const int32_t axis_index,
                                     float &r_strength)
{
  if (!gamepad.connected || axis_index < 0 || axis_index >= int32_t(gamepad.axes.size())) {
    return false;
  }
  r_strength = LN_NormalizeGamepadTriggerValue(gamepad.axes[size_t(axis_index)]);
  return true;
}

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
    m_hasPreviousMousePosition = false;
    m_previousMouseX = 0;
    m_previousMouseY = 0;
    for (int32_t gamepad_index = 0; gamepad_index < LN_MAX_GAMEPADS; gamepad_index++) {
      m_gamepads[gamepad_index] = LN_GamepadSnapshot();
      m_previousAxesActive[gamepad_index].fill(0);
      m_previousButtons[gamepad_index].fill(0);
      m_previousGamepadSerial[gamepad_index] = 0;
      m_previousAxesPressedRevision[gamepad_index].fill(0);
      m_previousAxesReleasedRevision[gamepad_index].fill(0);
      m_previousButtonsPressedRevision[gamepad_index].fill(0);
      m_previousButtonsReleasedRevision[gamepad_index].fill(0);
    }
    m_captureInputDevice = nullptr;
    m_inputClearRevision = 0;
    m_inputQueueCursors.clear();
    m_inputValueCursors.clear();
    m_textCursor = 0;
    return;
  }

  const bool reset_input_cursors =
      m_captureInputDevice != input_device ||
      m_inputClearRevision != input_device->GetClearRevision() ||
      m_inputQueueCursors.size() != SCA_IInputDevice::MAX_KEYS ||
      m_inputValueCursors.size() != SCA_IInputDevice::MAX_KEYS;
  if (reset_input_cursors) {
    m_captureInputDevice = input_device;
    m_inputClearRevision = input_device->GetClearRevision();
    m_inputQueueCursors.assign(SCA_IInputDevice::MAX_KEYS, 0);
    /* Every SCA input value vector keeps one baseline element across ClearInputs(). */
    m_inputValueCursors.assign(SCA_IInputDevice::MAX_KEYS, 1);
    m_textCursor = 0;
  }

  const std::wstring &device_text = input_device->GetText();
  const size_t first_new_text = std::min(m_textCursor, device_text.size());
  m_textInput.assign(device_text.begin() + first_new_text, device_text.end());
  m_textCursor = device_text.size();

  m_entries.resize(SCA_IInputDevice::MAX_KEYS);
  for (int32_t input_code = 0; input_code < SCA_IInputDevice::MAX_KEYS; input_code++) {
    SCA_InputEvent &event = input_device->GetInput(
        SCA_IInputDevice::SCA_EnumInputs(input_code));
    LN_InputSnapshotEntry &entry = m_entries[input_code];
    /* NONE/ACTIVE describe the state at the end of the sample. Edge states describe every
     * transition observed during it. Using Find() for the steady states would incorrectly leave
     * ACTIVE true after a press-and-release pair received in one outer frame. */
    for (const SCA_InputEvent::SCA_EnumInputs status : {SCA_InputEvent::NONE,
                                                        SCA_InputEvent::ACTIVE})
    {
      if (event.End(status)) {
        entry.status_mask |= uint8_t(1u << status);
      }
    }
    const size_t first_new_queue = std::min(m_inputQueueCursors[input_code],
                                            event.m_queue.size());
    bool has_new_activation = false;
    for (size_t queue_index = first_new_queue; queue_index < event.m_queue.size(); queue_index++) {
      const SCA_InputEvent::SCA_EnumInputs status = event.m_queue[queue_index];
      if (status == SCA_InputEvent::JUSTACTIVATED ||
          status == SCA_InputEvent::JUSTRELEASED)
      {
        entry.status_mask |= uint8_t(1u << status);
        has_new_activation |= status == SCA_InputEvent::JUSTACTIVATED;
      }
    }
    m_inputQueueCursors[input_code] = event.m_queue.size();
    if (!event.m_values.empty()) {
      entry.value = event.m_values.back();
    }
    entry.unicode = has_new_activation ? event.m_unicode : 0;
  }

  m_mouse.x = GetValue(SCA_IInputDevice::MOUSEX);
  m_mouse.y = GetValue(SCA_IInputDevice::MOUSEY);
  m_mouse.has_position = m_entries.size() > SCA_IInputDevice::MOUSEY;
  const bool has_previous_mouse_position = m_hasPreviousMousePosition;
  if (has_previous_mouse_position && m_mouse.has_position) {
    m_mouse.delta_x = SaturatingSubtract(m_mouse.x, m_previousMouseX);
    m_mouse.delta_y = SaturatingSubtract(m_mouse.y, m_previousMouseY);

    const SCA_InputEvent &mouse_x_event = input_device->GetInput(SCA_IInputDevice::MOUSEX);
    const SCA_InputEvent &mouse_y_event = input_device->GetInput(SCA_IInputDevice::MOUSEY);
    const int32_t frame_delta_x = DominantMouseDeltaSince(
        mouse_x_event,
        std::min(m_inputValueCursors[SCA_IInputDevice::MOUSEX], mouse_x_event.m_values.size()),
        true,
        m_previousMouseX);
    const int32_t frame_delta_y = DominantMouseDeltaSince(
        mouse_y_event,
        std::min(m_inputValueCursors[SCA_IInputDevice::MOUSEY], mouse_y_event.m_values.size()),
        true,
        m_previousMouseY);
    if (UnsignedMagnitude(frame_delta_x) > UnsignedMagnitude(m_mouse.delta_x)) {
      m_mouse.delta_x = frame_delta_x;
    }
    if (UnsignedMagnitude(frame_delta_y) > UnsignedMagnitude(m_mouse.delta_y)) {
      m_mouse.delta_y = frame_delta_y;
    }
  }
  else if (m_mouse.has_position) {
    /* A first/reconnected sample establishes the absolute baseline, but multiple events already
     * received during that sample still represent real relative motion. */
    const SCA_InputEvent &mouse_x_event = input_device->GetInput(SCA_IInputDevice::MOUSEX);
    const SCA_InputEvent &mouse_y_event = input_device->GetInput(SCA_IInputDevice::MOUSEY);
    m_mouse.delta_x = DominantMouseDeltaSince(
        mouse_x_event,
        std::min(m_inputValueCursors[SCA_IInputDevice::MOUSEX], mouse_x_event.m_values.size()),
        false,
        0);
    m_mouse.delta_y = DominantMouseDeltaSince(
        mouse_y_event,
        std::min(m_inputValueCursors[SCA_IInputDevice::MOUSEY], mouse_y_event.m_values.size()),
        false,
        0);
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

  const SCA_InputEvent &wheel_up = input_device->GetInput(SCA_IInputDevice::WHEELUPMOUSE);
  const SCA_InputEvent &wheel_down = input_device->GetInput(SCA_IInputDevice::WHEELDOWNMOUSE);
  m_mouse.wheel_delta = SaturatingAdd(
      WheelDeltaSince(
          wheel_up,
          std::min(m_inputValueCursors[SCA_IInputDevice::WHEELUPMOUSE], wheel_up.m_values.size())),
      WheelDeltaSince(
          wheel_down,
          std::min(m_inputValueCursors[SCA_IInputDevice::WHEELDOWNMOUSE],
                   wheel_down.m_values.size())));

  for (int32_t input_code = 0; input_code < SCA_IInputDevice::MAX_KEYS; input_code++) {
    m_inputValueCursors[input_code] = input_device->GetInput(
        SCA_IInputDevice::SCA_EnumInputs(input_code)).m_values.size();
  }

  for (int32_t gamepad_index = 0; gamepad_index < LN_MAX_GAMEPADS; gamepad_index++) {
    LN_GamepadSnapshot &gamepad = m_gamepads[gamepad_index];
    gamepad = LN_GamepadSnapshot();
    DEV_Joystick *joystick = DEV_Joystick::GetInstance(short(gamepad_index));
    if (joystick == nullptr || !joystick->Connected()) {
      /* A disconnect is also a release transition. Keep it observable even though current gamepad
       * state is disconnected; LN_GetGamepadControlState permits edge reads in that state. */
      gamepad.axes_released = m_previousAxesActive[gamepad_index];
      gamepad.released = m_previousButtons[gamepad_index];
      m_previousAxesActive[gamepad_index].fill(0);
      m_previousButtons[gamepad_index].fill(0);
      m_previousGamepadSerial[gamepad_index] = 0;
      m_previousAxesPressedRevision[gamepad_index].fill(0);
      m_previousAxesReleasedRevision[gamepad_index].fill(0);
      m_previousButtonsPressedRevision[gamepad_index].fill(0);
      m_previousButtonsReleasedRevision[gamepad_index].fill(0);
      continue;
    }

    const uint64_t gamepad_serial = joystick->GetInstanceSerial();
    if (m_previousGamepadSerial[gamepad_index] != gamepad_serial) {
      m_previousGamepadSerial[gamepad_index] = gamepad_serial;
      m_previousAxesActive[gamepad_index].fill(0);
      m_previousButtons[gamepad_index].fill(0);
      /* Establish a revision baseline for a newly observed controller. Consumers enabled after a
       * controller has been in use must not replay its entire historical event count. Held state
       * below still produces the expected initial Active/Pressed sample. */
      for (int32_t axis_index = 0; axis_index < LN_MAX_GAMEPAD_AXES; axis_index++) {
        m_previousAxesPressedRevision[gamepad_index][axis_index] =
            joystick->GetAxisPressRevision(axis_index);
        m_previousAxesReleasedRevision[gamepad_index][axis_index] =
            joystick->GetAxisReleaseRevision(axis_index);
      }
      for (int32_t button_index = 0; button_index < LN_MAX_GAMEPAD_BUTTONS; button_index++) {
        m_previousButtonsPressedRevision[gamepad_index][button_index] =
            joystick->GetButtonPressRevision(button_index);
        m_previousButtonsReleasedRevision[gamepad_index][button_index] =
            joystick->GetButtonReleaseRevision(button_index);
      }
    }

    gamepad.connected = true;
    gamepad.name = joystick->GetName();
    for (int32_t axis_index = 0; axis_index < LN_MAX_GAMEPAD_AXES; axis_index++) {
      gamepad.axes[axis_index] = joystick->GetAxisPosition(axis_index);
      const bool is_trigger_axis = axis_index == 4 || axis_index == 5;
      const float normalized = is_trigger_axis ?
                                   LN_NormalizeGamepadTriggerValue(gamepad.axes[axis_index]) :
                                   NormalizedSignedGamepadAxisMagnitude(gamepad.axes[axis_index]);
      const uint8_t active = normalized >= 0.1f ? 1 : 0;
      const uint8_t previous = m_previousAxesActive[gamepad_index][axis_index];
      const uint64_t pressed_revision = joystick->GetAxisPressRevision(axis_index);
      const uint64_t released_revision = joystick->GetAxisReleaseRevision(axis_index);
      gamepad.axes_active[axis_index] = active;
      gamepad.axes_pressed[axis_index] =
          (active && !previous) ||
                  pressed_revision != m_previousAxesPressedRevision[gamepad_index][axis_index] ?
              1 :
              0;
      gamepad.axes_released[axis_index] =
          (!active && previous) ||
                  released_revision != m_previousAxesReleasedRevision[gamepad_index][axis_index] ?
              1 :
              0;
      m_previousAxesActive[gamepad_index][axis_index] = active;
      m_previousAxesPressedRevision[gamepad_index][axis_index] = pressed_revision;
      m_previousAxesReleasedRevision[gamepad_index][axis_index] = released_revision;
    }
    for (int32_t button_index = 0; button_index < LN_MAX_GAMEPAD_BUTTONS; button_index++) {
      const uint8_t active = joystick->aButtonPressIsPositive(button_index) ? 1 : 0;
      const uint8_t previous = m_previousButtons[gamepad_index][button_index];
      const uint64_t pressed_revision = joystick->GetButtonPressRevision(button_index);
      const uint64_t released_revision = joystick->GetButtonReleaseRevision(button_index);
      gamepad.buttons[button_index] = active;
      gamepad.pressed[button_index] =
          (active && !previous) ||
                  pressed_revision != m_previousButtonsPressedRevision[gamepad_index][button_index] ?
              1 :
              0;
      gamepad.released[button_index] =
          (!active && previous) ||
                  released_revision != m_previousButtonsReleasedRevision[gamepad_index][button_index] ?
              1 :
              0;
      if (LN_InputMap::Get().HidesJoinPress(gamepad_index, button_index)) {
        /* The press that made this gamepad join (Press to Join), until let go. */
        gamepad.buttons[button_index] = 0;
        gamepad.pressed[button_index] = 0;
        gamepad.released[button_index] = 0;
      }
      /* As shown: a hidden press let go is no release. */
      m_previousButtons[gamepad_index][button_index] = gamepad.buttons[button_index];
      m_previousButtonsPressedRevision[gamepad_index][button_index] = pressed_revision;
      m_previousButtonsReleasedRevision[gamepad_index][button_index] = released_revision;
    }
  }
}

void LN_InputSnapshot::CopyFrom(const LN_InputSnapshot &other)
{
  m_entries = other.m_entries;
  m_gamepads = other.m_gamepads;
  m_previousAxesActive = other.m_previousAxesActive;
  m_previousButtons = other.m_previousButtons;
  m_previousGamepadSerial = other.m_previousGamepadSerial;
  m_previousAxesPressedRevision = other.m_previousAxesPressedRevision;
  m_previousAxesReleasedRevision = other.m_previousAxesReleasedRevision;
  m_previousButtonsPressedRevision = other.m_previousButtonsPressedRevision;
  m_previousButtonsReleasedRevision = other.m_previousButtonsReleasedRevision;
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
  m_captureInputDevice = other.m_captureInputDevice;
  m_inputClearRevision = other.m_inputClearRevision;
  m_inputQueueCursors = other.m_inputQueueCursors;
  m_inputValueCursors = other.m_inputValueCursors;
  m_textCursor = other.m_textCursor;
}

void LN_InputSnapshot::AccumulateFrame(const LN_InputSnapshot &frame)
{
  constexpr uint8_t transient_mask = uint8_t((1u << SCA_InputEvent::JUSTACTIVATED) |
                                             (1u << SCA_InputEvent::JUSTRELEASED));
  constexpr uint8_t steady_mask = uint8_t((1u << SCA_InputEvent::NONE) |
                                          (1u << SCA_InputEvent::ACTIVE));

  if (m_entries.size() < frame.m_entries.size()) {
    m_entries.resize(frame.m_entries.size());
  }
  for (size_t index = 0; index < m_entries.size(); index++) {
    const LN_InputSnapshotEntry incoming = index < frame.m_entries.size() ?
                                               frame.m_entries[index] :
                                               LN_InputSnapshotEntry();
    LN_InputSnapshotEntry &pending = m_entries[index];
    pending.status_mask = uint8_t((pending.status_mask & transient_mask) |
                                  (incoming.status_mask & (steady_mask | transient_mask)));
    pending.value = incoming.value;
    if (incoming.unicode != 0) {
      pending.unicode = incoming.unicode;
    }
    else if ((pending.status_mask & uint8_t(1u << SCA_InputEvent::JUSTACTIVATED)) == 0) {
      pending.unicode = 0;
    }
  }

  for (size_t gamepad_index = 0; gamepad_index < m_gamepads.size(); gamepad_index++) {
    LN_GamepadSnapshot &pending = m_gamepads[gamepad_index];
    const LN_GamepadSnapshot &incoming = frame.m_gamepads[gamepad_index];
    const auto pressed = pending.pressed;
    const auto released = pending.released;
    const auto axes_pressed = pending.axes_pressed;
    const auto axes_released = pending.axes_released;
    pending = incoming;
    for (size_t index = 0; index < pending.buttons.size(); index++) {
      pending.pressed[index] |= pressed[index];
      pending.released[index] |= released[index];
    }
    for (size_t index = 0; index < pending.axes.size(); index++) {
      pending.axes_pressed[index] |= axes_pressed[index];
      pending.axes_released[index] |= axes_released[index];
    }
  }

  const int32_t accumulated_delta_x = SaturatingAdd(m_mouse.delta_x, frame.m_mouse.delta_x);
  const int32_t accumulated_delta_y = SaturatingAdd(m_mouse.delta_y, frame.m_mouse.delta_y);
  const int32_t accumulated_wheel = SaturatingAdd(m_mouse.wheel_delta,
                                                   frame.m_mouse.wheel_delta);
  m_mouse = frame.m_mouse;
  m_mouse.delta_x = accumulated_delta_x;
  m_mouse.delta_y = accumulated_delta_y;
  m_mouse.wheel_delta = accumulated_wheel;
  m_textInput.append(frame.m_textInput);
  constexpr size_t max_buffered_text_codepoints = 4096;
  if (m_textInput.size() > max_buffered_text_codepoints) {
    m_textInput.erase(0, m_textInput.size() - max_buffered_text_codepoints);
  }

  m_hasDevice = frame.m_hasDevice;
  m_hasCanvas = frame.m_hasCanvas;
  m_canvasWidth = frame.m_canvasWidth;
  m_canvasHeight = frame.m_canvasHeight;
  m_canvasFullscreen = frame.m_canvasFullscreen;
  m_hasCanvasVSyncMode = frame.m_hasCanvasVSyncMode;
  m_canvasVSyncMode = frame.m_canvasVSyncMode;

  /* These histories belong to Capture(). Copying the frame history keeps this snapshot safe if it
   * is ever promoted to a capture source without changing the merge semantics above. */
  m_previousAxesActive = frame.m_previousAxesActive;
  m_previousButtons = frame.m_previousButtons;
  m_previousGamepadSerial = frame.m_previousGamepadSerial;
  m_previousAxesPressedRevision = frame.m_previousAxesPressedRevision;
  m_previousAxesReleasedRevision = frame.m_previousAxesReleasedRevision;
  m_previousButtonsPressedRevision = frame.m_previousButtonsPressedRevision;
  m_previousButtonsReleasedRevision = frame.m_previousButtonsReleasedRevision;
  m_hasPreviousMousePosition = frame.m_hasPreviousMousePosition;
  m_previousMouseX = frame.m_previousMouseX;
  m_previousMouseY = frame.m_previousMouseY;
  m_captureInputDevice = frame.m_captureInputDevice;
  m_inputClearRevision = frame.m_inputClearRevision;
  m_inputQueueCursors = frame.m_inputQueueCursors;
  m_inputValueCursors = frame.m_inputValueCursors;
  m_textCursor = frame.m_textCursor;
}

void LN_InputSnapshot::ClearTransientState()
{
  constexpr uint8_t transient_mask = uint8_t((1u << SCA_InputEvent::JUSTACTIVATED) |
                                             (1u << SCA_InputEvent::JUSTRELEASED));
  for (LN_InputSnapshotEntry &entry : m_entries) {
    entry.status_mask &= uint8_t(~transient_mask);
    entry.unicode = 0;
  }
  for (LN_GamepadSnapshot &gamepad : m_gamepads) {
    gamepad.axes_pressed.fill(0);
    gamepad.axes_released.fill(0);
    gamepad.pressed.fill(0);
    gamepad.released.fill(0);
  }
  m_mouse.delta_x = 0;
  m_mouse.delta_y = 0;
  m_mouse.wheel_delta = 0;
  m_textInput.clear();
}

void LN_InputSnapshot::ConsumePress(const int32_t input_code)
{
  if (input_code >= 0 && input_code < int32_t(m_entries.size())) {
    m_entries[input_code].status_mask &= uint8_t(~(1u << SCA_InputEvent::JUSTACTIVATED));
  }
}

void LN_InputSnapshot::ConsumeGamepadPress(const int32_t gamepad_index, const int32_t button)
{
  if (gamepad_index >= 0 && gamepad_index < LN_MAX_GAMEPADS && button >= 0 &&
      button < LN_MAX_GAMEPAD_BUTTONS)
  {
    m_gamepads[size_t(gamepad_index)].pressed[size_t(button)] = 0;
  }
}

void LN_InputSnapshot::ConsumeStroke(const int32_t input_code)
{
  if (input_code >= 0 && input_code < int32_t(m_entries.size())) {
    LN_InputSnapshotEntry &entry = m_entries[input_code];
    entry.status_mask = uint8_t(1u << SCA_InputEvent::NONE);
    entry.value = 0;
    entry.unicode = 0;
  }
}

void LN_InputSnapshot::ConsumeWheel()
{
  m_mouse.wheel_delta = 0;
  ConsumeStroke(SCA_IInputDevice::WHEELUPMOUSE);
  ConsumeStroke(SCA_IInputDevice::WHEELDOWNMOUSE);
}

void LN_InputSnapshot::SetGamepadForTests(const int32_t gamepad_index,
                                          const LN_GamepadSnapshot &gamepad)
{
  if (gamepad_index >= 0 && gamepad_index < LN_MAX_GAMEPADS) {
    m_gamepads[size_t(gamepad_index)] = gamepad;
  }
}

void LN_InputConsumption::ConsumeStroke(const int32_t input_code, const bool held)
{
  /* Consumed again for the press it started with: it simply goes on. */
  if (IsStrokeConsumed(input_code)) {
    return;
  }
  /* A quick tap can already be up again when the UI sees its press. */
  m_strokes.push_back({input_code, !held, false});
}

void LN_InputConsumption::ConsumePress(const int32_t input_code)
{
  if (!IsPressConsumed(input_code)) {
    m_presses.push_back(input_code);
  }
}

void LN_InputConsumption::ConsumeGamepadPress(const int32_t gamepad_index, const int32_t button)
{
  if (!IsGamepadPressConsumed(gamepad_index, button)) {
    m_gamepadPresses.emplace_back(gamepad_index, button);
  }
}

void LN_InputConsumption::ConsumeWheel()
{
  m_wheel = true;
}

void LN_InputConsumption::NoteSample(const LN_InputSnapshot &sample)
{
  for (auto stroke = m_strokes.begin(); stroke != m_strokes.end();) {
    if (stroke->released &&
        sample.GetStatus(stroke->input_code, SCA_InputEvent::JUSTACTIVATED))
    {
      /* A new press after the release is a new stroke, for game logic unless the UI takes it. */
      stroke = m_strokes.erase(stroke);
      continue;
    }
    if (!sample.GetStatus(stroke->input_code, SCA_InputEvent::ACTIVE)) {
      stroke->released = true;
    }
    ++stroke;
  }
}

void LN_InputConsumption::ApplyStrokesTo(LN_InputSnapshot &snapshot) const
{
  for (const Stroke &stroke : m_strokes) {
    snapshot.ConsumeStroke(stroke.input_code);
  }
}

void LN_InputConsumption::ApplyToFrame(LN_InputSnapshot &snapshot)
{
  for (Stroke &stroke : m_strokes) {
    snapshot.ConsumeStroke(stroke.input_code);
    stroke.hidden_from_frame = stroke.released;
  }
  for (const int32_t input_code : m_presses) {
    snapshot.ConsumePress(input_code);
  }
  for (const auto &[gamepad_index, button] : m_gamepadPresses) {
    snapshot.ConsumeGamepadPress(gamepad_index, button);
  }
  if (m_wheel) {
    snapshot.ConsumeWheel();
  }
}

void LN_InputConsumption::BeginFrame()
{
  m_presses.clear();
  m_gamepadPresses.clear();
  m_wheel = false;
  std::erase_if(m_strokes, [](const Stroke &stroke) { return stroke.hidden_from_frame; });
}

bool LN_InputConsumption::IsStrokeConsumed(const int32_t input_code) const
{
  return std::any_of(m_strokes.begin(), m_strokes.end(), [&](const Stroke &stroke) {
    return stroke.input_code == input_code;
  });
}

bool LN_InputConsumption::IsPressConsumed(const int32_t input_code) const
{
  return std::find(m_presses.begin(), m_presses.end(), input_code) != m_presses.end();
}

bool LN_InputConsumption::IsGamepadPressConsumed(const int32_t gamepad_index,
                                                 const int32_t button) const
{
  return std::find(m_gamepadPresses.begin(),
                   m_gamepadPresses.end(),
                   std::make_pair(gamepad_index, button)) != m_gamepadPresses.end();
}

void LN_InputConsumption::Clear()
{
  m_strokes.clear();
  m_presses.clear();
  m_gamepadPresses.clear();
  m_wheel = false;
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

const LN_InputActionState *LN_Snapshot::GetInputActionState(const int32_t action,
                                                            const int32_t user,
                                                            const bool raw) const
{
  if (m_tickContext == nullptr) {
    return nullptr;
  }
  const LN_InputActionTracker *tracker = raw && m_tickContext->raw_input_actions != nullptr ?
                                             m_tickContext->raw_input_actions :
                                             m_tickContext->input_actions;
  return tracker != nullptr ? tracker->Find(action, user) : nullptr;
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
      m_object.world_orientation = LN_Rotation::FromMatrix(gameobj->NodeGetWorldOrientation());
      m_object.local_orientation = LN_Rotation::FromMatrix(gameobj->NodeGetLocalOrientation());
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
      CaptureGameProperties(gameobj, *program);
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

void LN_Snapshot::CaptureGameProperties(KX_GameObject *gameobj, const LN_Program &program)
{
  m_gameProperties.clear();
  const std::vector<LN_GamePropertyRef> &property_refs = program.GetGamePropertyRefs();
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
            /* A vector, list or object is no number: the default, as when the node reads
             * another object. */
            if (is_number_or_text(property)) {
              value.int_value = int32_t(gameobj->GetPropertyNumber(
                  property_ref.name, property_ref.default_value.int_value));
            }
            break;
          case LN_ValueType::Float:
            if (is_number_or_text(property)) {
              value.float_value = gameobj->GetPropertyNumber(
                  property_ref.name, property_ref.default_value.float_value);
            }
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
}

void LN_Snapshot::RefreshGameProperties(KX_GameObject *gameobj, const LN_Program *program)
{
  if (gameobj != nullptr && program != nullptr &&
      (m_captureStats.captured_channels & LN_SNAPSHOT_CHANNEL_GAME_PROPERTY) != 0)
  {
    CaptureGameProperties(gameobj, *program);
  }
}

const LN_Value *LN_Snapshot::GetGamePropertyValue(uint32_t property_ref_index) const
{
  RequireCapturedChannel(LN_SNAPSHOT_CHANNEL_GAME_PROPERTY, "game property");
  if (property_ref_index >= m_gameProperties.size()) {
    return nullptr;
  }
  return &m_gameProperties[property_ref_index];
}
