/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_PersistenceCapability.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>

enum class LN_PersistenceObjectKind : uint8_t {
  Empty = 0,
  Mesh,
  Unsupported,
};

enum class LN_PersistenceRuntimeRole : uint8_t {
  Ordinary = 0,
  Occluder,
  Sensor,
  Navigation,
  Character,
  Vehicle,
  Unsupported,
};

enum class LN_PersistencePhysicsKind : uint8_t {
  None = 0,
  JoltRigid,
  JoltSoft,
  Unsupported,
};

/** Immutable facts captured when the runtime scene/source catalog is converted. */
struct LN_PersistenceCandidateDescriptor {
  LN_PersistenceObjectKind object_kind = LN_PersistenceObjectKind::Unsupported;
  LN_PersistenceRuntimeRole runtime_role = LN_PersistenceRuntimeRole::Unsupported;
  LN_PersistencePhysicsKind physics_kind = LN_PersistencePhysicsKind::Unsupported;
  bool scene_identity_valid = false;
  bool object_identity_valid = false;
  bool runtime_class_supported = false;
  bool expects_physics_body = false;
  bool physics_body_valid = false;
  bool has_authored_parent = false;
  bool has_runtime_parent = false;
  bool has_authored_children = false;
  bool has_runtime_children = false;
  bool has_collection_instance = false;
  bool has_constraints = false;
  bool has_python_runtime = false;
  bool has_logic_bricks = false;
  bool is_compound = false;
  bool has_lod = false;
  bool has_obstacle = false;
  bool has_scene_graph_controllers = false;
  bool has_soft_body_pin_dependency = false;
  bool reconstruction_state_valid = false;
  bool reconstruction_render_data_valid = false;
  bool reconstruction_properties_supported = false;
};

enum class LN_PersistenceCapabilityIssue : uint8_t {
  None = 0,
  MissingSceneIdentity,
  MissingObjectIdentity,
  UnsupportedObjectKind,
  UnsupportedRuntimeClass,
  UnsupportedRuntimeRole,
  AuthoredParent,
  RuntimeParent,
  AuthoredChildren,
  RuntimeChildren,
  CollectionInstance,
  ConstraintGraph,
  PythonRuntime,
  LogicBricks,
  CompoundPhysics,
  LevelOfDetail,
  ObstacleSimulation,
  SceneGraphController,
  SoftBodyPinDependency,
  InvalidReconstructionState,
  InvalidRenderData,
  UnsupportedReconstructionProperty,
  UnsupportedPhysicsBackend,
  MissingPhysicsBody,
  PhysicsRequiresMesh,
};

struct LN_PersistenceCapabilityRequirements {
  bool require_scene_identity = true;
  bool require_object_identity = true;
  bool require_independent_reconstruction = true;
  bool allow_normal_hierarchy = false;
};

/** Pure, side-effect-free Save Object State capability decision. */
LN_PersistenceCapabilityIssue LN_ValidatePersistenceCandidate(
    const LN_PersistenceCandidateDescriptor &descriptor,
    const LN_PersistenceCapabilityRequirements &requirements = {});

/** Stable bounded explanation suitable for composing an object-specific diagnostic. */
const char *LN_PersistenceCapabilityIssueText(LN_PersistenceCapabilityIssue issue);
