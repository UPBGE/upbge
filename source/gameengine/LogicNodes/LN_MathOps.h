/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * The math of Math's and Vector Math's newer operations (2026-11-16): pure functions of their
 * inputs. The VM's kinds (Float::MathFunction, Float::VectorMeasure, Vector::VectorFunction) and
 * the column kernels call these same functions, so a wide run gives the column pass's bits
 * (LogicNodes builds with -ffp-contract=off: an inline function compiles to the same arithmetic
 * in both). Definitions follow Blender's Math and Vector Math nodes where those have the
 * operation (safe square root, arcsine, arccosine, snap, wrap, ping-pong, project, reflect).
 */

#pragma once

#include "LN_Rotation.h"
#include "MT_Vector3.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

/** Float::MathFunction's operation (the expression's int_value). Appended only. */
enum class LN_MathFunction : int32_t {
  SquareRoot = 0,
  Tangent = 1,
  Arcsine = 2,
  Arccosine = 3,
  Arctangent = 4,
  /** atan2(A, B). */
  Arctan2 = 5,
  /** A wrapped into [C, B) (Blender's Wrap: Value, Max, Min). */
  Wrap = 6,
  /** A rounded down to a multiple of B. */
  Snap = 7,
  /** A bouncing between 0 and B. */
  PingPong = 8,
  /** The shortest turn from angle A to angle B, in [-pi, pi) (radians). */
  AngleDifference = 9,
  /** Map Range's Smooth Step: A clamped to 0..1, eased in and out (3t^2 - 2t^3). */
  SmoothStep = 10,
  /** Map Range's Smoother Step: A clamped to 0..1, eased more (6t^5 - 15t^4 + 10t^3). */
  SmootherStep = 11,
  Count,
};

/** Float::VectorMeasure's operation (the expression's int_value). Appended only. */
enum class LN_VectorMeasure : int32_t {
  Dot = 0,
  Distance = 1,
  /** The angle between A and B, 0 to pi (radians); 0 when either is zero. */
  Angle = 2,
  /** As Angle, negative when the turn from A to B goes clockwise around the Axis. */
  SignedAngle = 3,
  Count,
};

/** Vector::VectorFunction's operation (the expression's int_value). Appended only. */
enum class LN_VectorFunction : int32_t {
  Cross = 0,
  /** A projected onto B; zero when B is zero. */
  Project = 1,
  /** A reflected around the normal B (B need not be unit length; zero B gives A). */
  Reflect = 2,
  /** A shortened to at most Scale long. */
  LimitLength = 3,
  /** A moved toward B by at most Scale (B when nearer; a negative Scale moves away). */
  MoveTowards = 4,
  Count,
};

inline constexpr float LN_MATH_PI = 3.14159265358979323846f;

inline float LN_MathWrap(const float value, const float max, const float min)
{
  const float range = max - min;
  return range != 0.0f ? value - range * std::floor((value - min) / range) : min;
}

inline float LN_MathFunctionValue(const LN_MathFunction function, const float a, const float b,
                                  const float c)
{
  switch (function) {
    case LN_MathFunction::SquareRoot:
      return a > 0.0f ? std::sqrt(a) : 0.0f;
    case LN_MathFunction::Tangent:
      return std::tan(a);
    case LN_MathFunction::Arcsine:
      return a <= -1.0f ? -LN_MATH_PI * 0.5f : (a >= 1.0f ? LN_MATH_PI * 0.5f : std::asin(a));
    case LN_MathFunction::Arccosine:
      return a <= -1.0f ? LN_MATH_PI : (a >= 1.0f ? 0.0f : std::acos(a));
    case LN_MathFunction::Arctangent:
      return std::atan(a);
    case LN_MathFunction::Arctan2:
      return std::atan2(a, b);
    case LN_MathFunction::Wrap:
      return LN_MathWrap(a, b, c);
    case LN_MathFunction::Snap:
      return b != 0.0f ? std::floor(a / b) * b : 0.0f;
    case LN_MathFunction::PingPong: {
      if (b == 0.0f) {
        return 0.0f;
      }
      const float t = (a - b) / (b * 2.0f);
      return std::fabs((t - std::floor(t)) * b * 2.0f - b);
    }
    case LN_MathFunction::AngleDifference:
      return LN_MathWrap(b - a, LN_MATH_PI, -LN_MATH_PI);
    case LN_MathFunction::SmoothStep: {
      /* Blender's Map Range (smoothstep). */
      const float t = a <= 0.0f ? 0.0f : (a >= 1.0f ? 1.0f : a);
      return (3.0f - 2.0f * t) * t * t;
    }
    case LN_MathFunction::SmootherStep: {
      const float t = a <= 0.0f ? 0.0f : (a >= 1.0f ? 1.0f : a);
      return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
    case LN_MathFunction::Count:
      break;
  }
  return 0.0f;
}

inline float LN_VectorDot(const MT_Vector3 &a, const MT_Vector3 &b)
{
  return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}

inline MT_Vector3 LN_VectorCross(const MT_Vector3 &a, const MT_Vector3 &b)
{
  return MT_Vector3(a.y() * b.z() - a.z() * b.y(),
                    a.z() * b.x() - a.x() * b.z(),
                    a.x() * b.y() - a.y() * b.x());
}

inline float LN_VectorNorm(const MT_Vector3 &a)
{
  return std::sqrt(LN_VectorDot(a, a));
}

inline float LN_VectorMeasureValue(const LN_VectorMeasure measure, const MT_Vector3 &a,
                                   const MT_Vector3 &b, const MT_Vector3 &axis)
{
  switch (measure) {
    case LN_VectorMeasure::Dot:
      return LN_VectorDot(a, b);
    case LN_VectorMeasure::Distance:
      return LN_VectorNorm(MT_Vector3(a.x() - b.x(), a.y() - b.y(), a.z() - b.z()));
    case LN_VectorMeasure::Angle:
    case LN_VectorMeasure::SignedAngle: {
      /* atan2 of the cross's length and the dot: exact for small and nearly opposite angles. */
      const MT_Vector3 cross = LN_VectorCross(a, b);
      const float angle = std::atan2(LN_VectorNorm(cross), LN_VectorDot(a, b));
      if (measure == LN_VectorMeasure::SignedAngle && LN_VectorDot(cross, axis) < 0.0f) {
        return -angle;
      }
      return angle;
    }
    case LN_VectorMeasure::Count:
      break;
  }
  return 0.0f;
}

inline MT_Vector3 LN_VectorFunctionValue(const LN_VectorFunction function, const MT_Vector3 &a,
                                         const MT_Vector3 &b, const float scale)
{
  switch (function) {
    case LN_VectorFunction::Cross:
      return LN_VectorCross(a, b);
    case LN_VectorFunction::Project: {
      const float length_squared = LN_VectorDot(b, b);
      if (length_squared == 0.0f) {
        return MT_Vector3(0.0f, 0.0f, 0.0f);
      }
      const float factor = LN_VectorDot(a, b) / length_squared;
      return MT_Vector3(b.x() * factor, b.y() * factor, b.z() * factor);
    }
    case LN_VectorFunction::Reflect: {
      const float length = LN_VectorNorm(b);
      if (length == 0.0f) {
        return a;
      }
      const MT_Vector3 normal(b.x() / length, b.y() / length, b.z() / length);
      const float twice = 2.0f * LN_VectorDot(a, normal);
      return MT_Vector3(a.x() - twice * normal.x(),
                        a.y() - twice * normal.y(),
                        a.z() - twice * normal.z());
    }
    case LN_VectorFunction::LimitLength: {
      const float max_length = scale > 0.0f ? scale : 0.0f;
      const float length = LN_VectorNorm(a);
      if (length <= max_length) {
        return a;
      }
      const float factor = max_length / length;
      return MT_Vector3(a.x() * factor, a.y() * factor, a.z() * factor);
    }
    case LN_VectorFunction::MoveTowards: {
      const MT_Vector3 delta(b.x() - a.x(), b.y() - a.y(), b.z() - a.z());
      const float distance = LN_VectorNorm(delta);
      if (distance <= scale || distance == 0.0f) {
        return b;
      }
      const float factor = scale / distance;
      return MT_Vector3(a.x() + delta.x() * factor,
                        a.y() + delta.y() * factor,
                        a.z() + delta.z() * factor);
    }
    case LN_VectorFunction::Count:
      break;
  }
  return MT_Vector3(0.0f, 0.0f, 0.0f);
}

/** Vector::RotationFunction's operation (the expression's int_value, 2026-11-16): Rotation
 * Math's (a rotation from input0 A and input1 B) and Transform Vector's (a vector from input0,
 * the rotation input1 and the scale input2). Appended only. */
enum class LN_RotationFunction : int32_t {
  /** B applied after A, around the world's axes (A then B). */
  Combine = 0,
  /** B applied around A's own axes. */
  CombineLocal = 1,
  /** A undone. */
  Invert = 2,
  /** The turn from A to B (around the world's axes: Combine of A and it is B). */
  Difference = 3,
  /** The vector turned by the rotation (a direction from local to world). */
  RotateVector = 4,
  /** The vector turned back by the rotation (a direction from world to local). */
  UnrotateVector = 5,
  /** The vector scaled, then turned (a point's offset from local to world). */
  ScaleRotateVector = 6,
  /** The vector turned back, then divided by the scale (a zero scale axis gives 0). */
  UnrotateUnscaleVector = 7,
  Count,
};

inline bool LN_RotationFunctionGivesRotation(const LN_RotationFunction function)
{
  return function == LN_RotationFunction::Combine ||
         function == LN_RotationFunction::CombineLocal ||
         function == LN_RotationFunction::Invert || function == LN_RotationFunction::Difference;
}

inline LN_Rotation LN_RotationFunctionRotation(const LN_RotationFunction function,
                                               const LN_Rotation &a,
                                               const LN_Rotation &b)
{
  switch (function) {
    case LN_RotationFunction::Combine:
      return LN_Rotation(b.Quaternion() * a.Quaternion());
    case LN_RotationFunction::CombineLocal:
      return LN_Rotation(a.Quaternion() * b.Quaternion());
    case LN_RotationFunction::Invert:
      return LN_Rotation(a.Quaternion().conjugate());
    case LN_RotationFunction::Difference:
      return LN_Rotation(b.Quaternion() * a.Quaternion().conjugate());
    default:
      break;
  }
  return LN_Rotation::Identity();
}

inline MT_Vector3 LN_RotationFunctionVector(const LN_RotationFunction function,
                                            const MT_Vector3 &vector,
                                            const LN_Rotation &rotation,
                                            const MT_Vector3 &scale)
{
  const MT_Matrix3x3 matrix = rotation.ToMatrix();
  switch (function) {
    case LN_RotationFunction::RotateVector:
      return matrix * vector;
    case LN_RotationFunction::UnrotateVector:
      return matrix.transposed() * vector;
    case LN_RotationFunction::ScaleRotateVector:
      return matrix * MT_Vector3(vector.x() * scale.x(), vector.y() * scale.y(),
                                 vector.z() * scale.z());
    case LN_RotationFunction::UnrotateUnscaleVector: {
      const MT_Vector3 local = matrix.transposed() * vector;
      return MT_Vector3(scale.x() != 0.0f ? local.x() / scale.x() : 0.0f,
                        scale.y() != 0.0f ? local.y() / scale.y() : 0.0f,
                        scale.z() != 0.0f ? local.z() / scale.z() : 0.0f);
    }
    default:
      break;
  }
  return vector;
}

/* -------------------------------------------------------------------- */
/** \name Smooth Value (2026-11-16)
 *
 * A value that follows its Target over time, kept per tree (LN_RuntimeTree::SmoothValueState,
 * one per node and output: LN_Program::AcquireSmoothValueNode). Float::SmoothValue and
 * Vector::SmoothValue (the VM) and the column kernel NSmooth call LN_SmoothValueAdvance.
 * \{ */

/** Smooth Value's Mode (custom2; Float::SmoothValue's component_index, Vector::SmoothValue's
 * property_ref_index). Appended only. */
enum class LN_SmoothMode : int32_t {
  /** Closes the same share of the gap every second, whatever the frame rate: Speed 5 closes
   * 1 - e^-5 (99%) of it in one second. Never overshoots. */
  Smooth = 0,
  /** Moves toward the Target at Speed units a second and stops exactly on it. */
  ConstantSpeed = 1,
  /** A critically damped spring (Unity's SmoothDamp, Game Programming Gems 4): reaches the
   * Target in about Smooth Time seconds, keeps its speed when the Target moves, never overshoots
   * and never goes faster than Max Speed (0: no limit). */
  SmoothDamp = 2,
  Count,
};

/** A Smooth Value node's state in one tree: its value and Smooth Damp's velocity (a Float uses
 * the first of three). */
struct LN_SmoothValueState {
  bool initialized = false;
  /** The tick it last moved (a node read twice in one tick moves once). */
  uint64_t last_update_tick = UINT64_MAX;
  float value[3] = {0.0f, 0.0f, 0.0f};
  float velocity[3] = {0.0f, 0.0f, 0.0f};
};

/** Smooth Value's evaluation in tick `tick` (once a tick: a second call of the same tick changes
 * nothing). The first evaluation, a Reset or a value that is no longer a number jumps to the
 * Target (velocity 0); else one move of `dt` seconds (none when `dt` is not above 0). `size`: 1
 * (Float, the first of three) or 3. `speed`: Smooth's rate or Constant Speed's units a second;
 * Smooth Damp's Smooth Time. `max_speed`: Smooth Damp's limit (not above 0: none). */
inline void LN_SmoothValueAdvance(LN_SmoothValueState &state,
                                  const uint64_t tick,
                                  const LN_SmoothMode mode,
                                  const int size,
                                  const float target[3],
                                  const float speed,
                                  const float max_speed,
                                  const bool reset,
                                  const float dt)
{
  if (tick != UINT64_MAX && state.last_update_tick == tick) {
    return;
  }
  state.last_update_tick = tick;
  bool finite = true;
  for (int k = 0; k < size; k++) {
    finite = finite && std::isfinite(state.value[k]) && std::isfinite(state.velocity[k]);
  }
  if (!state.initialized || reset || !finite) {
    for (int k = 0; k < 3; k++) {
      state.value[k] = k < size ? target[k] : 0.0f;
      state.velocity[k] = 0.0f;
    }
    state.initialized = true;
    return;
  }
  if (!(dt > 0.0f)) {
    return;
  }
  float *value = state.value;
  float *velocity = state.velocity;
  switch (mode) {
    case LN_SmoothMode::Smooth: {
      const float rate = speed > 0.0f ? speed : 0.0f;
      const float share = 1.0f - std::exp(-(rate * dt));
      for (int k = 0; k < size; k++) {
        value[k] = value[k] + (target[k] - value[k]) * share;
      }
      break;
    }
    case LN_SmoothMode::ConstantSpeed: {
      const float rate = speed > 0.0f ? speed : 0.0f;
      const float step = rate * dt;
      float delta[3] = {0.0f, 0.0f, 0.0f};
      float squared = 0.0f;
      for (int k = 0; k < size; k++) {
        delta[k] = target[k] - value[k];
        squared = squared + delta[k] * delta[k];
      }
      const float distance = size == 1 ? std::fabs(delta[0]) : std::sqrt(squared);
      if (distance <= step || distance == 0.0f) {
        for (int k = 0; k < size; k++) {
          value[k] = target[k];
        }
      }
      else {
        const float factor = step / distance;
        for (int k = 0; k < size; k++) {
          value[k] = value[k] + delta[k] * factor;
        }
      }
      break;
    }
    case LN_SmoothMode::SmoothDamp: {
      const float smooth_time = speed > 0.0001f ? speed : 0.0001f;
      const float omega = 2.0f / smooth_time;
      const float x = omega * dt;
      const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
      float change[3] = {0.0f, 0.0f, 0.0f};
      float squared = 0.0f;
      for (int k = 0; k < size; k++) {
        change[k] = value[k] - target[k];
        squared = squared + change[k] * change[k];
      }
      if (max_speed > 0.0f && std::isfinite(max_speed)) {
        /* No farther from the Target than Max Speed can close in Smooth Time. */
        const float max_change = max_speed * smooth_time;
        if (size == 1) {
          change[0] = change[0] < -max_change ? -max_change :
                                                (change[0] > max_change ? max_change : change[0]);
        }
        else if (squared > max_change * max_change) {
          const float factor = max_change / std::sqrt(squared);
          for (int k = 0; k < size; k++) {
            change[k] = change[k] * factor;
          }
        }
      }
      float result[3] = {0.0f, 0.0f, 0.0f};
      float overshoot = 0.0f;
      for (int k = 0; k < size; k++) {
        const float to = value[k] - change[k];
        const float temp = (velocity[k] + omega * change[k]) * dt;
        velocity[k] = (velocity[k] - omega * temp) * decay;
        result[k] = to + (change[k] + temp) * decay;
        overshoot = overshoot + (target[k] - value[k]) * (result[k] - target[k]);
      }
      if (overshoot > 0.0f) {
        /* Past the Target: stop on it. */
        for (int k = 0; k < size; k++) {
          result[k] = target[k];
          velocity[k] = 0.0f;
        }
      }
      for (int k = 0; k < size; k++) {
        value[k] = result[k];
      }
      break;
    }
    case LN_SmoothMode::Count:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Noise (2026-11-16)
 *
 * Gradient (Perlin) noise in 1 to 4 dimensions, seeded by a hash (no tables, no state: the same
 * on every thread), several octaves summed (fBm). Float::Noise (the VM) and the column kernel
 * FNoise call LN_NoiseValue. Gradients and scales after Stefan Gustavson's noise1234.
 * \{ */

/** Float::Noise's settings in its int_value: the dimensions (1 W, 2 X Y, 3 the Vector, 4 the
 * Vector and W) in the low byte, the octaves (1 to 8) in the next. */
inline int32_t LN_NoiseSettings(const int dimensions, const int octaves)
{
  return (dimensions & 0xff) | ((octaves & 0xff) << 8);
}
inline int LN_NoiseDimensions(const int32_t settings)
{
  const int dimensions = settings & 0xff;
  return dimensions < 1 ? 1 : (dimensions > 4 ? 4 : dimensions);
}
inline int LN_NoiseOctaves(const int32_t settings)
{
  const int octaves = (settings >> 8) & 0xff;
  return octaves < 1 ? 1 : (octaves > 8 ? 8 : octaves);
}

/** 32 bits of a lattice point and a seed (murmur3's finalizer over a sum of products). */
inline uint32_t LN_NoiseHash(const int32_t x,
                             const int32_t y,
                             const int32_t z,
                             const int32_t w,
                             const uint32_t seed)
{
  uint32_t h = seed ^ (uint32_t(x) * 0x8da6b343u) ^ (uint32_t(y) * 0xd8163841u) ^
               (uint32_t(z) * 0xcb1ab31fu) ^ (uint32_t(w) * 0x9e3779b1u);
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

/** A coordinate the lattice can hold: anything not a number or beyond a billion is 0. */
inline float LN_NoiseCoordinate(const float value)
{
  return std::fabs(value) < 1.0e9f ? value : 0.0f;
}

inline float LN_NoiseFade(const float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float LN_NoiseLerp(const float t, const float a, const float b)
{
  return a + t * (b - a);
}

inline float LN_NoiseGrad1(const uint32_t hash, const float x)
{
  const uint32_t h = hash & 15u;
  float gradient = 1.0f + float(h & 7u);
  if ((h & 8u) != 0u) {
    gradient = -gradient;
  }
  return gradient * x;
}

inline float LN_NoiseGrad2(const uint32_t hash, const float x, const float y)
{
  const uint32_t h = hash & 7u;
  const float u = h < 4u ? x : y;
  const float v = h < 4u ? y : x;
  return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -2.0f * v : 2.0f * v);
}

inline float LN_NoiseGrad3(const uint32_t hash, const float x, const float y, const float z)
{
  const uint32_t h = hash & 15u;
  const float u = h < 8u ? x : y;
  const float v = h < 4u ? y : ((h == 12u || h == 14u) ? x : z);
  return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v);
}

inline float LN_NoiseGrad4(
    const uint32_t hash, const float x, const float y, const float z, const float w)
{
  const uint32_t h = hash & 31u;
  const float u = h < 24u ? x : y;
  const float v = h < 16u ? y : z;
  const float s = h < 8u ? z : w;
  return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v) + ((h & 4u) != 0u ? -s : s);
}

/** One octave: about -1 to 1, 0 on the lattice's points. */
inline float LN_PerlinNoise(const int dimensions,
                            const float x_in,
                            const float y_in,
                            const float z_in,
                            const float w_in,
                            const uint32_t seed)
{
  /* (Whole parts that fit an int32: a cast of anything bigger is undefined.) */
  const float px = LN_NoiseCoordinate(x_in), py = LN_NoiseCoordinate(y_in);
  const float pz = LN_NoiseCoordinate(z_in), pw = LN_NoiseCoordinate(w_in);
  const float fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz), fw = std::floor(pw);
  const int32_t x0 = int32_t(fx), y0 = int32_t(fy), z0 = int32_t(fz), w0 = int32_t(fw);
  const float x = px - fx, y = py - fy, z = pz - fz, w = pw - fw;
  const float u = LN_NoiseFade(x);
  switch (dimensions) {
    case 1: {
      const float n0 = LN_NoiseGrad1(LN_NoiseHash(x0, 0, 0, 0, seed), x);
      const float n1 = LN_NoiseGrad1(LN_NoiseHash(x0 + 1, 0, 0, 0, seed), x - 1.0f);
      return 0.188f * LN_NoiseLerp(u, n0, n1);
    }
    case 2: {
      const float v = LN_NoiseFade(y);
      const float n00 = LN_NoiseGrad2(LN_NoiseHash(x0, y0, 0, 0, seed), x, y);
      const float n10 = LN_NoiseGrad2(LN_NoiseHash(x0 + 1, y0, 0, 0, seed), x - 1.0f, y);
      const float n01 = LN_NoiseGrad2(LN_NoiseHash(x0, y0 + 1, 0, 0, seed), x, y - 1.0f);
      const float n11 = LN_NoiseGrad2(
          LN_NoiseHash(x0 + 1, y0 + 1, 0, 0, seed), x - 1.0f, y - 1.0f);
      return 0.507f * LN_NoiseLerp(v, LN_NoiseLerp(u, n00, n10), LN_NoiseLerp(u, n01, n11));
    }
    case 3: {
      const float v = LN_NoiseFade(y);
      const float t = LN_NoiseFade(z);
      float plane[2];
      for (int k = 0; k < 2; k++) {
        const int32_t zk = z0 + k;
        const float dz = z - float(k);
        const float n00 = LN_NoiseGrad3(LN_NoiseHash(x0, y0, zk, 0, seed), x, y, dz);
        const float n10 = LN_NoiseGrad3(LN_NoiseHash(x0 + 1, y0, zk, 0, seed), x - 1.0f, y, dz);
        const float n01 = LN_NoiseGrad3(LN_NoiseHash(x0, y0 + 1, zk, 0, seed), x, y - 1.0f, dz);
        const float n11 = LN_NoiseGrad3(
            LN_NoiseHash(x0 + 1, y0 + 1, zk, 0, seed), x - 1.0f, y - 1.0f, dz);
        plane[k] = LN_NoiseLerp(v, LN_NoiseLerp(u, n00, n10), LN_NoiseLerp(u, n01, n11));
      }
      return 0.936f * LN_NoiseLerp(t, plane[0], plane[1]);
    }
    default: {
      const float v = LN_NoiseFade(y);
      const float t = LN_NoiseFade(z);
      const float s = LN_NoiseFade(w);
      float cube[2];
      for (int m = 0; m < 2; m++) {
        const int32_t wm = w0 + m;
        const float dw = w - float(m);
        float plane[2];
        for (int k = 0; k < 2; k++) {
          const int32_t zk = z0 + k;
          const float dz = z - float(k);
          const float n00 = LN_NoiseGrad4(LN_NoiseHash(x0, y0, zk, wm, seed), x, y, dz, dw);
          const float n10 = LN_NoiseGrad4(
              LN_NoiseHash(x0 + 1, y0, zk, wm, seed), x - 1.0f, y, dz, dw);
          const float n01 = LN_NoiseGrad4(
              LN_NoiseHash(x0, y0 + 1, zk, wm, seed), x, y - 1.0f, dz, dw);
          const float n11 = LN_NoiseGrad4(
              LN_NoiseHash(x0 + 1, y0 + 1, zk, wm, seed), x - 1.0f, y - 1.0f, dz, dw);
          plane[k] = LN_NoiseLerp(v, LN_NoiseLerp(u, n00, n10), LN_NoiseLerp(u, n01, n11));
        }
        cube[m] = LN_NoiseLerp(t, plane[0], plane[1]);
      }
      return 0.8344f * LN_NoiseLerp(s, cube[0], cube[1]);
    }
  }
}

/** Noise's value, -1 to 1: `octaves` octaves of `dimensions`-dimensional noise at (Vector, W)
 * times Scale, each at twice the frequency of the one before and Roughness times its strength,
 * summed and divided by the strengths' sum. `channel` tells the three of a Vector output apart
 * (0, 1, 2); Seed gives another pattern. Each octave starts at its own offset from the seed, so
 * whole-number places are not all 0. */
inline float LN_NoiseValue(const int dimensions,
                           const int octaves,
                           const MT_Vector3 &vector,
                           const float w,
                           const float scale,
                           const float roughness,
                           const int32_t seed,
                           const uint32_t channel)
{
  const float px = vector.x() * scale;
  const float py = vector.y() * scale;
  const float pz = vector.z() * scale;
  const float pw = w * scale;
  const float gain = roughness > 0.0f ? (roughness < 1.0f ? roughness : 1.0f) : 0.0f;
  const uint32_t base = LN_NoiseHash(int32_t(channel), 0x51ed27, 0, 0, uint32_t(seed));
  float sum = 0.0f;
  float strength = 1.0f;
  float strengths = 0.0f;
  float frequency = 1.0f;
  for (int octave = 0; octave < octaves; octave++) {
    const uint32_t octave_seed = LN_NoiseHash(octave, 0, 0, 0, base);
    /* An offset of 0 to 256 per axis (8 bits of whole part, 8 of fraction). */
    const float ox = float((octave_seed >> 0) & 0xffffu) * (1.0f / 256.0f);
    const float oy = float((octave_seed >> 16) & 0xffffu) * (1.0f / 256.0f);
    const uint32_t more = LN_NoiseHash(octave, 1, 0, 0, base);
    const float oz = float((more >> 0) & 0xffffu) * (1.0f / 256.0f);
    const float ow = float((more >> 16) & 0xffffu) * (1.0f / 256.0f);
    float x = 0.0f, y = 0.0f, z = 0.0f, t = 0.0f;
    switch (dimensions) {
      case 1:
        x = pw * frequency + ox;
        break;
      case 2:
        x = px * frequency + ox;
        y = py * frequency + oy;
        break;
      case 3:
        x = px * frequency + ox;
        y = py * frequency + oy;
        z = pz * frequency + oz;
        break;
      default:
        x = px * frequency + ox;
        y = py * frequency + oy;
        z = pz * frequency + oz;
        t = pw * frequency + ow;
        break;
    }
    sum = sum + strength * LN_PerlinNoise(dimensions, x, y, z, t, octave_seed);
    strengths = strengths + strength;
    strength = strength * gain;
    frequency = frequency * 2.0f;
  }
  const float value = strengths > 0.0f ? sum / strengths : 0.0f;
  return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Random Value's shapes (2026-11-16)
 *
 * Vector::RandomShape (the VM) and the column kernel VRandomShape: a vector from three random
 * numbers in [0, 1) (LN_Random.h, salts 1, 2, 3), every place of the shape equally likely.
 * \{ */

/** Random Value's shape types (Vector::RandomShape's property_ref_index). Appended only. */
enum class LN_RandomShape : int32_t {
  /** A direction, one unit long, every direction equally likely. */
  Direction = 0,
  /** A point inside a ball of the Radius around (0, 0, 0). */
  InSphere = 1,
  /** A point inside a disc of the Radius on the X Y plane (Z 0). */
  InCircle = 2,
  /** A direction, one unit long, at most Angle away from Axis (a spread, a cone of fire). */
  InCone = 3,
  Count,
};

inline MT_Vector3 LN_RandomShapeVector(const LN_RandomShape shape,
                                       const float u1,
                                       const float u2,
                                       const float u3,
                                       const float size,
                                       const MT_Vector3 &axis)
{
  const float phi = 2.0f * LN_MATH_PI * u2;
  const float cos_phi = std::cos(phi);
  const float sin_phi = std::sin(phi);
  const float radius = size > 0.0f && std::isfinite(size) ? size : 0.0f;
  switch (shape) {
    case LN_RandomShape::Direction:
    case LN_RandomShape::InSphere: {
      const float z = 1.0f - 2.0f * u1;
      const float squared = 1.0f - z * z;
      const float r = squared > 0.0f ? std::sqrt(squared) : 0.0f;
      const float length = shape == LN_RandomShape::Direction ? 1.0f : radius * std::cbrt(u3);
      return MT_Vector3(r * cos_phi * length, r * sin_phi * length, z * length);
    }
    case LN_RandomShape::InCircle: {
      const float r = radius * std::sqrt(u1);
      return MT_Vector3(r * cos_phi, r * sin_phi, 0.0f);
    }
    case LN_RandomShape::InCone: {
      const float angle = size > 0.0f ? (size < LN_MATH_PI ? size : LN_MATH_PI) : 0.0f;
      const float z = 1.0f - u1 * (1.0f - std::cos(angle));
      const float squared = 1.0f - z * z;
      const float r = squared > 0.0f ? std::sqrt(squared) : 0.0f;
      const float lx = r * cos_phi, ly = r * sin_phi;
      /* Around the Axis (none or not a number: Z), the basis of Duff et al. 2017. */
      const float length = LN_VectorNorm(axis);
      const bool usable = length > 0.0f && std::isfinite(length);
      const float nx = usable ? axis.x() / length : 0.0f;
      const float ny = usable ? axis.y() / length : 0.0f;
      const float nz = usable ? axis.z() / length : 1.0f;
      const float sign = nz >= 0.0f ? 1.0f : -1.0f;
      const float a = -1.0f / (sign + nz);
      const float b = nx * ny * a;
      const MT_Vector3 b1(1.0f + sign * nx * nx * a, sign * b, -sign * nx);
      const MT_Vector3 b2(b, sign + ny * ny * a, -ny);
      return MT_Vector3(b1.x() * lx + b2.x() * ly + nx * z,
                        b1.y() * lx + b2.y() * ly + ny * z,
                        b1.z() * lx + b2.z() * ly + nz * z);
    }
    case LN_RandomShape::Count:
      break;
  }
  return MT_Vector3(0.0f, 0.0f, 0.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weighted random pick (Get Random List Item's Weights, 2026-11-16)
 * \{ */

/** The place picked with `unit` (a random number in [0, 1)) from weights: place i with the
 * chance weights[i] / their sum, a weight that is not a positive number never; -1 when none is
 * positive. `weight_of(i)` gives place i's weight. */
template<class WeightOf>
inline int32_t LN_RandomWeightedIndex(const float unit, const size_t count, WeightOf weight_of)
{
  float total = 0.0f;
  for (size_t index = 0; index < count; index++) {
    const float weight = weight_of(index);
    if (weight > 0.0f && std::isfinite(weight)) {
      total = total + weight;
    }
  }
  if (!(total > 0.0f) || !std::isfinite(total)) {
    return -1;
  }
  const float pick = unit * total;
  float cumulative = 0.0f;
  int32_t last = -1;
  for (size_t index = 0; index < count; index++) {
    const float weight = weight_of(index);
    if (!(weight > 0.0f && std::isfinite(weight))) {
      continue;
    }
    cumulative = cumulative + weight;
    last = int32_t(index);
    if (pick < cumulative) {
      return last;
    }
  }
  /* Rounding: the last positive place. */
  return last;
}

/** \} */
