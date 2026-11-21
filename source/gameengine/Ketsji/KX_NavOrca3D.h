/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavOrca3D.h
 *  \ingroup ketsji
 *
 * Optimal reciprocal collision avoidance in three dimensions for flyers and swimmers (navigation
 * phase 8, 2026-11-19): RVO2-3D's method (van den Berg, Guy, Snape, Lin, Manocha; its linear
 * programs 1 to 4 written again here, the library is not used). Each neighbour gives a half-space
 * of allowed velocities (a plane); the velocity nearest the preferred one inside all of them,
 * within the speed sphere, else the one breaking them least. Plain floats, no shared state: any
 * thread, the same answer for the same input.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace kx_orca3d {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  Vec3() = default;
  Vec3(const float x_, const float y_, const float z_) : x(x_), y(y_), z(z_) {}
};

inline Vec3 operator+(const Vec3 &a, const Vec3 &b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3 &a, const Vec3 &b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const float s, const Vec3 &a)
{
  return {s * a.x, s * a.y, s * a.z};
}
inline Vec3 operator*(const Vec3 &a, const float s)
{
  return {s * a.x, s * a.y, s * a.z};
}
inline Vec3 operator/(const Vec3 &a, const float s)
{
  return {a.x / s, a.y / s, a.z / s};
}
inline float dot(const Vec3 &a, const Vec3 &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3 &a, const Vec3 &b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float abs_sq(const Vec3 &a)
{
  return dot(a, a);
}
inline float length(const Vec3 &a)
{
  return std::sqrt(abs_sq(a));
}
inline Vec3 normalize(const Vec3 &a)
{
  const float l = length(a);
  return l > 0.0f ? a / l : Vec3();
}

constexpr float kEpsilon = 1e-5f;

struct Plane {
  Vec3 point, normal;
};
struct Line {
  Vec3 point, direction;
};

struct Neighbour {
  Vec3 position, velocity;
  float radius = 0.5f;
  /** The part of the dodge this agent takes (0.5: shared; 1: the other won't dodge). */
  float responsibility = 0.5f;
};

struct Params {
  float time_horizon = 3.0f;
  float time_step = 1.0f / 60.0f;
};

/** On a line (the meeting of two planes): the best point within the speed sphere and the planes
 * before `count`. */
inline bool linear_program1(const std::vector<Plane> &planes,
                            const size_t count,
                            const Line &line,
                            const float radius,
                            const Vec3 &opt,
                            const bool direction_opt,
                            Vec3 &result)
{
  const float dot_product = dot(line.point, line.direction);
  const float discriminant = dot_product * dot_product + radius * radius - abs_sq(line.point);
  if (discriminant < 0.0f) {
    return false;
  }
  const float root = std::sqrt(discriminant);
  float t_left = -dot_product - root, t_right = -dot_product + root;
  for (size_t i = 0; i < count; i++) {
    const float numerator = dot(planes[i].point - line.point, planes[i].normal);
    const float denominator = dot(line.direction, planes[i].normal);
    if (denominator * denominator <= kEpsilon) {
      if (numerator > 0.0f) {
        return false;
      }
      continue;
    }
    const float t = numerator / denominator;
    if (denominator >= 0.0f) {
      t_left = std::fmax(t_left, t);
    }
    else {
      t_right = std::fmin(t_right, t);
    }
    if (t_left > t_right) {
      return false;
    }
  }
  if (direction_opt) {
    result = dot(opt, line.direction) > 0.0f ? line.point + t_right * line.direction :
                                               line.point + t_left * line.direction;
  }
  else {
    const float t = dot(line.direction, opt - line.point);
    if (t < t_left) {
      result = line.point + t_left * line.direction;
    }
    else if (t > t_right) {
      result = line.point + t_right * line.direction;
    }
    else {
      result = line.point + t * line.direction;
    }
  }
  return true;
}

/** On plane `index`: the best point within the speed sphere and the planes before it. */
inline bool linear_program2(const std::vector<Plane> &planes,
                            const size_t index,
                            const float radius,
                            const Vec3 &opt,
                            const bool direction_opt,
                            Vec3 &result)
{
  const Plane &plane = planes[index];
  const float plane_dist = dot(plane.point, plane.normal);
  const float plane_dist_sq = plane_dist * plane_dist;
  const float radius_sq = radius * radius;
  if (plane_dist_sq > radius_sq) {
    return false;
  }
  const float plane_radius_sq = radius_sq - plane_dist_sq;
  const Vec3 plane_center = plane_dist * plane.normal;
  if (direction_opt) {
    const Vec3 on_plane = opt - dot(opt, plane.normal) * plane.normal;
    const float on_plane_sq = abs_sq(on_plane);
    result = on_plane_sq <= kEpsilon ?
                 plane_center :
                 plane_center + std::sqrt(plane_radius_sq / on_plane_sq) * on_plane;
  }
  else {
    result = opt + dot(plane.point - opt, plane.normal) * plane.normal;
    if (abs_sq(result) > radius_sq) {
      const Vec3 from_center = result - plane_center;
      const float from_center_sq = abs_sq(from_center);
      result = plane_center + std::sqrt(plane_radius_sq / from_center_sq) * from_center;
    }
  }
  for (size_t i = 0; i < index; i++) {
    if (dot(planes[i].normal, planes[i].point - result) > 0.0f) {
      const Vec3 cross_product = cross(planes[i].normal, plane.normal);
      if (abs_sq(cross_product) <= kEpsilon) {
        /* Parallel planes, this one fully outside the other. */
        return false;
      }
      Line line;
      line.direction = normalize(cross_product);
      const Vec3 line_normal = cross(line.direction, plane.normal);
      line.point = plane.point + (dot(planes[i].point - plane.point, planes[i].normal) /
                                  dot(line_normal, planes[i].normal)) *
                                     line_normal;
      if (!linear_program1(planes, i, line, radius, opt, direction_opt, result)) {
        return false;
      }
    }
  }
  return true;
}

/** All planes: the best point within the speed sphere; returns the first plane it failed at
 * (`planes.size()`: none). */
inline size_t linear_program3(const std::vector<Plane> &planes,
                              const float radius,
                              const Vec3 &opt,
                              const bool direction_opt,
                              Vec3 &result)
{
  if (direction_opt) {
    result = opt * radius;
  }
  else if (abs_sq(opt) > radius * radius) {
    result = normalize(opt) * radius;
  }
  else {
    result = opt;
  }
  for (size_t i = 0; i < planes.size(); i++) {
    if (dot(planes[i].normal, planes[i].point - result) > 0.0f) {
      const Vec3 kept = result;
      if (!linear_program2(planes, i, radius, opt, direction_opt, result)) {
        result = kept;
        return i;
      }
    }
  }
  return planes.size();
}

/** No velocity keeps every plane: the one breaking them least (from plane `begin` on). */
inline void linear_program4(const std::vector<Plane> &planes,
                            const size_t begin,
                            const float radius,
                            Vec3 &result,
                            std::vector<Plane> &scratch)
{
  float distance = 0.0f;
  for (size_t i = begin; i < planes.size(); i++) {
    if (dot(planes[i].normal, planes[i].point - result) > distance) {
      scratch.clear();
      for (size_t j = 0; j < i; j++) {
        Plane plane;
        const Vec3 cross_product = cross(planes[j].normal, planes[i].normal);
        if (abs_sq(cross_product) <= kEpsilon) {
          if (dot(planes[i].normal, planes[j].normal) > 0.0f) {
            continue;
          }
          plane.point = 0.5f * (planes[i].point + planes[j].point);
        }
        else {
          const Vec3 line_normal = cross(cross_product, planes[i].normal);
          plane.point = planes[i].point + (dot(planes[j].point - planes[i].point,
                                               planes[j].normal) /
                                           dot(line_normal, planes[j].normal)) *
                                              line_normal;
        }
        plane.normal = normalize(planes[j].normal - planes[i].normal);
        scratch.push_back(plane);
      }
      const Vec3 kept = result;
      if (linear_program3(scratch, radius, planes[i].normal, true, result) < scratch.size()) {
        result = kept;
      }
      distance = dot(planes[i].normal, planes[i].point - result);
    }
  }
}

/** The new velocity of an agent at `position` moving `velocity`, wanting `preferred`. */
inline Vec3 solve(const Vec3 &position,
                  const Vec3 &velocity,
                  const Vec3 &preferred,
                  const float radius,
                  const float max_speed,
                  const Neighbour *neighbours,
                  const size_t count,
                  const Params &params,
                  std::vector<Plane> &planes,
                  std::vector<Plane> &scratch)
{
  planes.clear();
  const float inv_horizon = 1.0f / std::fmax(params.time_horizon, params.time_step);
  for (size_t n = 0; n < count; n++) {
    const Neighbour &other = neighbours[n];
    const Vec3 relative_position = other.position - position;
    const Vec3 relative_velocity = velocity - other.velocity;
    const float dist_sq = abs_sq(relative_position);
    const float combined = radius + other.radius;
    const float combined_sq = combined * combined;
    Vec3 u;
    Plane plane;
    if (dist_sq > combined_sq) {
      const Vec3 w = relative_velocity - inv_horizon * relative_position;
      const float w_length_sq = abs_sq(w);
      const float dot_product = dot(w, relative_position);
      if (dot_product < 0.0f && dot_product * dot_product > combined_sq * w_length_sq) {
        /* The cut-off sphere. */
        const float w_length = std::sqrt(w_length_sq);
        const Vec3 unit_w = w / w_length;
        plane.normal = unit_w;
        u = (combined * inv_horizon - w_length) * unit_w;
      }
      else {
        /* The cone's side. */
        const float a = dist_sq;
        const float b = dot(relative_position, relative_velocity);
        const float c = abs_sq(relative_velocity) -
                        abs_sq(cross(relative_position, relative_velocity)) /
                            (dist_sq - combined_sq);
        const float t = (b + std::sqrt(std::fmax(b * b - a * c, 0.0f))) / a;
        const Vec3 w2 = relative_velocity - t * relative_position;
        const float w_length = length(w2);
        if (w_length <= kEpsilon) {
          continue;
        }
        const Vec3 unit_w = w2 / w_length;
        plane.normal = unit_w;
        u = (combined * t - w_length) * unit_w;
      }
    }
    else {
      /* Already touching: apart within this step. */
      const float inv_step = 1.0f / params.time_step;
      const Vec3 w = relative_velocity - inv_step * relative_position;
      const float w_length = length(w);
      if (w_length <= kEpsilon) {
        continue;
      }
      const Vec3 unit_w = w / w_length;
      plane.normal = unit_w;
      u = (combined * inv_step - w_length) * unit_w;
    }
    plane.point = velocity + other.responsibility * u;
    planes.push_back(plane);
  }
  Vec3 result;
  const size_t failed = linear_program3(planes, max_speed, preferred, false, result);
  if (failed < planes.size()) {
    linear_program4(planes, failed, max_speed, result, scratch);
  }
  return result;
}

}  // namespace kx_orca3d
