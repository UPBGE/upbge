/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Tween Value's easing presets and repeat (2026-11-17), and the Oscillate node's waves: once for
 * the runtime kinds, the column kernel and the tests. The easings are Robert Penner's equations
 * (as Godot's Tween and the web's easings): an "In" curve f(t) on 0..1, Out = 1 - f(1 - t),
 * In Out = f(2t) / 2 below the middle, then 1 - f(2 - 2t) / 2. Back and Elastic leave 0..1 on
 * purpose (they overshoot); every one gives 0 at 0 and 1 at 1. Worked out in double.
 *
 * Tween Value keeps them in custom2 above its two flags (bit 0 On Demand, bit 1 Instant Reset):
 * bits 2-5 the transition, 6-7 the ease, 8-9 the repeat; the compiler hands custom2 >> 2 to the
 * Tween's expression (secondary_int_value). Every node saved before has 0 there: its curve, once.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "LN_Random.h"
#include "MT_Vector3.h"

/** Tween Value's Easing (custom2 bits 2-5). */
enum class LN_TweenTransition : int32_t {
  /** The node's curve (every older node). */
  Curve = 0,
  Linear = 1,
  Sine = 2,
  Quad = 3,
  Cubic = 4,
  Quart = 5,
  Quint = 6,
  Expo = 7,
  Circ = 8,
  /** Pulls back a little before going (In), overshoots and settles (Out). */
  Back = 9,
  /** Springs around the end like a rubber band. */
  Elastic = 10,
  /** Bounces at the end like a dropped ball (Out). */
  Bounce = 11,
};

/** Tween Value's Ease (custom2 bits 6-7). */
enum class LN_TweenEase : int32_t { In = 0, Out = 1, InOut = 2 };

/** Tween Value's Repeat (custom2 bits 8-9). */
enum class LN_TweenRepeat : int32_t {
  /** Stops at the end (every older node). */
  Once = 0,
  /** Starts again from the other end: Reached at each end it reaches. */
  Loop = 1,
  /** Turns round at each end: Reached at each end it reaches. */
  PingPong = 2,
};

/** The Tween expression's secondary_int_value (custom2 >> 2) taken apart. */
inline LN_TweenTransition LN_TweenTransitionOf(const int32_t packed)
{
  const int32_t value = packed & 0xF;
  return value <= int32_t(LN_TweenTransition::Bounce) ? LN_TweenTransition(value) :
                                                        LN_TweenTransition::Curve;
}
inline LN_TweenEase LN_TweenEaseOf(const int32_t packed)
{
  const int32_t value = (packed >> 4) & 0x3;
  return value <= int32_t(LN_TweenEase::InOut) ? LN_TweenEase(value) : LN_TweenEase::In;
}
inline LN_TweenRepeat LN_TweenRepeatOf(const int32_t packed)
{
  const int32_t value = (packed >> 6) & 0x3;
  return value <= int32_t(LN_TweenRepeat::PingPong) ? LN_TweenRepeat(value) :
                                                      LN_TweenRepeat::Once;
}

/** The "In" form of `transition` at `t` in 0..1. */
inline double LN_EaseInCurve(const LN_TweenTransition transition, const double t)
{
  constexpr double kPi = 3.14159265358979323846;
  switch (transition) {
    case LN_TweenTransition::Sine:
      return 1.0 - std::cos(t * kPi / 2.0);
    case LN_TweenTransition::Quad:
      return t * t;
    case LN_TweenTransition::Cubic:
      return t * t * t;
    case LN_TweenTransition::Quart:
      return t * t * t * t;
    case LN_TweenTransition::Quint:
      return t * t * t * t * t;
    case LN_TweenTransition::Expo:
      return t <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * t - 10.0);
    case LN_TweenTransition::Circ:
      return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t));
    case LN_TweenTransition::Back: {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      return c3 * t * t * t - c1 * t * t;
    }
    case LN_TweenTransition::Elastic:
      if (t <= 0.0) {
        return 0.0;
      }
      if (t >= 1.0) {
        return 1.0;
      }
      return -std::pow(2.0, 10.0 * t - 10.0) * std::sin((t * 10.0 - 10.75) * (2.0 * kPi / 3.0));
    case LN_TweenTransition::Bounce: {
      /* The In form is the mirrored Out form (a ball dropped, seen backwards). */
      const double x = 1.0 - t;
      constexpr double n1 = 7.5625;
      constexpr double d1 = 2.75;
      double out;
      if (x < 1.0 / d1) {
        out = n1 * x * x;
      }
      else if (x < 2.0 / d1) {
        const double y = x - 1.5 / d1;
        out = n1 * y * y + 0.75;
      }
      else if (x < 2.5 / d1) {
        const double y = x - 2.25 / d1;
        out = n1 * y * y + 0.9375;
      }
      else {
        const double y = x - 2.625 / d1;
        out = n1 * y * y + 0.984375;
      }
      return 1.0 - out;
    }
    case LN_TweenTransition::Linear:
    case LN_TweenTransition::Curve:
      break;
  }
  return t;
}

/** `factor` (0..1, clamped) eased by `transition` and `ease` (not for Curve: the node's curve). */
inline float LN_Ease(const LN_TweenTransition transition, const LN_TweenEase ease, const float factor)
{
  double t = std::isfinite(factor) ? double(factor) : 0.0;
  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  switch (ease) {
    case LN_TweenEase::In:
      return float(LN_EaseInCurve(transition, t));
    case LN_TweenEase::Out:
      return float(1.0 - LN_EaseInCurve(transition, 1.0 - t));
    case LN_TweenEase::InOut:
      return float(t < 0.5 ? LN_EaseInCurve(transition, 2.0 * t) / 2.0 :
                             1.0 - LN_EaseInCurve(transition, 2.0 - 2.0 * t) / 2.0);
  }
  return float(t);
}

/* ---- Oscillate (2026-11-17) ---- */

/** Oscillate's Wave (custom1). */
enum class LN_OscillateWave : int32_t { Sine = 0, Triangle = 1, Square = 2, Sawtooth = 3 };
/** Vector::Oscillate's int_value bit for Orbit (else Bob of the wave in the lower bits). */
inline constexpr int32_t LN_OSCILLATE_ORBIT = 16;

/** Oscillate's Random Phase: the object's own start in its cycle (0..1), from its tree's random
 * stream and the node (LN_Random.h; tick 0: the same all play long, the same in every play with
 * the same game seed). */
inline float LN_OscillateRandomPhase(const uint32_t identity, const uint64_t stream)
{
  return LN_RandomUnitFloat(
      LN_RandomBits(LN_RandomNodePart(LN_GetRandomGameSeed(), identity, 0), 0, stream, 0));
}

/** Where in its cycle Oscillate is (0..1): time x Frequency + Phase (+ the object's own phase),
 * only the fraction kept (in double, so hours of play keep their precision). */
inline float LN_OscillateCycle(const float time,
                               const float frequency,
                               const float phase,
                               const float random_phase)
{
  const double cycles = double(time) * double(frequency) + double(phase) + double(random_phase);
  if (!std::isfinite(cycles)) {
    return 0.0f;
  }
  const double fraction = cycles - std::floor(cycles);
  /* (A fraction a hair below 1 in double may round to 1 in float: that is 0 again.) */
  const float result = float(fraction);
  return result >= 1.0f ? 0.0f : result;
}

/** The wave at `cycle` (0..1), from -1 to 1, starting at 0 going up (Square: 1 for the first
 * half, -1 for the second; Sawtooth: rises from -1 to 1 over a cycle, at 0 at its start). */
inline float LN_OscillateWaveValue(const LN_OscillateWave wave, const float cycle)
{
  constexpr double kPi = 3.14159265358979323846;
  const double t = double(cycle);
  switch (wave) {
    case LN_OscillateWave::Triangle: {
      double shifted = t - 0.25;
      shifted -= std::floor(shifted);
      return float(4.0 * std::fabs(shifted - 0.5) - 1.0);
    }
    case LN_OscillateWave::Square:
      return t < 0.5 ? 1.0f : -1.0f;
    case LN_OscillateWave::Sawtooth: {
      double shifted = t + 0.5;
      shifted -= std::floor(shifted);
      return float(2.0 * shifted - 1.0);
    }
    case LN_OscillateWave::Sine:
      break;
  }
  return float(std::sin(2.0 * kPi * t));
}

/** Oscillate's Bob: the Axis times Amplitude times the wave. */
inline MT_Vector3 LN_OscillateBob(const MT_Vector3 &axis, const float amplitude, const float wave)
{
  const float offset = amplitude * wave;
  return MT_Vector3(axis.x() * offset, axis.y() * offset, axis.z() * offset);
}

/** Oscillate's Orbit: a point `amplitude` away from the axis, `cycle` of the way round it (0: on
 * +X projected off the axis, or +Y for an axis near X; turning the right-hand way round the axis:
 * around Z from +X towards +Y). A zero axis is Z. */
inline MT_Vector3 LN_OscillateOrbit(const MT_Vector3 &axis, const float amplitude, const float cycle)
{
  constexpr double kPi = 3.14159265358979323846;
  double ax = axis.x(), ay = axis.y(), az = axis.z();
  const double length = std::sqrt(ax * ax + ay * ay + az * az);
  if (!(length > 1.0e-12) || !std::isfinite(length)) {
    ax = 0.0;
    ay = 0.0;
    az = 1.0;
  }
  else {
    ax /= length;
    ay /= length;
    az /= length;
  }
  const bool near_x = std::fabs(ax) > 0.9;
  const double rx = near_x ? 0.0 : 1.0, ry = near_x ? 1.0 : 0.0;
  const double along = rx * ax + ry * ay;
  double ux = rx - ax * along, uy = ry - ay * along, uz = -az * along;
  const double u_length = std::sqrt(ux * ux + uy * uy + uz * uz);
  ux /= u_length;
  uy /= u_length;
  uz /= u_length;
  const double vx = ay * uz - az * uy, vy = az * ux - ax * uz, vz = ax * uy - ay * ux;
  const double angle = 2.0 * kPi * double(cycle);
  const double c = std::cos(angle) * double(amplitude), s = std::sin(angle) * double(amplitude);
  return MT_Vector3(float(c * ux + s * vx), float(c * uy + s * vy), float(c * uz + s * vz));
}

/** Oscillate's Value: Center + Amplitude x the wave at `cycle`. */
inline float LN_OscillateValue(const int32_t wave,
                               const float cycle,
                               const float amplitude,
                               const float center)
{
  return center + amplitude * LN_OscillateWaveValue(LN_OscillateWave(wave & 3), cycle);
}

/** Vector::Oscillate: Bob (the wave in `mode`'s lower bits) or Orbit (LN_OSCILLATE_ORBIT). */
inline MT_Vector3 LN_OscillateVector(const int32_t mode,
                                     const float cycle,
                                     const MT_Vector3 &axis,
                                     const float amplitude)
{
  if ((mode & LN_OSCILLATE_ORBIT) != 0) {
    return LN_OscillateOrbit(axis, amplitude, cycle);
  }
  return LN_OscillateBob(axis, amplitude, LN_OscillateWaveValue(LN_OscillateWave(mode & 3), cycle));
}
