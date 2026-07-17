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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file PHY_RigidBodyConstraintSettings.h
 *  \ingroup phys
 */

#pragma once

#include <cstdint>

enum class PHY_RigidBodyConstraintType : uint8_t {
  Point = 0,
  Hinge = 1,
  Slider = 3,
  Generic = 5,
  GenericSpring = 6,
  Fixed = 8,
  Piston = 9,
  Motor = 11,
};

enum class PHY_RigidBodyConstraintSpringType : uint8_t {
  Spring1 = 0,
  Spring2 = 1,
};

enum PHY_RigidBodyConstraintFlag : uint32_t {
  PHY_RB_CONSTRAINT_ENABLED = (1u << 0),
  PHY_RB_CONSTRAINT_DISABLE_COLLISIONS = (1u << 1),
  PHY_RB_CONSTRAINT_USE_BREAKING = (1u << 2),
  PHY_RB_CONSTRAINT_OVERRIDE_SOLVER_ITERATIONS = (1u << 3),
  PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X = (1u << 4),
  PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Y = (1u << 5),
  PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Z = (1u << 6),
  PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X = (1u << 7),
  PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Y = (1u << 8),
  PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z = (1u << 9),
  PHY_RB_CONSTRAINT_USE_SPRING_X = (1u << 10),
  PHY_RB_CONSTRAINT_USE_SPRING_Y = (1u << 11),
  PHY_RB_CONSTRAINT_USE_SPRING_Z = (1u << 12),
  PHY_RB_CONSTRAINT_USE_SPRING_ANG_X = (1u << 13),
  PHY_RB_CONSTRAINT_USE_SPRING_ANG_Y = (1u << 14),
  PHY_RB_CONSTRAINT_USE_SPRING_ANG_Z = (1u << 15),
  PHY_RB_CONSTRAINT_USE_MOTOR_LIN = (1u << 16),
  PHY_RB_CONSTRAINT_USE_MOTOR_ANG = (1u << 17),
  PHY_RB_CONSTRAINT_JOLT_OVERRIDE_SOLVER_ITERATIONS = (1u << 18),
};

struct PHY_RigidBodyConstraintSettings {
  PHY_RigidBodyConstraintType type = PHY_RigidBodyConstraintType::Point;
  PHY_RigidBodyConstraintSpringType spring_type = PHY_RigidBodyConstraintSpringType::Spring2;
  uint32_t flags = PHY_RB_CONSTRAINT_ENABLED | PHY_RB_CONSTRAINT_DISABLE_COLLISIONS;

  int num_solver_iterations = 10;
  int jolt_velocity_solver_iterations = 10;
  int jolt_position_solver_iterations = 2;
  float breaking_threshold = 10.0f;

  float limit_lin_x_lower = -1.0f;
  float limit_lin_x_upper = 1.0f;
  float limit_lin_y_lower = -1.0f;
  float limit_lin_y_upper = 1.0f;
  float limit_lin_z_lower = -1.0f;
  float limit_lin_z_upper = 1.0f;
  float limit_ang_x_lower = -0.7853981633974483f;
  float limit_ang_x_upper = 0.7853981633974483f;
  float limit_ang_y_lower = -0.7853981633974483f;
  float limit_ang_y_upper = 0.7853981633974483f;
  float limit_ang_z_lower = -0.7853981633974483f;
  float limit_ang_z_upper = 0.7853981633974483f;

  float spring_stiffness_x = 10.0f;
  float spring_stiffness_y = 10.0f;
  float spring_stiffness_z = 10.0f;
  float spring_stiffness_ang_x = 10.0f;
  float spring_stiffness_ang_y = 10.0f;
  float spring_stiffness_ang_z = 10.0f;
  float spring_damping_x = 0.5f;
  float spring_damping_y = 0.5f;
  float spring_damping_z = 0.5f;
  float spring_damping_ang_x = 0.5f;
  float spring_damping_ang_y = 0.5f;
  float spring_damping_ang_z = 0.5f;

  float motor_lin_target_velocity = 1.0f;
  float motor_ang_target_velocity = 1.0f;
  float motor_lin_max_impulse = 1.0f;
  float motor_ang_max_impulse = 1.0f;
};
