/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * Builders the Logic Nodes tests share: hand-made node trees (TestNode, the Init* functions,
 * links), scene-graph test objects (RootRelation, UIObject, BatchObject, PositionedGameObject),
 * the Every N Steps rig (SteppedTrees) and the NPC brains of the benchmarks (CompileBrain,
 * BrainOf). Header only: every function is inline, everything is in `ln_test`. A test file does
 * `#include "LN_TestTrees.h"` and `using namespace ln_test;` (lean runtime plan, phase A0;
 * moved verbatim from LN_TreeCompiler_test.cpp and LN_RuntimeTree_test.cpp). */

#include "testing/testing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "BKE_colortools.hh"
#include "DEV_InputDevice.h"
#include "DNA_ID_enums.h"
#include "DNA_collection_types.h"
#include "DNA_color_types.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "LN_BindingSource.h"
#include "LN_CommandBuffer.h"
#include "LN_JobPool.h"
#include "LN_Manager.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "LN_TreeBatch.h"
#include "LN_StrictDump.h"
#include "LN_TreeCompiler.h"
#include "LN_UIService.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"
#include "SG_Node.h"

namespace ln_test {

template<size_t Size> void SetCString(char (&dst)[Size], const char *value)
{
  dst[0] = '\0';
  std::strncpy(dst, value, Size - 1);
  dst[Size - 1] = '\0';
}

template<typename T> void Append(blender::ListBaseT<T> &list, T &item)
{
  item.next = nullptr;
  item.prev = static_cast<T *>(list.last);
  if (list.last != nullptr) {
    static_cast<T *>(list.last)->next = &item;
  }
  else {
    list.first = &item;
  }
  list.last = &item;
}

template<typename T> void Remove(blender::ListBaseT<T> &list, T &item)
{
  if (item.prev != nullptr) {
    item.prev->next = item.next;
  }
  else {
    list.first = item.next;
  }
  if (item.next != nullptr) {
    item.next->prev = item.prev;
  }
  else {
    list.last = item.prev;
  }
  item.next = nullptr;
  item.prev = nullptr;
}

inline void InitTree(blender::bNodeTree &tree)
{
  SetCString(tree.id.name, "NTCompilerProbe");
  SetCString(tree.idname, "LogicNodeTree");
  tree.type = blender::NTREE_LOGIC;
}

inline void InitNode(blender::bNode &node,
                     const char *idname,
                     const char *name,
                     int32_t identifier)
{
  SetCString(node.idname, idname);
  SetCString(node.name, name);
  node.identifier = identifier;
}

inline void InitMaterial(blender::Material &material, const char *name)
{
  *reinterpret_cast<uint16_t *>(material.id.name) = uint16_t(blender::ID_MA);
  std::strncpy(material.id.name + 2, name, sizeof(material.id.name) - 3);
  material.id.name[sizeof(material.id.name) - 1] = '\0';
}

inline void InitGeometryModifierGroup(blender::bNodeTree &tree,
                                      blender::GeometryNodeAssetTraits &traits,
                                      const char *name)
{
  *reinterpret_cast<uint16_t *>(tree.id.name) = uint16_t(blender::ID_NT);
  std::strncpy(tree.id.name + 2, name, sizeof(tree.id.name) - 3);
  tree.id.name[sizeof(tree.id.name) - 1] = '\0';
  tree.type = blender::NTREE_GEOMETRY;
  traits.flag = blender::GEO_NODE_ASSET_MODIFIER;
  tree.geometry_node_asset_traits = &traits;
}

inline void InitSocket(blender::bNodeSocket &socket,
                       const char *name,
                       blender::eNodeSocketDatatype type,
                       void *default_value,
                       const char *idname = nullptr)
{
  SetCString(socket.identifier, name);
  SetCString(socket.name, name);
  if (idname != nullptr) {
    SetCString(socket.idname, idname);
  }
  socket.type = type;
  socket.default_value = default_value;
}

inline void InitSocketWithIdentifier(blender::bNodeSocket &socket,
                                     const char *identifier,
                                     const char *name,
                                     blender::eNodeSocketDatatype type,
                                     void *default_value,
                                     const char *idname = nullptr)
{
  SetCString(socket.identifier, identifier);
  SetCString(socket.name, name);
  if (idname != nullptr) {
    SetCString(socket.idname, idname);
  }
  socket.type = type;
  socket.default_value = default_value;
}

/* Object Grid View's inputs for its list (Object, Property) and its Moving Items, Drop Hints,
 * Filter and Use sections, at their defaults: Items gives the list, Moving Items is off. Declare
 * it as a local next to the grid node: the node points at it. */
struct GridListSockets {
  std::array<blender::bNodeSocket, 18> sockets = {};
  blender::bNodeSocketValueObject object = {};
  blender::bNodeSocketValueString property = {};
  blender::bNodeSocketValueBoolean handle_moves = {};
  blender::bNodeSocketValueString split_action = {};
  blender::bNodeSocketValueInt split_amount = {};
  blender::bNodeSocketValueBoolean swap = {};
  blender::bNodeSocketValueBoolean drop_into_world = {};
  blender::bNodeSocketValueBoolean drop_at_pointer = {};
  blender::bNodeSocketValueBoolean drop_in_front = {};
  blender::bNodeSocketValueFloat distance = {};
  blender::bNodeSocketValueFloat hover_height = {};
  blender::bNodeSocketValueObject hint_text = {};
  blender::bNodeSocketValueString accept_hint = {};
  blender::bNodeSocketValueString refuse_hint = {};
  blender::bNodeSocketValueString show_categories = {};
  blender::bNodeSocketValueString search = {};
  blender::bNodeSocketValueString use_action = {};
  blender::bNodeSocketValueString use_keys = {};

  void AppendTo(blender::bNode &grid)
  {
    swap.value = true;
    distance.value = 1.5f;
    SetCString(accept_hint.value, "Put {name} here");
    SetCString(refuse_hint.value, "{name} does not fit here");
    const std::array<std::tuple<const char *, blender::eNodeSocketDatatype, void *>, 18> inputs = {
        {{"Object", blender::SOCK_OBJECT, &object},
         {"Property", blender::SOCK_STRING, &property},
         {"Handle Moves", blender::SOCK_BOOLEAN, &handle_moves},
         {"Split Action", blender::SOCK_STRING, &split_action},
         {"Split Amount", blender::SOCK_INT, &split_amount},
         {"Swap", blender::SOCK_BOOLEAN, &swap},
         {"Drop Into World", blender::SOCK_BOOLEAN, &drop_into_world},
         {"At Pointer", blender::SOCK_BOOLEAN, &drop_at_pointer},
         {"Drop In Front", blender::SOCK_BOOLEAN, &drop_in_front},
         {"Distance", blender::SOCK_FLOAT, &distance},
         {"Hover Height", blender::SOCK_FLOAT, &hover_height},
         {"Hint Text", blender::SOCK_OBJECT, &hint_text},
         {"Accept Hint", blender::SOCK_STRING, &accept_hint},
         {"Refuse Hint", blender::SOCK_STRING, &refuse_hint},
         {"Show Categories", blender::SOCK_STRING, &show_categories},
         {"Search", blender::SOCK_STRING, &search},
         {"Use Action", blender::SOCK_STRING, &use_action},
         {"Use Keys", blender::SOCK_STRING, &use_keys}}};
    for (size_t index = 0; index < inputs.size(); ++index) {
      const auto &[name, type, value] = inputs[index];
      InitSocket(sockets[index], name, type, value);
      Append(grid.inputs, sockets[index]);
    }
  }

  blender::bNodeSocket &Input(const char *name)
  {
    for (blender::bNodeSocket &socket : sockets) {
      if (std::strcmp(socket.name, name) == 0) {
        return socket;
      }
    }
    return sockets.front();
  }
};

inline void InitLink(blender::bNodeLink &link,
                     blender::bNode &from_node,
                     blender::bNodeSocket &from_socket,
                     blender::bNode &to_node,
                     blender::bNodeSocket &to_socket)
{
  link.fromnode = &from_node;
  link.fromsock = &from_socket;
  link.tonode = &to_node;
  link.tosock = &to_socket;
}

struct MaterialParameterValueSockets {
  blender::bNodeSocket generic_value = {};
  blender::bNodeSocket float_value = {};
  blender::bNodeSocket integer_value = {};
  blender::bNodeSocket boolean_value = {};
  blender::bNodeSocket vector_value = {};
  blender::bNodeSocket color_value = {};
};

struct SetParentNodeSockets {
  blender::bNodeSocket flow = {};
  blender::bNodeSocket child = {};
  blender::bNodeSocket parent = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket compound = {};
  blender::bNodeSocket ghost = {};
};

inline void InitSetParentNode(blender::bNode &node,
                              SetParentNodeSockets &sockets,
                              blender::bNodeSocketValueObject &child_default,
                              blender::bNodeSocketValueObject &parent_default,
                              blender::bNodeSocketValueString &bone_name_default,
                              blender::bNodeSocketValueBoolean &compound_default,
                              blender::bNodeSocketValueBoolean &ghost_default,
                              const int32_t identifier)
{
  InitNode(node, "LogicNativeSetParent", "Set Parent", identifier);
  InitSocket(sockets.flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(sockets.child, "Child Object", blender::SOCK_OBJECT, &child_default);
  InitSocket(sockets.parent, "Parent Object", blender::SOCK_OBJECT, &parent_default);
  InitSocket(sockets.bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(sockets.compound, "Compound", blender::SOCK_BOOLEAN, &compound_default);
  InitSocket(sockets.ghost, "Ghost", blender::SOCK_BOOLEAN, &ghost_default);
  Append(node.inputs, sockets.flow);
  Append(node.inputs, sockets.child);
  Append(node.inputs, sockets.parent);
  Append(node.inputs, sockets.bone_name);
  Append(node.inputs, sockets.compound);
  Append(node.inputs, sockets.ghost);
}

inline void AppendMaterialParameterValueSockets(blender::bNode &node,
                                                MaterialParameterValueSockets &sockets,
                                                void *float_default,
                                                void *color_default)
{
  InitSocketWithIdentifier(sockets.generic_value, "Value", "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocketWithIdentifier(sockets.float_value,
                           "Float Value",
                           "Value",
                           blender::SOCK_FLOAT,
                           float_default,
                           "NodeSocketLogicFloat");
  InitSocketWithIdentifier(sockets.integer_value,
                           "Integer Value",
                           "Value",
                           blender::SOCK_INT,
                           nullptr,
                           "NodeSocketLogicInt");
  InitSocketWithIdentifier(sockets.boolean_value,
                           "Boolean Value",
                           "Value",
                           blender::SOCK_BOOLEAN,
                           nullptr,
                           "NodeSocketLogicBool");
  InitSocketWithIdentifier(sockets.vector_value,
                           "Vector Value",
                           "Value",
                           blender::SOCK_VECTOR,
                           nullptr,
                           "NodeSocketLogicVector");
  InitSocketWithIdentifier(sockets.color_value,
                           "Color Value",
                           "Value",
                           blender::SOCK_RGBA,
                           color_default,
                           "NodeSocketLogicColor");
  Append(node.inputs, sockets.generic_value);
  Append(node.inputs, sockets.float_value);
  Append(node.inputs, sockets.integer_value);
  Append(node.inputs, sockets.boolean_value);
  Append(node.inputs, sockets.vector_value);
  Append(node.inputs, sockets.color_value);
}

struct SetObjectAttributeAuxSockets {
  blender::bNodeSocket xyz = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket position = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket color = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket include_children = {};
  blender::bNodeSocket teleport = {};
  blender::bNodeSocket stop_linear = {};
  blender::bNodeSocket stop_angular = {};
  blender::bNodeSocket done = {};
  blender::bNodeSocketValueVector xyz_default = {};
  blender::bNodeSocketValueBoolean teleport_default = {};
  blender::bNodeSocketValueBoolean stop_linear_default = {};
  blender::bNodeSocketValueBoolean stop_angular_default = {};
};

inline void InitSetObjectAttributeVectorNode(blender::bNode &node,
                                             SetObjectAttributeAuxSockets &aux,
                                             blender::bNodeSocket &flow,
                                             blender::bNodeSocket &value,
                                             const char *name,
                                             const int32_t identifier,
                                             const int attribute_type,
                                             void *value_default,
                                             const char *flow_identifier = "Flow",
                                             const char *flow_socket_idname = nullptr)
{
  InitNode(node, "LogicNativeSetObjectAttribute", name, identifier);
  node.custom1 = attribute_type;
  /* The editor initializes all three component-mask bits plus the initialized bit. */
  node.custom2 = 15;
  aux.xyz_default.value[0] = 1.0f;
  aux.xyz_default.value[1] = 1.0f;
  aux.xyz_default.value[2] = 1.0f;

  InitSocket(flow, flow_identifier, blender::SOCK_BOOLEAN, nullptr, flow_socket_idname);
  InitSocket(aux.xyz, "XYZ", blender::SOCK_VECTOR, &aux.xyz_default);
  InitSocket(aux.object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(value, "Value", blender::SOCK_VECTOR, value_default);
  InitSocket(aux.position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(aux.scale, "Scale", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.color, "Color", blender::SOCK_RGBA, nullptr);
  InitSocket(aux.visible, "Visible", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.include_children, "Include Children", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.teleport, "Teleport", blender::SOCK_BOOLEAN, &aux.teleport_default);
  InitSocket(
      aux.stop_linear, "Stop Linear Velocity", blender::SOCK_BOOLEAN, &aux.stop_linear_default);
  InitSocket(aux.stop_angular,
             "Stop Angular Velocity",
             blender::SOCK_BOOLEAN,
             &aux.stop_angular_default);
  InitSocket(aux.done, "Done", blender::SOCK_BOOLEAN, nullptr);

  Append(node.inputs, flow);
  Append(node.inputs, aux.xyz);
  Append(node.inputs, aux.object);
  Append(node.inputs, value);
  Append(node.inputs, aux.position);
  Append(node.inputs, aux.rotation);
  Append(node.inputs, aux.scale);
  Append(node.inputs, aux.color);
  Append(node.inputs, aux.visible);
  Append(node.inputs, aux.include_children);
  Append(node.inputs, aux.teleport);
  Append(node.inputs, aux.stop_linear);
  Append(node.inputs, aux.stop_angular);
  Append(node.outputs, aux.done);
}

struct GetObjectAttributeAuxSockets {
  blender::bNodeSocket object = {};
  blender::bNodeSocket name = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket position = {};
  blender::bNodeSocket orientation = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket color = {};
};

inline void InitGetObjectAttributeVectorNode(blender::bNode &node,
                                             GetObjectAttributeAuxSockets &aux,
                                             blender::bNodeSocket &vector,
                                             const char *name,
                                             const int32_t identifier,
                                             const int attribute_type)
{
  InitNode(node, "LogicNativeGetObjectAttribute", name, identifier);
  node.custom1 = attribute_type;
  InitSocket(aux.object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(aux.name, "Name", blender::SOCK_STRING, nullptr);
  InitSocket(vector, "Vector", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.visible, "Visible", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.orientation, "Orientation", blender::SOCK_ROTATION, nullptr);
  InitSocket(aux.scale, "Scale", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.color, "Color", blender::SOCK_RGBA, nullptr);

  Append(node.inputs, aux.object);
  Append(node.outputs, aux.name);
  Append(node.outputs, vector);
  Append(node.outputs, aux.visible);
  Append(node.outputs, aux.position);
  Append(node.outputs, aux.orientation);
  Append(node.outputs, aux.scale);
  Append(node.outputs, aux.color);
}

inline bool HasWarningContaining(const LN_CompileReport &report, const char *needle)
{
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    if (issue.severity == LN_CompileSeverity::Warning &&
        issue.message.find(needle) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

inline bool HasErrorContaining(const LN_CompileReport &report, const char *needle)
{
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    if (issue.severity == LN_CompileSeverity::Error &&
        issue.message.find(needle) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

inline const char *SeverityName(const LN_CompileSeverity severity)
{
  switch (severity) {
    case LN_CompileSeverity::Info:
      return "info";
    case LN_CompileSeverity::Warning:
      return "warning";
    case LN_CompileSeverity::Error:
      return "error";
  }
  return "unknown";
}

inline std::string FormatCompileReport(const LN_CompileReport &report)
{
  std::string result;
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    result += SeverityName(issue.severity);
    result += ": ";
    result += issue.message;
    result += "\n";
  }
  return result;
}

/* A node for compile tests that keeps its sockets and their defaults alive with it. Declare it
 * as a local: the tree points at it. */
struct TestNode {
  blender::bNode node = {};
  std::deque<blender::bNodeSocket> sockets;
  std::deque<blender::bNodeSocketValueFloat> floats;
  std::deque<blender::bNodeSocketValueInt> ints;
  std::deque<blender::bNodeSocketValueBoolean> bools;
  std::deque<blender::bNodeSocketValueString> strings;
  std::deque<blender::bNodeSocketValueObject> objects;
  std::deque<blender::bNodeSocketValueVector> vectors;

  TestNode(blender::bNodeTree &tree, const char *idname, const char *name, const int32_t id)
  {
    InitNode(node, idname, name, id);
    Append(tree.nodes, node);
  }
  TestNode(const TestNode &) = delete;

  blender::bNodeSocket &add(const bool input,
                            const char *name,
                            const blender::eNodeSocketDatatype type,
                            void *default_value,
                            const char *idname = nullptr)
  {
    blender::bNodeSocket &socket = sockets.emplace_back();
    InitSocket(socket, name, type, default_value, idname);
    Append(input ? node.inputs : node.outputs, socket);
    return socket;
  }
  blender::bNodeSocket &flow(const bool input, const char *name)
  {
    return add(input, name, blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  }
  blender::bNodeSocket &in_float(const char *name, const float value)
  {
    floats.emplace_back().value = value;
    return add(true, name, blender::SOCK_FLOAT, &floats.back());
  }
  blender::bNodeSocket &in_int(const char *name, const int value)
  {
    ints.emplace_back().value = value;
    return add(true, name, blender::SOCK_INT, &ints.back());
  }
  blender::bNodeSocket &in_bool(const char *name, const bool value)
  {
    bools.emplace_back().value = value;
    return add(true, name, blender::SOCK_BOOLEAN, &bools.back());
  }
  blender::bNodeSocket &in_string(const char *name, const char *value)
  {
    SetCString(strings.emplace_back().value, value);
    return add(true, name, blender::SOCK_STRING, &strings.back());
  }
  blender::bNodeSocket &in_object(const char *name)
  {
    return add(true, name, blender::SOCK_OBJECT, &objects.emplace_back());
  }
  blender::bNodeSocket &in_vector(const char *name, const MT_Vector3 &value)
  {
    blender::bNodeSocketValueVector &vector = vectors.emplace_back();
    vector.dimensions = 3;
    vector.value[0] = value.x();
    vector.value[1] = value.y();
    vector.value[2] = value.z();
    return add(true, name, blender::SOCK_VECTOR, &vector);
  }
};

/* A game property read while the game runs: the Value output. */
inline blender::bNodeSocket &AddPropertyRead(TestNode &read, const char *property)
{
  read.in_object("Object");
  read.in_string("Property", property);
  return read.add(false, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
}

/* A scene-graph node for a test object, so the UI can place and hit it. */
class RootRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child, const SG_Node *, bool &parent_updated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    parent_updated = true;
    return true;
  }
  SG_ParentRelation *NewCopy() override
  {
    return new RootRelation();
  }
};

template<typename ObjectType> struct UIObject {
  explicit UIObject(KX_Scene *scene) : node(&object, scene, callbacks)
  {
    node.SetParentRelation(new RootRelation());
    object.SetSGNode(&node);
  }
  ~UIObject()
  {
    object.SetSGNode(nullptr);
  }
  SG_Callbacks callbacks;
  ObjectType object;
  SG_Node node;
};

/* Every N Steps (On Logic + Physics Step). Trees of one program on their own objects, spread
 * 0, 1, 2... as the manager numbers them, run through Logic + Physics steps as the manager runs
 * them: each tree is told the step, then runs if it has work in it (`skip_idle`) or always. A
 * tree must print the same either way: sleeping through a step may never change what it does. */
struct SteppedTrees {
  std::vector<std::unique_ptr<KX_GameObject>> owners;
  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;

  SteppedTrees(const std::shared_ptr<LN_Program> &program, const uint32_t count)
  {
    for (uint32_t index = 0; index < count; index++) {
      owners.push_back(std::make_unique<KX_GameObject>());
      trees.push_back(std::make_unique<LN_RuntimeTree>(program, owners.back().get(), index, 0));
      trees.back()->SetStepSpread(index);
    }
  }
  ~SteppedTrees()
  {
    trees.clear();
  }

  static LN_TickContext Context(const uint64_t step, const double delta)
  {
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.use_fixed_timestep = true;
    context.tick_index = step + 1;
    context.phase_tick_index = step;
    context.steps_done = step;
    context.delta_time = delta;
    context.unscaled_dt = delta;
    return context;
  }

  static std::vector<std::string> Prints(LN_CommandBuffer &command_buffer)
  {
    std::vector<std::string> messages;
    for (const LN_CommandBuffer::Command &command : command_buffer.TakeRecordedCommands()) {
      if (command.type == LN_CommandBuffer::CommandType::Print) {
        messages.push_back(command.property_name);
      }
    }
    return messages;
  }

  /** What each tree printed in step `step` (from 0) of `delta` seconds; `r_ticked`: whether
   * each had work in it (CanTick). */
  std::vector<std::vector<std::string>> Step(const uint64_t step,
                                             const double delta,
                                             const bool skip_idle,
                                             std::vector<bool> *r_ticked = nullptr)
  {
    std::vector<std::vector<std::string>> prints(trees.size());
    if (r_ticked != nullptr) {
      r_ticked->assign(trees.size(), false);
    }
    for (size_t index = 0; index < trees.size(); index++) {
      LN_RuntimeTree &tree = *trees[index];
      tree.PrepareStepGates(step, delta, delta);
      const bool ticks = tree.CanTick(LN_Event::OnFixedUpdate);
      if (r_ticked != nullptr) {
        (*r_ticked)[index] = ticks;
      }
      if (skip_idle && !ticks) {
        continue;
      }
      LN_CommandBuffer command_buffer;
      command_buffer.BeginRecording();
      tree.ExecuteReady(command_buffer, Context(step, delta));
      command_buffer.EndRecording();
      prints[index] = Prints(command_buffer);
    }
    return prints;
  }

  /** prints[step][tree] over `steps` steps. */
  std::vector<std::vector<std::vector<std::string>>> Run(const uint64_t steps,
                                                         const double delta,
                                                         const bool skip_idle)
  {
    std::vector<std::vector<std::vector<std::string>>> prints;
    for (uint64_t step = 0; step < steps; step++) {
      prints.push_back(Step(step, delta, skip_idle));
    }
    return prints;
  }
};

/** A positioned object for batch tests (an SG node at the root). */
struct BatchObject : UIObject<KX_GameObject> {
  explicit BatchObject(const MT_Vector3 &position) : UIObject<KX_GameObject>(nullptr)
  {
    object.NodeSetWorldPosition(position);
    object.NodeUpdateGS(0.0);
  }
};

/** Records Set Game Property writes by property name through the Ref append (plan A6 retired the
 * by-name append): one program holding a property ref for each of `names`, and a tree on a
 * stand-in owner that gives the refs. The target object of a write is any object (even a fake
 * pointer). Lives while the buffers that recorded through it are used. */
class NamedPropertyWriter {
 public:
  explicit NamedPropertyWriter(const std::vector<std::string> &names)
      : m_program(std::make_shared<LN_Program>())
  {
    for (const std::string &name : names) {
      LN_GamePropertyRef property_ref;
      property_ref.name = name;
      property_ref.name_id = m_program->InternString(name);
      m_refs[name] = m_program->AddGamePropertyRef(property_ref);
    }
    m_tree = std::make_unique<LN_RuntimeTree>(m_program, &m_owner.object, 0, 0);
  }
  /** The stand-in tree (benchmarks record through it directly). */
  LN_RuntimeTree *Tree()
  {
    return m_tree.get();
  }
  void Write(LN_CommandBuffer &buffer,
             KX_GameObject *object,
             const std::string &name,
             const LN_Value &value,
             const uint64_t sort_key,
             const uint32_t source_ref_index)
  {
    buffer.AppendSetGamePropertyRef(
        m_tree.get(), object, m_refs.at(name), value, sort_key, source_ref_index);
  }
  void Modify(LN_CommandBuffer &buffer,
              KX_GameObject *object,
              const std::string &name,
              const LN_PropertyModify &modify,
              const uint64_t sort_key,
              const uint32_t source_ref_index)
  {
    buffer.AppendModifyGameProperty(
        m_tree.get(), object, m_refs.at(name), modify, sort_key, source_ref_index);
  }
  void WriteNumber(LN_CommandBuffer &buffer,
                   KX_GameObject *object,
                   const std::string &name,
                   const LN_PropertyNumber &number,
                   const uint64_t sort_key,
                   const uint32_t source_ref_index)
  {
    buffer.AppendSetGamePropertyNumberRef(
        m_tree.get(), object, m_refs.at(name), number, sort_key, source_ref_index);
  }

 private:
  std::shared_ptr<LN_Program> m_program;
  BatchObject m_owner{MT_Vector3(0.0f, 0.0f, 0.0f)};
  std::unique_ptr<LN_RuntimeTree> m_tree;
  std::map<std::string, uint32_t> m_refs;
};

struct BrainOptions {
  /** On Logic + Physics Step instead of On Update. */
  bool step = false;
  /** A move of the object before the first read of the distance. */
  bool move_first = false;
  /** The second branch reads the distance again after the move (the once-a-pass test). */
  bool second_branch = false;
  /** Apply Movement moves another object (a constant object input). */
  blender::Object *moved_object = nullptr;
  float threshold = 6.0f;
};

/** The NPC brain of the 10,000-NPC benchmark (research.py brain_of): the distance from the
 * object to the origin; nearer than the threshold: walk on, else turn and walk; store the
 * distance in "dist". Compiled while its nodes live. */
inline std::shared_ptr<LN_Program> CompileBrain(const BrainOptions &options)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode event(
      tree, options.step ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate", "Event", 1);
  blender::bNodeSocket &tick = event.flow(false, "Out");
  TestNode distance(tree, "LogicNativeGetDistance", "Distance Between", 2);
  distance.node.custom1 = 0; /* End A: an object, the owner when empty. */
  distance.node.custom2 = 1; /* End B: a position. */
  distance.in_object("Object A");
  distance.in_vector("Position A", MT_Vector3(0.0f, 0.0f, 0.0f));
  distance.in_object("Object B");
  distance.in_vector("Position B", MT_Vector3(0.0f, 0.0f, 0.0f));
  blender::bNodeSocket &distance_out = distance.add(
      false, "Distance", blender::SOCK_FLOAT, nullptr);
  distance.add(false, "Squared Distance", blender::SOCK_FLOAT, nullptr);
  distance.add(false, "Valid", blender::SOCK_BOOLEAN, nullptr);
  TestNode near(tree, "LogicNativeCompare", "Compare", 3);
  near.node.custom1 = 3; /* Less Than */
  blender::bNodeSocket &near_a = near.in_float("A", 0.0f);
  near.in_float("B", options.threshold);
  blender::bNodeSocket &near_result = near.add(false, "Result", blender::SOCK_BOOLEAN, nullptr);
  TestNode branch(tree, "LogicNativeBranch", "Branch", 4);
  blender::bNodeSocket &branch_flow = branch.flow(true, "Flow");
  blender::bNodeSocket &branch_condition = branch.in_bool("Condition", false);
  blender::bNodeSocket &branch_true = branch.flow(false, "True");
  blender::bNodeSocket &branch_false = branch.flow(false, "False");
  const auto add_movement = [&](TestNode &node,
                                const MT_Vector3 &vector) -> blender::bNodeSocket & {
    node.flow(true, "Flow");
    blender::bNodeSocket &object = node.in_object("Object");
    if (options.moved_object != nullptr) {
      static_cast<blender::bNodeSocketValueObject *>(object.default_value)->value =
          options.moved_object;
    }
    node.in_vector("Vector", vector);
    node.in_bool("Local", true);
    return node.flow(false, "Done");
  };
  TestNode approach(tree, "LogicNativeApplyMovement", "Apply Movement", 5);
  add_movement(approach, MT_Vector3(0.5f, 0.0f, 0.0f));
  TestNode turn(tree, "LogicNativeApplyRotation", "Apply Rotation", 6);
  blender::bNodeSocket &turn_flow = turn.flow(true, "Flow");
  turn.in_object("Object");
  blender::bNodeSocketValueRotation turn_rotation = {};
  turn_rotation.value_euler[2] = 0.25f;
  turn.add(true, "Rotation", blender::SOCK_ROTATION, &turn_rotation);
  turn.in_bool("Local", true);
  blender::bNodeSocket &turn_done = turn.flow(false, "Done");
  TestNode walk(tree, "LogicNativeApplyMovement", "Apply Movement.001", 7);
  add_movement(walk, MT_Vector3(0.25f, 0.0f, 0.0f));
  TestNode store(tree, "LogicNativeSetGamePropertyFloat", "Set Game Property", 8);
  blender::bNodeSocket &store_flow = store.flow(true, "Flow");
  store.in_object("Object");
  store.in_string("Property", "dist");
  blender::bNodeSocket &store_value = store.in_float("Value", 0.0f);
  store.flow(false, "Done");
  TestNode first_move(tree, "LogicNativeApplyMovement", "Apply Movement.002", 9);
  blender::bNodeSocket &first_move_done = add_movement(first_move, MT_Vector3(0.0f, 1.0f, 0.0f));
  TestNode again(tree, "LogicNativeBranch", "Branch.001", 10);
  blender::bNodeSocket &again_flow = again.flow(true, "Flow");
  blender::bNodeSocket &again_condition = again.in_bool("Condition", false);
  again.flow(false, "True");
  blender::bNodeSocket &again_false = again.flow(false, "False");
  TestNode far(tree, "LogicNativePrint", "Print", 11);
  blender::bNodeSocket &far_flow = far.flow(true, "Flow");
  far.in_string("Value", "far");

  std::deque<blender::bNodeLink> links;
  const auto link = [&](TestNode &from,
                        blender::bNodeSocket &from_socket,
                        TestNode &to,
                        blender::bNodeSocket &to_socket) {
    InitLink(links.emplace_back(), from.node, from_socket, to.node, to_socket);
    Append(tree.links, links.back());
  };
  if (options.move_first) {
    link(event,
         tick,
         first_move,
         *static_cast<blender::bNodeSocket *>(first_move.node.inputs.first));
    link(first_move, first_move_done, branch, branch_flow);
    link(first_move, first_move_done, store, store_flow);
  }
  else {
    link(event, tick, branch, branch_flow);
    link(event, tick, store, store_flow);
  }
  link(distance, distance_out, near, near_a);
  link(near, near_result, branch, branch_condition);
  link(branch,
       branch_true,
       approach,
       *static_cast<blender::bNodeSocket *>(approach.node.inputs.first));
  link(branch, branch_false, turn, turn_flow);
  link(turn, turn_done, walk, *static_cast<blender::bNodeSocket *>(walk.node.inputs.first));
  link(distance, distance_out, store, store_value);
  if (options.second_branch) {
    /* After the near route's move: is the object still near? (Once a pass: yes.) */
    blender::bNodeSocket &approach_done = *static_cast<blender::bNodeSocket *>(
        approach.node.outputs.first);
    link(approach, approach_done, again, again_flow);
    link(near, near_result, again, again_condition);
    link(again, again_false, far, far_flow);
  }
  std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_NE(program, nullptr);
  if (program != nullptr) {
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
  }
  return program;
}

class TestRootParentRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child,
                              const SG_Node * /*parent*/,
                              bool &parentUpdated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    parentUpdated = false;
    return true;
  }

  SG_ParentRelation *NewCopy() override
  {
    return new TestRootParentRelation();
  }
};

class PositionedGameObject {
 public:
  explicit PositionedGameObject(const MT_Vector3 &position = MT_Vector3(0.0f, 0.0f, 0.0f))
      : node(&object, nullptr, callbacks)
  {
    node.SetParentRelation(new TestRootParentRelation());
    object.SetSGNode(&node);
    SetPosition(position);
  }

  ~PositionedGameObject()
  {
    object.SetSGNode(nullptr);
  }

  void SetPosition(const MT_Vector3 &position)
  {
    object.NodeSetWorldPosition(position);
    object.NodeUpdateGS(0.0);
  }

  SG_Callbacks callbacks;
  KX_GameObject object;
  SG_Node node;
};

/** What BrainOf builds (research.py `brain_of`, the brain of the 10,000-NPC benchmark cases). */
struct BrainOfOptions {
  /** Bookkeeping chains: read the float property "t<i>", add 0.01, write it back. */
  int chains = 4;
  /** > 0: On Logic + Physics Step with this Every N Steps instead of On Update. */
  int step_every = 0;
  /** Measure the distance (Get Distance, End B the origin) instead of reading "dist". */
  bool measure_distance = true;
  /** Also store the Player's visibility ("seen"): Get Object Attribute with its Object set. */
  bool look = false;
  /** Walk toward the origin when nearer than this. */
  float threshold = 6.0f;
};

/** research.py's brain_of: [On Update | On Logic + Physics Step] -> Branch (distance nearer than
 * the threshold): True walks on (local, 0.002 y), False turns (local, 0.002 about z) and walks;
 * the distance is stored in "dist" every run, then (look) the visibility in "seen", then the
 * chains. Nodes: 8 + 3 x chains (+ 2 with look), as in research.py. The same wiring and numbers
 * as the game benchmark, so the unit tests, the differential tester and the goldens run the
 * program the benchmarks run. Compiled while its nodes live; null when it does not compile. */
inline std::shared_ptr<LN_Program> BrainOf(const BrainOfOptions &options)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  /* Everything the tree points at lives in these until it is compiled. */
  std::deque<TestNode> nodes_storage;
  int32_t next_id = 1;
  const auto add_node = [&](const char *idname, const char *name) -> TestNode & {
    nodes_storage.emplace_back(tree, idname, name, next_id++);
    return nodes_storage.back();
  };
  std::deque<blender::bNodeLink> links;
  const auto link = [&](TestNode &from,
                        blender::bNodeSocket &from_socket,
                        TestNode &to,
                        blender::bNodeSocket &to_socket) {
    InitLink(links.emplace_back(), from.node, from_socket, to.node, to_socket);
    Append(tree.links, links.back());
  };
  const auto first_input = [](TestNode &node) -> blender::bNodeSocket & {
    return *static_cast<blender::bNodeSocket *>(node.node.inputs.first);
  };

  TestNode &event = add_node(
      options.step_every > 0 ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate", "Event");
  if (options.step_every > 0) {
    event.node.custom1 = int16_t(options.step_every); /* Every N Steps */
  }
  blender::bNodeSocket &update = event.flow(false, "Out");

  TestNode *distance = nullptr;
  blender::bNodeSocket *distance_out = nullptr;
  if (options.measure_distance) {
    distance = &add_node("LogicNativeGetDistance", "Distance Between");
    distance->node.custom1 = 0; /* End A: an object, the owner when empty. */
    distance->node.custom2 = 1; /* End B: a position, the origin. */
    distance->in_object("Object A");
    distance->in_vector("Position A", MT_Vector3(0.0f, 0.0f, 0.0f));
    distance->in_object("Object B");
    distance->in_vector("Position B", MT_Vector3(0.0f, 0.0f, 0.0f));
    distance_out = &distance->add(false, "Distance", blender::SOCK_FLOAT, nullptr);
    distance->add(false, "Squared Distance", blender::SOCK_FLOAT, nullptr);
    distance->add(false, "Valid", blender::SOCK_BOOLEAN, nullptr);
  }
  else {
    /* No position read: the stored distance itself (the same node count). */
    distance = &add_node("LogicNativeGetGamePropertyFloat", "Get Property");
    distance->in_object("Object");
    distance->in_string("Property", "dist");
    distance_out = &distance->add(false, "Value", blender::SOCK_FLOAT, nullptr);
  }
  TestNode &near = add_node("LogicNativeCompare", "Compare");
  near.node.custom1 = 3; /* Less Than */
  blender::bNodeSocket &near_a = near.in_float("A", 0.0f);
  near.in_float("B", options.threshold);
  blender::bNodeSocket &near_result = near.add(false, "Result", blender::SOCK_BOOLEAN, nullptr);
  link(*distance, *distance_out, near, near_a);

  TestNode &branch = add_node("LogicNativeBranch", "Branch");
  blender::bNodeSocket &branch_flow = branch.flow(true, "Flow");
  blender::bNodeSocket &branch_condition = branch.in_bool("Condition", false);
  blender::bNodeSocket &branch_true = branch.flow(false, "True");
  blender::bNodeSocket &branch_false = branch.flow(false, "False");
  link(event, update, branch, branch_flow);
  link(near, near_result, branch, branch_condition);

  const auto add_movement = [&](const char *name, const MT_Vector3 &vector) -> TestNode & {
    TestNode &node = add_node("LogicNativeApplyMovement", name);
    node.flow(true, "Flow");
    node.in_object("Object");
    node.in_vector("Vector", vector);
    node.in_bool("Local", true);
    node.flow(false, "Done");
    return node;
  };
  TestNode &approach = add_movement("Apply Movement", MT_Vector3(0.0f, 0.002f, 0.0f));
  link(branch, branch_true, approach, first_input(approach));
  TestNode &turn = add_node("LogicNativeApplyRotation", "Apply Rotation");
  turn.flow(true, "Flow");
  turn.in_object("Object");
  blender::bNodeSocketValueRotation turn_rotation = {};
  turn_rotation.value_euler[2] = 0.002f;
  turn.add(true, "Rotation", blender::SOCK_ROTATION, &turn_rotation);
  turn.in_bool("Local", true);
  blender::bNodeSocket &turn_done = turn.flow(false, "Done");
  link(branch, branch_false, turn, first_input(turn));
  TestNode &walk = add_movement("Apply Movement.001", MT_Vector3(0.0f, 0.001f, 0.0f));
  link(turn, turn_done, walk, first_input(walk));

  const auto add_property_write = [&](const char *idname,
                                      const char *name,
                                      const char *property,
                                      TestNode *flow_from,
                                      blender::bNodeSocket *flow_socket) -> TestNode & {
    TestNode &node = add_node(idname, name);
    blender::bNodeSocket &flow = node.flow(true, "Flow");
    node.in_object("Object");
    node.in_string("Property", property);
    if (std::strcmp(idname, "LogicNativeSetGamePropertyBool") == 0) {
      node.in_bool("Value", false);
    }
    else {
      node.in_float("Value", 0.0f);
    }
    node.flow(false, "Done");
    link(*flow_from, *flow_socket, node, flow);
    return node;
  };
  const auto done_of = [](TestNode &node) -> blender::bNodeSocket & {
    return *static_cast<blender::bNodeSocket *>(node.node.outputs.first);
  };
  const auto value_of = [](TestNode &node) -> blender::bNodeSocket & {
    /* The Value input is the last one of a property write. */
    return *static_cast<blender::bNodeSocket *>(node.node.inputs.last);
  };
  TestNode &store = add_property_write(
      "LogicNativeSetGamePropertyFloat", "Set Game Property", "dist", &event, &update);
  link(*distance, *distance_out, store, value_of(store));
  TestNode *tail = &store;
  if (options.look) {
    TestNode &look = add_node("LogicNativeGetObjectAttribute", "Get Object Attribute");
    look.node.custom1 = 2; /* Visible */
    look.in_object("Object");
    look.add(false, "Name", blender::SOCK_STRING, nullptr);
    look.add(false, "Vector", blender::SOCK_VECTOR, nullptr);
    blender::bNodeSocket &visible = look.add(false, "Visible", blender::SOCK_BOOLEAN, nullptr);
    look.add(false, "Position", blender::SOCK_VECTOR, nullptr);
    look.add(false, "Orientation", blender::SOCK_ROTATION, nullptr);
    look.add(false, "Scale", blender::SOCK_VECTOR, nullptr);
    look.add(false, "Color", blender::SOCK_RGBA, nullptr);
    TestNode &seen = add_property_write(
        "LogicNativeSetGamePropertyBool", "Set Game Property Bool", "seen", tail, &done_of(*tail));
    link(look, visible, seen, value_of(seen));
    tail = &seen;
  }
  for (int index = 0; index < options.chains; index++) {
    const std::string property = "t" + std::to_string(index);
    TestNode &read = add_node("LogicNativeGetGamePropertyFloat", "Get Property");
    read.in_object("Object");
    read.in_string("Property", property.c_str());
    blender::bNodeSocket &read_value = read.add(false, "Value", blender::SOCK_FLOAT, nullptr);
    TestNode &add = add_node("LogicNativeMath", "Math");
    add.node.custom1 = blender::NODE_MATH_ADD;
    blender::bNodeSocket &add_a = add.in_float("A", 0.0f);
    add.in_float("B", 0.01f);
    blender::bNodeSocket &add_result = add.add(false, "Result", blender::SOCK_FLOAT, nullptr);
    link(read, read_value, add, add_a);
    TestNode &write = add_property_write("LogicNativeSetGamePropertyFloat",
                                         "Set Game Property",
                                         property.c_str(),
                                         tail,
                                         &done_of(*tail));
    link(add, add_result, write, value_of(write));
    tail = &write;
  }

  std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_NE(program, nullptr);
  if (program != nullptr) {
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
  }
  return program;
}


/** What a flush did to the objects (lean runtime plan A2): per object its local and world pose
 * (bit patterns), the world write stamp and every game property (type, text, number bits).
 * Objects by position in the list, not by pointer. */
inline std::string DumpWorld(const std::vector<KX_GameObject *> &objects)
{
  std::string out;
  size_t index = 0;
  for (KX_GameObject *object : objects) {
    out += "object[" + std::to_string(index++) + "] " + LN_DumpQuoted(object->GetName());
    out += " local_position=" + LN_DumpVector3(object->NodeGetLocalPosition());
    out += " local_orientation=" + LN_DumpMatrix3(object->NodeGetLocalOrientation());
    out += " local_scale=" + LN_DumpVector3(object->NodeGetLocalScaling());
    out += " world_position=" + LN_DumpVector3(object->NodeGetWorldPosition());
    out += " world_orientation=" + LN_DumpMatrix3(object->NodeGetWorldOrientation());
    out += " world_scale=" + LN_DumpVector3(object->NodeGetWorldScaling());
    out += " world_stamp=" +
           (object->GetSGNode() != nullptr ? std::to_string(object->GetSGNode()->GetWorldStamp()) :
                                             std::string("-"));
    std::vector<std::string> names = object->GetPropertyNames();
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
      EXP_Value *property = object->GetProperty(name);
      out += " property[" + name + "]=";
      if (property == nullptr) {
        out += "-";
        continue;
      }
      /* A NaN's text as one ("-nan" and "nan": its sign is no logic, LN_DumpFloatBits). */
      std::string text = property->GetText();
      for (size_t at = text.find("-nan"); at != std::string::npos && !LN_DumpNaNAsIs();
           at = text.find("-nan", at))
      {
        text.erase(at, 1);
      }
      out += std::to_string(property->GetValueType()) + "/" + LN_DumpQuoted(text) + "/" +
             LN_DumpDoubleBits(property->GetNumber());
    }
    out += "\n";
  }
  return out;
}

/** Flushes `buffer` and returns what it printed (the Print node's lines), the number of warning
 * lines, and the world afterwards. */
inline std::string FlushAndDumpWorld(LN_CommandBuffer &buffer,
                                     const std::vector<KX_GameObject *> &objects)
{
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  buffer.Flush();
  const std::string printed = testing::internal::GetCapturedStdout();
  const std::string errors = testing::internal::GetCapturedStderr();
  size_t warnings = 0;
  for (size_t at = errors.find("Warning"); at != std::string::npos;
       at = errors.find("Warning", at + 1))
  {
    warnings++;
  }
  for (size_t at = printed.find("Warning"); at != std::string::npos;
       at = printed.find("Warning", at + 1))
  {
    warnings++;
  }
  return "printed:\n" + printed + "warnings: " + std::to_string(warnings) + "\n" +
         DumpWorld(objects);
}


/* ---- Golden files (lean runtime plan A2) ----
 * A golden is a text kept in tests/fixtures/lean_golden/NAME.txt: the program dump of a tree, or a
 * dump of what trees recorded. It starts with a header of `#` lines: the generator version (the
 * format of what is written) and the toolchain fingerprint (compiler, C library, instruction set:
 * folded constants and library math can differ between them). The test compares the text below
 * the header exactly. A different fingerprint is reported as "re-record needed", not as a failed
 * comparison. LN_RECORD_GOLDEN=1 writes the file instead of comparing (record, read the diff in
 * git, commit it). At most 2 MB of goldens (plan rule 19). */

constexpr int kGoldenGeneratorVersion = 1;

inline std::string GoldenFingerprint()
{
  std::string text = std::string("compiler=") + __VERSION__;
#if defined(__GLIBC__)
  text += " glibc=" + std::to_string(__GLIBC__) + "." + std::to_string(__GLIBC_MINOR__);
#endif
#if defined(__AVX2__)
  text += " avx2";
#endif
#if defined(__FMA__)
  text += " fma";
#endif
#if defined(__SSE4_2__)
  text += " sse4.2";
#endif
  text += " pointer=" + std::to_string(sizeof(void *) * 8);
  return text;
}

inline std::string GoldenPath(const std::string &name)
{
  return std::string(LOGIC_NODES_TEST_SOURCE_DIR) +
         "/source/gameengine/LogicNodes/tests/fixtures/lean_golden/" + name + ".txt";
}

/** FNV-1a 64 of a text, as 16 hex digits (for goldens that keep a hash of a long dump). */
inline std::string GoldenHash(const std::string &text)
{
  uint64_t hash = 1469598103934665603ull;
  for (const char c : text) {
    hash = (hash ^ uint64_t(static_cast<unsigned char>(c))) * 1099511628211ull;
  }
  char out[20];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
  return out;
}

/** Compares `actual` with the golden `name`. Returns "" when it matches (or was recorded), else
 * the reason: missing, a different toolchain fingerprint or generator version (re-record needed),
 * or the first differing line. */
inline std::string CheckGolden(const std::string &name, const std::string &actual)
{
  const std::string header = "# generator-version: " + std::to_string(kGoldenGeneratorVersion) +
                             "\n# fingerprint: " + GoldenFingerprint() + "\n";
  const std::string path = GoldenPath(name);
  if (std::getenv("LN_RECORD_GOLDEN") != nullptr) {
    std::ofstream file(path, std::ios::binary);
    file << header << actual;
    return file ? std::string() : "could not write " + path;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "golden " + name + " is missing (run with LN_RECORD_GOLDEN=1 to record it)";
  }
  std::stringstream stored;
  stored << file.rdbuf();
  const std::string text = stored.str();
  if (text.compare(0, header.size(), header) != 0) {
    std::string have = text.substr(0, text.find("\n", text.find("\n") + 1));
    return "golden " + name + ": re-record needed, the generator version or toolchain differs "
           "(file: " + have + ")";
  }
  const std::string body = text.substr(header.size());
  if (body == actual) {
    return std::string();
  }
  size_t line = 1, start = 0;
  while (start < body.size() && start < actual.size()) {
    const size_t end_a = body.find('\n', start);
    const size_t end_b = actual.find('\n', start);
    if (end_a != end_b || body.compare(start, end_a - start, actual, start, end_b - start) != 0) {
      break;
    }
    start = end_a + 1;
    line++;
  }
  const auto line_at = [&](const std::string &t) {
    const size_t at = t.find('\n', start);
    return t.substr(start < t.size() ? start : t.size(),
                    (at == std::string::npos ? t.size() : at) - std::min(start, t.size()));
  };
  return "golden " + name + " differs at line " + std::to_string(line) + ":\n  golden: " +
         line_at(body).substr(0, 300) + "\n  actual: " + line_at(actual).substr(0, 300);
}

}  // namespace ln_test
