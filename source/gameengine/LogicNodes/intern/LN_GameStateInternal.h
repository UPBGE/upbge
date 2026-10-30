/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateInternal.h
 *  \ingroup logicnodes
 *
 * Private checkpoint model and codec/storage boundary. This is intentionally not part of the
 * public Logic Nodes API.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "BLI_uuid.h"
#include "DNA_modifier_types.h"
#include "LN_GameState.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

namespace upbge::logic_nodes::game_state {

inline constexpr char kFormatName[] = "UPBGE_CXX_LOGIC_NODES_CHECKPOINT";
inline constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
inline constexpr size_t kMaxDecodedMemoryBytes = 128u * 1024u * 1024u;
inline constexpr size_t kMaxObjects = 100000u;
inline constexpr size_t kMaxPropertiesPerObject = 4096u;
inline constexpr size_t kMaxMaterialSlotsPerObject = 32767u;
inline constexpr size_t kMaxMaterialParametersPerObject = 4096u;
inline constexpr size_t kMaxSharedMaterials = 32768u;
inline constexpr size_t kMaxMaterialNodesPerAsset = 65536u;
inline constexpr size_t kMaxMaterialInputsPerNode = 4096u;
inline constexpr size_t kMaxLogicTreesPerObject = 4096u;
inline constexpr size_t kMaxCollectionMembershipsPerObject = 4096u;
inline constexpr size_t kMaxModifiersPerObject = 4096u;
inline constexpr size_t kMaxGeometryNodesInputsPerModifier = 4096u;
inline constexpr size_t kMaxGeometryNodesOutputsPerModifier = 4096u;
inline constexpr size_t kMaxRuntimeNodeTrees = 65536u;
inline constexpr size_t kMaxRuntimeNodesPerTree = 65536u;
inline constexpr size_t kMaxRuntimeNodeInputsPerNode = 4096u;
inline constexpr size_t kMaxNameBytes = 4096u;
inline constexpr size_t kMaxIdentityBytes = 16384u;
inline constexpr size_t kMaxStringPropertyBytes = 1024u * 1024u;
inline constexpr size_t kMaxMaterialStringBytes = 1023u;
inline constexpr size_t kMaxPendingRequests = 32u;
inline constexpr size_t kMaxJsonNestingDepth = 32u;
inline constexpr float kOrientationTolerance = 1.0e-3f;
inline constexpr float kScaleTolerance = 1.0e-5f;
/** Durable creator-facing flags. Evaluated-only Virtual/DisableTemporary flags are not state. */
inline constexpr uint32_t kPersistentModifierModeMask =
    uint32_t(blender::eModifierMode_Realtime) | uint32_t(blender::eModifierMode_Render) |
    uint32_t(blender::eModifierMode_Editmode) | uint32_t(blender::eModifierMode_OnCage) |
    uint32_t(blender::eModifierMode_ApplyOnSpline);

enum class PropertyType : uint8_t {
  Bool = 0,
  Int,
  Float,
  String,
  Vector3,
  Vector4,
  Object,
  List,
  Dict,
};

/** A saved Vector, Color, Object, List or Dictionary property value, or an item of one. */
struct PropertyData {
  enum class Kind : uint8_t { Bool = 0, Int, Float, String, Vector3, Vector4, Object, List, Dict };
  Kind kind = Kind::Bool;
  bool bool_value = false;
  int64_t int_value = 0;
  double float_value = 0.0;
  /** String text; for an Object its Save Game identity, its name or empty for none. */
  std::string text;
  /** Object: \a text is the name of an object that has no Save Game identity. */
  bool object_by_name = false;
  float vector[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<PropertyData> items;
  std::vector<std::pair<std::string, PropertyData>> entries;
};

/** Saved values nested deeper or holding more values are refused. The depth counts JSON levels
 * below the property (a list adds one, a dictionary two), so a save always fits the file's
 * #kMaxJsonNestingDepth. */
inline constexpr int kMaxPropertyDataDepth = 24;
inline constexpr size_t kMaxPropertyDataItems = 65536u;

/** Memory a saved value takes, for the Save Game budget. */
inline size_t property_data_bytes(const PropertyData &data)
{
  size_t bytes = sizeof(PropertyData) + data.text.size();
  for (const PropertyData &item : data.items) {
    bytes += property_data_bytes(item);
  }
  for (const auto &entry : data.entries) {
    bytes += entry.first.size() + property_data_bytes(entry.second);
  }
  return bytes;
}

struct PropertyState {
  std::string name;
  PropertyType type = PropertyType::String;
  bool bool_value = false;
  int64_t int_value = 0;
  float float_value = 0.0f;
  std::string string_value;
  /** Vector3, Vector4, Object, List and Dict properties. */
  PropertyData data;
  bool timer = false;
  bool exists = true;
};

struct MaterialSlotState {
  bool has_material = false;
  std::string material_name;
  std::string library_path;
};

struct MaterialParameterState {
  std::string attribute_name;
  std::array<float, 4> values = {0.0f, 0.0f, 0.0f, 1.0f};
};

enum class MaterialSocketValueType : uint8_t {
  Bool = 0,
  Int,
  Menu,
  IntVector,
  Float,
  Vector,
  Rotation,
  Color,
  String,
  Object,
  Image,
  Collection,
  Texture,
  Material,
  Font,
  Scene,
  Text,
  Mask,
  Sound,
};

struct DatablockReferenceState {
  bool has_value = false;
  std::string name;
  std::string library_path;
};

struct MaterialSocketValueState {
  std::string socket_identifier;
  MaterialSocketValueType type = MaterialSocketValueType::Float;
  bool bool_value = false;
  int32_t int_value = 0;
  uint8_t dimensions = 0;
  std::array<int32_t, 3> int_values = {0, 0, 0};
  std::array<float, 4> float_values = {0.0f, 0.0f, 0.0f, 0.0f};
  std::string string_value;
  DatablockReferenceState datablock_value;
};

struct SharedMaterialNodeState {
  int32_t node_identifier = 0;
  std::string node_type;
  std::vector<MaterialSocketValueState> inputs;
};

struct SharedMaterialState {
  MaterialSlotState material;
  std::vector<SharedMaterialNodeState> nodes;
};

enum class RuntimeNodeTreeType : uint8_t {
  Geometry = 0,
  Compositor,
};

struct RuntimeNodeState {
  int32_t node_identifier = 0;
  std::string node_type;
  bool muted = false;
  std::vector<MaterialSocketValueState> inputs;
};

struct RuntimeNodeTreeState {
  RuntimeNodeTreeType type = RuntimeNodeTreeType::Geometry;
  std::string name;
  std::string library_path;
  std::string structural_fingerprint;
  std::vector<RuntimeNodeState> nodes;
};

struct CompositorState {
  bool has_node_tree = false;
  std::string node_tree_name;
  std::string node_tree_library_path;
  std::string node_tree_structural_fingerprint;
};

struct LogicTreeAttachmentState {
  std::string tree_name;
  std::string library_path;
  uint32_t applied_tree_index = 0;
  bool enabled = true;
};

enum class GeometryNodesInputMode : uint8_t {
  Fallback = 0,
  Value,
  Attribute,
  Layer,
};

struct GeometryNodesInputState {
  std::string socket_identifier;
  GeometryNodesInputMode mode = GeometryNodesInputMode::Value;
  MaterialSocketValueState value;
  std::string attribute_name;
  std::string layer_name;
};

struct GeometryNodesOutputState {
  std::string socket_identifier;
  std::string attribute_name;
};

struct NonGeometryModifierState {
  std::string name;
  int32_t type = 0;
  int32_t persistent_uid = 0;
  uint32_t mode = 0;
};

struct GeometryNodesModifierState {
  std::string name;
  int32_t persistent_uid = 0;
  uint32_t stack_index = 0;
  uint32_t mode = 0;
  std::string node_group_name;
  std::string node_group_library_path;
  std::string node_group_fingerprint;
  std::vector<GeometryNodesInputState> inputs;
  std::vector<GeometryNodesOutputState> outputs;
};

enum class HierarchyRelation : uint8_t {
  Root = 0,
  Normal,
};

struct HierarchyState {
  HierarchyRelation relation = HierarchyRelation::Root;
  std::string parent_id;
  bool compound = false;
  bool ghost = false;
};

enum class RigidBodyMode : uint8_t {
  Dynamic = 0,
  StaticCollider,
  Ghost,
  NoCollision,
};

/** Base motion ownership, independent of the temporary collision-state mode. */
enum class RigidBodyRuntimeMotionType : uint8_t {
  Authored = 0,
  Dynamic,
  Kinematic,
};

struct RigidBodyState {
  bool dynamic_body = false;
  RigidBodyMode mode = RigidBodyMode::Dynamic;
  RigidBodyRuntimeMotionType runtime_motion_type = RigidBodyRuntimeMotionType::Authored;
  bool ghost = false;
  bool physics_suspended = false;
  bool awake = false;
  MT_Vector3 linear_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 angular_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 pending_force = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 pending_torque = MT_Vector3(0.0f, 0.0f, 0.0f);
  float mass = 0.0f;
  float friction = 0.5f;
  float restitution = 0.0f;
  float linear_damping = 0.0f;
  float angular_damping = 0.0f;
  float linear_velocity_min = -1.0f;
  float linear_velocity_max = -1.0f;
  float angular_velocity_min = 0.0f;
  float angular_velocity_max = 0.0f;
  float gravity_factor = 1.0f;
  bool allow_sleeping = true;
  bool rotation_enabled = false;
  bool ccd_enabled = false;
  bool dynamic_ccd_enabled = false;
  std::array<bool, 6> axis_locks = {false, false, false, false, false, false};
  uint16_t collision_group = 0;
  uint16_t collision_mask = 0;
};

/** One Set Collection Member change of a Save Object State object: the collection by name (and
 * library file for a linked one), and whether the object was put in or taken out. */
struct CollectionMembershipState {
  std::string collection_name;
  std::string library_path;
  bool member = true;
};

struct ObjectState {
  std::string persistence_id;
  std::string source_id;
  std::string name;
  bool spawned = false;
  bool has_existence = false;
  bool exists = true;
  MT_Vector3 creation_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 creation_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 creation_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  bool has_transform = false;
  MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 world_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  std::vector<PropertyState> properties;
  bool has_material_state = false;
  std::vector<MaterialSlotState> material_slots;
  std::vector<MaterialParameterState> material_parameters;
  bool has_runtime_structure_state = false;
  std::vector<LogicTreeAttachmentState> logic_trees;
  std::vector<NonGeometryModifierState> non_geometry_modifiers;
  std::vector<GeometryNodesModifierState> geometry_nodes_modifiers;
  bool has_hierarchy_state = false;
  HierarchyState hierarchy;
  bool has_rigid_body_state = false;
  RigidBodyState rigid_body;
  /** Set Collection Member's changes, oldest first (live Save Object State records only; none
   * written when empty, and a record without them restores none). */
  std::vector<CollectionMembershipState> collection_memberships;
};

struct Checkpoint {
  std::string scene_uuid;
  std::string scene_name;
  std::vector<ObjectState> authored_objects;
  std::vector<ObjectState> spawned_instances;
  std::vector<SharedMaterialState> shared_materials;
  std::vector<RuntimeNodeTreeState> runtime_node_trees;
  CompositorState compositor;
};

using CommitTemporaryFn =
    std::function<LN_GameStateResult(const std::string &temporary_path,
                                     const std::string &destination_path,
                                     std::string &r_error)>;

bool contains_embedded_null(const std::string &value);
bool is_valid_checkpoint_text(const std::string &value,
                              size_t maximum,
                              bool allow_empty = true);
bool consume_budget(size_t &remaining, size_t bytes);
bool is_valid_orientation(const MT_Matrix3x3 &value);
bool is_nonsingular_scale(const MT_Vector3 &value);
std::string uuid_string(const blender::bUUID &uuid);
bool is_canonical_uuid(const std::string &text);
bool parse_canonical_uuid(const std::string &text, blender::UUID &r_uuid);

/** Serialize the current checkpoint document, including its checksum. */
LN_GameStateResult encode_checkpoint(const Checkpoint &checkpoint,
                                     std::string &r_contents,
                                     std::string &r_error);

/** Strictly validate and decode one complete checkpoint document. */
LN_GameStateResult decode_checkpoint(std::string contents,
                                     Checkpoint &r_checkpoint,
                                     std::string &r_error);

uint64_t next_request_id();
bool reserve_process_path(const std::string &filepath,
                          const void *owner,
                          std::string &r_error);
void release_process_path(const std::string &filepath, const void *owner, size_t count = 1);

LN_GameStateResult commit_checkpoint_temporary(const std::string &temporary_path,
                                               const std::string &destination_path,
                                               std::string &r_error);
LN_GameStateResult write_checkpoint_atomic(const std::string &filepath,
                                           const Checkpoint &checkpoint,
                                           uint64_t request_id,
                                           const CommitTemporaryFn &commit_temporary,
                                           std::string &r_error);
LN_GameStateResult read_checkpoint(const std::string &filepath,
                                   Checkpoint &r_checkpoint,
                                   std::string &r_error);

}  // namespace upbge::logic_nodes::game_state
