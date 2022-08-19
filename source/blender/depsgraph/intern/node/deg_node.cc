/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup depsgraph
 */

#include "intern/node/deg_node.h"

#include <cstdio>

#include "BLI_utildefines.h"

#include "intern/depsgraph.h"
#include "intern/depsgraph_relation.h"
#include "intern/eval/deg_eval_copy_on_write.h"
#include "intern/node/deg_node_component.h"
#include "intern/node/deg_node_factory.h"
#include "intern/node/deg_node_id.h"
#include "intern/node/deg_node_operation.h"
#include "intern/node/deg_node_time.h"

namespace blender::deg {

const char *nodeClassAsString(NodeClass node_class)
{
  switch (node_class) {
    case NodeClass::GENERIC:
      return "GENERIC";
    case NodeClass::COMPONENT:
      return "COMPONENT";
    case NodeClass::OPERATION:
      return "OPERATION";
  }
  BLI_assert_msg(0, "Unhandled node class, should never happen.");
  return "UNKNOWN";
}

const char *nodeTypeAsString(NodeType type)
{
  switch (type) {
    case NodeType::UNDEFINED:
      return "UNDEFINED";
    case NodeType::OPERATION:
      return "OPERATION";
    /* **** Generic Types **** */
    case NodeType::TIMESOURCE:
      return "TIMESOURCE";
    case NodeType::ID_REF:
      return "ID_REF";
    /* **** Outer Types **** */
    case NodeType::PARAMETERS:
      return "PARAMETERS";
    case NodeType::ANIMATION:
      return "ANIMATION";
    case NodeType::TRANSFORM:
      return "TRANSFORM";
    case NodeType::GEOMETRY:
      return "GEOMETRY";
    case NodeType::SEQUENCER:
      return "SEQUENCER";
    case NodeType::LAYER_COLLECTIONS:
      return "LAYER_COLLECTIONS";
    case NodeType::COPY_ON_WRITE:
      return "COPY_ON_WRITE";
    case NodeType::OBJECT_FROM_LAYER:
      return "OBJECT_FROM_LAYER";
    /* **** Evaluation-Related Outer Types (with Subdata) **** */
    case NodeType::EVAL_POSE:
      return "EVAL_POSE";
    case NodeType::BONE:
      return "BONE";
    case NodeType::PARTICLE_SYSTEM:
      return "PARTICLE_SYSTEM";
    case NodeType::PARTICLE_SETTINGS:
      return "PARTICLE_SETTINGS";
    case NodeType::SHADING:
      return "SHADING";
    case NodeType::CACHE:
      return "CACHE";
    case NodeType::POINT_CACHE:
      return "POINT_CACHE";
    case NodeType::IMAGE_ANIMATION:
      return "IMAGE_ANIMATION";
    case NodeType::BATCH_CACHE:
      return "BATCH_CACHE";
    case NodeType::DUPLI:
      return "DUPLI";
    case NodeType::SYNCHRONIZATION:
      return "SYNCHRONIZATION";
    case NodeType::AUDIO:
      return "AUDIO";
    case NodeType::ARMATURE:
      return "ARMATURE";
    case NodeType::GENERIC_DATABLOCK:
      return "GENERIC_DATABLOCK";
    case NodeType::VISIBILITY:
      return "VISIBILITY";
    case NodeType::SIMULATION:
      return "SIMULATION";
    case NodeType::NTREE_OUTPUT:
      return "NTREE_OUTPUT";

    /* Total number of meaningful node types. */
    case NodeType::NUM_TYPES:
      return "SpecialCase";
  }
  BLI_assert_msg(0, "Unhandled node type, should never happen.");
  return "UNKNOWN";
}

NodeType nodeTypeFromSceneComponent(eDepsSceneComponentType component)
{
  switch (component) {
    case DEG_SCENE_COMP_PARAMETERS:
      return NodeType::PARAMETERS;
    case DEG_SCENE_COMP_ANIMATION:
      return NodeType::ANIMATION;
    case DEG_SCENE_COMP_SEQUENCER:
      return NodeType::SEQUENCER;
  }
  return NodeType::UNDEFINED;
}

eDepsSceneComponentType nodeTypeToSceneComponent(NodeType type)
{
  switch (type) {
    case NodeType::PARAMETERS:
      return DEG_SCENE_COMP_PARAMETERS;
    case NodeType::ANIMATION:
      return DEG_SCENE_COMP_ANIMATION;
    case NodeType::SEQUENCER:
      return DEG_SCENE_COMP_SEQUENCER;

    case NodeType::OPERATION:
    case NodeType::TIMESOURCE:
    case NodeType::ID_REF:
    case NodeType::LAYER_COLLECTIONS:
    case NodeType::COPY_ON_WRITE:
    case NodeType::OBJECT_FROM_LAYER:
    case NodeType::AUDIO:
    case NodeType::ARMATURE:
    case NodeType::GENERIC_DATABLOCK:
    case NodeType::PARTICLE_SYSTEM:
    case NodeType::PARTICLE_SETTINGS:
    case NodeType::POINT_CACHE:
    case NodeType::IMAGE_ANIMATION:
    case NodeType::BATCH_CACHE:
    case NodeType::DUPLI:
    case NodeType::SYNCHRONIZATION:
    case NodeType::UNDEFINED:
    case NodeType::NUM_TYPES:
    case NodeType::TRANSFORM:
    case NodeType::GEOMETRY:
    case NodeType::EVAL_POSE:
    case NodeType::BONE:
    case NodeType::SHADING:
    case NodeType::CACHE:
    case NodeType::SIMULATION:
    case NodeType::NTREE_OUTPUT:
      return DEG_SCENE_COMP_PARAMETERS;

    case NodeType::VISIBILITY:
      BLI_assert_msg(0, "Visibility component is supposed to be only used internally.");
      return DEG_SCENE_COMP_PARAMETERS;
  }
  BLI_assert_msg(0, "Unhandled node type, not supposed to happen.");
  return DEG_SCENE_COMP_PARAMETERS;
}

NodeType nodeTypeFromObjectComponent(eDepsObjectComponentType component_type)
{
  switch (component_type) {
    case DEG_OB_COMP_ANY:
      return NodeType::UNDEFINED;
    case DEG_OB_COMP_PARAMETERS:
      return NodeType::PARAMETERS;
    case DEG_OB_COMP_ANIMATION:
      return NodeType::ANIMATION;
    case DEG_OB_COMP_TRANSFORM:
      return NodeType::TRANSFORM;
    case DEG_OB_COMP_GEOMETRY:
      return NodeType::GEOMETRY;
    case DEG_OB_COMP_EVAL_POSE:
      return NodeType::EVAL_POSE;
    case DEG_OB_COMP_BONE:
      return NodeType::BONE;
    case DEG_OB_COMP_SHADING:
      return NodeType::SHADING;
    case DEG_OB_COMP_CACHE:
      return NodeType::CACHE;
  }
  return NodeType::UNDEFINED;
}

eDepsObjectComponentType nodeTypeToObjectComponent(NodeType type)
{
  switch (type) {
    case NodeType::PARAMETERS:
      return DEG_OB_COMP_PARAMETERS;
    case NodeType::ANIMATION:
      return DEG_OB_COMP_ANIMATION;
    case NodeType::TRANSFORM:
      return DEG_OB_COMP_TRANSFORM;
    case NodeType::GEOMETRY:
      return DEG_OB_COMP_GEOMETRY;
    case NodeType::EVAL_POSE:
      return DEG_OB_COMP_EVAL_POSE;
    case NodeType::BONE:
      return DEG_OB_COMP_BONE;
    case NodeType::SHADING:
      return DEG_OB_COMP_SHADING;
    case NodeType::CACHE:
      return DEG_OB_COMP_CACHE;

    case NodeType::OPERATION:
    case NodeType::TIMESOURCE:
    case NodeType::ID_REF:
    case NodeType::SEQUENCER:
    case NodeType::LAYER_COLLECTIONS:
    case NodeType::COPY_ON_WRITE:
    case NodeType::OBJECT_FROM_LAYER:
    case NodeType::AUDIO:
    case NodeType::ARMATURE:
    case NodeType::GENERIC_DATABLOCK:
    case NodeType::PARTICLE_SYSTEM:
    case NodeType::PARTICLE_SETTINGS:
    case NodeType::POINT_CACHE:
    case NodeType::IMAGE_ANIMATION:
    case NodeType::BATCH_CACHE:
    case NodeType::DUPLI:
    case NodeType::SYNCHRONIZATION:
    case NodeType::SIMULATION:
    case NodeType::NTREE_OUTPUT:
    case NodeType::UNDEFINED:
    case NodeType::NUM_TYPES:
      return DEG_OB_COMP_PARAMETERS;

    case NodeType::VISIBILITY:
      BLI_assert_msg(0, "Visibility component is supposed to be only used internally.");
      return DEG_OB_COMP_PARAMETERS;
  }
  BLI_assert_msg(0, "Unhandled node type, not suppsed to happen.");
  return DEG_OB_COMP_PARAMETERS;
}

/*******************************************************************************
 * Type information.
 */

Node::TypeInfo::TypeInfo(NodeType type, const char *type_name, int id_recalc_tag)
    : type(type), type_name(type_name), id_recalc_tag(id_recalc_tag)
{
}

/*******************************************************************************
 * Evaluation statistics.
 */

Node::Stats::Stats()
{
  reset();
}

void Node::Stats::reset()
{
  current_time = 0.0;
}

void Node::Stats::reset_current()
{
  current_time = 0.0;
}

/*******************************************************************************
 * Node itself.
 */

Node::Node()
{
  name = "";
}

Node::~Node()
{
  /* Free links. */
  /* NOTE: We only free incoming links. This is to avoid double-free of links
   * when we're trying to free same link from both its sides. We don't have
   * dangling links so this is not a problem from memory leaks point of view. */
  for (Relation *rel : inlinks) {
    delete rel;
  }
}

string Node::identifier() const
{
  return string(nodeTypeAsString(type)) + " : " + name;
}

NodeClass Node::get_class() const
{
  if (type == NodeType::OPERATION) {
    return NodeClass::OPERATION;
  }
  if (type < NodeType::PARAMETERS) {
    return NodeClass::GENERIC;
  }

  return NodeClass::COMPONENT;
}

/*******************************************************************************
 * Generic nodes definition.
 */

DEG_DEPSNODE_DEFINE(TimeSourceNode, NodeType::TIMESOURCE, "Time Source");
static DepsNodeFactoryImpl<TimeSourceNode> DNTI_TIMESOURCE;

DEG_DEPSNODE_DEFINE(IDNode, NodeType::ID_REF, "ID Node");
static DepsNodeFactoryImpl<IDNode> DNTI_ID_REF;

void deg_register_base_depsnodes()
{
  register_node_typeinfo(&DNTI_TIMESOURCE);
  register_node_typeinfo(&DNTI_ID_REF);
}

}  // namespace blender::deg
