/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavOrca.h
 *  \ingroup ketsji
 *  Optimal Reciprocal Collision Avoidance (ORCA), the avoidance of crowds of NPCs: each NPC
 *  works out, from its neighbours' places and last velocities, the velocities that would hit
 *  none of them within a time horizon (each pair sharing the dodge), and picks the allowed one
 *  nearest the velocity it wants (a 2D linear program).
 *
 *  The method and its solver follow J. van den Berg, S. J. Guy, M. Lin, D. Manocha,
 *  "Reciprocal n-body Collision Avoidance" (2011) and the RVO2 library (Apache-2.0,
 *  https://github.com/snape/RVO2), written again here on plain floats. Walls are simpler than
 *  RVO2's polygon obstacles: each nearby wall segment is a hard half-plane at its closest point
 *  (the navmesh keeps NPCs away from walls anyway).
 *
 *  It reads nothing but its arguments: the same inputs give the same velocity on any thread.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace kx_orca {

struct Vec2 {
  float x = 0.0f, y = 0.0f;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2 &o) const
  {
    return Vec2(x + o.x, y + o.y);
  }
  Vec2 operator-(const Vec2 &o) const
  {
    return Vec2(x - o.x, y - o.y);
  }
  Vec2 operator-() const
  {
    return Vec2(-x, -y);
  }
  Vec2 operator*(float s) const
  {
    return Vec2(x * s, y * s);
  }
  Vec2 operator/(float s) const
  {
    return Vec2(x / s, y / s);
  }
};

inline Vec2 operator*(float s, const Vec2 &v)
{
  return Vec2(v.x * s, v.y * s);
}
inline float dot(const Vec2 &a, const Vec2 &b)
{
  return a.x * b.x + a.y * b.y;
}
inline float det(const Vec2 &a, const Vec2 &b)
{
  return a.x * b.y - a.y * b.x;
}
inline float abs_sq(const Vec2 &v)
{
  return dot(v, v);
}
inline float length(const Vec2 &v)
{
  return std::sqrt(abs_sq(v));
}
inline Vec2 normalize(const Vec2 &v)
{
  const float l = length(v);
  return l > 0.0f ? v / l : Vec2();
}

/** A half-plane of allowed velocities: the side left of `direction` through `point`. */
struct Line {
  Vec2 point, direction;
};

constexpr float kEpsilon = 1e-5f;

/** Solves on line `line_no`, within the speed circle and every earlier line. */
inline bool linear_program1(const std::vector<Line> &lines,
                            const size_t line_no,
                            const float radius,
                            const Vec2 &opt_velocity,
                            const bool direction_opt,
                            Vec2 &result)
{
  const Line &line = lines[line_no];
  const float dot_product = dot(line.point, line.direction);
  const float discriminant = dot_product * dot_product + radius * radius - abs_sq(line.point);
  if (discriminant < 0.0f) {
    /* The speed circle lies wholly outside this line's half-plane. */
    return false;
  }
  const float sqrt_discriminant = std::sqrt(discriminant);
  float t_left = -dot_product - sqrt_discriminant;
  float t_right = -dot_product + sqrt_discriminant;
  for (size_t i = 0; i < line_no; i++) {
    const float denominator = det(line.direction, lines[i].direction);
    const float numerator = det(lines[i].direction, line.point - lines[i].point);
    if (std::fabs(denominator) <= kEpsilon) {
      /* Parallel lines. */
      if (numerator < 0.0f) {
        return false;
      }
      continue;
    }
    const float t = numerator / denominator;
    if (denominator >= 0.0f) {
      t_right = std::min(t_right, t);
    }
    else {
      t_left = std::max(t_left, t);
    }
    if (t_left > t_right) {
      return false;
    }
  }
  if (direction_opt) {
    result = dot(opt_velocity, line.direction) > 0.0f ? line.point + t_right * line.direction :
                                                        line.point + t_left * line.direction;
  }
  else {
    const float t = dot(line.direction, opt_velocity - line.point);
    result = line.point + std::clamp(t, t_left, t_right) * line.direction;
  }
  return true;
}

/** The velocity nearest `opt_velocity` (or furthest along it, `direction_opt`) allowed by every
 * line within the speed circle. Returns the number of lines satisfied (all: lines.size()). */
inline size_t linear_program2(const std::vector<Line> &lines,
                              const float radius,
                              const Vec2 &opt_velocity,
                              const bool direction_opt,
                              Vec2 &result)
{
  if (direction_opt) {
    result = opt_velocity * radius;
  }
  else if (abs_sq(opt_velocity) > radius * radius) {
    result = normalize(opt_velocity) * radius;
  }
  else {
    result = opt_velocity;
  }
  for (size_t i = 0; i < lines.size(); i++) {
    if (det(lines[i].direction, lines[i].point - result) > 0.0f) {
      const Vec2 previous = result;
      if (!linear_program1(lines, i, radius, opt_velocity, direction_opt, result)) {
        result = previous;
        return i;
      }
    }
  }
  return lines.size();
}

/** When no velocity satisfies every line: the one that breaks the agents' lines (after the
 * first `obstacle_lines`, which always hold) the least. */
inline void linear_program3(const std::vector<Line> &lines,
                            const size_t obstacle_lines,
                            const size_t begin_line,
                            const float radius,
                            Vec2 &result,
                            std::vector<Line> &scratch)
{
  float distance = 0.0f;
  for (size_t i = begin_line; i < lines.size(); i++) {
    if (det(lines[i].direction, lines[i].point - result) > distance) {
      scratch.assign(lines.begin(), lines.begin() + std::ptrdiff_t(obstacle_lines));
      for (size_t j = obstacle_lines; j < i; j++) {
        Line line;
        const float determinant = det(lines[i].direction, lines[j].direction);
        if (std::fabs(determinant) <= kEpsilon) {
          if (dot(lines[i].direction, lines[j].direction) > 0.0f) {
            /* The same direction. */
            continue;
          }
          line.point = 0.5f * (lines[i].point + lines[j].point);
        }
        else {
          line.point = lines[i].point +
                       (det(lines[j].direction, lines[i].point - lines[j].point) / determinant) *
                           lines[i].direction;
        }
        line.direction = normalize(lines[j].direction - lines[i].direction);
        scratch.push_back(line);
      }
      const Vec2 previous = result;
      if (linear_program2(scratch,
                          radius,
                          Vec2(-lines[i].direction.y, lines[i].direction.x),
                          true,
                          result) < scratch.size())
      {
        /* Only rounding gets here: keep the last velocity found. */
        result = previous;
      }
      distance = det(lines[i].direction, lines[i].point - result);
    }
  }
}

struct Neighbour {
  Vec2 position;
  Vec2 velocity;
  float radius = 0.0f;
  /** The share of the dodge this agent takes: 0.5 between equals, 1 against something that
   * won't move (an obstacle, an NPC standing still). */
  float responsibility = 0.5f;
};

struct Wall {
  Vec2 a, b;
  /** The way in (unit; zero: none known), for an agent standing on the wall's line (from the
   * navmesh's own winding: such an agent got no wall at all). */
  Vec2 inward;
  /** Whether the walkable side wraps round its end `a` / `b` (a corner the way may turn round;
   * false: another wall goes on there, or none known). */
  bool a_convex = false, b_convex = false;
};

struct Params {
  /** How far ahead (s) other agents are avoided. */
  float time_horizon = 2.0f;
  /** How far ahead (s) walls are. */
  float time_horizon_walls = 0.5f;
  /** The step (s): agents already overlapping push apart within it. */
  float time_step = 1.0f / 60.0f;
  /** How near a wall's line the agent's centre may come. A navmesh's walls are already the
   * agent's radius in from the real walls: 0 (else they would count it twice). */
  float wall_radius = 0.0f;
  /** How near (m, along the wall) a corner a wall no longer holds the agent: the corner is the
   * end of a way's leg, and a wall's line held as far as the corner stopped an agent within a
   * hair of it whose way on clipped the corner by that hair (2026-11-19). 0: walls hold to their
   * ends. The navmesh is the agent's radius in from the real wall: a few cm cut off a corner
   * touch nothing. */
  float wall_corner_release = 0.0f;
};

/** The ORCA lines of one agent (at the origin of `position`'s space), walls first. */
inline void build_lines(const Vec2 &position,
                        const Vec2 &velocity,
                        const float radius,
                        const Neighbour *neighbours,
                        const size_t neighbour_count,
                        const Wall *walls,
                        const size_t wall_count,
                        const Params &params,
                        std::vector<Line> &r_lines,
                        size_t &r_wall_lines)
{
  r_lines.clear();
  /* Walls: never closer than the radius, at most at the speed that reaches it in the horizon. */
  const float inv_wall_horizon = 1.0f / std::max(params.time_horizon_walls, params.time_step);
  for (size_t i = 0; i < wall_count; i++) {
    const Vec2 ab = walls[i].b - walls[i].a;
    const float len_sq = abs_sq(ab);
    if (len_sq <= kEpsilon) {
      continue;
    }
    /* Only beside the segment: at its ends the next segment holds the agent (an end's direction
     * to the agent would forbid going round a corner the path turns at). */
    const float t = dot(position - walls[i].a, ab) / len_sq;
    if (t <= 0.0f || t >= 1.0f) {
      continue;
    }
    if (params.wall_corner_release > 0.0f) {
      const float len = std::sqrt(len_sq);
      if ((walls[i].a_convex && t * len < params.wall_corner_release) ||
          (walls[i].b_convex && (1.0f - t) * len < params.wall_corner_release))
      {
        continue;
      }
    }
    const Vec2 closest = walls[i].a + t * ab;
    const Vec2 away = position - closest;
    float dist = length(away);
    Vec2 n;
    if (abs_sq(walls[i].inward) > 0.5f) {
      /* Its way in known (the navmesh's winding): beside the wall but behind its line, the agent
       * stands round a corner of it, never in it (the navmesh holds it): the line would hold it
       * back from the corner it walks round (DetourCrowd leaves such walls out: 2026-11-19, a
       * guard crawled 1.5 s at each corner so). On the line (where the slide along the navmesh
       * leaves one pressing into it): held in. */
      const float side = dot(away, walls[i].inward);
      if (side < -1e-3f) {
        continue;
      }
      n = walls[i].inward;
      dist = std::max(side, 0.0f);
    }
    else if (dist > 1e-3f) {
      n = away / dist;
    }
    else {
      continue;
    }
    /* n . v >= -(dist - wall_radius) / horizon. */
    const float k = -(dist - params.wall_radius) * inv_wall_horizon;
    Line line;
    line.point = n * k;
    line.direction = Vec2(n.y, -n.x);
    r_lines.push_back(line);
  }
  r_wall_lines = r_lines.size();

  const float inv_time_horizon = 1.0f / params.time_horizon;
  for (size_t i = 0; i < neighbour_count; i++) {
    const Neighbour &other = neighbours[i];
    const Vec2 relative_position = other.position - position;
    const Vec2 relative_velocity = velocity - other.velocity;
    const float dist_sq = abs_sq(relative_position);
    const float combined_radius = radius + other.radius;
    const float combined_radius_sq = combined_radius * combined_radius;
    Line line;
    Vec2 u;
    if (dist_sq > combined_radius_sq) {
      const Vec2 w = relative_velocity - inv_time_horizon * relative_position;
      const float w_length_sq = abs_sq(w);
      const float dot_product1 = dot(w, relative_position);
      if (dot_product1 < 0.0f && dot_product1 * dot_product1 > combined_radius_sq * w_length_sq) {
        /* Onto the cut-off circle. */
        const float w_length = std::sqrt(w_length_sq);
        if (w_length <= kEpsilon) {
          continue;
        }
        const Vec2 unit_w = w / w_length;
        line.direction = Vec2(unit_w.y, -unit_w.x);
        u = (combined_radius * inv_time_horizon - w_length) * unit_w;
      }
      else {
        /* Onto a leg of the cone. */
        const float leg = std::sqrt(dist_sq - combined_radius_sq);
        if (det(relative_position, w) > 0.0f) {
          line.direction = Vec2(relative_position.x * leg - relative_position.y * combined_radius,
                                relative_position.x * combined_radius +
                                    relative_position.y * leg) /
                           dist_sq;
        }
        else {
          line.direction = -Vec2(relative_position.x * leg + relative_position.y * combined_radius,
                                 -relative_position.x * combined_radius +
                                     relative_position.y * leg) /
                           dist_sq;
        }
        const float dot_product2 = dot(relative_velocity, line.direction);
        u = dot_product2 * line.direction - relative_velocity;
      }
    }
    else {
      /* Already overlapping: apart within one step. */
      const float inv_time_step = 1.0f / params.time_step;
      Vec2 w = relative_velocity - inv_time_step * relative_position;
      float w_length = length(w);
      if (w_length <= kEpsilon) {
        /* Same place, same velocity: part along a fixed side. */
        w = Vec2(1.0f, 0.0f);
        w_length = 1.0f;
      }
      const Vec2 unit_w = w / w_length;
      line.direction = Vec2(unit_w.y, -unit_w.x);
      u = (combined_radius * inv_time_step - w_length) * unit_w;
    }
    line.point = velocity + other.responsibility * u;
    r_lines.push_back(line);
  }
}

/** The new velocity of one agent: the allowed velocity nearest `preferred`, at most
 * `max_speed`. `scratch` is reused between calls (one per thread). */
inline Vec2 solve(const Vec2 &position,
                  const Vec2 &velocity,
                  const Vec2 &preferred,
                  const float radius,
                  const float max_speed,
                  const Neighbour *neighbours,
                  const size_t neighbour_count,
                  const Wall *walls,
                  const size_t wall_count,
                  const Params &params,
                  std::vector<Line> &lines,
                  std::vector<Line> &scratch)
{
  size_t wall_lines = 0;
  build_lines(position,
              velocity,
              radius,
              neighbours,
              neighbour_count,
              walls,
              wall_count,
              params,
              lines,
              wall_lines);
  Vec2 result;
  const size_t satisfied = linear_program2(lines, max_speed, preferred, false, result);
  if (satisfied < lines.size()) {
    linear_program3(lines, wall_lines, satisfied, max_speed, result, scratch);
  }
  return result;
}

}  // namespace kx_orca
