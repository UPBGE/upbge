/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * The math of the aiming nodes, with no physics: Aim for Arc (the launch velocity that lands a
 * throw on a target) and Ray to Plane (where a ray meets a flat plane). Pure functions of their
 * inputs (their kinds are PURE): the VM's kinds and the tests share them.
 */

#pragma once

#include "MT_Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/** Aim for Arc's Solve By (the node's custom1). */
enum class LN_ArcSolveMode : int32_t {
  /** The launch speed is given; the angle is found (two arcs reach: low and high). */
  Speed = 0,
  /** The top of the arc is given (above the higher of start and target); the speed is found. */
  ArcHeight = 1,
};

struct LN_ArcSolution {
  /** The launch velocity (world units per second). */
  MT_Vector3 velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Seconds from the launch to the target; 0 when it cannot reach. */
  float flight_time = 0.0f;
  bool can_reach = false;
};

namespace ln_aim_detail {

struct Vec3d {
  double x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3d D(const MT_Vector3 &v)
{
  return {double(v.x()), double(v.y()), double(v.z())};
}
inline MT_Vector3 F(const Vec3d &v)
{
  return MT_Vector3(float(v.x), float(v.y), float(v.z));
}
inline Vec3d Add(const Vec3d &a, const Vec3d &b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3d Sub(const Vec3d &a, const Vec3d &b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3d Mul(const Vec3d &a, const double s)
{
  return {a.x * s, a.y * s, a.z * s};
}
inline double Dot(const Vec3d &a, const Vec3d &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double Length(const Vec3d &a)
{
  return std::sqrt(Dot(a, a));
}
inline bool Finite(const MT_Vector3 &v)
{
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

/** A result whose numbers are all finite, else "cannot reach" (huge inputs overflow). */
inline LN_ArcSolution Checked(const Vec3d &velocity, const double time, const bool reach)
{
  LN_ArcSolution result;
  const MT_Vector3 v = F(velocity);
  if (!Finite(v) || !std::isfinite(float(time))) {
    return result;
  }
  result.velocity = v;
  result.flight_time = reach ? float(std::max(time, 0.0)) : 0.0f;
  result.can_reach = reach;
  return result;
}

}  // namespace ln_aim_detail

/**
 * Aim for Arc: the velocity to launch from `start` so that a body falling with `gravity` (any
 * direction) passes through `target`.
 *
 * Speed: `value` is the launch speed. Of the two arcs that reach, `high_arc` picks the steeper
 * (longer) one. When the target is out of reach the result is the throw at that speed that goes
 * farthest toward it (angle halfway between the target's direction and straight up), Can Reach
 * false and Flight Time 0. Without gravity: a straight line.
 *
 * Arc Height: `value` is how far the top of the arc rises above the higher of the two ends (0:
 * the top is the higher end itself); the speed follows. Needs gravity.
 */
inline LN_ArcSolution LN_SolveArc(const MT_Vector3 &start,
                                  const MT_Vector3 &target,
                                  const MT_Vector3 &gravity,
                                  const LN_ArcSolveMode mode,
                                  const float value,
                                  const bool high_arc)
{
  using namespace ln_aim_detail;
  LN_ArcSolution none;
  if (!Finite(start) || !Finite(target) || !Finite(gravity) || !std::isfinite(value) ||
      value < 0.0f)
  {
    return none;
  }
  const Vec3d delta = Sub(D(target), D(start));
  const double distance = Length(delta);
  const double tiny = 1.0e-6 * std::max(1.0, distance);
  const double g = Length(D(gravity));
  const double v = double(value);

  if (g <= 1.0e-9) {
    /* No gravity: a straight line, only by speed. */
    if (mode != LN_ArcSolveMode::Speed) {
      return none;
    }
    if (distance <= tiny) {
      return Checked({}, 0.0, true);
    }
    if (v <= 0.0) {
      return none;
    }
    return Checked(Mul(delta, v / distance), distance / v, true);
  }

  const Vec3d up = Mul(D(gravity), -1.0 / g);
  const double y = Dot(delta, up);
  const Vec3d horizontal = Sub(delta, Mul(up, y));
  const double x = Length(horizontal);
  const bool vertical = x <= tiny;
  const Vec3d forward = vertical ? Vec3d{} : Mul(horizontal, 1.0 / x);

  if (mode == LN_ArcSolveMode::ArcHeight) {
    const double top = std::max(0.0, y) + v;
    const double rise_speed = std::sqrt(2.0 * g * top);
    const double fall = top - y;
    const double time = rise_speed / g + std::sqrt(std::max(0.0, 2.0 * fall / g));
    if (time <= 1.0e-12) {
      /* No rise and no fall: only a target where the throw starts. */
      return vertical ? Checked({}, 0.0, true) : none;
    }
    return Checked(Add(Mul(horizontal, 1.0 / time), Mul(up, rise_speed)), time, true);
  }

  /* By speed. */
  if (v <= 0.0) {
    return distance <= tiny ? Checked({}, 0.0, true) : none;
  }
  if (vertical) {
    if (y >= 0.0) {
      const double disc = v * v - 2.0 * g * y;
      if (disc < 0.0) {
        /* Straight up is the farthest toward it. */
        return Checked(Mul(up, v), 0.0, false);
      }
      const double root = std::sqrt(disc);
      /* Low: passing on the way up; high: on the way back down. */
      return Checked(Mul(up, v), (high_arc ? v + root : v - root) / g, true);
    }
    const double root = std::sqrt(v * v - 2.0 * g * y);
    if (high_arc) {
      return Checked(Mul(up, v), (v + root) / g, true);
    }
    return Checked(Mul(up, -v), (root - v) / g, true);
  }

  const double v2 = v * v;
  const double disc = v2 * v2 - g * (g * x * x + 2.0 * y * v2);
  if (disc < 0.0) {
    const double angle = 0.5 * (M_PI * 0.5 + std::atan2(y, x));
    const Vec3d velocity = Add(Mul(forward, v * std::cos(angle)), Mul(up, v * std::sin(angle)));
    return Checked(velocity, 0.0, false);
  }
  const double root = std::sqrt(disc);
  const double angle = std::atan((high_arc ? v2 + root : v2 - root) / (g * x));
  const double cos_angle = std::cos(angle);
  const Vec3d velocity = Add(Mul(forward, v * cos_angle), Mul(up, v * std::sin(angle)));
  return Checked(velocity, x / (v * cos_angle), true);
}

/**
 * Ray to Plane: how far along the ray (from its origin, in world units) it meets the plane, or -1
 * when it never does (parallel, the plane behind it, a zero direction or normal, numbers that are
 * not finite). `relative_origin` is the ray's origin minus any point of the plane; neither
 * `direction` nor `normal` needs to be unit length.
 */
inline float LN_RayPlaneDistance(const MT_Vector3 &relative_origin,
                                 const MT_Vector3 &direction,
                                 const MT_Vector3 &normal)
{
  using namespace ln_aim_detail;
  if (!Finite(relative_origin) || !Finite(direction) || !Finite(normal)) {
    return -1.0f;
  }
  const double direction_length = Length(D(direction));
  const double normal_length = Length(D(normal));
  if (direction_length <= 1.0e-12 || normal_length <= 1.0e-12) {
    return -1.0f;
  }
  const Vec3d d = Mul(D(direction), 1.0 / direction_length);
  const Vec3d n = Mul(D(normal), 1.0 / normal_length);
  const double facing = Dot(d, n);
  if (std::abs(facing) <= 1.0e-9) {
    return -1.0f;
  }
  const double t = -Dot(D(relative_origin), n) / facing;
  if (!(t >= 0.0) || !std::isfinite(float(t))) {
    return -1.0f;
  }
  return float(t);
}

/** Ray to Plane's Point: the origin moved `distance` along the unit direction; zero for -1. */
inline MT_Vector3 LN_RayPlanePoint(const MT_Vector3 &origin,
                                   const MT_Vector3 &direction,
                                   const float distance)
{
  using namespace ln_aim_detail;
  const double length = Finite(direction) ? Length(D(direction)) : 0.0;
  if (!(distance >= 0.0f) || length <= 1.0e-12 || !Finite(origin)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const MT_Vector3 point = F(Add(D(origin), Mul(D(direction), double(distance) / length)));
  return Finite(point) ? point : MT_Vector3(0.0f, 0.0f, 0.0f);
}

/** Ray to Plane's Normal: the plane's unit normal turned toward the ray's start; zero for -1. */
inline MT_Vector3 LN_RayPlaneNormal(const MT_Vector3 &direction,
                                    const MT_Vector3 &normal,
                                    const float distance)
{
  using namespace ln_aim_detail;
  const double length = Finite(normal) ? Length(D(normal)) : 0.0;
  if (!(distance >= 0.0f) || length <= 1.0e-12 || !Finite(direction)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const Vec3d n = Mul(D(normal), 1.0 / length);
  return F(Dot(D(direction), n) > 0.0 ? Mul(n, -1.0) : n);
}
