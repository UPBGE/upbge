/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_PersistenceCapability.cpp
 *  \ingroup logicnodes
 */

#include "LN_PersistenceCapability.h"

LN_PersistenceCapabilityIssue LN_ValidatePersistenceCandidate(
    const LN_PersistenceCandidateDescriptor &descriptor,
    const LN_PersistenceCapabilityRequirements &requirements)
{
  if (requirements.require_scene_identity && !descriptor.scene_identity_valid) {
    return LN_PersistenceCapabilityIssue::MissingSceneIdentity;
  }
  if (requirements.require_object_identity && !descriptor.object_identity_valid) {
    return LN_PersistenceCapabilityIssue::MissingObjectIdentity;
  }
  if (descriptor.object_kind == LN_PersistenceObjectKind::Unsupported) {
    return LN_PersistenceCapabilityIssue::UnsupportedObjectKind;
  }
  if (!descriptor.runtime_class_supported) {
    return LN_PersistenceCapabilityIssue::UnsupportedRuntimeClass;
  }
  if (descriptor.runtime_role != LN_PersistenceRuntimeRole::Ordinary) {
    return LN_PersistenceCapabilityIssue::UnsupportedRuntimeRole;
  }
  if (requirements.require_independent_reconstruction) {
    if (!requirements.allow_normal_hierarchy) {
      if (descriptor.has_authored_parent) {
        return LN_PersistenceCapabilityIssue::AuthoredParent;
      }
      if (descriptor.has_runtime_parent) {
        return LN_PersistenceCapabilityIssue::RuntimeParent;
      }
      if (descriptor.has_authored_children) {
        return LN_PersistenceCapabilityIssue::AuthoredChildren;
      }
      if (descriptor.has_runtime_children) {
        return LN_PersistenceCapabilityIssue::RuntimeChildren;
      }
    }
    if (descriptor.has_collection_instance) {
      return LN_PersistenceCapabilityIssue::CollectionInstance;
    }
    if (descriptor.has_constraints) {
      return LN_PersistenceCapabilityIssue::ConstraintGraph;
    }
    if (descriptor.has_python_runtime) {
      return LN_PersistenceCapabilityIssue::PythonRuntime;
    }
    if (descriptor.has_logic_bricks) {
      return LN_PersistenceCapabilityIssue::LogicBricks;
    }
    if (descriptor.is_compound) {
      return LN_PersistenceCapabilityIssue::CompoundPhysics;
    }
    if (descriptor.has_lod) {
      return LN_PersistenceCapabilityIssue::LevelOfDetail;
    }
    if (descriptor.has_obstacle) {
      return LN_PersistenceCapabilityIssue::ObstacleSimulation;
    }
    if (descriptor.has_scene_graph_controllers) {
      return LN_PersistenceCapabilityIssue::SceneGraphController;
    }
    if (descriptor.has_soft_body_pin_dependency) {
      return LN_PersistenceCapabilityIssue::SoftBodyPinDependency;
    }
    if (!descriptor.reconstruction_state_valid) {
      return LN_PersistenceCapabilityIssue::InvalidReconstructionState;
    }
    if (!descriptor.reconstruction_render_data_valid) {
      return LN_PersistenceCapabilityIssue::InvalidRenderData;
    }
    if (!descriptor.reconstruction_properties_supported) {
      return LN_PersistenceCapabilityIssue::UnsupportedReconstructionProperty;
    }
  }
  if (descriptor.physics_kind == LN_PersistencePhysicsKind::Unsupported) {
    return LN_PersistenceCapabilityIssue::UnsupportedPhysicsBackend;
  }
  if (descriptor.expects_physics_body &&
      (descriptor.physics_kind == LN_PersistencePhysicsKind::None ||
       !descriptor.physics_body_valid))
  {
    return LN_PersistenceCapabilityIssue::MissingPhysicsBody;
  }
  if ((descriptor.physics_kind == LN_PersistencePhysicsKind::JoltRigid ||
       descriptor.physics_kind == LN_PersistencePhysicsKind::JoltSoft) &&
      descriptor.object_kind != LN_PersistenceObjectKind::Mesh)
  {
    return LN_PersistenceCapabilityIssue::PhysicsRequiresMesh;
  }
  return LN_PersistenceCapabilityIssue::None;
}

const char *LN_PersistenceCapabilityIssueText(const LN_PersistenceCapabilityIssue issue)
{
  switch (issue) {
    case LN_PersistenceCapabilityIssue::None:
      return "supported";
    case LN_PersistenceCapabilityIssue::MissingSceneIdentity:
      return "checkpoint Scene has no durable persistence UUID";
    case LN_PersistenceCapabilityIssue::MissingObjectIdentity:
      return "object/source has no durable persistence UUID";
    case LN_PersistenceCapabilityIssue::UnsupportedObjectKind:
      return "only Empty and Mesh objects can be independently reconstructed";
    case LN_PersistenceCapabilityIssue::UnsupportedRuntimeClass:
      return "runtime object class cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::UnsupportedRuntimeRole:
      return "special runtime roles cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::AuthoredParent:
      return "authored parent hierarchies are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::RuntimeParent:
      return "runtime-parented objects are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::AuthoredChildren:
      return "authored child hierarchies are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::RuntimeChildren:
      return "runtime child hierarchies are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::CollectionInstance:
      return "collection instances cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::ConstraintGraph:
      return "constraint graphs cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::PythonRuntime:
      return "Python proxies/components are outside C++ Logic Nodes persistence";
    case LN_PersistenceCapabilityIssue::LogicBricks:
      return "Logic Bricks are outside C++ Logic Nodes persistence";
    case LN_PersistenceCapabilityIssue::CompoundPhysics:
      return "compound physics hierarchies are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::LevelOfDetail:
      return "LOD-managed objects cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::ObstacleSimulation:
      return "obstacle-simulation objects cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::SceneGraphController:
      return "scene-graph animation controllers cannot be independently reconstructed";
    case LN_PersistenceCapabilityIssue::SoftBodyPinDependency:
      return "soft-body pin groups/objects are cross-object dependencies and are not independently reconstructable";
    case LN_PersistenceCapabilityIssue::InvalidReconstructionState:
      return "the baseline transform, visual, or activity state is invalid for reconstruction";
    case LN_PersistenceCapabilityIssue::InvalidRenderData:
      return "the baseline render-mesh recipe is incomplete or does not match the object kind";
    case LN_PersistenceCapabilityIssue::UnsupportedReconstructionProperty:
      return "baseline game properties exceed limits or contain an unsupported value/marker contract";
    case LN_PersistenceCapabilityIssue::UnsupportedPhysicsBackend:
      return "only plain objects and supported Jolt bodies are reconstructable";
    case LN_PersistenceCapabilityIssue::MissingPhysicsBody:
      return "the expected Jolt body was not constructed or is no longer valid";
    case LN_PersistenceCapabilityIssue::PhysicsRequiresMesh:
      return "persistent Jolt bodies must be mesh based";
  }
  return "unknown persistence capability issue";
}
