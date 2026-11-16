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
 * Contributor(s): lordloki.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file DEV_JoystickResponse.h
 *  \ingroup device
 *
 * Response curves applied to raw gamepad axis values when they are read. Pure functions: no
 * state and no SDL calls, so they cost nothing unless a caller asks for a value.
 */

#pragma once

#include <cmath>

/* Defaults. A 12 % inner deadzone absorbs the rest offset of worn sticks without costing fine-aim
 * resolution on new ones. A 95 % outer edge lets every pad reach full deflection in every
 * direction. Triggers rest on a mechanical stop, so they need far less deadzone than sticks. */
constexpr float DEV_STICK_DEADZONE_DEFAULT = 0.12f;
constexpr float DEV_STICK_OUTER_DEFAULT = 0.95f;
constexpr float DEV_STICK_EXPONENT_DEFAULT = 1.0f;
constexpr float DEV_TRIGGER_DEADZONE_DEFAULT = 0.05f;

/**
 * Scaled radial deadzone for one stick.
 *
 * Radial: the threshold applies to the length of the (x, y) vector. Per-axis thresholds carve a
 * cross through the stick's disc, so a near-diagonal push snaps to a cardinal direction.
 *
 * Scaled: lengths in [inner, outer] are remapped to [0, 1], so the output starts just above 0 at
 * the deadzone edge instead of jumping from 0 straight to `inner`.
 *
 * \param rawx, rawy: SDL axis values in [-32768, 32767]; +y points down.
 * \param exponent: response curve applied to the remapped length (1 = linear).
 * \param out: direction preserved, length in [0, 1].
 * \note Requires 0 <= inner < outer <= 1 and exponent > 0.
 */
inline void DEV_RadialDeadzone(
    int rawx, int rawy, float inner, float outer, float exponent, float out[2])
{
  /* Clamping the length through `outer` also absorbs pads whose square gates report corners
   * beyond the unit circle (|v| up to ~1.41). */
  const float x = float(rawx) * (1.0f / 32767.0f);
  const float y = float(rawy) * (1.0f / 32767.0f);
  const float len2 = x * x + y * y;
  if (len2 <= inner * inner) { /* At rest: no sqrt, no division. */
    out[0] = out[1] = 0.0f;
    return;
  }
  const float len = std::sqrt(len2);
  float t = (std::fmin(len, outer) - inner) / (outer - inner);
  if (exponent != 1.0f) {
    t = std::pow(t, exponent);
  }
  const float s = t / len; /* Rescale the vector to length t, keeping its direction. */
  out[0] = x * s;
  out[1] = y * s;
}

/**
 * Trigger value in [0, 1] with a deadzone at the rest end, remapped so the output starts at 0 at
 * the deadzone edge. SDL gamepad triggers report [0, 32767].
 * \note Requires 0 <= deadzone < 1.
 */
inline float DEV_TriggerResponse(int raw, float deadzone)
{
  const float v = float(raw) * (1.0f / 32767.0f) - deadzone;
  return (v > 0.0f) ? std::fmin(v / (1.0f - deadzone), 1.0f) : 0.0f;
}
