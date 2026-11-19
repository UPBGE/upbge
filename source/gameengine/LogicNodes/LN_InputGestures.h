/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Input gestures and combos (2026-11-17), once for the runtime kinds and the tests: an input
 * node's Input Type Tap, Double Tap and Hold (Keyboard Key, Mouse Button, Gamepad Button, Input
 * Action), and the Input Combo node (Sequence, Chord). Each works from the input's state in a
 * step: held (active), pressed and released since the last one (a press and release within one
 * step both show). Times are the tick's clock (LN_TickContext::current_time: real time, no
 * timescale: a tap is as quick in slow motion).
 */

#pragma once

#include <cstdint>

/** Input Type's gestures (the input nodes' custom1 & LOGIC_NODE_INPUT_MODE_MASK; 0 Pressed,
 * 1 While Held, 2 Released before). */
enum class LN_InputGesture : int32_t {
  /** Released within Time of its press. */
  Tap = 3,
  /** A second press within Time of the first. */
  DoubleTap = 4,
  /** Held for Time: once a press. */
  Hold = 5,
};

inline bool LN_IsInputGesture(const int32_t mode)
{
  return mode >= int32_t(LN_InputGesture::Tap) && mode <= int32_t(LN_InputGesture::Hold);
}

/** A tiny margin for times worked out from the frame clock's doubles. */
inline constexpr double LN_INPUT_TIME_EPSILON = 1.0e-6;

struct LN_InputGestureState {
  /** When the press being held (or the last one) began; below 0: none seen. */
  double press_time = -1.0;
  /** Double Tap: the first press of a pair; below 0: none waiting. */
  double first_press_time = -1.0;
  bool hold_fired = false;
  /** The tick it was last worked out in and its answer (several reads of a tick agree). */
  uint64_t tick = UINT64_MAX;
  bool fired = false;
};

/** One step of `gesture`: whether it fires now. */
inline bool LN_InputGestureStep(LN_InputGestureState &state,
                                const LN_InputGesture gesture,
                                const double now,
                                const double time,
                                const bool active,
                                const bool pressed,
                                const bool released)
{
  const double limit = time < 0.0 ? 0.0 : time;
  bool fired = false;
  if (pressed) {
    if (gesture == LN_InputGesture::DoubleTap) {
      if (state.first_press_time >= 0.0 &&
          now - state.first_press_time <= limit + LN_INPUT_TIME_EPSILON)
      {
        fired = true;
        state.first_press_time = -1.0;
      }
      else {
        state.first_press_time = now;
      }
    }
    state.press_time = now;
    state.hold_fired = false;
  }
  if (gesture == LN_InputGesture::Tap && released && state.press_time >= 0.0 &&
      now - state.press_time <= limit + LN_INPUT_TIME_EPSILON)
  {
    fired = true;
  }
  if (gesture == LN_InputGesture::Hold && active && state.press_time >= 0.0 &&
      !state.hold_fired && now - state.press_time + LN_INPUT_TIME_EPSILON >= limit)
  {
    fired = true;
    state.hold_fired = true;
  }
  if (released && !active) {
    state.press_time = -1.0;
  }
  return fired;
}

/** Input Combo's Mode (custom1). */
enum class LN_InputComboMode : int32_t { Sequence = 0, Chord = 1 };

struct LN_InputComboState {
  /** Which steps were held in the last step (bits 0-3). */
  uint32_t held = 0;
  /** Sequence: steps pressed in order so far and when the last of them was. */
  int32_t progress = 0;
  double last_time = -1.0;
  /** Chord: the first press since none was held; fired while all stay held. */
  double first_press_time = -1.0;
  bool chord_fired = false;
  uint64_t tick = UINT64_MAX;
  bool fired = false;
};

/** One step of the combo: `linked` the steps that take part (bits 0-3, in order), `held` the
 * ones held now. Whether it is done now; `r_progress` its Progress. */
inline bool LN_InputComboStep(LN_InputComboState &state,
                              const LN_InputComboMode mode,
                              const double now,
                              const double time,
                              const bool strict,
                              const uint32_t linked,
                              uint32_t held,
                              int32_t &r_progress)
{
  const double limit = time < 0.0 ? 0.0 : time;
  held &= linked;
  const uint32_t pressed = held & ~state.held;
  state.held = held;
  int32_t count = 0;
  int32_t order[4] = {};
  for (int32_t step = 0; step < 4; step++) {
    if ((linked & (1u << step)) != 0) {
      order[count++] = step;
    }
  }
  bool fired = false;
  if (mode == LN_InputComboMode::Chord) {
    if (held == 0) {
      state.first_press_time = -1.0;
    }
    else if (pressed != 0 && state.first_press_time < 0.0) {
      state.first_press_time = now;
    }
    if (held != linked) {
      state.chord_fired = false;
    }
    else if (count > 0 && !state.chord_fired && state.first_press_time >= 0.0 &&
             now - state.first_press_time <= limit + LN_INPUT_TIME_EPSILON)
    {
      fired = true;
      state.chord_fired = true;
    }
    int32_t held_count = 0;
    for (int32_t step = 0; step < 4; step++) {
      held_count += (held & (1u << step)) != 0 ? 1 : 0;
    }
    r_progress = held_count;
    return fired;
  }
  /* Sequence: too long since the last step starts it again. */
  if (state.progress > 0 && now - state.last_time > limit + LN_INPUT_TIME_EPSILON) {
    state.progress = 0;
  }
  for (int32_t step = 0; step < 4 && count > 0; step++) {
    if ((pressed & (1u << step)) == 0) {
      continue;
    }
    if (step == order[state.progress]) {
      state.progress++;
      state.last_time = now;
      if (state.progress == count) {
        fired = true;
        state.progress = 0;
      }
    }
    else if (strict) {
      /* A wrong step: again from the start (it may be the first step itself). */
      state.progress = step == order[0] ? 1 : 0;
      state.last_time = now;
    }
  }
  r_progress = state.progress;
  return fired;
}
