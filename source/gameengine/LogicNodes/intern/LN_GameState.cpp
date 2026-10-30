/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameState.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameState.h"
#include "LN_GameStateGeometryNodes.h"
#include "LN_GameStateInternal.h"
#include "LN_GameStateMaterial.h"
#include "LN_GameStateNodeTrees.h"
#include "LN_Manager.h"
#include "LN_PersistenceCapability.h"
#include "LN_Program.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_system.h"
#include "BLI_uuid.h"
#include "BKE_appdir.hh"
#include "BKE_context.hh"
#include "BKE_game_persistence.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"
#include "BL_GameStateReconstruction.h"
#include "CM_Message.h"
#include "DNA_collection_types.h"
#include "DNA_object_force_types.h"
#include "DEG_depsgraph_query.hh"
#include "EXP_ArrayValue.h"
#include "EXP_BoolValue.h"
#include "EXP_DictValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_ListValue.h"
#include "EXP_StringValue.h"
#include "EXP_Value.h"
#include "EXP_Vector3Value.h"
#include "EXP_Vector4Value.h"
#include "KX_GameObject.h"
#include "KX_EmptyObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_NodeRelationships.h"
#include "KX_ObjectReferenceValue.h"
#include "KX_Scene.h"
#include "PHY_IPhysicsController.h"
#include "SCA_TimeEventManager.h"
#include "SG_Node.h"

namespace {

using namespace upbge::logic_nodes::game_state;
using blender::G;

blender::Main *current_main()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  return bmain ? bmain : G_MAIN;
}

struct CaptureParticipant {
  KX_GameObject *object = nullptr;
  blender::bUUID uuid;
  blender::bUUID source_uuid;
  std::string name;
  bool spawned = false;
  bool save_object_state = false;
  bool exists = true;
  MT_Vector3 creation_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 creation_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 creation_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 supported_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  size_t material_slot_count = 0;
  std::vector<LogicTreeAttachmentState> logic_trees;
  const ModifierBaseline *modifier_baseline = nullptr;
};

struct TargetKey {
  uint32_t tree_index = LN_INVALID_INDEX;
  uint32_t tree_generation = 0;
  uint32_t expression_index = LN_INVALID_INDEX;

  bool operator==(const TargetKey &other) const
  {
    return tree_index == other.tree_index && tree_generation == other.tree_generation &&
           expression_index == other.expression_index;
  }
};

struct TargetKeyHash {
  size_t operator()(const TargetKey &key) const
  {
    size_t value = std::hash<uint32_t>{}(key.tree_index);
    value ^= std::hash<uint32_t>{}(key.tree_generation) + 0x9e3779b9u + (value << 6u) +
             (value >> 2u);
    value ^= std::hash<uint32_t>{}(key.expression_index) + 0x9e3779b9u + (value << 6u) +
             (value >> 2u);
    return value;
  }
};


TargetKey target_key(const LN_GameStateCompletionTarget &target)
{
  return {
      target.runtime_tree_id.index, target.runtime_tree_id.generation, target.expression_index};
}

bool is_finite(const MT_Vector3 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

bool is_finite(const MT_Vector4 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()) &&
         std::isfinite(value.w());
}

bool is_finite(const MT_Matrix3x3 &value)
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (!std::isfinite(value[row][column])) {
        return false;
      }
    }
  }
  return true;
}

template<typename Destination, typename Source>
void copy_rigid_body_state_fields(Destination &destination, const Source &source)
{
  destination.dynamic_body = source.dynamic_body;
  destination.ghost = source.ghost;
  destination.physics_suspended = source.physics_suspended;
  destination.awake = source.awake;
  destination.linear_velocity = source.linear_velocity;
  destination.angular_velocity = source.angular_velocity;
  destination.pending_force = source.pending_force;
  destination.pending_torque = source.pending_torque;
  destination.mass = source.mass;
  destination.friction = source.friction;
  destination.restitution = source.restitution;
  destination.linear_damping = source.linear_damping;
  destination.angular_damping = source.angular_damping;
  destination.linear_velocity_min = source.linear_velocity_min;
  destination.linear_velocity_max = source.linear_velocity_max;
  destination.angular_velocity_min = source.angular_velocity_min;
  destination.angular_velocity_max = source.angular_velocity_max;
  destination.gravity_factor = source.gravity_factor;
  destination.allow_sleeping = source.allow_sleeping;
  destination.rotation_enabled = source.rotation_enabled;
  destination.ccd_enabled = source.ccd_enabled;
  destination.dynamic_ccd_enabled = source.dynamic_ccd_enabled;
  destination.axis_locks = source.axis_locks;
  destination.collision_group = source.collision_group;
  destination.collision_mask = source.collision_mask;
}

bool checkpoint_rigid_body_state(const PHY_GameStateRigidBodyState &physics,
                                 RigidBodyState &r_state)
{
  copy_rigid_body_state_fields(r_state, physics);
  switch (physics.runtime_motion_type) {
    case PHY_RuntimeMotionType::Authored:
      r_state.runtime_motion_type = RigidBodyRuntimeMotionType::Authored;
      break;
    case PHY_RuntimeMotionType::Dynamic:
      r_state.runtime_motion_type = RigidBodyRuntimeMotionType::Dynamic;
      break;
    case PHY_RuntimeMotionType::Kinematic:
      r_state.runtime_motion_type = RigidBodyRuntimeMotionType::Kinematic;
      break;
    default:
      return false;
  }
  switch (physics.mode) {
    case PHY_DynamicsMode::Dynamic:
      r_state.mode = RigidBodyMode::Dynamic;
      return true;
    case PHY_DynamicsMode::StaticCollider:
      r_state.mode = RigidBodyMode::StaticCollider;
      return true;
    case PHY_DynamicsMode::Ghost:
      r_state.mode = RigidBodyMode::Ghost;
      return true;
    case PHY_DynamicsMode::NoCollision:
      r_state.mode = RigidBodyMode::NoCollision;
      return true;
  }
  return false;
}

bool physics_rigid_body_state(const RigidBodyState &saved,
                              PHY_GameStateRigidBodyState &r_state)
{
  copy_rigid_body_state_fields(r_state, saved);
  switch (saved.runtime_motion_type) {
    case RigidBodyRuntimeMotionType::Authored:
      r_state.runtime_motion_type = PHY_RuntimeMotionType::Authored;
      break;
    case RigidBodyRuntimeMotionType::Dynamic:
      r_state.runtime_motion_type = PHY_RuntimeMotionType::Dynamic;
      break;
    case RigidBodyRuntimeMotionType::Kinematic:
      r_state.runtime_motion_type = PHY_RuntimeMotionType::Kinematic;
      break;
    default:
      return false;
  }
  switch (saved.mode) {
    case RigidBodyMode::Dynamic:
      r_state.mode = PHY_DynamicsMode::Dynamic;
      return true;
    case RigidBodyMode::StaticCollider:
      r_state.mode = PHY_DynamicsMode::StaticCollider;
      return true;
    case RigidBodyMode::Ghost:
      r_state.mode = PHY_DynamicsMode::Ghost;
      return true;
    case RigidBodyMode::NoCollision:
      r_state.mode = PHY_DynamicsMode::NoCollision;
      return true;
  }
  return false;
}


bool scales_match(const MT_Vector3 &a, const MT_Vector3 &b)
{
  for (int axis = 0; axis < 3; axis++) {
    const float magnitude = std::max({1.0f, std::abs(a[axis]), std::abs(b[axis])});
    if (std::abs(a[axis] - b[axis]) > kScaleTolerance * magnitude) {
      return false;
    }
  }
  return true;
}

bool positions_match(const MT_Vector3 &a, const MT_Vector3 &b)
{
  constexpr float tolerance = 1.0e-6f;
  for (int axis = 0; axis < 3; axis++) {
    const float magnitude = std::max({1.0f, std::abs(a[axis]), std::abs(b[axis])});
    if (std::abs(a[axis] - b[axis]) > tolerance * magnitude) {
      return false;
    }
  }
  return true;
}

bool orientations_match(const MT_Matrix3x3 &a, const MT_Matrix3x3 &b)
{
  constexpr float tolerance = 1.0e-6f;
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (std::abs(a[row][column] - b[row][column]) > tolerance) {
        return false;
      }
    }
  }
  return true;
}

enum class PersistenceParentRelation : uint8_t {
  Root = 0,
  Normal,
  Vertex,
  Unsupported,
};

PersistenceParentRelation persistence_parent_relation(SG_Node *node)
{
  if (node == nullptr) {
    return PersistenceParentRelation::Unsupported;
  }
  if (node->GetSGParent() == nullptr) {
    return PersistenceParentRelation::Root;
  }
  SG_ParentRelation *relation = node->GetParentRelation();
  if (relation == nullptr) {
    return PersistenceParentRelation::Unsupported;
  }
  if (relation->IsVertexRelation()) {
    return PersistenceParentRelation::Vertex;
  }
  if (dynamic_cast<KX_NormalParentRelation *>(relation) != nullptr) {
    return PersistenceParentRelation::Normal;
  }
  return PersistenceParentRelation::Unsupported;
}

bool resolve_parent_object(KX_GameObject &object,
                           KX_GameObject *&r_parent,
                           bool &r_normal_relation,
                           std::string &r_error)
{
  r_parent = nullptr;
  r_normal_relation = true;
  SG_Node *node = object.GetSGNode();
  if (node == nullptr) {
    r_error = "object has no scene-graph node";
    return false;
  }
  size_t traversed = 0;
  while (node->GetSGParent() != nullptr) {
    if (persistence_parent_relation(node) != PersistenceParentRelation::Normal) {
      r_normal_relation = false;
    }
    node = node->GetSGParent();
    if (++traversed > kMaxObjects) {
      r_error = "runtime scene graph contains a parent cycle";
      return false;
    }
    if (node->GetSGClientObject() != nullptr) {
      r_parent = static_cast<KX_GameObject *>(node->GetSGClientObject());
      if (r_parent == &object) {
        r_error = "runtime scene graph contains a self-parent";
        return false;
      }
      return true;
    }
  }
  return true;
}

bool resolve_normal_parent(KX_GameObject &object,
                           KX_GameObject *&r_parent,
                           std::string &r_error)
{
  bool normal_relation = true;
  if (!resolve_parent_object(object, r_parent, normal_relation, r_error)) {
    return false;
  }
  if (!normal_relation) {
    r_error = "only normal object parenting can be restored by Save Object State";
    return false;
  }
  return true;
}

LN_GameStateResult validate_save_object_state_hierarchy(KX_Scene &scene, std::string &r_error)
{
  EXP_ListValue<KX_GameObject> *objects = scene.GetObjectList();
  if (objects == nullptr) {
    r_error = "runtime scene object catalog is unavailable";
    return LN_GameStateResult::InternalError;
  }
  for (KX_GameObject *object : *objects) {
    if (object == nullptr || object->GetSGNode() == nullptr) {
      r_error = "runtime scene object catalog contains an invalid object";
      return LN_GameStateResult::InternalError;
    }
    KX_GameObject *parent = nullptr;
    bool normal_relation = true;
    std::string hierarchy_error;
    if (!resolve_parent_object(*object, parent, normal_relation, hierarchy_error)) {
      if (!object->GetSaveGameObjectState()) {
        continue;
      }
      r_error = "marked object '" + object->GetName() + "': " + hierarchy_error;
      return LN_GameStateResult::UnsupportedContent;
    }
    const bool object_saved = object->GetSaveGameObjectState();
    const bool parent_saved = parent != nullptr && parent->GetSaveGameObjectState();
    if (parent != nullptr && object_saved != parent_saved) {
      r_error = "Save Object State hierarchy must be closed: object '" + object->GetName() +
                "' and its parent must both be marked";
      return LN_GameStateResult::UnsupportedContent;
    }
    if ((object_saved || parent_saved) && !normal_relation) {
      r_error = "marked hierarchy at object '" + object->GetName() +
                "' uses a relation that Save Object State cannot restore exactly";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (object_saved &&
        (object->GetParentCompoundIntent() || object->GetParentGhostIntent()))
    {
      r_error = "marked object '" + object->GetName() +
                "' uses unsupported compound/ghost parenting";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (object_saved &&
        (!is_finite(object->NodeGetWorldPosition()) ||
         !is_valid_orientation(object->NodeGetWorldOrientation()) ||
         !is_nonsingular_scale(object->NodeGetWorldScaling())))
    {
      r_error = "marked object '" + object->GetName() +
                "' has a singular or invalid world transform";
      return LN_GameStateResult::UnsupportedContent;
    }
  }
  return LN_GameStateResult::Success;
}

bool read_scene_identity(const blender::Scene *scene, blender::bUUID &r_uuid)
{
  return scene != nullptr &&
         BKE_game_persistence_identity_read(scene->id, r_uuid) ==
             BKE_GamePersistenceIdentityState::Valid;
}

bool classify_property(EXP_Value *property, PropertyType &r_type);
bool property_contract_matches(EXP_Value *property, PropertyType type, bool timer);

PropertyType declared_property_type(const KX_GameObject::SaveGamePropertyMetadata &metadata)
{
  switch (metadata.type) {
    case KX_GameObject::SaveGamePropertyMetadata::Type::Bool:
      return PropertyType::Bool;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Int:
      return PropertyType::Int;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Float:
      return PropertyType::Float;
    case KX_GameObject::SaveGamePropertyMetadata::Type::String:
      return PropertyType::String;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Vector3:
      return PropertyType::Vector3;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Vector4:
      return PropertyType::Vector4;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Object:
      return PropertyType::Object;
    case KX_GameObject::SaveGamePropertyMetadata::Type::List:
      return PropertyType::List;
    case KX_GameObject::SaveGamePropertyMetadata::Type::Dict:
      return PropertyType::Dict;
  }
  return PropertyType::String;
}

/** An object's Save Game identity, or false when it can't be saved by reference. */
using ObjectIdentityFn = std::function<bool(KX_GameObject *object, PropertyData &r_data)>;

/** Saves a Vector, Color, Object, List or Dictionary value; \a r_items counts all values. */
bool property_data_from_value(EXP_Value *value,
                              const ObjectIdentityFn &identity,
                              const int depth,
                              size_t &r_items,
                              PropertyData &r_data,
                              std::string &r_error)
{
  /* Vectors and objects take two more levels in the file. */
  if (depth + 2 > kMaxPropertyDataDepth || ++r_items > kMaxPropertyDataItems) {
    r_error = "is nested too deep or holds too many values";
    return false;
  }
  switch (value->GetValueType()) {
    case VALUE_BOOL_TYPE:
      r_data.kind = PropertyData::Kind::Bool;
      r_data.bool_value = static_cast<EXP_BoolValue *>(value)->GetBool();
      return true;
    case VALUE_INT_TYPE:
      r_data.kind = PropertyData::Kind::Int;
      r_data.int_value = static_cast<EXP_IntValue *>(value)->GetInt();
      return true;
    case VALUE_FLOAT_TYPE:
      r_data.kind = PropertyData::Kind::Float;
      r_data.float_value = static_cast<EXP_FloatValue *>(value)->GetFloat();
      if (!std::isfinite(r_data.float_value)) {
        r_error = "contains a non-finite float";
        return false;
      }
      return true;
    case VALUE_STRING_TYPE:
      r_data.kind = PropertyData::Kind::String;
      r_data.text = value->GetText();
      if (!is_valid_checkpoint_text(r_data.text, kMaxStringPropertyBytes)) {
        r_error = "contains invalid or oversized text";
        return false;
      }
      return true;
    case VALUE_VECTOR3_TYPE: {
      r_data.kind = PropertyData::Kind::Vector3;
      const MT_Vector3 &vector = static_cast<EXP_Vector3Value *>(value)->GetVector3();
      for (int axis = 0; axis < 3; axis++) {
        r_data.vector[axis] = float(vector[axis]);
      }
      break;
    }
    case VALUE_VECTOR4_TYPE: {
      r_data.kind = PropertyData::Kind::Vector4;
      const MT_Vector4 &vector = static_cast<EXP_Vector4Value *>(value)->GetVector4();
      for (int axis = 0; axis < 4; axis++) {
        r_data.vector[axis] = float(vector[axis]);
      }
      break;
    }
    case VALUE_OBJECT_TYPE: {
      r_data.kind = PropertyData::Kind::Object;
      KX_GameObject *object = static_cast<KX_ObjectReferenceValue *>(value)->GetObject();
      if (object != nullptr && !identity(object, r_data)) {
        r_error = "refers to the spawned object '" + object->GetName() +
                  "', which is not saved itself";
        return false;
      }
      return true;
    }
    case VALUE_ARRAY_TYPE:
      r_data.kind = PropertyData::Kind::List;
      for (EXP_Value *item : static_cast<EXP_ArrayValue *>(value)->GetItems()) {
        r_data.items.emplace_back();
        if (!property_data_from_value(
                item, identity, depth + 1, r_items, r_data.items.back(), r_error))
        {
          return false;
        }
      }
      return true;
    case VALUE_DICT_TYPE:
      r_data.kind = PropertyData::Kind::Dict;
      for (const auto &entry : static_cast<EXP_DictValue *>(value)->GetEntries()) {
        if (!is_valid_checkpoint_text(entry.first, kMaxNameBytes, false)) {
          r_error = "contains an invalid dictionary key";
          return false;
        }
        r_data.entries.emplace_back(entry.first, PropertyData());
        if (!property_data_from_value(
                entry.second, identity, depth + 2, r_items, r_data.entries.back().second, r_error))
        {
          return false;
        }
      }
      return true;
    default:
      r_error = "contains a value that Save Game can't store";
      return false;
  }
  for (const float component : r_data.vector) {
    if (!std::isfinite(component)) {
      r_error = "contains a non-finite vector";
      return false;
    }
  }
  return true;
}

/** The game object of a saved Object value: nullptr means none, false that it is missing. */
using ObjectResolveFn = std::function<bool(const PropertyData &data, KX_GameObject *&r_object)>;

EXP_Value *value_from_property_data(const PropertyData &data, const ObjectResolveFn &resolve)
{
  switch (data.kind) {
    case PropertyData::Kind::Bool:
      return new EXP_BoolValue(data.bool_value);
    case PropertyData::Kind::Int:
      return new EXP_IntValue(cInt(data.int_value));
    case PropertyData::Kind::Float:
      return new EXP_FloatValue(float(data.float_value));
    case PropertyData::Kind::String:
      return new EXP_StringValue(data.text, "");
    case PropertyData::Kind::Vector3:
      return new EXP_Vector3Value(MT_Vector3(data.vector));
    case PropertyData::Kind::Vector4:
      return new EXP_Vector4Value(MT_Vector4(data.vector));
    case PropertyData::Kind::Object: {
      KX_GameObject *object = nullptr;
      return resolve(data, object) ? new KX_ObjectReferenceValue(object) : nullptr;
    }
    case PropertyData::Kind::List: {
      EXP_ArrayValue *list = new EXP_ArrayValue();
      for (const PropertyData &item : data.items) {
        EXP_Value *value = value_from_property_data(item, resolve);
        if (value == nullptr) {
          list->Release();
          return nullptr;
        }
        list->Append(value);
      }
      return list;
    }
    case PropertyData::Kind::Dict: {
      EXP_DictValue *dict = new EXP_DictValue();
      for (const auto &entry : data.entries) {
        EXP_Value *value = value_from_property_data(entry.second, resolve);
        if (value == nullptr) {
          dict->Release();
          return nullptr;
        }
        dict->Set(entry.first, value);
      }
      return dict;
    }
  }
  return nullptr;
}

bool reconstruction_properties_supported(KX_GameObject &object)
{
  const int property_count = object.GetPropertyCount();
  if (property_count < 0 || size_t(property_count) > kMaxPropertiesPerObject ||
      object.GetSaveGameProperties().size() > kMaxPropertiesPerObject)
  {
    return false;
  }
  try {
    const std::vector<std::string> names = object.GetPropertyNames();
    if (names.size() != size_t(property_count)) {
      return false;
    }
    size_t text_bytes = 0;
    for (const std::string &name : names) {
      if (name.empty() || name.size() > kMaxNameBytes || contains_embedded_null(name) ||
          text_bytes > kMaxFileBytes - name.size())
      {
        return false;
      }
      text_bytes += name.size();
      EXP_Value *value = object.GetProperty(name);
      PropertyType type;
      if (value == nullptr || !classify_property(value, type)) {
        return false;
      }
      const bool timer = value->GetProperty("timer") != nullptr;
      if (!property_contract_matches(value, type, timer)) {
        return false;
      }
      if (type == PropertyType::Float &&
          !std::isfinite(static_cast<EXP_FloatValue *>(value)->GetFloat()))
      {
        return false;
      }
      if (type == PropertyType::String) {
        const std::string text = value->GetText();
        if (text.size() > kMaxStringPropertyBytes || contains_embedded_null(text) ||
            text_bytes > kMaxFileBytes - text.size())
        {
          return false;
        }
        text_bytes += text.size();
      }
      if (const KX_GameObject::SaveGamePropertyMetadata *metadata =
              object.GetSaveGamePropertyMetadata(name))
      {
        if (type != declared_property_type(*metadata) || timer != metadata->timer) {
          return false;
        }
      }
    }
    for (const auto &item : object.GetSaveGameProperties()) {
      if (item.first.empty() || item.first.size() > kMaxNameBytes ||
          contains_embedded_null(item.first) || object.GetProperty(item.first) == nullptr)
      {
        return false;
      }
    }
  }
  catch (...) {
    return false;
  }
  return true;
}

LN_PersistenceCandidateDescriptor build_candidate_descriptor(KX_Scene &scene,
                                                               KX_GameObject &runtime_object,
                                                               const bool inactive_source = false)
{
  LN_PersistenceCandidateDescriptor descriptor;
  const blender::Scene *blender_scene = scene.GetBlenderScene();
  const blender::Object *object = runtime_object.GetBlenderObject();
  blender::bUUID scene_uuid;
  descriptor.scene_identity_valid = read_scene_identity(blender_scene, scene_uuid);
  descriptor.object_identity_valid =
      !blender::BLI_uuid_is_nil(runtime_object.GetPersistenceSourceUuid());
  descriptor.runtime_class_supported = typeid(runtime_object) == typeid(KX_EmptyObject);
  if (object == nullptr) {
    return descriptor;
  }

  if (object->type == blender::OB_EMPTY) {
    descriptor.object_kind = LN_PersistenceObjectKind::Empty;
  }
  else if (object->type == blender::OB_MESH) {
    descriptor.object_kind = LN_PersistenceObjectKind::Mesh;
  }

  switch (object->body_type) {
    case blender::OB_BODY_TYPE_NO_COLLISION:
    case blender::OB_BODY_TYPE_STATIC:
    case blender::OB_BODY_TYPE_DYNAMIC:
    case blender::OB_BODY_TYPE_RIGID:
    case blender::OB_BODY_TYPE_KINEMATIC:
    case blender::OB_BODY_TYPE_SOFT:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Ordinary;
      break;
    case blender::OB_BODY_TYPE_OCCLUDER:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Occluder;
      break;
    case blender::OB_BODY_TYPE_SENSOR:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Sensor;
      break;
    case blender::OB_BODY_TYPE_NAVMESH:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Navigation;
      break;
    case blender::OB_BODY_TYPE_CHARACTER:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Character;
      break;
    case blender::OB_BODY_TYPE_VEHICLE:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Vehicle;
      break;
    default:
      descriptor.runtime_role = LN_PersistenceRuntimeRole::Unsupported;
      break;
  }
  if ((object->gameflag & blender::OB_NAVMESH) != 0) {
    descriptor.runtime_role = LN_PersistenceRuntimeRole::Navigation;
  }
  else if ((object->gameflag & blender::OB_CHARACTER) != 0) {
    descriptor.runtime_role = LN_PersistenceRuntimeRole::Character;
  }
  else if ((object->gameflag & blender::OB_SENSOR) != 0) {
    descriptor.runtime_role = LN_PersistenceRuntimeRole::Sensor;
  }
  else if ((object->gameflag2 & blender::OB_HAS_VEHICLE) != 0) {
    descriptor.runtime_role = LN_PersistenceRuntimeRole::Vehicle;
  }

  descriptor.expects_physics_body = ELEM(object->body_type,
                                         blender::OB_BODY_TYPE_STATIC,
                                         blender::OB_BODY_TYPE_DYNAMIC,
                                         blender::OB_BODY_TYPE_RIGID,
                                         blender::OB_BODY_TYPE_KINEMATIC,
                                         blender::OB_BODY_TYPE_SOFT);
  PHY_IPhysicsController *controller = runtime_object.GetPhysicsController();
  if (controller == nullptr) {
    descriptor.physics_kind = LN_PersistencePhysicsKind::None;
  }
  else if (!controller->SupportsGameStatePersistence()) {
    descriptor.physics_kind = LN_PersistencePhysicsKind::Unsupported;
  }
  else {
    descriptor.physics_kind = controller->IsSoftBody() ? LN_PersistencePhysicsKind::JoltSoft :
                                                         LN_PersistencePhysicsKind::JoltRigid;
    descriptor.physics_body_valid = controller->HasValidGameStateBody();
  }

  descriptor.has_authored_parent = object->parent != nullptr;
  descriptor.has_runtime_parent = runtime_object.GetParent() != nullptr;
  descriptor.has_authored_children = runtime_object.GetSGNode() != nullptr &&
                                     !runtime_object.GetSGNode()->GetSGChildren().empty();
  descriptor.has_runtime_children = descriptor.has_authored_children;
  descriptor.has_collection_instance = object->instance_collection != nullptr;
  descriptor.has_constraints = !BLI_listbase_is_empty(&object->constraints) ||
                               !runtime_object.m_rigidbodyConstraints.empty();
#ifdef WITH_PYTHON
  descriptor.has_python_runtime = object->custom_object != nullptr ||
                                  !BLI_listbase_is_empty(&object->components);
#endif
  descriptor.has_logic_bricks = !BLI_listbase_is_empty(&object->sensors) ||
                                !BLI_listbase_is_empty(&object->controllers) ||
                                !BLI_listbase_is_empty(&object->actuators);
  descriptor.is_compound = (object->gameflag & blender::OB_CHILD) != 0 ||
                           (controller != nullptr && controller->IsCompound());
  descriptor.has_lod = runtime_object.GetLodManager() != nullptr;
  descriptor.has_obstacle = (object->gameflag & blender::OB_HASOBSTACLE) != 0;
  descriptor.has_scene_graph_controllers = runtime_object.GetSGNode() != nullptr &&
                                           !runtime_object.GetSGNode()
                                                ->GetSGControllerList()
                                                .empty();
  descriptor.has_soft_body_pin_dependency =
      object->bsoft != nullptr &&
      (object->bsoft->pin_object != nullptr || object->bsoft->pin_vgroup[0] != '\0');
  const KX_GameObject::ActivityCullingInfo &activity = runtime_object.GetActivityCullingInfo();
  descriptor.reconstruction_state_valid =
      runtime_object.GetSGNode() != nullptr && !runtime_object.GetName().empty() &&
      runtime_object.GetName().size() <= kMaxNameBytes &&
      !contains_embedded_null(runtime_object.GetName()) &&
      (runtime_object.GetLayer() != 0 || inactive_source) &&
      is_finite(runtime_object.NodeGetWorldPosition()) &&
      is_valid_orientation(runtime_object.NodeGetWorldOrientation()) &&
      is_finite(runtime_object.NodeGetWorldScaling()) &&
      is_finite(runtime_object.GetObjectColor()) && std::isfinite(activity.m_physicsRadius) &&
      std::isfinite(activity.m_logicRadius);
  descriptor.reconstruction_render_data_valid =
      (descriptor.object_kind == LN_PersistenceObjectKind::Empty &&
       runtime_object.GetMeshCount() == 0) ||
      (descriptor.object_kind == LN_PersistenceObjectKind::Mesh &&
       runtime_object.GetMeshCount() > 0);
  for (int index = 0;
       descriptor.reconstruction_render_data_valid && index < runtime_object.GetMeshCount();
       index++)
  {
    descriptor.reconstruction_render_data_valid = runtime_object.GetMesh(index) != nullptr;
  }
  descriptor.reconstruction_properties_supported =
      reconstruction_properties_supported(runtime_object);
  return descriptor;
}

LN_GameStateResult validate_candidate(const LN_PersistenceCandidateDescriptor &descriptor,
                                      const std::string &name,
                                      std::string &r_error,
                                      const bool allow_normal_hierarchy = true)
{
  if (descriptor.physics_kind == LN_PersistencePhysicsKind::JoltSoft) {
    r_error = "persistence candidate '" + name +
              "': Save Object State cannot rewind Jolt soft-body particle state";
    return LN_GameStateResult::UnsupportedContent;
  }
  LN_PersistenceCapabilityRequirements requirements;
  requirements.allow_normal_hierarchy = allow_normal_hierarchy;
  const LN_PersistenceCapabilityIssue issue = LN_ValidatePersistenceCandidate(descriptor,
                                                                               requirements);
  if (issue == LN_PersistenceCapabilityIssue::None) {
    return LN_GameStateResult::Success;
  }
  r_error = "persistence candidate '" + name + "': " + LN_PersistenceCapabilityIssueText(issue);
  return ELEM(issue,
              LN_PersistenceCapabilityIssue::MissingSceneIdentity,
              LN_PersistenceCapabilityIssue::MissingObjectIdentity) ?
             LN_GameStateResult::InvalidRequest :
             LN_GameStateResult::UnsupportedContent;
}


bool classify_property(EXP_Value *property, PropertyType &r_type)
{
  switch (property->GetValueType()) {
    case VALUE_VECTOR3_TYPE:
      r_type = PropertyType::Vector3;
      return true;
    case VALUE_VECTOR4_TYPE:
      r_type = PropertyType::Vector4;
      return true;
    case VALUE_OBJECT_TYPE:
      r_type = PropertyType::Object;
      return true;
    case VALUE_ARRAY_TYPE:
      r_type = PropertyType::List;
      return true;
    case VALUE_DICT_TYPE:
      r_type = PropertyType::Dict;
      return true;
    default:
      break;
  }
  if (dynamic_cast<EXP_BoolValue *>(property) != nullptr) {
    r_type = PropertyType::Bool;
    return true;
  }
  if (dynamic_cast<EXP_IntValue *>(property) != nullptr) {
    r_type = PropertyType::Int;
    return true;
  }
  if (dynamic_cast<EXP_FloatValue *>(property) != nullptr) {
    r_type = PropertyType::Float;
    return true;
  }
  if (dynamic_cast<EXP_StringValue *>(property) != nullptr) {
    r_type = PropertyType::String;
    return true;
  }
  return false;
}

bool property_contract_matches(EXP_Value *property,
                               const PropertyType type,
                               const bool timer)
{
  PropertyType runtime_type;
  if (property == nullptr || !classify_property(property, runtime_type) ||
      runtime_type != type || property->GetPropertyCount() != int(timer))
  {
    return false;
  }
  EXP_Value *timer_marker = property->GetProperty("timer");
  if (!timer) {
    return timer_marker == nullptr;
  }
  return type == PropertyType::Float && timer_marker != nullptr &&
         timer_marker->GetValueType() == VALUE_BOOL_TYPE &&
         static_cast<EXP_BoolValue *>(timer_marker)->GetBool() &&
         timer_marker->GetPropertyCount() == 0;
}

bool capture_property(const std::string &name,
                      const KX_GameObject::SaveGamePropertyMetadata &metadata,
                      EXP_Value *property,
                      const ObjectIdentityFn &identity,
                      PropertyState &r_state,
                      std::string &r_error)
{
  if (!is_valid_checkpoint_text(name, kMaxNameBytes, false)) {
    r_error = "invalid Save Game property name";
    return false;
  }
  r_state.name = name;
  r_state.timer = metadata.timer;
  r_state.type = declared_property_type(metadata);
  if (property == nullptr) {
    r_state.exists = false;
    return true;
  }
  if (!property_contract_matches(property, r_state.type, r_state.timer)) {
    r_error = "marked property '" + name + "' no longer matches its declared type";
    return false;
  }
  switch (r_state.type) {
    case PropertyType::Bool:
      r_state.bool_value = static_cast<EXP_BoolValue *>(property)->GetBool();
      break;
    case PropertyType::Int:
      r_state.int_value = static_cast<EXP_IntValue *>(property)->GetInt();
      break;
    case PropertyType::Float:
      r_state.float_value = static_cast<EXP_FloatValue *>(property)->GetFloat();
      if (!std::isfinite(r_state.float_value)) {
        r_error = "marked property '" + name + "' contains a non-finite float";
        return false;
      }
      break;
    case PropertyType::String:
      r_state.string_value = static_cast<EXP_StringValue *>(property)->GetText();
      if (!is_valid_checkpoint_text(r_state.string_value, kMaxStringPropertyBytes)) {
        r_error = "marked property '" + name + "' contains invalid or oversized text";
        return false;
      }
      break;
    case PropertyType::Vector3:
    case PropertyType::Vector4:
    case PropertyType::Object:
    case PropertyType::List:
    case PropertyType::Dict: {
      size_t items = 0;
      std::string data_error;
      if (!property_data_from_value(property, identity, 0, items, r_state.data, data_error)) {
        r_error = "marked property '" + name + "' " + data_error;
        return false;
      }
      break;
    }
  }
  return true;
}

LN_GameStateResult capture_checkpoint(KX_Scene &scene,
                                      const std::vector<CaptureParticipant> &participants,
                                      MaterialCaptureContext &material_context,
                                      const GeometryNodesAssetCatalog &geometry_nodes_assets,
                                      RuntimeNodeTreeCapture &node_tree_capture,
                                      Checkpoint &r_checkpoint,
                                      std::string &r_error)
{
  blender::Scene *blender_scene = scene.GetBlenderScene();
  blender::bUUID scene_uuid;
  if (!read_scene_identity(blender_scene, scene_uuid)) {
    r_error = "runtime scene has no durable checkpoint UUID";
    return LN_GameStateResult::InvalidRequest;
  }
  r_checkpoint.scene_uuid = uuid_string(scene_uuid);
  r_checkpoint.scene_name = scene.GetName();
  if (!is_valid_checkpoint_text(r_checkpoint.scene_name, kMaxNameBytes)) {
    r_error = "scene name contains invalid or oversized text";
    return LN_GameStateResult::UnsupportedContent;
  }

  SCA_TimeEventManager *time_manager = scene.GetTimeEventManager();
  std::unordered_map<const EXP_Value *, size_t> time_property_counts;
  try {
    if (time_manager != nullptr) {
      const std::vector<EXP_Value *> &time_properties =
          time_manager->PersistenceTimeProperties();
      time_property_counts.reserve(time_properties.size());
      for (const EXP_Value *property : time_properties) {
        ++time_property_counts[property];
      }
    }
  }
  catch (...) {
    r_error = "could not validate timer-property state for Save Game";
    return LN_GameStateResult::InternalError;
  }

  if (participants.size() > kMaxObjects) {
    r_error = "runtime scene exceeds the checkpoint participant limit";
    return LN_GameStateResult::UnsupportedContent;
  }

  size_t budget = kMaxFileBytes;
  if (!consume_budget(budget, sizeof(Checkpoint) + r_checkpoint.scene_name.size())) {
    r_error = "Save Game data exceeds the memory budget";
    return LN_GameStateResult::UnsupportedContent;
  }
  r_checkpoint.authored_objects.clear();
  r_checkpoint.spawned_instances.clear();
  r_checkpoint.authored_objects.reserve(participants.size());
  r_checkpoint.spawned_instances.reserve(participants.size());

  std::unordered_map<KX_GameObject *, std::string> structural_identities;
  std::unordered_map<KX_GameObject *, std::string> saved_identities;
  try {
    structural_identities.reserve(participants.size());
    saved_identities.reserve(participants.size());
    for (const CaptureParticipant &participant : participants) {
      if (participant.exists && participant.object != nullptr) {
        saved_identities.emplace(participant.object, uuid_string(participant.uuid));
      }
    }
    for (const CaptureParticipant &participant : participants) {
      if (!participant.exists || participant.object == nullptr ||
          !participant.save_object_state)
      {
        continue;
      }
      if (!structural_identities.emplace(participant.object, uuid_string(participant.uuid)).second) {
        r_error = "checkpoint registry contains a duplicate structural object";
        return LN_GameStateResult::InternalError;
      }
    }
  }
  catch (...) {
    r_error = "could not allocate the Save Object State hierarchy index";
    return LN_GameStateResult::InternalError;
  }

  for (const CaptureParticipant &participant : participants) {
    KX_GameObject *object = participant.object;
    if (!participant.exists) {
      if (participant.spawned || !participant.save_object_state ||
          blender::BLI_uuid_is_nil(participant.uuid))
      {
        continue;
      }
      ObjectState state;
      state.persistence_id = uuid_string(participant.uuid);
      state.name = participant.name;
      state.has_existence = true;
      state.exists = false;
      if (!is_valid_checkpoint_text(state.name, kMaxNameBytes) ||
          !consume_budget(budget,
                          sizeof(ObjectState) + state.persistence_id.size() * 2u +
                              state.name.size() * 2u + 128u))
      {
        r_error = "removed object identity exceeds the Save Game limit";
        return LN_GameStateResult::UnsupportedContent;
      }
      r_checkpoint.authored_objects.push_back(std::move(state));
      continue;
    }
    if (object == nullptr) {
      r_error = "checkpoint registry contains an invalid live object";
      return LN_GameStateResult::InternalError;
    }
    if (!participant.spawned && !participant.save_object_state &&
        !object->GetSaveGameObjectState() && !object->HasSaveGameProperties())
    {
      continue;
    }
    if (participant.spawned && !object->GetSaveGameObjectState()) {
      continue;
    }
    if (blender::BLI_uuid_is_nil(participant.uuid) ||
        (participant.spawned && blender::BLI_uuid_is_nil(participant.source_uuid)))
    {
      r_error = "checkpoint registry contains a live participant with an invalid UUID";
      return LN_GameStateResult::InvalidRequest;
    }

    ObjectState state;
    state.persistence_id = uuid_string(participant.uuid);
    state.name = participant.name;
    state.spawned = participant.spawned;
    state.has_existence = participant.save_object_state;
    if (participant.spawned) {
      state.source_id = uuid_string(participant.source_uuid);
      state.creation_position = participant.creation_position;
      state.creation_orientation = participant.creation_orientation;
      state.creation_scale = participant.creation_scale;
      if (!is_finite(state.creation_position) ||
          !is_valid_orientation(state.creation_orientation) ||
          !is_nonsingular_scale(state.creation_scale))
      {
        r_error = "spawned object '" + state.name + "' has an invalid creation transform";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
    if (state.persistence_id.empty() || state.persistence_id.size() > kMaxIdentityBytes ||
        contains_embedded_null(state.persistence_id) ||
        !is_valid_checkpoint_text(state.name, kMaxNameBytes))
    {
      r_error = "object identity exceeds the Save Game limit";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (!consume_budget(budget,
                        sizeof(ObjectState) + state.persistence_id.size() * 2u +
                            state.name.size() * 2u + 128u))
    {
      r_error = "Save Game data exceeds the memory budget";
      return LN_GameStateResult::UnsupportedContent;
    }

    if (object->GetSaveGameObjectState()) {
      if (object->GetSGNode() == nullptr) {
        r_error = "marked object '" + state.name + "' has no runtime scene-graph node";
        return LN_GameStateResult::UnsupportedContent;
      }
      if (persistence_parent_relation(object->GetSGNode()) ==
          PersistenceParentRelation::Unsupported)
      {
        r_error = "marked object '" + state.name +
                  "' uses a parent relation that Save Game cannot restore exactly";
        return LN_GameStateResult::UnsupportedContent;
      }
      state.has_transform = true;
      state.world_position = object->NodeGetWorldPosition();
      state.world_orientation = object->NodeGetWorldOrientation();
      state.world_scale = object->NodeGetWorldScaling();
      if (!is_finite(state.world_position) || !is_valid_orientation(state.world_orientation) ||
          !is_nonsingular_scale(state.world_scale))
      {
        r_error = "marked object '" + state.name + "' has an invalid world transform";
        return LN_GameStateResult::UnsupportedContent;
      }
      if (PHY_IPhysicsController *controller = object->GetPhysicsController()) {
        if (!controller->SupportsGameStatePersistence() ||
            !controller->HasValidGameStateBody() || controller->IsSoftBody())
        {
          r_error = "marked object '" + state.name +
                    "' does not have a supported Jolt rigid body";
          return LN_GameStateResult::UnsupportedContent;
        }
        const bool scale_supported = controller->PrepareGameStateScaling(state.world_scale,
                                                                         state.world_scale);
        controller->ClearPreparedGameStateScaling();
        if (!scale_supported) {
          r_error = "marked Jolt rigid body '" + state.name +
                    "' cannot reproduce its saved scale";
          return LN_GameStateResult::UnsupportedContent;
        }
        PHY_GameStateRigidBodyState physics_state;
        if (!controller->CaptureGameStateRigidBody(physics_state) ||
            !checkpoint_rigid_body_state(physics_state, state.rigid_body))
        {
          r_error = "marked Jolt rigid body '" + state.name +
                    "' has an inconsistent runtime state";
          return LN_GameStateResult::UnsupportedContent;
        }
        state.has_rigid_body_state = true;
      }
    }

    if (object->GetSaveGameObjectState()) {
      const LN_GameStateResult material_result = capture_material_state(
          *object, participant.material_slot_count, material_context, state, budget, r_error);
      if (!LN_GameStateSucceeded(material_result)) {
        return material_result;
      }
      state.logic_trees = participant.logic_trees;
      state.has_runtime_structure_state = true;
      for (const LogicTreeAttachmentState &tree : state.logic_trees) {
        if (!consume_budget(budget,
                            sizeof(LogicTreeAttachmentState) + tree.tree_name.size() +
                                tree.library_path.size() + 2u))
        {
          r_error = "C++ Logic Node attachments for object '" + state.name +
                    "' exceed the Save Game memory budget";
          return LN_GameStateResult::UnsupportedContent;
        }
      }
      if (participant.modifier_baseline == nullptr) {
        r_error = "object '" + state.name +
                  "' has no authored modifier baseline";
        return LN_GameStateResult::InternalError;
      }
      const LN_GameStateResult modifier_result = capture_modifier_state(
          *object,
          *participant.modifier_baseline,
          geometry_nodes_assets,
          node_tree_capture,
          state,
          budget,
          r_error);
      if (!LN_GameStateSucceeded(modifier_result)) {
        return modifier_result;
      }
    }

    if (object->GetSaveGameObjectState()) {
      state.has_hierarchy_state = true;
      state.hierarchy.compound = object->GetParentCompoundIntent();
      state.hierarchy.ghost = object->GetParentGhostIntent();
      if (state.hierarchy.compound || state.hierarchy.ghost) {
        r_error = "marked object '" + state.name +
                  "' uses compound/ghost parenting, which cannot be restored transactionally";
        return LN_GameStateResult::UnsupportedContent;
      }
      KX_GameObject *parent = nullptr;
      std::string hierarchy_error;
      if (!resolve_normal_parent(*object, parent, hierarchy_error)) {
        r_error = "marked object '" + state.name + "': " + hierarchy_error;
        return LN_GameStateResult::UnsupportedContent;
      }
      if (parent != nullptr) {
        const auto parent_identity = structural_identities.find(parent);
        if (parent_identity == structural_identities.end()) {
          r_error = "marked object '" + state.name +
                    "' has a parent that is not marked Save Object State";
          return LN_GameStateResult::UnsupportedContent;
        }
        state.hierarchy.relation = HierarchyRelation::Normal;
        state.hierarchy.parent_id = parent_identity->second;
      }

      /* Set Collection Member's changes, in their order, each by collection name and library. */
      const std::vector<std::pair<const blender::Collection *, bool>> &memberships =
          object->GetCollectionMemberships();
      if (memberships.size() > kMaxCollectionMembershipsPerObject) {
        r_error = "marked object '" + state.name +
                  "' exceeds the Save Game collection membership limit";
        return LN_GameStateResult::UnsupportedContent;
      }
      state.collection_memberships.reserve(memberships.size());
      for (const auto &[collection, member] : memberships) {
        /* KX_Scene::NoteCollectionsChanged drops collections that left the scene. */
        if (!scene.CollectionInScene(collection)) {
          continue;
        }
        CollectionMembershipState membership;
        membership.collection_name = collection->id.name + 2;
        membership.library_path = collection->id.lib ? collection->id.lib->filepath :
                                                       std::string();
        membership.member = member;
        if (!is_valid_checkpoint_text(membership.collection_name, kMaxNameBytes, false) ||
            !is_valid_checkpoint_text(membership.library_path, kMaxIdentityBytes) ||
            !consume_budget(budget,
                            sizeof(CollectionMembershipState) +
                                membership.collection_name.size() +
                                membership.library_path.size() + 2u))
        {
          r_error = "collection membership '" + membership.collection_name +
                    "' of marked object '" + state.name + "' cannot be saved";
          return LN_GameStateResult::UnsupportedContent;
        }
        state.collection_memberships.push_back(std::move(membership));
      }
    }

    std::vector<std::string> property_names;
    property_names.reserve(object->GetSaveGameProperties().size());
    for (const auto &item : object->GetSaveGameProperties()) {
      property_names.push_back(item.first);
    }
    if (property_names.size() > kMaxPropertiesPerObject) {
      r_error = "object '" + state.name + "' exceeds the Save Game property limit";
      return LN_GameStateResult::UnsupportedContent;
    }
    std::sort(property_names.begin(), property_names.end());
    state.properties.reserve(property_names.size());
    /* Objects saved themselves are referred to by identity, other authored ones by name. */
    const ObjectIdentityFn identity = [&](KX_GameObject *target, PropertyData &r_data) {
      const auto saved = saved_identities.find(target);
      if (saved != saved_identities.end()) {
        r_data.text = saved->second;
        return true;
      }
      if (target->IsReplica() || !is_valid_checkpoint_text(target->GetName(), kMaxNameBytes)) {
        return false;
      }
      r_data.text = target->GetName();
      r_data.object_by_name = true;
      return true;
    };
    for (const std::string &name : property_names) {
      PropertyState property;
      const KX_GameObject::SaveGamePropertyMetadata *metadata =
          object->GetSaveGamePropertyMetadata(name);
      EXP_Value *runtime_property = object->GetProperty(name);
      if (metadata == nullptr ||
          !capture_property(name, *metadata, runtime_property, identity, property, r_error))
      {
        r_error = "object '" + state.name + "': " + r_error;
        return LN_GameStateResult::UnsupportedContent;
      }
      if (runtime_property != nullptr) {
        const auto membership = time_property_counts.find(runtime_property);
        const size_t membership_count = membership == time_property_counts.end() ?
                                            0u :
                                            membership->second;
        if (membership_count != size_t(metadata->timer)) {
          r_error = "object '" + state.name + "': marked property '" + name +
                    "' has inconsistent timer-manager membership";
          return LN_GameStateResult::UnsupportedContent;
        }
      }
      if (!consume_budget(budget,
                          sizeof(PropertyState) + property.name.size() * 2u +
                              property.string_value.size() +
                              property_data_bytes(property.data) + 128u))
      {
        r_error = "Save Game data exceeds the memory budget";
        return LN_GameStateResult::UnsupportedContent;
      }
      state.properties.push_back(std::move(property));
    }
    if (state.spawned) {
      r_checkpoint.spawned_instances.push_back(std::move(state));
    }
    else {
      r_checkpoint.authored_objects.push_back(std::move(state));
    }
  }
  const LN_GameStateResult material_result =
      finalize_material_capture(material_context, r_checkpoint, r_error);
  if (!LN_GameStateSucceeded(material_result)) {
    return material_result;
  }
  const LN_GameStateResult compositor_result =
      node_tree_capture.CaptureCompositor(*blender_scene, r_checkpoint, budget, r_error);
  if (!LN_GameStateSucceeded(compositor_result)) {
    return compositor_result;
  }
  const LN_GameStateResult node_tree_result =
      node_tree_capture.Finalize(r_checkpoint, r_error);
  if (!LN_GameStateSucceeded(node_tree_result)) {
    return node_tree_result;
  }
  auto by_uuid = [](const ObjectState &a, const ObjectState &b) {
    return a.persistence_id < b.persistence_id;
  };
  std::sort(r_checkpoint.authored_objects.begin(), r_checkpoint.authored_objects.end(), by_uuid);
  std::sort(r_checkpoint.spawned_instances.begin(), r_checkpoint.spawned_instances.end(), by_uuid);
  return LN_GameStateResult::Success;
}


EXP_Value *create_property_value(const PropertyState &property, const ObjectResolveFn &resolve)
{
  using ScopedExpValue = std::unique_ptr<EXP_Value, void (*)(EXP_Value *)>;
  const auto release_value = [](EXP_Value *value) {
    if (value != nullptr) {
      value->Release();
    }
  };
  ScopedExpValue value(nullptr, release_value);
  switch (property.type) {
    case PropertyType::Bool:
      value.reset(new EXP_BoolValue(property.bool_value, property.name));
      break;
    case PropertyType::Int:
      value.reset(new EXP_IntValue(property.int_value, property.name));
      break;
    case PropertyType::Float:
      value.reset(new EXP_FloatValue(property.float_value, property.name));
      break;
    case PropertyType::String:
      value.reset(new EXP_StringValue(property.string_value, property.name));
      break;
    case PropertyType::Vector3:
    case PropertyType::Vector4:
    case PropertyType::Object:
    case PropertyType::List:
    case PropertyType::Dict:
      value.reset(value_from_property_data(property.data, resolve));
      break;
  }
  if (value != nullptr && property.timer) {
    ScopedExpValue timer(new EXP_BoolValue(true), release_value);
    value->SetProperty("timer", timer.get());
  }
  return value.release();
}

struct ResolvedObjectState {
  KX_GameObject *object = nullptr;
  KX_GameObject *desired_parent = nullptr;
  const ObjectState *saved = nullptr;
  const ModifierBaseline *modifier_baseline = nullptr;
  MT_Vector3 target_world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 target_world_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 target_world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 prepared_local_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 prepared_local_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 prepared_local_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  size_t depth = 0;
  MT_Vector3 original_world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 original_world_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 original_world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  bool apply_hierarchy = false;
  bool apply_rigid_body = false;
  bool position_changed = false;
  bool orientation_changed = false;
  bool scale_changed = false;
};

struct LoadStateTarget {
  KX_GameObject *object = nullptr;
  KX_GameObject *desired_parent = nullptr;
  const ObjectState *saved = nullptr;
  const ModifierBaseline *modifier_baseline = nullptr;
  bool apply_transform = false;
  bool apply_hierarchy = false;
  bool apply_rigid_body = false;
  bool apply_material = false;
  size_t hierarchy_depth = 0;
  MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 world_orientation = MT_Matrix3x3::Identity();
  MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
};

struct PredictedWorldTransform {
  MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Matrix3x3 orientation = MT_Matrix3x3::Identity();
  MT_Vector3 scale = MT_Vector3(1.0f, 1.0f, 1.0f);
};

bool predict_normal_world_transform(const PredictedWorldTransform &parent,
                                    const MT_Vector3 &local_position,
                                    const MT_Matrix3x3 &local_orientation,
                                    const MT_Vector3 &local_scale,
                                    PredictedWorldTransform &r_world)
{
  if (!is_finite(parent.position) || !is_finite(parent.orientation) ||
      !is_finite(parent.scale) || !is_finite(local_position) ||
      !is_finite(local_orientation) || !is_finite(local_scale))
  {
    return false;
  }
  const MT_Matrix3x3 parent_basis = parent.orientation.scaled(
      parent.scale[0], parent.scale[1], parent.scale[2]);
  const MT_Matrix3x3 local_basis = local_orientation.scaled(
      local_scale[0], local_scale[1], local_scale[2]);
  const MT_Matrix3x3 world_basis = parent_basis * local_basis;
  r_world.position = parent.position + parent_basis * local_position;
  for (int axis = 0; axis < 3; ++axis) {
    r_world.scale[axis] = world_basis.getColumn(axis).length();
    if (!std::isfinite(r_world.scale[axis]) ||
        r_world.scale[axis] < std::numeric_limits<float>::epsilon())
    {
      return false;
    }
  }
  r_world.orientation = world_basis.scaled(1.0f / r_world.scale[0],
                                            1.0f / r_world.scale[1],
                                            1.0f / r_world.scale[2]);
  return is_finite(r_world.position) && is_finite(r_world.orientation);
}

bool decompose_representable_local_basis(const MT_Matrix3x3 &basis,
                                         MT_Matrix3x3 &r_orientation,
                                         MT_Vector3 &r_scale)
{
  if (!is_finite(basis)) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    r_scale[axis] = basis.getColumn(axis).length();
    if (!std::isfinite(r_scale[axis]) ||
        r_scale[axis] < std::numeric_limits<float>::epsilon())
    {
      return false;
    }
  }
  r_orientation = basis.scaled(
      1.0f / r_scale[0], 1.0f / r_scale[1], 1.0f / r_scale[2]);
  if (r_orientation.determinant() < 0.0f) {
    int flip_axis = 0;
    for (int axis = 1; axis < 3; ++axis) {
      if (std::abs(r_scale[axis]) > std::abs(r_scale[flip_axis])) {
        flip_axis = axis;
      }
    }
    r_scale[flip_axis] = -r_scale[flip_axis];
    r_orientation.setColumn(flip_axis, -r_orientation.getColumn(flip_axis));
  }
  return is_valid_orientation(r_orientation);
}

size_t object_depth(KX_GameObject &object, const size_t object_limit, bool &r_valid)
{
  SG_Node *node = object.GetSGNode();
  if (node == nullptr) {
    r_valid = false;
    return 0;
  }
  size_t depth = 0;
  std::unordered_set<SG_Node *> ancestors;
  ancestors.insert(node);
  for (SG_Node *parent = node->GetSGParent(); parent != nullptr; parent = parent->GetSGParent()) {
    if (!ancestors.insert(parent).second || ++depth > object_limit) {
      r_valid = false;
      return 0;
    }
  }
  r_valid = true;
  return depth;
}

void apply_world_transform(KX_GameObject &object, const ResolvedObjectState &resolved)
{
  SG_Node *node = object.GetSGNode();
  BLI_assert(node != nullptr);

  /* Write SG directly. KX_GameObject's convenience setters also mutate physics and may allocate a
   * scaled Jolt shape; checkpoint commit installs only the shape prepared during preflight. */
  node->SetLocalPosition(resolved.prepared_local_position);
  node->SetLocalOrientation(resolved.prepared_local_orientation);
  node->SetLocalScale(resolved.prepared_local_scale);
  object.NodeUpdateGS(0.0);

  PHY_IPhysicsController *controller = object.GetPhysicsController();
  if (controller != nullptr) {
    if (controller->IsSoftBody()) {
      if (resolved.position_changed || resolved.orientation_changed) {
        controller->SetSoftBodyTransform(
            resolved.target_world_position - resolved.original_world_position,
            resolved.target_world_orientation);
      }
    }
    else {
      if (resolved.scale_changed) {
        controller->CommitPreparedGameStateScaling();
      }
      if (resolved.position_changed || resolved.orientation_changed) {
        controller->SetTransform();
      }
    }
  }
}

using LoadCommitPrelude = void (*)(void *data);

LN_GameStateResult apply_object_states(KX_Scene &scene,
                                       const std::vector<LoadStateTarget> &targets,
                                       const LoadCommitPrelude commit_prelude,
                                       void *commit_data,
                                       std::string &r_error)
{
  std::vector<ResolvedObjectState> transforms;
  struct PreparedProperty {
    KX_GameObject *object = nullptr;
    EXP_Value *current = nullptr;
    EXP_Value *replacement = nullptr;
    EXP_Value::PersistencePropertyNode insertion_node;
    std::string name;
    bool timer = false;
    bool remove = false;
    bool replacement_map_reference = false;
  };
  std::vector<PreparedProperty> properties;
  auto release_prepared = [&properties]() {
    for (PreparedProperty &property : properties) {
      EXP_Value::PersistenceReleasePropertyNode(property.insertion_node);
      if (property.replacement_map_reference && property.replacement != nullptr) {
        property.replacement->Release();
        property.replacement_map_reference = false;
      }
      if (property.replacement != nullptr) {
        property.replacement->Release();
        property.replacement = nullptr;
      }
    }
  };
  auto clear_prepared_shapes = [&transforms]() {
    for (const ResolvedObjectState &resolved : transforms) {
      if (PHY_IPhysicsController *controller = resolved.object->GetPhysicsController()) {
        controller->ClearPreparedGameStateScaling();
      }
    }
  };
  auto clear_prepared_rigid_bodies = [&transforms]() {
    for (const ResolvedObjectState &resolved : transforms) {
      if (PHY_IPhysicsController *controller = resolved.object->GetPhysicsController()) {
        controller->ClearPreparedGameStateRigidBody();
      }
    }
  };

  try {
    size_t property_count = 0;
    for (const LoadStateTarget &target : targets) {
      if (target.object == nullptr || target.saved == nullptr ||
          property_count > kMaxObjects * kMaxPropertiesPerObject - target.saved->properties.size())
      {
        r_error = "Load Game state plan is invalid or oversized";
        return LN_GameStateResult::InternalError;
      }
      property_count += target.saved->properties.size();
    }
    transforms.reserve(targets.size());
    properties.reserve(property_count);
    /* Object values: saved objects by identity, other authored objects by name. */
    std::unordered_map<std::string, KX_GameObject *> objects_by_identity;
    objects_by_identity.reserve(targets.size());
    for (const LoadStateTarget &target : targets) {
      objects_by_identity.emplace(target.saved->persistence_id, target.object);
    }
    std::string missing_object;
    const ObjectResolveFn resolve_saved_object = [&](const PropertyData &data,
                                                     KX_GameObject *&r_object) {
      r_object = nullptr;
      if (data.text.empty()) {
        return true;
      }
      if (data.object_by_name) {
        r_object = scene.GetObjectList()->FindValue(data.text);
        if (r_object == nullptr) {
          r_object = scene.GetInactiveList()->FindValue(data.text);
        }
      }
      else if (const auto found = objects_by_identity.find(data.text);
               found != objects_by_identity.end())
      {
        r_object = found->second;
      }
      if (r_object == nullptr) {
        missing_object = data.text;
      }
      return r_object != nullptr;
    };
    for (const LoadStateTarget &target : targets) {
      const ObjectState &state = *target.saved;
      KX_GameObject *object = target.object;
      if (target.apply_transform) {
        bool valid_depth = true;
        const size_t depth = target.apply_hierarchy ?
                                 target.hierarchy_depth :
                                 object_depth(*object, kMaxObjects, valid_depth);
        if (!valid_depth || depth > kMaxObjects || object->GetSGNode() == nullptr ||
            (target.desired_parent != nullptr &&
             (target.desired_parent == object ||
              target.desired_parent->GetScene() != &scene ||
              target.desired_parent->GetSGNode() == nullptr)) ||
            !is_finite(object->NodeGetWorldPosition()) ||
            !is_valid_orientation(object->NodeGetWorldOrientation()) ||
            !is_nonsingular_scale(object->NodeGetWorldScaling()) ||
            !is_finite(target.world_position) ||
            !is_valid_orientation(target.world_orientation) ||
            !is_nonsingular_scale(target.world_scale))
        {
          release_prepared();
          r_error = "runtime hierarchy is invalid for marked object '" + state.name + "'";
          return LN_GameStateResult::Incompatible;
        }
        ResolvedObjectState resolved;
        resolved.object = object;
        resolved.desired_parent = target.desired_parent;
        resolved.saved = &state;
        resolved.depth = depth;
        resolved.apply_hierarchy = target.apply_hierarchy;
        resolved.apply_rigid_body = target.apply_rigid_body;
        /* Jolt-managed parenting owns the body while it lasts, and a state saved under it is the
         * transient parenting state. When that parenting stays, keep the live body state. */
        if (resolved.apply_rigid_body && object->GetParentPhysicsManaged() &&
            (!resolved.apply_hierarchy || object->GetParent() == target.desired_parent))
        {
          resolved.apply_rigid_body = false;
        }
        resolved.target_world_position = target.world_position;
        resolved.target_world_orientation = target.world_orientation;
        resolved.target_world_scale = target.world_scale;
        resolved.original_world_position = object->NodeGetWorldPosition();
        resolved.original_world_orientation = object->NodeGetWorldOrientation();
        resolved.original_world_scale = object->NodeGetWorldScaling();
        resolved.position_changed =
            !positions_match(resolved.original_world_position, target.world_position);
        resolved.orientation_changed =
            !orientations_match(resolved.original_world_orientation, target.world_orientation);
        resolved.scale_changed = !scales_match(resolved.original_world_scale, target.world_scale);
        transforms.push_back(std::move(resolved));
      }
      for (const PropertyState &saved_property : state.properties) {
        const KX_GameObject::SaveGamePropertyMetadata *metadata =
            object->GetSaveGamePropertyMetadata(saved_property.name);
        if (metadata == nullptr) {
          continue;
        }
        if (declared_property_type(*metadata) != saved_property.type ||
            metadata->timer != saved_property.timer)
        {
          release_prepared();
          r_error = "saved property contract no longer matches object '" + state.name + "'";
          return LN_GameStateResult::Incompatible;
        }
        EXP_Value *current = object->GetProperty(saved_property.name);
        if (current != nullptr &&
            !property_contract_matches(current, saved_property.type, saved_property.timer))
        {
          release_prepared();
          r_error = "runtime property contract no longer matches object '" + state.name + "'";
          return LN_GameStateResult::Incompatible;
        }
        if (!saved_property.exists) {
          if (current != nullptr) {
            PreparedProperty prepared;
            prepared.object = object;
            prepared.current = current;
            prepared.name = saved_property.name;
            prepared.timer = saved_property.timer;
            prepared.remove = true;
            properties.push_back(std::move(prepared));
          }
          continue;
        }
        EXP_Value *replacement = create_property_value(saved_property, resolve_saved_object);
        if (replacement == nullptr) {
          release_prepared();
          if (!missing_object.empty()) {
            r_error = "property '" + saved_property.name + "' of object '" + state.name +
                      "' refers to the object '" + missing_object +
                      "', which is unavailable in the current runtime scene";
            return LN_GameStateResult::Incompatible;
          }
          r_error = "could not prepare property '" + saved_property.name + "' for object '" +
                    state.name + "'";
          return LN_GameStateResult::InternalError;
        }
        PreparedProperty prepared;
        prepared.object = object;
        prepared.current = current;
        prepared.replacement = replacement;
        prepared.name = saved_property.name;
        prepared.timer = saved_property.timer;
        if (current == nullptr) {
          if (!EXP_Value::PersistencePreparePropertyNode(
                  prepared.name, replacement, prepared.insertion_node))
          {
            replacement->Release();
            release_prepared();
            r_error = "could not prepare property-map insertion for '" + saved_property.name +
                      "' on object '" + state.name + "'";
            return LN_GameStateResult::InternalError;
          }
        }
        else {
          replacement->AddRef();
          prepared.replacement_map_reference = true;
        }
        try {
          properties.push_back(std::move(prepared));
        }
        catch (...) {
          EXP_Value::PersistenceReleasePropertyNode(prepared.insertion_node);
          if (prepared.replacement_map_reference) {
            replacement->Release();
          }
          replacement->Release();
          throw;
        }
      }
    }
    std::stable_sort(transforms.begin(),
                     transforms.end(),
                     [](const ResolvedObjectState &a, const ResolvedObjectState &b) {
                       return a.depth < b.depth;
                     });

    size_t timers_to_attach = 0;
    bool needs_time_manager = false;
    for (const PreparedProperty &property : properties) {
      timers_to_attach += size_t(!property.remove && property.timer);
      needs_time_manager |= property.timer;
    }
    SCA_TimeEventManager *time_manager = scene.GetTimeEventManager();
    if ((needs_time_manager && time_manager == nullptr) ||
        (time_manager != nullptr &&
         !time_manager->PersistenceReserveTimeProperties(timers_to_attach)))
    {
      release_prepared();
      r_error = "could not reserve timer-property state for Load Game";
      return LN_GameStateResult::InternalError;
    }
    std::unordered_map<const EXP_Value *, size_t> time_property_counts;
    if (time_manager != nullptr) {
      const std::vector<EXP_Value *> &time_properties =
          time_manager->PersistenceTimeProperties();
      time_property_counts.reserve(time_properties.size());
      for (const EXP_Value *property : time_properties) {
        ++time_property_counts[property];
      }
    }
    for (const PreparedProperty &property : properties) {
      if (property.current == nullptr) {
        continue;
      }
      const auto membership = time_property_counts.find(property.current);
      const size_t registration_count = membership == time_property_counts.end() ?
                                            0u :
                                            membership->second;
      if (registration_count != size_t(property.timer)) {
        release_prepared();
        r_error = "runtime timer-manager contract no longer matches property '" + property.name +
                  "'";
        return LN_GameStateResult::Incompatible;
      }
    }

    /* Save Object State captures a closed hierarchy, so every desired parent has a saved final
     * world transform. Compute all local transforms without consulting the mutable current graph. */
    std::unordered_map<KX_GameObject *, PredictedWorldTransform> final_world;
    final_world.reserve(transforms.size());
    for (const ResolvedObjectState &resolved : transforms) {
      PredictedWorldTransform target;
      target.position = resolved.target_world_position;
      target.orientation = resolved.target_world_orientation;
      target.scale = resolved.target_world_scale;
      if (!final_world.emplace(resolved.object, target).second) {
        release_prepared();
        r_error = "Load Game transform plan contains a duplicate object";
        return LN_GameStateResult::InternalError;
      }
    }

    for (ResolvedObjectState &resolved : transforms) {
      if (resolved.apply_hierarchy &&
          (resolved.saved->hierarchy.compound || resolved.saved->hierarchy.ghost ||
           resolved.object->GetParentCompoundIntent() ||
           resolved.object->GetParentGhostIntent()))
      {
        release_prepared();
        r_error = "object '" + resolved.saved->name +
                  "' uses unsupported compound/ghost parenting";
        return LN_GameStateResult::UnsupportedContent;
      }
      PHY_IPhysicsController *controller = resolved.object->GetPhysicsController();
      if (resolved.apply_rigid_body) {
        if (resolved.saved->has_rigid_body_state) {
          if (controller == nullptr || !controller->SupportsGameStatePersistence() ||
              !controller->HasValidGameStateBody() || controller->IsSoftBody())
          {
            release_prepared();
            r_error = "saved Jolt rigid body for object '" + resolved.saved->name +
                      "' is incompatible with the current runtime object";
            return LN_GameStateResult::Incompatible;
          }
        }
        else if (controller != nullptr) {
          release_prepared();
          r_error = "saved non-physics object '" + resolved.saved->name +
                    "' has a physics controller in the current runtime scene";
          return LN_GameStateResult::Incompatible;
        }
      }
      if (resolved.desired_parent == nullptr) {
        resolved.prepared_local_position = resolved.target_world_position;
        resolved.prepared_local_orientation = resolved.target_world_orientation;
        resolved.prepared_local_scale = resolved.target_world_scale;
        continue;
      }
      const auto parent = final_world.find(resolved.desired_parent);
      if (parent == final_world.end()) {
        release_prepared();
        r_error = "saved parent for object '" + resolved.saved->name +
                  "' is not part of the complete Save Object State graph";
        return LN_GameStateResult::Incompatible;
      }
      const PredictedWorldTransform &parent_world = parent->second;
      if (!is_finite(parent_world.position) || !is_finite(parent_world.orientation) ||
          !is_finite(parent_world.scale) ||
          std::abs(parent_world.scale[0]) < std::numeric_limits<float>::epsilon() ||
          std::abs(parent_world.scale[1]) < std::numeric_limits<float>::epsilon() ||
          std::abs(parent_world.scale[2]) < std::numeric_limits<float>::epsilon())
      {
        release_prepared();
        r_error = "cannot restore object '" + resolved.saved->name +
                  "' below a singular saved parent transform";
        return LN_GameStateResult::Incompatible;
      }
      const MT_Matrix3x3 parent_basis = parent_world.orientation.scaled(
          parent_world.scale[0], parent_world.scale[1], parent_world.scale[2]);
      const float parent_determinant = parent_basis.determinant();
      if (!std::isfinite(parent_determinant) ||
          std::abs(parent_determinant) < std::numeric_limits<float>::epsilon())
      {
        release_prepared();
        r_error = "cannot restore object '" + resolved.saved->name +
                  "' below a singular saved parent basis";
        return LN_GameStateResult::Incompatible;
      }
      const MT_Matrix3x3 inverse_parent_basis = parent_basis.inverse();
      const MT_Matrix3x3 target_basis = resolved.target_world_orientation.scaled(
          resolved.target_world_scale[0],
          resolved.target_world_scale[1],
          resolved.target_world_scale[2]);
      resolved.prepared_local_position =
          inverse_parent_basis * (resolved.target_world_position - parent_world.position);
      const MT_Matrix3x3 local_basis = inverse_parent_basis * target_basis;
      PredictedWorldTransform verified;
      if (!is_finite(resolved.prepared_local_position) ||
          !decompose_representable_local_basis(local_basis,
                                               resolved.prepared_local_orientation,
                                               resolved.prepared_local_scale) ||
          !predict_normal_world_transform(parent_world,
                                          resolved.prepared_local_position,
                                          resolved.prepared_local_orientation,
                                          resolved.prepared_local_scale,
                                          verified) ||
          !positions_match(verified.position, resolved.target_world_position) ||
          !orientations_match(verified.orientation, resolved.target_world_orientation) ||
          !scales_match(verified.scale, resolved.target_world_scale))
      {
        release_prepared();
        r_error = "world transform for object '" + resolved.saved->name +
                  "' is not representable below its saved parent";
        return LN_GameStateResult::Incompatible;
      }
    }

    /* Set Collection Member's changes replace the object's own (none saved: none kept). Every
     * saved collection must still be in the scene. */
    struct PreparedMemberships {
      KX_GameObject *object = nullptr;
      std::vector<std::pair<const blender::Collection *, bool>> memberships;
    };
    std::vector<PreparedMemberships> memberships;
    memberships.reserve(transforms.size());
    for (const ResolvedObjectState &resolved : transforms) {
      if (!resolved.apply_hierarchy) {
        continue;
      }
      PreparedMemberships prepared;
      prepared.object = resolved.object;
      prepared.memberships.reserve(resolved.saved->collection_memberships.size());
      for (const CollectionMembershipState &saved : resolved.saved->collection_memberships) {
        const blender::Collection *collection = scene.FindSceneCollection(
            saved.collection_name, saved.library_path);
        if (collection == nullptr) {
          release_prepared();
          r_error = "collection '" + saved.collection_name + "' of saved object '" +
                    resolved.saved->name + "' is missing in the current game";
          return LN_GameStateResult::Incompatible;
        }
        prepared.memberships.emplace_back(collection, saved.member);
      }
      memberships.push_back(std::move(prepared));
    }

    /* Every shape allocation happens before the first SG/Jolt mutation. */
    for (const ResolvedObjectState &resolved : transforms) {
      PHY_IPhysicsController *controller = resolved.object->GetPhysicsController();
      if (controller == nullptr || controller->IsSoftBody() || !resolved.scale_changed) {
        continue;
      }
      if (!controller->PrepareGameStateScaling(resolved.target_world_scale,
                                               resolved.original_world_scale))
      {
        for (const ResolvedObjectState &prepared : transforms) {
          if (PHY_IPhysicsController *prepared_controller =
                  prepared.object->GetPhysicsController())
          {
            prepared_controller->ClearPreparedGameStateScaling();
          }
        }
        release_prepared();
        r_error = "could not prepare Jolt scale for object '" + resolved.saved->name + "'";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
    for (const ResolvedObjectState &resolved : transforms) {
      if (!resolved.apply_rigid_body || !resolved.saved->has_rigid_body_state) {
        continue;
      }
      PHY_IPhysicsController *controller = resolved.object->GetPhysicsController();
      PHY_GameStateRigidBodyState physics_state;
      if (controller == nullptr ||
          !physics_rigid_body_state(resolved.saved->rigid_body, physics_state) ||
          !controller->PrepareGameStateRigidBody(physics_state))
      {
        clear_prepared_rigid_bodies();
        clear_prepared_shapes();
        release_prepared();
        r_error = "could not prepare saved Jolt rigid-body state for object '" +
                  resolved.saved->name + "'";
        return LN_GameStateResult::Incompatible;
      }
    }

    /* Point of no return: all allocations and expected failure checks are complete. Properties
     * are installed first so reconstructed OnInit can never observe a transformed partial state. */
    BLI_assert(commit_prelude != nullptr);
    commit_prelude(commit_data);
    for (const ResolvedObjectState &resolved : transforms) {
      if (!resolved.apply_hierarchy) {
        continue;
      }
      const bool restored = resolved.object->RestoreSaveObjectStateParent(
          resolved.desired_parent);
      BLI_assert(restored);
      (void)restored;
    }
    for (PreparedMemberships &prepared : memberships) {
      scene.RestoreCollectionMemberships(prepared.object, std::move(prepared.memberships));
    }
    for (PreparedProperty &property : properties) {
      if (property.remove) {
        if (property.timer) {
          const bool detached = time_manager->PersistenceDetachTimeProperty(property.current);
          BLI_assert(detached);
          (void)detached;
        }
        EXP_Value::PersistencePropertyNode removed =
            property.object->PersistenceExtractProperty(property.name);
        BLI_assert(!removed.empty() && removed.mapped() == property.current);
        EXP_Value::PersistenceReleasePropertyNode(removed);
        continue;
      }
      if (property.current != nullptr) {
        if (property.timer) {
          const bool detached = time_manager->PersistenceDetachTimeProperty(property.current);
          BLI_assert(detached);
          (void)detached;
        }
        EXP_Value *previous =
            property.object->PersistenceReplaceProperty(property.name, property.replacement);
        BLI_assert(previous == property.current);
        property.replacement_map_reference = false;
        if (property.timer) {
          const bool attached =
              time_manager->PersistenceAttachPreparedTimeProperty(property.replacement);
          BLI_assert(attached);
          (void)attached;
        }
        previous->Release();
      }
      else {
        const bool inserted =
            property.object->PersistenceInsertPreparedProperty(property.insertion_node);
        BLI_assert(inserted);
        (void)inserted;
        if (property.timer) {
          const bool attached =
              time_manager->PersistenceAttachPreparedTimeProperty(property.replacement);
          BLI_assert(attached);
          (void)attached;
        }
      }
    }
    for (const ResolvedObjectState &resolved : transforms) {
      apply_world_transform(*resolved.object, resolved);
    }
    /* Restore semantic Jolt state last: transforms may wake bodies and parenting may suspend
     * dynamics, so awake/suspended state and pending impulses become authoritative only here. */
    for (const ResolvedObjectState &resolved : transforms) {
      if (!resolved.apply_rigid_body || !resolved.saved->has_rigid_body_state) {
        continue;
      }
      PHY_IPhysicsController *controller = resolved.object->GetPhysicsController();
      BLI_assert(controller != nullptr);
      controller->CommitPreparedGameStateRigidBody();
    }
  }
  catch (...) {
    clear_prepared_rigid_bodies();
    clear_prepared_shapes();
    release_prepared();
    throw;
  }
  release_prepared();
  return LN_GameStateResult::Success;
}

}  // namespace

class LN_GameStateLoadPayload::Impl {
 public:
  explicit Impl(upbge::logic_nodes::game_state::Checkpoint checkpoint)
      : checkpoint(std::move(checkpoint))
  {
  }

  upbge::logic_nodes::game_state::Checkpoint checkpoint;
};

class LN_GameStateService::Impl {
 public:
  struct UuidHash {
    size_t operator()(const blender::UUID &uuid) const
    {
      return size_t(uuid.hash());
    }
  };

  struct AuthoredEntry {
    blender::UUID uuid;
    KX_GameObject *live_object = nullptr;
    std::string name;
    LN_PersistenceCandidateDescriptor descriptor;
    MT_Vector3 supported_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
    size_t material_slot_count = 0;
    bool save_object_state = false;
    BL_GameStateReconstructionRecipe reconstruction_recipe;
    ModifierBaseline modifier_baseline;
    bool reconstruction_recipe_valid = false;
  };

  struct SourceEntry {
    blender::UUID uuid;
    KX_GameObject *source = nullptr;
    LN_PersistenceCandidateDescriptor descriptor;
    MT_Vector3 supported_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
    size_t material_slot_count = 0;
    ModifierBaseline modifier_baseline;
    LN_GameStateResult modifier_baseline_result = LN_GameStateResult::Success;
    std::string modifier_baseline_error;
  };

  struct SpawnEntry {
    KX_GameObject *root = nullptr;
    KX_GameObject *source = nullptr;
    std::vector<KX_GameObject *> members;
    blender::UUID source_uuid;
    blender::UUID instance_uuid;
    MT_Vector3 creation_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Matrix3x3 creation_orientation = MT_Matrix3x3::Identity();
    MT_Vector3 creation_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
    float life_seconds = 0.0f;
    bool persistent = false;
  };

  LN_GameStateResult InitializeRegistry(KX_Scene &scene, std::string &r_error)
  {
    r_error.clear();
    blender::Scene *blender_scene = scene.GetBlenderScene();
    if (blender_scene == nullptr) {
      r_error = "runtime scene has no Blender scene identity";
      return LN_GameStateResult::InvalidRequest;
    }
    blender::bUUID scene_uuid;
    read_scene_identity(blender_scene, scene_uuid);

    blender::Main *bmain = current_main();
    if (bmain == nullptr) {
      r_error = "runtime Main database is unavailable for material persistence";
      return LN_GameStateResult::InternalError;
    }
    MaterialAssetCatalog material_assets;
    const LN_GameStateResult material_catalog_result =
        build_material_asset_catalog(*bmain, material_assets, r_error);
    if (!LN_GameStateSucceeded(material_catalog_result)) {
      return material_catalog_result;
    }
    GeometryNodesAssetCatalog geometry_nodes_assets;
    const LN_GameStateResult geometry_nodes_catalog_result =
        build_geometry_nodes_asset_catalog(*bmain, geometry_nodes_assets, r_error);
    if (!LN_GameStateSucceeded(geometry_nodes_catalog_result)) {
      return geometry_nodes_catalog_result;
    }

    decltype(m_authored) authored;
    decltype(m_authoredByObject) authored_by_object;
    decltype(m_sourcesByObject) sources_by_object;
    decltype(m_sourcesByUuid) sources_by_uuid;
    decltype(m_duplicateIdentityObjects) duplicate_identity_objects;
    decltype(m_authoredAndSourceUuids) authored_and_source_uuids;
    std::unordered_map<blender::UUID, KX_GameObject *, UuidHash> addressable_owner;
    std::unordered_set<blender::UUID, UuidHash> duplicate_uuids;
    std::string identity_error;

    EXP_ListValue<KX_GameObject> *active = scene.GetObjectList();
    EXP_ListValue<KX_GameObject> *inactive = scene.GetInactiveList();
    if (active == nullptr || inactive == nullptr) {
      r_error = "runtime scene object catalogs are unavailable";
      return LN_GameStateResult::InternalError;
    }
    try {
      authored.reserve(size_t(active->GetCount()));
      authored_by_object.reserve(size_t(active->GetCount()));
      sources_by_object.reserve(size_t(inactive->GetCount()));
      sources_by_uuid.reserve(size_t(inactive->GetCount()));
      duplicate_identity_objects.reserve(size_t(active->GetCount() + inactive->GetCount()));
      addressable_owner.reserve(size_t(active->GetCount() + inactive->GetCount()));
      duplicate_uuids.reserve(size_t(active->GetCount() + inactive->GetCount()));
      for (KX_GameObject *object : *active) {
        /* A Shared placed object (Game Drawing) is saved like any placed object. */
        if (object == nullptr || object->IsReplica() ||
            (object->IsUpbgeDupliInstance() && !object->IsShared()) ||
            (!object->GetSaveGameObjectState() && !object->HasSaveGameProperties()))
        {
          continue;
        }
        const blender::UUID uuid(object->GetPersistenceSourceUuid());
        if (blender::BLI_uuid_is_nil(uuid)) {
          if (identity_error.empty()) {
            identity_error = "active checkpoint participant '" + object->GetName() +
                             "' has no durable persistence UUID";
          }
          continue;
        }
        const auto owner = addressable_owner.emplace(uuid, object);
        if (!owner.second) {
          duplicate_identity_objects.insert(owner.first->second);
          duplicate_identity_objects.insert(object);
          duplicate_uuids.insert(uuid);
          if (identity_error.empty()) {
            identity_error = "checkpoint identity " + uuid_string(uuid) +
                             " is duplicated by active/source objects";
          }
        }
        AuthoredEntry entry;
        entry.uuid = uuid;
        entry.live_object = object;
        entry.name = object->GetName();
        entry.descriptor = build_candidate_descriptor(scene, *object);
        entry.save_object_state = object->GetSaveGameObjectState();
        if (object->GetSGNode() != nullptr) {
          entry.supported_scale = object->NodeGetWorldScaling();
        }
        if (object->GetBlenderObject() != nullptr) {
          entry.material_slot_count = size_t(object->GetBlenderObject()->totcol);
          if (entry.save_object_state) {
            const LN_GameStateResult baseline_result = capture_modifier_baseline(
                *object->GetBlenderObject(),
                geometry_nodes_assets,
                entry.modifier_baseline,
                r_error);
            if (!LN_GameStateSucceeded(baseline_result)) {
              r_error = "could not capture the authored modifier baseline for '" +
                        entry.name + "': " + r_error;
              return baseline_result;
            }
          }
        }
        if (entry.save_object_state &&
            LN_ValidatePersistenceCandidate(
                entry.descriptor, {true, true, true, true}) ==
                LN_PersistenceCapabilityIssue::None)
        {
          const BL_GameStateReconstructionResult recipe_result =
              BL_CaptureGameStateReconstructionRecipe(
                  scene, *object, entry.reconstruction_recipe, r_error);
          if (recipe_result != BL_GameStateReconstructionResult::Success) {
            r_error = "validated persistence candidate '" + entry.name +
                      "' could not produce its reconstruction recipe: " + r_error;
            return LN_GameStateResult::InternalError;
          }
          entry.reconstruction_recipe_valid = true;
        }
        if (authored.emplace(uuid, std::move(entry)).second) {
          authored_by_object.emplace(object, uuid);
        }
      }
      for (KX_GameObject *source : *inactive) {
        if (source == nullptr) {
          r_error = "inactive source catalog contains a null object";
          return LN_GameStateResult::InternalError;
        }
        SourceEntry source_entry;
        source_entry.uuid = blender::UUID(source->GetPersistenceSourceUuid());
        source_entry.source = source;
        /* Converted inactive templates intentionally have layer zero. Normal AddReplicaObject
         * publishes a replica on its active reference/scene layer, so zero is valid only for this
         * catalog role and must not weaken authored or live-instance validation. */
        source_entry.descriptor = build_candidate_descriptor(scene, *source, true);
        if (source->GetSGNode() != nullptr) {
          source_entry.supported_scale = source->NodeGetWorldScaling();
        }
        if (source->GetBlenderObject() != nullptr) {
          source_entry.material_slot_count = size_t(source->GetBlenderObject()->totcol);
          source_entry.modifier_baseline_result = capture_modifier_baseline(
              *source->GetBlenderObject(),
              geometry_nodes_assets,
              source_entry.modifier_baseline,
              source_entry.modifier_baseline_error);
          if (source_entry.modifier_baseline_result == LN_GameStateResult::InternalError) {
            r_error = "could not capture the authored modifier baseline for source '" +
                      source->GetName() + "': " +
                      source_entry.modifier_baseline_error;
            return source_entry.modifier_baseline_result;
          }
        }
        sources_by_object.emplace(source, std::move(source_entry));
        if (blender::BLI_uuid_is_nil(source->GetPersistenceSourceUuid())) {
          continue;
        }
        const blender::UUID uuid(source->GetPersistenceSourceUuid());
        const auto owner = addressable_owner.emplace(uuid, source);
        if (!owner.second) {
          duplicate_identity_objects.insert(owner.first->second);
          duplicate_identity_objects.insert(source);
          duplicate_uuids.insert(uuid);
          if (identity_error.empty()) {
            identity_error = "checkpoint identity " + uuid_string(uuid) +
                             " is duplicated by active/source objects";
          }
        }
        sources_by_uuid.emplace(uuid, source);
      }
      for (const blender::UUID &uuid : duplicate_uuids) {
        sources_by_uuid.erase(uuid);
      }
      authored_and_source_uuids.reserve(addressable_owner.size());
      for (const auto &item : addressable_owner) {
        authored_and_source_uuids.insert(item.first);
      }
    }
    catch (const std::exception &exception) {
      r_error = std::string("could not allocate checkpoint registry: ") + exception.what();
      return LN_GameStateResult::InternalError;
    }

    m_scene = &scene;
    m_sceneUuid = blender::UUID(scene_uuid);
    m_authored = std::move(authored);
    m_authoredByObject = std::move(authored_by_object);
    m_sourcesByObject = std::move(sources_by_object);
    m_sourcesByUuid = std::move(sources_by_uuid);
    m_duplicateIdentityObjects = std::move(duplicate_identity_objects);
    m_authoredAndSourceUuids = std::move(authored_and_source_uuids);
    m_materialAssets = std::move(material_assets);
    m_geometryNodesAssets = std::move(geometry_nodes_assets);
    m_identityCatalogValid = identity_error.empty() && !blender::BLI_uuid_is_nil(m_sceneUuid);
    m_identityCatalogError = identity_error.empty() && blender::BLI_uuid_is_nil(m_sceneUuid) ?
                                 "checkpoint Scene has no durable persistence UUID" :
                                 std::move(identity_error);
    m_registryValid = true;
    return LN_GameStateResult::Success;
  }

  LN_GameStateResult PreflightAddObject(KX_Scene &scene,
                                        KX_GameObject &source,
                                        const float life_seconds,
                                        const bool full_copy,
                                        std::string &r_error) const
  {
    if (m_reconciling) {
      r_error = "Add Object persistence is blocked during Load Game reconciliation";
      return LN_GameStateResult::Busy;
    }
    if (!m_registryValid || m_scene != &scene) {
      r_error = "checkpoint registry is not initialized for this scene";
      return LN_GameStateResult::InvalidRequest;
    }
    const auto source_found = m_sourcesByObject.find(&source);
    if (source_found == m_sourcesByObject.end()) {
      r_error = "Add Object source is not in the converted inactive source catalog";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (!source.GetSaveGameObjectState()) {
      return LN_GameStateResult::Success;
    }
    return ValidatePersistentSource(scene, source, life_seconds, full_copy, r_error);
  }

  LN_GameStateResult ValidatePersistentSource(KX_Scene &scene,
                                              KX_GameObject &source,
                                              const float life_seconds,
                                              const bool full_copy,
                                              std::string &r_error) const
  {
    if (!m_registryValid || m_scene != &scene) {
      r_error = "checkpoint registry is not initialized for this scene";
      return LN_GameStateResult::InvalidRequest;
    }
    const auto source_found = m_sourcesByObject.find(&source);
    if (source_found == m_sourcesByObject.end()) {
      r_error = "persistent Add Object source is not in the inactive source catalog";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (full_copy) {
      r_error = "persistent Add Object does not support Full Copy";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (!std::isfinite(life_seconds) || life_seconds != 0.0f) {
      r_error = "persistent Add Object requires finite Life exactly equal to zero";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (m_duplicateIdentityObjects.find(&source) != m_duplicateIdentityObjects.end()) {
      r_error = "persistent Add Object source has a duplicate checkpoint UUID";
      return LN_GameStateResult::InvalidRequest;
    }
    if (!LN_GameStateSucceeded(source_found->second.modifier_baseline_result)) {
      r_error = "persistent Add Object source '" + source.GetName() +
                "' has unsupported authored modifier state: " +
                source_found->second.modifier_baseline_error;
      return source_found->second.modifier_baseline_result;
    }
    return validate_candidate(
        source_found->second.descriptor, source.GetName(), r_error, false);
  }

  LN_GameStateResult ValidateAuthoredExistenceRuntime(
      KX_Scene &scene, const AuthoredEntry &entry, std::string &r_error) const
  {
    KX_GameObject *object = entry.live_object;
    if (object == nullptr) {
      return LN_GameStateResult::Success;
    }
    SG_Node *node = object->GetSGNode();
    if (object->GetScene() != &scene || node == nullptr ||
        blender::UUID(object->GetPersistenceSourceUuid()) != entry.uuid)
    {
      r_error = "authored existence participant '" + entry.name +
                "' moved outside its registered runtime scene or identity";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (!object->m_rigidbodyConstraints.empty()) {
      r_error = "Save Object State participant '" + entry.name +
                "' gained an unsupported runtime constraint";
      return LN_GameStateResult::UnsupportedContent;
    }

    PHY_IPhysicsController *controller = object->GetPhysicsController();
    const LN_PersistencePhysicsKind expected = entry.descriptor.physics_kind;
    if (expected == LN_PersistencePhysicsKind::None) {
      if (controller != nullptr) {
        r_error = "authored existence participant '" + entry.name +
                  "' unexpectedly gained a physics controller";
        return LN_GameStateResult::UnsupportedContent;
      }
      return LN_GameStateResult::Success;
    }
    if (controller == nullptr || !controller->SupportsGameStatePersistence() ||
        !controller->HasValidGameStateBody())
    {
      r_error = "authored existence participant '" + entry.name +
                "' no longer has its expected valid Jolt body";
      return LN_GameStateResult::UnsupportedContent;
    }
    const LN_PersistencePhysicsKind actual = controller->IsSoftBody() ?
                                                 LN_PersistencePhysicsKind::JoltSoft :
                                                 LN_PersistencePhysicsKind::JoltRigid;
    if (actual != expected) {
      r_error = "authored existence participant '" + entry.name +
                "' changed its Jolt body kind";
      return LN_GameStateResult::UnsupportedContent;
    }
    return LN_GameStateResult::Success;
  }

  LN_GameStateResult ValidateSpawnRuntime(KX_Scene &scene,
                                          const SpawnEntry &entry,
                                          std::string &r_error) const
  {
    if (entry.root == nullptr || entry.source == nullptr || entry.root->GetScene() != &scene ||
        entry.members.size() != 1 || entry.members.front() != entry.root)
    {
      r_error = "persistent Add Object must own one independent root in the calling scene";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (entry.root->GetSGNode() == nullptr || !entry.root->m_rigidbodyConstraints.empty()) {
      r_error = "persistent Add Object root gained an unsupported runtime constraint";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (!is_finite(entry.root->NodeGetWorldPosition()) ||
        !is_valid_orientation(entry.root->NodeGetWorldOrientation()) ||
        !is_finite(entry.root->NodeGetWorldScaling()))
    {
      r_error = "persistent Add Object root has an invalid runtime transform";
      return LN_GameStateResult::UnsupportedContent;
    }
    const auto source_found = m_sourcesByObject.find(entry.source);
    if (source_found == m_sourcesByObject.end()) {
      r_error = "persistent Add Object source left the inactive source catalog";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (entry.root->GetSaveGameObjectState() &&
        (entry.root->GetBlenderObject() == nullptr ||
         entry.root->GetBlenderObject()->totcol < 0 ||
         size_t(entry.root->GetBlenderObject()->totcol) !=
             source_found->second.material_slot_count))
    {
      r_error = "persistent Add Object material-slot topology differs from its source";
      return LN_GameStateResult::UnsupportedContent;
    }

    PHY_IPhysicsController *controller = entry.root->GetPhysicsController();
    const LN_PersistencePhysicsKind expected = source_found->second.descriptor.physics_kind;
    if (expected == LN_PersistencePhysicsKind::None) {
      if (controller != nullptr) {
        r_error = "persistent replica unexpectedly created a physics controller";
        return LN_GameStateResult::UnsupportedContent;
      }
      return LN_GameStateResult::Success;
    }
    if (controller == nullptr || !controller->SupportsGameStatePersistence() ||
        !controller->HasValidGameStateBody())
    {
      r_error = "persistent replica did not create its expected valid Jolt body";
      return LN_GameStateResult::UnsupportedContent;
    }
    const LN_PersistencePhysicsKind actual = controller->IsSoftBody() ?
                                                 LN_PersistencePhysicsKind::JoltSoft :
                                                 LN_PersistencePhysicsKind::JoltRigid;
    if (actual != expected) {
      r_error = "persistent replica Jolt body kind differs from its converted source recipe";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (actual == LN_PersistencePhysicsKind::JoltSoft &&
        (!scales_match(entry.creation_scale, source_found->second.supported_scale) ||
         !scales_match(entry.root->NodeGetWorldScaling(), source_found->second.supported_scale)))
    {
      r_error = "persistent soft-body creation/current scale differs from its source-supported "
                "scale";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (actual == LN_PersistencePhysicsKind::JoltRigid) {
      const MT_Vector3 current_scale = entry.root->NodeGetWorldScaling();
      auto can_prepare_scale = [controller, &current_scale](const MT_Vector3 &scale) {
        const bool supported = controller->PrepareGameStateScaling(scale, current_scale);
        controller->ClearPreparedGameStateScaling();
        return supported;
      };
      if (!can_prepare_scale(entry.creation_scale) ||
          (entry.root->GetSaveGameObjectState() &&
           !scales_match(entry.creation_scale, entry.root->NodeGetWorldScaling()) &&
           !can_prepare_scale(entry.root->NodeGetWorldScaling())))
      {
        r_error = "persistent Jolt rigid body cannot reproduce its creation/current scale";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
    return LN_GameStateResult::Success;
  }

  LN_GameStateResult ValidateRemovalTopology(
      KX_Scene &scene,
      KX_GameObject *registered_root_key,
      const SpawnEntry &entry,
      std::vector<KX_GameObject *> &r_removal_candidates,
      std::string &r_error) const
  {
    r_removal_candidates.clear();
    if (registered_root_key == nullptr || entry.members.empty()) {
      r_error = "tracked Add Object member ownership is incomplete";
      return LN_GameStateResult::UnsupportedContent;
    }
    try {
      std::unordered_set<KX_GameObject *> members;
      members.reserve(entry.members.size());
      r_removal_candidates.reserve(entry.members.size());

      for (KX_GameObject *member : entry.members) {
        if (member == nullptr || member->GetSGNode() == nullptr || member->GetScene() != &scene ||
            !members.insert(member).second)
        {
          r_error = "tracked Add Object member set contains a stale, duplicate, or foreign object";
          return LN_GameStateResult::UnsupportedContent;
        }
        const auto owner = m_spawnRootByMember.find(member);
        if (owner == m_spawnRootByMember.end() || owner->second != registered_root_key) {
          r_error = "tracked Add Object member escaped its registry ownership";
          return LN_GameStateResult::UnsupportedContent;
        }
        r_removal_candidates.push_back(member);
      }
    }
    catch (const std::exception &exception) {
      r_error = std::string("could not validate tracked Add Object removal ownership: ") +
                exception.what();
      return LN_GameStateResult::InternalError;
    }
    catch (...) {
      r_error = "could not validate tracked Add Object removal ownership";
      return LN_GameStateResult::InternalError;
    }
    return LN_GameStateResult::Success;
  }

  bool GenerateInstanceUuid(blender::UUID &r_uuid) const
  {
    constexpr int max_attempts = 32;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
      const blender::UUID candidate(blender::BLI_uuid_generate_random());
      if (!blender::BLI_uuid_is_nil(candidate) &&
          m_usedInstanceUuids.find(candidate) == m_usedInstanceUuids.end() &&
          m_authoredAndSourceUuids.find(candidate) == m_authoredAndSourceUuids.end())
      {
        r_uuid = candidate;
        return true;
      }
    }
    return false;
  }

  LN_GameStateResult RegisterAddedObject(KX_Scene &scene,
                                         KX_GameObject &source,
                                         KX_GameObject &root,
                                         const std::vector<KX_GameObject *> &members,
                                         const float life_seconds,
                                         const bool full_copy,
                                         std::string &r_error)
  {
    const LN_GameStateResult preflight =
        PreflightAddObject(scene, source, life_seconds, full_copy, r_error);
    if (!LN_GameStateSucceeded(preflight)) {
      return preflight;
    }
    if (full_copy) {
      return LN_GameStateResult::Success;
    }
    if (members.empty() || std::find(members.begin(), members.end(), &root) == members.end()) {
      r_error = "Add Object did not report its complete created member set";
      return LN_GameStateResult::InternalError;
    }
    if (source.GetSaveGameObjectState() && members.size() != 1) {
      r_error = "persistent Add Object hierarchy reconstruction is not supported in this release";
      return LN_GameStateResult::UnsupportedContent;
    }

    SpawnEntry entry;
    entry.root = &root;
    entry.source = &source;
    entry.members = members;
    entry.source_uuid = blender::UUID(source.GetPersistenceSourceUuid());
    entry.creation_position = root.NodeGetWorldPosition();
    entry.creation_orientation = root.NodeGetWorldOrientation();
    entry.creation_scale = root.NodeGetWorldScaling();
    entry.life_seconds = life_seconds;
    entry.persistent = source.GetSaveGameObjectState();
    if (!is_finite(entry.creation_position) || !is_valid_orientation(entry.creation_orientation) ||
        !is_finite(entry.creation_scale))
    {
      r_error = "Add Object produced an invalid creation transform";
      return LN_GameStateResult::InternalError;
    }
    if (entry.persistent) {
      const LN_GameStateResult runtime_validation = ValidateSpawnRuntime(scene, entry, r_error);
      if (!LN_GameStateSucceeded(runtime_validation)) {
        return runtime_validation;
      }
    }
    if (entry.persistent) {
      if (!GenerateInstanceUuid(entry.instance_uuid)) {
        r_error = "could not generate a unique persistent instance UUID";
        return LN_GameStateResult::InternalError;
      }
    }
    if (m_spawns.find(&root) != m_spawns.end()) {
      r_error = "Add Object root is already registered";
      return LN_GameStateResult::InternalError;
    }
    for (size_t index = 0; index < members.size(); index++) {
      if (members[index] == nullptr || members[index]->GetScene() != &scene ||
          members[index]->GetSGNode() == nullptr ||
          m_spawnRootByMember.find(members[index]) != m_spawnRootByMember.end())
      {
        r_error = "Add Object member is already owned by another tracked root";
        return LN_GameStateResult::InternalError;
      }
      if (std::find(members.begin(), members.begin() + index, members[index]) !=
          members.begin() + index)
      {
        r_error = "Add Object reported a duplicate member";
        return LN_GameStateResult::InternalError;
      }
    }

    bool uuid_registered = false;
    bool uuid_indexed = false;
    bool spawn_registered = false;
    auto rollback = [&]() {
      for (KX_GameObject *member : members) {
        const auto found = m_spawnRootByMember.find(member);
        if (found != m_spawnRootByMember.end() && found->second == &root) {
          m_spawnRootByMember.erase(found);
        }
      }
      if (spawn_registered) {
        m_spawns.erase(&root);
      }
      if (uuid_indexed) {
        m_spawnByInstanceUuid.erase(entry.instance_uuid);
      }
      if (uuid_registered) {
        m_usedInstanceUuids.erase(entry.instance_uuid);
      }
    };
    try {
      if (entry.persistent) {
        uuid_registered = m_usedInstanceUuids.insert(entry.instance_uuid).second;
        if (!uuid_registered) {
          r_error = "persistent instance UUID collided during registry commit";
          return LN_GameStateResult::InternalError;
        }
      }
      spawn_registered = m_spawns.emplace(&root, entry).second;
      if (!spawn_registered) {
        rollback();
        r_error = "Add Object root collided during registry commit";
        return LN_GameStateResult::InternalError;
      }
      if (entry.persistent) {
        uuid_indexed = m_spawnByInstanceUuid.emplace(entry.instance_uuid, &root).second;
        if (!uuid_indexed) {
          rollback();
          r_error = "persistent instance UUID collided in the live-instance index";
          return LN_GameStateResult::InternalError;
        }
      }
      for (KX_GameObject *member : members) {
        const bool inserted = m_spawnRootByMember.emplace(member, &root).second;
        if (!inserted) {
          rollback();
          r_error = "Add Object member ownership collided during registry commit";
          return LN_GameStateResult::InternalError;
        }
      }
    }
    catch (...) {
      rollback();
      r_error = "could not allocate Add Object checkpoint metadata";
      return LN_GameStateResult::InternalError;
    }
    root.SetPersistenceSourceUuid(source.GetPersistenceSourceUuid());
    root.SetSaveGameObjectState(entry.persistent);
    if (entry.persistent) {
      root.SetPersistenceInstanceUuid(entry.instance_uuid);
    }
    else {
      root.ClearPersistenceInstanceUuid();
    }
    return LN_GameStateResult::Success;
  }

  LN_GameStateResult SetSaveExistence(KX_Scene &scene,
                                      KX_GameObject *root,
                                      const bool enabled,
                                      std::string &r_error)
  {
    if (m_reconciling) {
      r_error = "Set Save Object State is blocked during Load Game reconciliation";
      return LN_GameStateResult::Busy;
    }
    if (!m_registryValid || m_scene != &scene || root == nullptr) {
      r_error = "target is not a live tracked normal Add Object root in this scene";
      return LN_GameStateResult::InvalidRequest;
    }
    const auto member = m_spawnRootByMember.find(root);
    if (member == m_spawnRootByMember.end() || member->second != root) {
      r_error = member == m_spawnRootByMember.end() ?
                    "target is outside the tracked normal Add Object path" :
                    "target is a tracked member, not its Add Object root";
      return LN_GameStateResult::InvalidRequest;
    }
    auto found = m_spawns.find(root);
    if (found == m_spawns.end()) {
      r_error = "tracked Add Object root is stale";
      return LN_GameStateResult::InvalidRequest;
    }
    SpawnEntry &entry = found->second;
    if (entry.root != root || entry.source == nullptr || root->GetScene() != &scene ||
        root->GetSGNode() == nullptr ||
        blender::UUID(root->GetPersistenceSourceUuid()) != entry.source_uuid ||
        blender::UUID(root->GetPersistenceInstanceUuid()) != entry.instance_uuid ||
        root->GetSaveGameObjectState() != entry.persistent)
    {
      r_error = "tracked Add Object root registry metadata is incomplete or stale";
      return LN_GameStateResult::InvalidRequest;
    }
    if (entry.persistent == enabled) {
      return LN_GameStateResult::Success;
    }
    if (enabled) {
      const LN_GameStateResult validation =
          ValidatePersistentSource(scene, *entry.source, entry.life_seconds, false, r_error);
      if (!LN_GameStateSucceeded(validation)) {
        return validation;
      }
      const LN_GameStateResult runtime_validation = ValidateSpawnRuntime(scene, entry, r_error);
      if (!LN_GameStateSucceeded(runtime_validation)) {
        return runtime_validation;
      }
      blender::UUID instance_uuid = entry.instance_uuid;
      if (blender::BLI_uuid_is_nil(instance_uuid)) {
        if (!GenerateInstanceUuid(instance_uuid)) {
          r_error = "could not generate a unique persistent instance UUID";
          return LN_GameStateResult::InternalError;
        }
        bool uuid_reserved = false;
        try {
          uuid_reserved = m_usedInstanceUuids.insert(instance_uuid).second;
          if (!uuid_reserved) {
            r_error = "persistent instance UUID collided during promotion";
            return LN_GameStateResult::InternalError;
          }
          if (!m_spawnByInstanceUuid.emplace(instance_uuid, root).second) {
            m_usedInstanceUuids.erase(instance_uuid);
            r_error = "persistent instance UUID collided during promotion indexing";
            return LN_GameStateResult::InternalError;
          }
        }
        catch (...) {
          if (uuid_reserved) {
            m_usedInstanceUuids.erase(instance_uuid);
          }
          r_error = "could not allocate a persistent instance UUID";
          return LN_GameStateResult::InternalError;
        }
        entry.instance_uuid = instance_uuid;
      }
      else if (m_usedInstanceUuids.find(instance_uuid) == m_usedInstanceUuids.end()) {
        r_error = "tracked demoted instance UUID is no longer reserved";
        return LN_GameStateResult::InternalError;
      }
      else {
        const auto indexed = m_spawnByInstanceUuid.find(instance_uuid);
        if (indexed == m_spawnByInstanceUuid.end() || indexed->second != root) {
          r_error = "tracked demoted instance UUID index is inconsistent";
          return LN_GameStateResult::InternalError;
        }
      }
      entry.persistent = true;
      root->SetPersistenceInstanceUuid(entry.instance_uuid);
      root->SetSaveGameObjectState(true);
    }
    else {
      entry.persistent = false;
      root->SetSaveGameObjectState(false);
    }
    return LN_GameStateResult::Success;
  }

  void NotifyObjectRemoved(KX_GameObject &object, const KX_ObjectRemovalReason reason)
  {
    (void)reason;
    if (const auto authored = m_authoredByObject.find(&object);
        authored != m_authoredByObject.end())
    {
      const auto entry = m_authored.find(authored->second);
      if (entry != m_authored.end()) {
        entry->second.live_object = nullptr;
      }
      m_authoredByObject.erase(authored);
    }
    const auto member = m_spawnRootByMember.find(&object);
    if (member == m_spawnRootByMember.end()) {
      return;
    }
    KX_GameObject *root = member->second;
    m_spawnRootByMember.erase(member);
    const auto spawn = m_spawns.find(root);
    if (spawn != m_spawns.end()) {
      SpawnEntry &entry = spawn->second;
      entry.members.erase(std::remove(entry.members.begin(), entry.members.end(), &object),
                          entry.members.end());
      if (entry.root == &object) {
        entry.root = nullptr;
      }
      if (entry.members.empty()) {
        if (!blender::BLI_uuid_is_nil(entry.instance_uuid)) {
          m_spawnByInstanceUuid.erase(entry.instance_uuid);
          m_usedInstanceUuids.erase(entry.instance_uuid);
        }
        m_spawns.erase(spawn);
      }
    }
  }

  LN_GameStateResult ApplyCheckpoint(KX_Scene &scene,
                                     const Checkpoint &checkpoint,
                                     std::string &r_error)
  {
    r_error.clear();
    if (m_reconciling) {
      r_error = "Load Game reconciliation is already active";
      return LN_GameStateResult::Busy;
    }
    if (!m_registryValid || m_scene != &scene) {
      r_error = "checkpoint registry is not initialized for the calling runtime scene";
      return LN_GameStateResult::InvalidRequest;
    }
    if (!m_identityCatalogValid) {
      r_error = m_identityCatalogError;
      return LN_GameStateResult::InvalidRequest;
    }
    if (checkpoint.scene_uuid != SceneUuidText()) {
      r_error = "Save Game belongs to a different checkpoint scene";
      return LN_GameStateResult::Incompatible;
    }

    blender::Scene *blender_scene = scene.GetBlenderScene();
    blender::ViewLayer *view_layer = blender_scene ?
                                         blender::BKE_view_layer_default_view(blender_scene) :
                                         nullptr;
    blender::Depsgraph *depsgraph =
        blender_scene && view_layer ? blender::BKE_scene_get_depsgraph(blender_scene, view_layer) :
                                      nullptr;
    if (depsgraph == nullptr || blender::DEG_get_input_scene(depsgraph) != blender_scene ||
        blender::DEG_get_input_view_layer(depsgraph) != view_layer)
    {
      r_error = "the calling runtime scene's exact depsgraph is unavailable";
      return LN_GameStateResult::InternalError;
    }
    LN_Manager *manager = scene.GetLogicNodeManager();
    if (manager == nullptr) {
      r_error = "the calling scene has no C++ Logic Nodes manager";
      return LN_GameStateResult::InternalError;
    }
    const LN_GameStateResult hierarchy_validation =
        validate_save_object_state_hierarchy(scene, r_error);
    if (!LN_GameStateSucceeded(hierarchy_validation)) {
      return hierarchy_validation;
    }

    struct AuthoredPlan {
      AuthoredEntry *entry = nullptr;
      const ObjectState *saved = nullptr;
      KX_GameObject *final_object = nullptr;
      KX_GameObject *remove_object = nullptr;
      bool stage = false;
    };
    struct SpawnPlan {
      const ObjectState *saved = nullptr;
      blender::UUID instance_uuid;
      blender::UUID source_uuid;
      KX_GameObject *source = nullptr;
      const SourceEntry *source_entry = nullptr;
      KX_GameObject *root = nullptr;
      KX_GameObject *current_root_key = nullptr;
      SpawnEntry *current_entry = nullptr;
      std::vector<KX_GameObject *> staged_members;
      bool staged = false;
    };
    struct RemovalPlan {
      KX_GameObject *registered_root_key = nullptr;
      std::vector<KX_GameObject *> candidates;
    };

    std::vector<AuthoredPlan> authored_plans;
    std::vector<SpawnPlan> spawn_plans;
    std::vector<RemovalPlan> removal_plans;
    std::unordered_set<KX_GameObject *> matched_spawn_keys;
    size_t skipped_authored = 0;
    size_t skipped_spawn_sources = 0;

    try {
      authored_plans.reserve(checkpoint.authored_objects.size());
      spawn_plans.reserve(checkpoint.spawned_instances.size());
      removal_plans.reserve(m_spawns.size());
      matched_spawn_keys.reserve(checkpoint.spawned_instances.size());

      size_t indexed_instance_count = 0;
      for (auto &item : m_spawns) {
        KX_GameObject *root_key = item.first;
        SpawnEntry &entry = item.second;
        if (root_key == nullptr || entry.source == nullptr || entry.members.empty() ||
            (entry.root != nullptr && entry.root != root_key))
        {
          r_error = "tracked Add Object registry contains incomplete ownership metadata";
          return LN_GameStateResult::InternalError;
        }
        if (blender::BLI_uuid_is_nil(entry.instance_uuid)) {
          if (entry.persistent) {
            r_error = "persistent Add Object registry entry has no instance UUID";
            return LN_GameStateResult::InternalError;
          }
          continue;
        }
        indexed_instance_count++;
        const auto indexed = m_spawnByInstanceUuid.find(entry.instance_uuid);
        if (indexed == m_spawnByInstanceUuid.end() || indexed->second != root_key ||
            m_usedInstanceUuids.find(entry.instance_uuid) == m_usedInstanceUuids.end())
        {
          r_error = "tracked Add Object instance UUID indexes are inconsistent";
          return LN_GameStateResult::InternalError;
        }
        if (entry.root != nullptr &&
            (blender::UUID(entry.root->GetPersistenceInstanceUuid()) != entry.instance_uuid ||
             blender::UUID(entry.root->GetPersistenceSourceUuid()) != entry.source_uuid ||
             entry.root->GetSaveGameObjectState() != entry.persistent))
        {
          r_error = "tracked Add Object KX persistence metadata diverged from its registry";
          return LN_GameStateResult::InternalError;
        }
      }
      if (indexed_instance_count != m_spawnByInstanceUuid.size() ||
          indexed_instance_count != m_usedInstanceUuids.size())
      {
        r_error = "tracked Add Object instance UUID catalog contains orphaned reservations";
        return LN_GameStateResult::InternalError;
      }

      for (const ObjectState &saved : checkpoint.authored_objects) {
        blender::UUID uuid;
        if (!parse_canonical_uuid(saved.persistence_id, uuid)) {
          r_error = "decoded authored object contains an invalid UUID";
          return LN_GameStateResult::CorruptData;
        }
        const auto found = m_authored.find(uuid);
        if (found == m_authored.end()) {
          skipped_authored++;
          continue;
        }
        AuthoredEntry &entry = found->second;
        if (entry.live_object != nullptr &&
            (entry.live_object->GetSGNode() == nullptr || entry.live_object->GetScene() != &scene ||
             blender::UUID(entry.live_object->GetPersistenceSourceUuid()) != entry.uuid))
        {
          r_error = "authored persistence participant '" + entry.name +
                    "' moved outside its registered runtime scene or identity";
          return LN_GameStateResult::UnsupportedContent;
        }
        if (entry.live_object != nullptr &&
            entry.live_object->GetSaveGameObjectState() != entry.save_object_state)
        {
          r_error = "authored persistence participant '" + entry.name +
                    "' KX metadata diverged from its registry";
          return LN_GameStateResult::InternalError;
        }

        AuthoredPlan plan;
        plan.entry = &entry;
        plan.saved = &saved;
        plan.final_object = entry.live_object;
        if (saved.has_existence && entry.save_object_state) {
          const LN_GameStateResult validation =
              validate_candidate(entry.descriptor, entry.name, r_error);
          if (!LN_GameStateSucceeded(validation)) {
            return validation;
          }
          if (saved.exists && entry.live_object == nullptr) {
            if (!entry.reconstruction_recipe_valid) {
              r_error = "authored persistence participant '" + entry.name +
                        "' has no validated reconstruction recipe";
              return LN_GameStateResult::UnsupportedContent;
            }
            plan.stage = true;
          }
          else if (!saved.exists && entry.live_object != nullptr) {
            const LN_PersistenceCandidateDescriptor current =
                build_candidate_descriptor(scene, *entry.live_object);
            const LN_GameStateResult current_validation =
                validate_candidate(current, entry.name, r_error);
            if (!LN_GameStateSucceeded(current_validation)) {
              return current_validation;
            }
            plan.remove_object = entry.live_object;
            plan.final_object = nullptr;
          }
        }
        authored_plans.push_back(std::move(plan));
      }

      for (const ObjectState &saved : checkpoint.spawned_instances) {
        SpawnPlan plan;
        plan.saved = &saved;
        if (!parse_canonical_uuid(saved.persistence_id, plan.instance_uuid) ||
            !parse_canonical_uuid(saved.source_id, plan.source_uuid))
        {
          r_error = "decoded spawned instance contains an invalid UUID";
          return LN_GameStateResult::CorruptData;
        }
        if (m_authoredAndSourceUuids.find(plan.instance_uuid) !=
            m_authoredAndSourceUuids.end())
        {
          r_error = "saved spawned instance UUID collides with a current authored/source UUID";
          return LN_GameStateResult::Incompatible;
        }

        const auto current_index = m_spawnByInstanceUuid.find(plan.instance_uuid);
        if (current_index != m_spawnByInstanceUuid.end()) {
          const auto current = m_spawns.find(current_index->second);
          if (current == m_spawns.end() || current->second.instance_uuid != plan.instance_uuid) {
            r_error = "saved instance matched an inconsistent current runtime registry entry";
            return LN_GameStateResult::InternalError;
          }
          if (current->second.source_uuid != plan.source_uuid) {
            r_error = "saved instance/source UUID pair differs from the current live instance";
            return LN_GameStateResult::Incompatible;
          }
          if (current->second.root != nullptr) {
            plan.current_root_key = current->first;
            plan.current_entry = &current->second;
            plan.root = current->second.root;
          }
        }

        const auto source = m_sourcesByUuid.find(plan.source_uuid);
        if (source == m_sourcesByUuid.end()) {
          if (plan.root != nullptr) {
            r_error = "live saved instance source left the current inactive source catalog";
            return LN_GameStateResult::UnsupportedContent;
          }
          skipped_spawn_sources++;
          continue;
        }
        plan.source = source->second;
        const LN_GameStateResult source_validation =
            ValidatePersistentSource(scene, *plan.source, 0.0f, false, r_error);
        if (!LN_GameStateSucceeded(source_validation)) {
          return source_validation;
        }
        const SourceEntry &source_entry = m_sourcesByObject.at(plan.source);
        plan.source_entry = &source_entry;
        const bool restore_current_transform =
            saved.has_transform &&
            (plan.root != nullptr ? plan.root->GetSaveGameObjectState() :
                                    plan.source->GetSaveGameObjectState());
        const MT_Vector3 target_scale = restore_current_transform ? saved.world_scale :
                                                                   saved.creation_scale;
        if (source_entry.descriptor.physics_kind == LN_PersistencePhysicsKind::JoltSoft &&
            (!scales_match(saved.creation_scale, source_entry.supported_scale) ||
             !scales_match(target_scale, source_entry.supported_scale)))
        {
          r_error = "saved persistent soft body requires a scale different from its current "
                    "source-supported scale";
          return LN_GameStateResult::UnsupportedContent;
        }
        if (plan.root != nullptr) {
          const LN_GameStateResult runtime_validation =
              ValidateSpawnRuntime(scene, *plan.current_entry, r_error);
          if (!LN_GameStateSucceeded(runtime_validation)) {
            return runtime_validation;
          }
          matched_spawn_keys.insert(plan.current_root_key);
        }
        else {
          plan.staged = true;
        }
        spawn_plans.push_back(std::move(plan));
      }

      for (const auto &item : m_spawns) {
        /* Save Object State is opt-in. Transient and explicitly demoted Add Object roots are
         * unmarked runtime state, so a checkpoint must neither remove nor recreate them. */
        if (!item.second.persistent ||
            matched_spawn_keys.find(item.first) != matched_spawn_keys.end())
        {
          continue;
        }
        RemovalPlan removal;
        removal.registered_root_key = item.first;
        const LN_GameStateResult topology = ValidateRemovalTopology(
            scene, item.first, item.second, removal.candidates, r_error);
        if (!LN_GameStateSucceeded(topology)) {
          return topology;
        }
        removal_plans.push_back(std::move(removal));
      }
    }
    catch (const std::exception &exception) {
      r_error = std::string("could not build Load Game reconciliation plan: ") + exception.what();
      return LN_GameStateResult::InternalError;
    }
    catch (...) {
      r_error = "could not build Load Game reconciliation plan";
      return LN_GameStateResult::InternalError;
    }

    m_reconciling = true;
    struct ReconciliationScope {
      bool &active;
      ~ReconciliationScope()
      {
        active = false;
      }
    } reconciliation_scope{m_reconciling};

    std::vector<KX_GameObject *> staged_objects;
    try {
      staged_objects.reserve(authored_plans.size() + spawn_plans.size());
    }
    catch (...) {
      r_error = "could not reserve staged Load Game ownership";
      return LN_GameStateResult::InternalError;
    }
    auto rollback_staged = [&]() {
      for (auto object = staged_objects.rbegin(); object != staged_objects.rend(); ++object) {
        if (*object != nullptr && (*object)->GetSGNode() != nullptr &&
            (*object)->GetScene() == &scene)
        {
          scene.RemoveObject(*object, KX_ObjectRemovalReason::LoadReconcile);
        }
      }
      staged_objects.clear();
    };

    try {
      for (AuthoredPlan &plan : authored_plans) {
        if (!plan.stage) {
          continue;
        }
        KX_GameObject *object = nullptr;
        const BL_GameStateReconstructionResult reconstructed = BL_ReconstructGameStateObject(
            scene, *depsgraph, plan.entry->reconstruction_recipe, object, r_error);
        if (reconstructed != BL_GameStateReconstructionResult::Success || object == nullptr) {
          rollback_staged();
          return reconstructed == BL_GameStateReconstructionResult::UnsupportedContent ?
                     LN_GameStateResult::UnsupportedContent :
                     LN_GameStateResult::InternalError;
        }
        staged_objects.push_back(object);
        const LN_PersistenceCandidateDescriptor reconstructed_descriptor =
            build_candidate_descriptor(scene, *object);
        const LN_GameStateResult validation =
            validate_candidate(reconstructed_descriptor, plan.entry->name, r_error);
        if (!LN_GameStateSucceeded(validation)) {
          rollback_staged();
          return validation;
        }
        plan.final_object = object;
      }

      for (SpawnPlan &plan : spawn_plans) {
        if (!plan.staged) {
          continue;
        }
        std::vector<KX_GameObject *> members;
        KX_GameObject *root = scene.AddReplicaObject(plan.source, nullptr, 0.0f, &members);
        if (root == nullptr) {
          rollback_staged();
          r_error = "normal runtime replication failed while staging a saved instance";
          return LN_GameStateResult::InternalError;
        }
        /* AddReplicaObject returns one caller reference in addition to scene-list ownership.
         * Transfer to list-only ownership immediately and put it under the transaction rollback
         * guard before any subsequent validation can fail. */
        root->Release();
        staged_objects.push_back(root);
        manager->SetRuntimeTreesActiveForObject(root, false);
        SpawnEntry staged_entry;
        staged_entry.root = root;
        staged_entry.source = plan.source;
        staged_entry.members = members;
        staged_entry.source_uuid = plan.source_uuid;
        staged_entry.instance_uuid = plan.instance_uuid;
        staged_entry.creation_position = plan.saved->creation_position;
        staged_entry.creation_orientation = plan.saved->creation_orientation;
        staged_entry.creation_scale = plan.saved->creation_scale;
        staged_entry.persistent = true;
        const LN_GameStateResult validation = ValidateSpawnRuntime(scene, staged_entry, r_error);
        if (!LN_GameStateSucceeded(validation)) {
          rollback_staged();
          return validation;
        }
        plan.root = root;
        plan.staged_members = std::move(members);
      }
    }
    catch (const std::exception &exception) {
      rollback_staged();
      r_error = std::string("Load Game staging failed: ") + exception.what();
      return LN_GameStateResult::InternalError;
    }
    catch (...) {
      rollback_staged();
      r_error = "Load Game staging failed";
      return LN_GameStateResult::InternalError;
    }

    std::vector<LoadStateTarget> state_targets;
    std::unordered_map<AuthoredEntry *, KX_GameObject *> final_authored_objects;
    decltype(m_authoredByObject) final_authored_by_object;
    decltype(m_spawns) final_spawns;
    decltype(m_spawnRootByMember) final_spawn_root_by_member;
    decltype(m_spawnByInstanceUuid) final_spawn_by_instance_uuid;
    decltype(m_usedInstanceUuids) final_used_instance_uuids;
    std::vector<KX_GameObject *> removal_roots;
    try {
      state_targets.reserve(authored_plans.size() + spawn_plans.size());
      final_authored_objects.reserve(m_authored.size());
      final_authored_by_object.reserve(m_authored.size());
      for (auto &item : m_authored) {
        final_authored_objects.emplace(&item.second, item.second.live_object);
      }
      for (const AuthoredPlan &plan : authored_plans) {
        final_authored_objects[plan.entry] = plan.final_object;
        if (plan.final_object == nullptr) {
          continue;
        }
        LoadStateTarget target;
        target.object = plan.final_object;
        target.saved = plan.saved;
        target.modifier_baseline = &plan.entry->modifier_baseline;
        target.apply_transform = plan.saved->has_transform &&
                                 plan.final_object->GetSaveGameObjectState();
        target.apply_material = plan.saved->has_material_state &&
                                plan.final_object->GetSaveGameObjectState();
        target.world_position = plan.saved->world_position;
        target.world_orientation = plan.saved->world_orientation;
        target.world_scale = plan.saved->world_scale;
        if (target.apply_transform || target.apply_material || !plan.saved->properties.empty()) {
          state_targets.push_back(std::move(target));
        }
      }
      for (const auto &item : final_authored_objects) {
        if (item.second != nullptr &&
            !final_authored_by_object.emplace(item.second, item.first->uuid).second)
        {
          rollback_staged();
          r_error = "final authored runtime registry contains a duplicate live object";
          return LN_GameStateResult::InternalError;
        }
      }

      final_spawns.reserve(m_spawns.size() + spawn_plans.size());
      final_spawn_root_by_member.reserve(m_spawnRootByMember.size() + spawn_plans.size());
      final_spawn_by_instance_uuid.reserve(m_spawnByInstanceUuid.size() + spawn_plans.size());
      final_used_instance_uuids.reserve(m_usedInstanceUuids.size() + spawn_plans.size());

      auto insert_final_spawn = [&](KX_GameObject *registered_root_key,
                                    const SpawnEntry &entry) {
        if (registered_root_key == nullptr || entry.source == nullptr || entry.members.empty() ||
            (entry.root != nullptr && entry.root != registered_root_key) ||
            !final_spawns.emplace(registered_root_key, entry).second)
        {
          return false;
        }
        for (KX_GameObject *member : entry.members) {
          if (member == nullptr ||
              !final_spawn_root_by_member.emplace(member, registered_root_key).second)
          {
            return false;
          }
        }
        if (!blender::BLI_uuid_is_nil(entry.instance_uuid) &&
            (!final_spawn_by_instance_uuid.emplace(entry.instance_uuid, registered_root_key)
                  .second ||
             !final_used_instance_uuids.insert(entry.instance_uuid).second))
        {
          return false;
        }
        return true;
      };

      /* Preserve every unmarked current instance, including a demoted instance's reserved UUID,
       * so Set Save Object State can promote it again after this load. A demoted instance matched by
       * the checkpoint is handled by its SpawnPlan and becomes persistent again. */
      for (const auto &item : m_spawns) {
        if (item.second.persistent ||
            matched_spawn_keys.find(item.first) != matched_spawn_keys.end())
        {
          continue;
        }
        if (!insert_final_spawn(item.first, item.second)) {
          rollback_staged();
          r_error = "final transient-instance registry contains duplicate or invalid ownership";
          return LN_GameStateResult::InternalError;
        }
      }

      for (const SpawnPlan &plan : spawn_plans) {
        SpawnEntry entry;
        if (plan.current_entry != nullptr) {
          entry = *plan.current_entry;
        }
        else {
          entry.root = plan.root;
          entry.source = plan.source;
          entry.members = plan.staged_members;
        }
        entry.root = plan.root;
        entry.source = plan.source;
        entry.source_uuid = plan.source_uuid;
        entry.instance_uuid = plan.instance_uuid;
        entry.creation_position = plan.saved->creation_position;
        entry.creation_orientation = plan.saved->creation_orientation;
        entry.creation_scale = plan.saved->creation_scale;
        entry.life_seconds = 0.0f;
        entry.persistent = true;
        if (entry.root == nullptr || entry.members.size() != 1 ||
            entry.members.front() != entry.root || !insert_final_spawn(entry.root, entry))
        {
          rollback_staged();
          r_error = "final saved-instance registry contains duplicate or invalid ownership";
          return LN_GameStateResult::InternalError;
        }

        LoadStateTarget target;
        target.object = entry.root;
        target.saved = plan.saved;
        target.modifier_baseline = plan.source_entry ?
                                       &plan.source_entry->modifier_baseline :
                                       nullptr;
        target.apply_transform = true;
        target.apply_material = plan.saved->has_material_state;
        const bool restore_current = plan.saved->has_transform;
        target.world_position = restore_current ? plan.saved->world_position :
                                                  plan.saved->creation_position;
        target.world_orientation = restore_current ? plan.saved->world_orientation :
                                                     plan.saved->creation_orientation;
        target.world_scale = restore_current ? plan.saved->world_scale :
                                               plan.saved->creation_scale;
        state_targets.push_back(std::move(target));
      }

      constexpr size_t no_parent = std::numeric_limits<size_t>::max();
      std::unordered_map<std::string, size_t> structural_by_identity;
      std::unordered_map<KX_GameObject *, size_t> structural_by_object;
      std::vector<size_t> parent_indices(state_targets.size(), no_parent);
      structural_by_identity.reserve(state_targets.size());
      structural_by_object.reserve(state_targets.size());
      for (size_t index = 0; index < state_targets.size(); ++index) {
        LoadStateTarget &target = state_targets[index];
        if (!target.apply_transform) {
          continue;
        }
        if (!target.saved->has_existence || !target.saved->exists ||
            !target.saved->has_hierarchy_state || !target.saved->has_material_state ||
            !target.saved->has_runtime_structure_state)
        {
          rollback_staged();
          r_error = "decoded Save Object State record is incomplete for object '" +
                    target.saved->name + "'";
          return LN_GameStateResult::CorruptData;
        }
        if (target.saved->hierarchy.compound || target.saved->hierarchy.ghost) {
          rollback_staged();
          r_error = "saved object '" + target.saved->name +
                    "' uses unsupported compound/ghost parenting";
          return LN_GameStateResult::UnsupportedContent;
        }
        target.apply_hierarchy = true;
        target.apply_rigid_body = true;
        if (!structural_by_identity.emplace(target.saved->persistence_id, index).second ||
            !structural_by_object.emplace(target.object, index).second)
        {
          rollback_staged();
          r_error = "final Save Object State graph contains a duplicate object or identity";
          return LN_GameStateResult::InternalError;
        }
      }
      for (size_t index = 0; index < state_targets.size(); ++index) {
        LoadStateTarget &target = state_targets[index];
        if (!target.apply_hierarchy ||
            target.saved->hierarchy.relation == HierarchyRelation::Root)
        {
          continue;
        }
        const auto parent = structural_by_identity.find(target.saved->hierarchy.parent_id);
        if (parent == structural_by_identity.end()) {
          rollback_staged();
          r_error = "saved parent for object '" + target.saved->name +
                    "' is unavailable in the current runtime scene";
          return LN_GameStateResult::Incompatible;
        }
        parent_indices[index] = parent->second;
        target.desired_parent = state_targets[parent->second].object;
      }

      std::vector<uint8_t> hierarchy_color(state_targets.size(), 0);
      std::vector<size_t> hierarchy_path;
      hierarchy_path.reserve(state_targets.size());
      for (size_t start = 0; start < state_targets.size(); ++start) {
        if (!state_targets[start].apply_hierarchy || hierarchy_color[start] == 2) {
          continue;
        }
        hierarchy_path.clear();
        size_t cursor = start;
        while (hierarchy_color[cursor] == 0) {
          hierarchy_color[cursor] = 1;
          hierarchy_path.push_back(cursor);
          if (parent_indices[cursor] == no_parent) {
            break;
          }
          cursor = parent_indices[cursor];
        }
        if (hierarchy_color[cursor] == 1 && parent_indices[cursor] != no_parent) {
          rollback_staged();
          r_error = "final Save Object State graph contains a hierarchy cycle";
          return LN_GameStateResult::CorruptData;
        }
        for (auto path_item = hierarchy_path.rbegin(); path_item != hierarchy_path.rend();
             ++path_item)
        {
          const size_t index = *path_item;
          if (parent_indices[index] == no_parent) {
            state_targets[index].hierarchy_depth = 0;
          }
          else {
            const size_t parent_depth = state_targets[parent_indices[index]].hierarchy_depth;
            if (parent_depth == kMaxObjects) {
              rollback_staged();
              r_error = "saved object hierarchy exceeds the supported depth";
              return LN_GameStateResult::CorruptData;
            }
            state_targets[index].hierarchy_depth = parent_depth + 1;
          }
          hierarchy_color[index] = 2;
        }
      }

      std::unordered_set<KX_GameObject *> removal_objects;
      removal_objects.reserve(authored_plans.size() + removal_plans.size());
      for (const AuthoredPlan &plan : authored_plans) {
        if (plan.remove_object != nullptr &&
            !removal_objects.insert(plan.remove_object).second)
        {
          rollback_staged();
          r_error = "Load Game removal plan contains a duplicate authored object";
          return LN_GameStateResult::InternalError;
        }
      }
      for (const RemovalPlan &removal : removal_plans) {
        for (KX_GameObject *candidate : removal.candidates) {
          if (candidate == nullptr || candidate->GetScene() != &scene ||
              candidate->GetSGNode() == nullptr ||
              !removal_objects.insert(candidate).second)
          {
            rollback_staged();
            r_error = "Load Game removal plan contains a stale or duplicate object";
            return LN_GameStateResult::InternalError;
          }
        }
      }
      for (KX_GameObject *object : removal_objects) {
        if (structural_by_object.find(object) != structural_by_object.end()) {
          rollback_staged();
          r_error = "Load Game planned the same object for restoration and removal";
          return LN_GameStateResult::InternalError;
        }
      }

      auto planned_parent = [&](KX_GameObject *object,
                                KX_GameObject *&r_parent,
                                std::string &r_parent_error) {
        const auto target = structural_by_object.find(object);
        if (target != structural_by_object.end()) {
          r_parent = state_targets[target->second].desired_parent;
          return true;
        }
        return resolve_normal_parent(*object, r_parent, r_parent_error);
      };

      EXP_ListValue<KX_GameObject> *active_objects = scene.GetObjectList();
      if (active_objects == nullptr) {
        rollback_staged();
        r_error = "runtime scene object catalog is unavailable";
        return LN_GameStateResult::InternalError;
      }
      for (KX_GameObject *object : *active_objects) {
        if (object == nullptr || !object->GetSaveGameObjectState() ||
            removal_objects.find(object) != removal_objects.end())
        {
          continue;
        }
        KX_GameObject *cursor = object;
        for (size_t depth = 0; depth <= kMaxObjects; ++depth) {
          KX_GameObject *parent = nullptr;
          std::string parent_error;
          if (!planned_parent(cursor, parent, parent_error)) {
            rollback_staged();
            r_error = "cannot validate the planned hierarchy for object '" +
                      object->GetName() + "': " + parent_error;
            return LN_GameStateResult::UnsupportedContent;
          }
          if (parent == nullptr) {
            break;
          }
          if (removal_objects.find(parent) != removal_objects.end()) {
            rollback_staged();
            r_error = "Load Game would destroy surviving marked object '" + object->GetName() +
                      "' through a removed parent";
            return LN_GameStateResult::Incompatible;
          }
          if (depth == kMaxObjects) {
            rollback_staged();
            r_error = "planned Save Object State hierarchy contains a cycle";
            return LN_GameStateResult::Incompatible;
          }
          cursor = parent;
        }
      }

      removal_roots.reserve(removal_objects.size());
      for (KX_GameObject *object : removal_objects) {
        bool has_removed_ancestor = false;
        KX_GameObject *cursor = object;
        for (size_t depth = 0; depth <= kMaxObjects; ++depth) {
          KX_GameObject *parent = nullptr;
          std::string parent_error;
          if (!planned_parent(cursor, parent, parent_error)) {
            rollback_staged();
            r_error = "cannot validate a planned removal hierarchy: " + parent_error;
            return LN_GameStateResult::UnsupportedContent;
          }
          if (parent == nullptr) {
            break;
          }
          if (removal_objects.find(parent) != removal_objects.end()) {
            has_removed_ancestor = true;
            break;
          }
          if (depth == kMaxObjects) {
            rollback_staged();
            r_error = "planned removal hierarchy contains a cycle";
            return LN_GameStateResult::Incompatible;
          }
          cursor = parent;
        }
        if (!has_removed_ancestor) {
          removal_roots.push_back(object);
        }
      }
    }
    catch (const std::exception &exception) {
      rollback_staged();
      r_error = std::string("could not prepare final Load Game registry/state: ") +
                exception.what();
      return LN_GameStateResult::InternalError;
    }
    catch (...) {
      rollback_staged();
      r_error = "could not prepare final Load Game registry/state";
      return LN_GameStateResult::InternalError;
    }

    std::vector<LN_RuntimeTreeRestoreTarget> runtime_tree_targets;
    try {
      runtime_tree_targets.reserve(state_targets.size());
      for (const LoadStateTarget &target : state_targets) {
        if (!target.saved->has_runtime_structure_state) {
          continue;
        }
        LN_RuntimeTreeRestoreTarget restore_target;
        restore_target.gameobj = target.object;
        restore_target.attachments.reserve(target.saved->logic_trees.size());
        for (const LogicTreeAttachmentState &saved_tree : target.saved->logic_trees) {
          restore_target.attachments.push_back({saved_tree.tree_name,
                                                saved_tree.library_path,
                                                saved_tree.applied_tree_index,
                                                saved_tree.enabled});
        }
        runtime_tree_targets.push_back(std::move(restore_target));
      }
    }
    catch (...) {
      rollback_staged();
      r_error = "could not allocate the Load Game C++ Logic Node attachment plan";
      return LN_GameStateResult::InternalError;
    }

    LN_PreparedRuntimeTreeRestore prepared_runtime_trees;
    const LN_GameStateResult runtime_tree_prepare_result =
        manager->PrepareGameStateRuntimeTreeRestore(
            runtime_tree_targets, removal_roots, prepared_runtime_trees, r_error);
    if (!LN_GameStateSucceeded(runtime_tree_prepare_result))
    {
      rollback_staged();
      return runtime_tree_prepare_result;
    }

    blender::Main *bmain = current_main();
    if (bmain == nullptr) {
      rollback_staged();
      r_error = "runtime Main database is unavailable for object-state restoration";
      return LN_GameStateResult::InternalError;
    }

    PreparedRuntimeNodeTreeRestore prepared_node_trees;
    const LN_GameStateResult node_tree_prepare_result =
        prepare_runtime_node_tree_restore(*bmain,
                                          *blender_scene,
                                          m_geometryNodesAssets,
                                          checkpoint,
                                          prepared_node_trees,
                                          r_error);
    if (!LN_GameStateSucceeded(node_tree_prepare_result)) {
      prepared_node_trees.Rollback();
      rollback_staged();
      return node_tree_prepare_result;
    }

    std::vector<ModifierRestoreTarget> modifier_targets;
    try {
      modifier_targets.reserve(state_targets.size());
      for (const LoadStateTarget &target : state_targets) {
        if (target.saved->has_runtime_structure_state) {
          modifier_targets.push_back(
              {target.object, target.saved, target.modifier_baseline});
        }
      }
    }
    catch (...) {
      rollback_staged();
      r_error = "could not allocate the Load Game modifier plan";
      return LN_GameStateResult::InternalError;
    }
    PreparedModifierRestore prepared_modifiers;
    const LN_GameStateResult modifier_prepare_result = prepare_modifier_restore(
        *bmain,
        m_geometryNodesAssets,
        modifier_targets,
        prepared_modifiers,
        r_error);
    if (!LN_GameStateSucceeded(modifier_prepare_result)) {
      prepared_modifiers.Rollback();
      rollback_staged();
      return modifier_prepare_result;
    }

    std::vector<MaterialRestoreTarget> material_targets;
    try {
      material_targets.reserve(state_targets.size());
      for (const LoadStateTarget &target : state_targets) {
        if (target.apply_material) {
          material_targets.push_back({target.object, target.saved});
        }
      }
    }
    catch (...) {
      rollback_staged();
      r_error = "could not allocate the Load Game material plan";
      return LN_GameStateResult::InternalError;
    }

    PreparedMaterialRestore prepared_materials;
    const LN_GameStateResult material_prepare_result = prepare_material_restore(
        *bmain, m_materialAssets, checkpoint, material_targets, prepared_materials, r_error);
    if (!LN_GameStateSucceeded(material_prepare_result)) {
      prepared_materials.Rollback();
      rollback_staged();
      return material_prepare_result;
    }

    struct CommitData {
      KX_Scene *scene = nullptr;
      LN_Manager *manager = nullptr;
      const std::vector<SpawnPlan> *spawn_plans = nullptr;
      PreparedRuntimeNodeTreeRestore *node_trees = nullptr;
      PreparedModifierRestore *modifiers = nullptr;
      PreparedMaterialRestore *materials = nullptr;
      LN_PreparedRuntimeTreeRestore *runtime_trees = nullptr;
    };
    CommitData commit_data{
        &scene,
        manager,
        &spawn_plans,
        &prepared_node_trees,
        &prepared_modifiers,
        &prepared_materials,
        &prepared_runtime_trees};
    const LoadCommitPrelude commit_prelude = [](void *data) {
      CommitData &commit = *static_cast<CommitData *>(data);
      const auto &plans = *commit.spawn_plans;
      commit.node_trees->ReserveCommitUpdates(*commit.scene);
      commit.modifiers->ReserveCommitUpdates(*commit.scene);
      commit.materials->ReserveCommitUpdates(*commit.scene);
      for (const SpawnPlan &plan : plans) {
        plan.root->SetPersistenceSourceUuid(plan.source_uuid);
        plan.root->SetPersistenceInstanceUuid(plan.instance_uuid);
        plan.root->SetSaveGameObjectState(true);
      }
      commit.node_trees->Commit(*commit.scene);
      commit.modifiers->Commit(*commit.scene);
      commit.materials->Commit(*commit.scene);
      commit.manager->CommitGameStateRuntimeTreeRestore(*commit.runtime_trees);
    };
    try {
      const LN_GameStateResult state_result =
          apply_object_states(scene, state_targets, commit_prelude, &commit_data, r_error);
      if (!LN_GameStateSucceeded(state_result)) {
        prepared_materials.Rollback();
        rollback_staged();
        return state_result;
      }
    }
    catch (const std::exception &exception) {
      prepared_materials.Rollback();
      rollback_staged();
      r_error = std::string("could not prepare Load Game object state: ") + exception.what();
      return LN_GameStateResult::InternalError;
    }
    catch (...) {
      prepared_materials.Rollback();
      rollback_staged();
      r_error = "could not prepare Load Game object state";
      return LN_GameStateResult::InternalError;
    }

    /* No-fail commit tail. Every recursive removal topology and replacement container was already
     * validated/allocated before the first property or SG/Jolt mutation. */
    for (KX_GameObject *root : removal_roots) {
      scene.RemoveObject(root, KX_ObjectRemovalReason::LoadReconcile);
    }
    for (const auto &item : final_authored_objects) {
      item.first->live_object = item.second;
    }
    m_authoredByObject.swap(final_authored_by_object);
    m_spawns.swap(final_spawns);
    m_spawnRootByMember.swap(final_spawn_root_by_member);
    m_spawnByInstanceUuid.swap(final_spawn_by_instance_uuid);
    m_usedInstanceUuids.swap(final_used_instance_uuids);
    for (KX_GameObject *object : staged_objects) {
      manager->SetRuntimeTreesActiveForObject(object, true);
    }

    if (skipped_authored != 0 || skipped_spawn_sources != 0) {
      CM_Warning("Load Game skipped " << skipped_authored
                                      << " authored record(s) and "
                                      << skipped_spawn_sources
                                      << " spawned source record(s) removed from the current build");
    }
    return LN_GameStateResult::Success;
  }

  LN_GameStateResult CaptureCheckpoint(KX_Scene &scene,
                                       Checkpoint &r_checkpoint,
                                       std::string &r_error) const
  {
    if (!m_registryValid || m_scene != &scene) {
      r_error = "checkpoint registry is not initialized for this scene";
      return LN_GameStateResult::InvalidRequest;
    }
    if (!m_identityCatalogValid) {
      r_error = m_identityCatalogError.empty() ? "checkpoint identity catalog is invalid" :
                                                 m_identityCatalogError;
      return LN_GameStateResult::InvalidRequest;
    }
    const LN_GameStateResult hierarchy_validation =
        validate_save_object_state_hierarchy(scene, r_error);
    if (!LN_GameStateSucceeded(hierarchy_validation)) {
      return hierarchy_validation;
    }
    LN_Manager *manager = scene.GetLogicNodeManager();
    if (manager == nullptr) {
      r_error = "the calling scene has no C++ Logic Nodes manager";
      return LN_GameStateResult::InternalError;
    }
    auto capture_runtime_tree_attachments =
        [&](KX_GameObject *object, CaptureParticipant &participant) {
          std::vector<LN_RuntimeTreeRecipe> recipes;
          if (!manager->CaptureRuntimeTreeRecipes(object, recipes, r_error)) {
            return false;
          }
          if (recipes.size() > kMaxLogicTreesPerObject) {
            r_error = "object '" + participant.name +
                      "' exceeds the C++ Logic Node attachment limit";
            return false;
          }
          participant.logic_trees.reserve(recipes.size());
          for (const LN_RuntimeTreeRecipe &recipe : recipes) {
            if (recipe.program == nullptr) {
              r_error = "object '" + participant.name +
                        "' has an incomplete C++ Logic Node attachment";
              return false;
            }
            LogicTreeAttachmentState state;
            state.tree_name = recipe.program->GetSourceTreeName();
            state.library_path = recipe.program->GetSourceTreeLibraryPath();
            state.applied_tree_index = recipe.applied_tree_index;
            state.enabled = recipe.enabled;
            if (!is_valid_checkpoint_text(state.tree_name, kMaxNameBytes, false) ||
                !is_valid_checkpoint_text(state.library_path, kMaxIdentityBytes))
            {
              r_error = "object '" + participant.name +
                        "' has a C++ Logic Node tree without a durable asset identity";
              return false;
            }
            participant.logic_trees.push_back(std::move(state));
          }
          return true;
        };
    std::vector<CaptureParticipant> participants;
    try {
      const size_t authored_capacity = std::min(m_authored.size(), kMaxObjects);
      const size_t spawn_capacity = std::min(m_spawns.size(), kMaxObjects - authored_capacity);
      participants.reserve(authored_capacity + spawn_capacity);
      for (const auto &item : m_authored) {
        const AuthoredEntry &entry = item.second;
        if (entry.live_object != nullptr &&
            (entry.live_object->GetScene() != &scene || entry.live_object->GetSGNode() == nullptr ||
             blender::UUID(entry.live_object->GetPersistenceSourceUuid()) != entry.uuid))
        {
          r_error = "authored persistence participant '" + entry.name +
                    "' moved outside its registered runtime scene or identity";
          return LN_GameStateResult::UnsupportedContent;
        }
        if (entry.save_object_state) {
          const LN_GameStateResult validation =
              validate_candidate(entry.descriptor, entry.name, r_error);
          if (!LN_GameStateSucceeded(validation)) {
            return validation;
          }
          const LN_GameStateResult runtime_validation =
              ValidateAuthoredExistenceRuntime(scene, entry, r_error);
          if (!LN_GameStateSucceeded(runtime_validation)) {
            return runtime_validation;
          }
        }
        if (entry.live_object != nullptr &&
            entry.live_object->GetSaveGameObjectState() != entry.save_object_state)
        {
          r_error = "authored participant '" + entry.name +
                    "' KX persistence metadata diverged from its registry";
          return LN_GameStateResult::InternalError;
        }
        if (participants.size() == kMaxObjects) {
          r_error = "runtime scene exceeds the checkpoint participant limit";
          return LN_GameStateResult::UnsupportedContent;
        }
        CaptureParticipant participant;
        participant.object = entry.live_object;
        participant.uuid = entry.uuid;
        participant.name = entry.name;
        participant.save_object_state = entry.save_object_state;
        participant.exists = entry.live_object != nullptr;
        participant.supported_scale = entry.supported_scale;
        participant.material_slot_count = entry.material_slot_count;
        participant.modifier_baseline = &entry.modifier_baseline;
        if (participant.exists && participant.save_object_state &&
            !capture_runtime_tree_attachments(participant.object, participant))
        {
          return LN_GameStateResult::UnsupportedContent;
        }
        participants.push_back(std::move(participant));
      }
      for (const auto &item : m_spawns) {
        const SpawnEntry &entry = item.second;
        if (!entry.persistent || entry.root == nullptr) {
          continue;
        }
        if (entry.source == nullptr || entry.root->GetScene() != &scene ||
            blender::UUID(entry.root->GetPersistenceSourceUuid()) != entry.source_uuid ||
            blender::UUID(entry.root->GetPersistenceInstanceUuid()) != entry.instance_uuid ||
            !entry.root->GetSaveGameObjectState())
        {
          r_error = "tracked Add Object KX persistence metadata diverged from its registry";
          return LN_GameStateResult::InternalError;
        }
        const LN_GameStateResult source_validation = ValidatePersistentSource(
            scene, *entry.source, entry.life_seconds, false, r_error);
        if (!LN_GameStateSucceeded(source_validation)) {
          return source_validation;
        }
        const LN_GameStateResult runtime_validation = ValidateSpawnRuntime(scene, entry, r_error);
        if (!LN_GameStateSucceeded(runtime_validation)) {
          return runtime_validation;
        }
        if (participants.size() == kMaxObjects) {
          r_error = "runtime scene exceeds the checkpoint participant limit";
          return LN_GameStateResult::UnsupportedContent;
        }
        CaptureParticipant participant;
        participant.object = entry.root;
        participant.uuid = entry.instance_uuid;
        participant.source_uuid = entry.source_uuid;
        participant.name = entry.root->GetName();
        participant.spawned = true;
        participant.save_object_state = true;
        participant.exists = true;
        participant.creation_position = entry.creation_position;
        participant.creation_orientation = entry.creation_orientation;
        participant.creation_scale = entry.creation_scale;
        if (const auto source = m_sourcesByObject.find(entry.source);
            source != m_sourcesByObject.end())
        {
          participant.supported_scale = source->second.supported_scale;
          participant.material_slot_count = source->second.material_slot_count;
          participant.modifier_baseline = &source->second.modifier_baseline;
        }
        if (!capture_runtime_tree_attachments(participant.object, participant)) {
          return LN_GameStateResult::UnsupportedContent;
        }
        participants.push_back(std::move(participant));
      }
    }
    catch (...) {
      r_error = "could not allocate the checkpoint capture snapshot";
      return LN_GameStateResult::InternalError;
    }
    try {
      MaterialCaptureContext material_context(m_materialAssets);
      RuntimeNodeTreeCapture node_tree_capture(m_geometryNodesAssets);
      return capture_checkpoint(
          scene,
          participants,
          material_context,
          m_geometryNodesAssets,
          node_tree_capture,
          r_checkpoint,
          r_error);
    }
    catch (...) {
      r_error = "could not allocate the checkpoint capture caches";
      return LN_GameStateResult::InternalError;
    }
  }

  std::string SceneUuidText() const
  {
    return blender::BLI_uuid_is_nil(m_sceneUuid) ? std::string() : uuid_string(m_sceneUuid);
  }

  struct WorkItem {
    uint64_t request_id = 0;
    uint64_t epoch = 0;
    LN_GameStateOperation operation = LN_GameStateOperation::Save;
    std::string filepath;
    LN_GameStateCompletionTarget target;
    Checkpoint checkpoint;
    bool capture_deferred = false;
  };

  struct Result {
    uint64_t epoch = 0;
    LN_GameStateOperation operation = LN_GameStateOperation::Save;
    std::string filepath;
    LN_GameStateCompletionTarget target;
    Checkpoint checkpoint;
    LN_GameStateResult status = LN_GameStateResult::InternalError;
    std::string error;
  };

  struct WorkerState {
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable exit_condition;
    std::optional<WorkItem> work;
    std::optional<Result> result;
    std::atomic<uint64_t> epoch{1};
    std::atomic<bool> stopping{false};
    bool exited = false;
  };

  struct ActiveOperation {
    LN_GameStateOperation operation = LN_GameStateOperation::Save;
    std::string filepath;
    LN_GameStateCompletionTarget target;
  };

  Impl() : m_workerState(std::make_shared<WorkerState>()) {}

  ~Impl()
  {
    m_pending.clear();
    m_activeTargets.clear();
    const std::shared_ptr<WorkerState> state = m_workerState;
    {
      std::lock_guard lock(state->mutex);
      state->stopping.store(true, std::memory_order_release);
      state->epoch.fetch_add(1, std::memory_order_release);
      state->work.reset();
    }
    state->condition.notify_one();
    if (m_worker.joinable()) {
      m_worker.join();
    }
    ReleaseAllPaths();
  }

  bool CanEnqueue(const LN_GameStateCompletionTarget &target, std::string &r_error)
  {
    if (m_reconciling) {
      r_error = "Save/Load Game request is blocked during reconciliation";
      return false;
    }
    const TargetKey key = target_key(target);
    if (m_activeTargets.size() >= kMaxPendingRequests) {
      r_error = "too many Save/Load Game operations are pending";
      return false;
    }
    if (m_activeTargets.find(key) != m_activeTargets.end()) {
      r_error = "this Save/Load Game node already has an operation pending";
      return false;
    }
    return true;
  }

  bool HasUncommittedLoad() const
  {
    if (m_active && m_active->operation == LN_GameStateOperation::Load) {
      return true;
    }
    return std::any_of(m_pending.begin(), m_pending.end(), [](const WorkItem &work) {
      return work.operation == LN_GameStateOperation::Load;
    });
  }

  bool Enqueue(WorkItem work, std::string &r_error)
  {
    const TargetKey key = target_key(work.target);
    if (m_activeTargets.size() >= kMaxPendingRequests) {
      r_error = "too many Save/Load Game operations are pending";
      return false;
    }
    if (!ReservePath(work.filepath, r_error)) {
      return false;
    }
    bool target_inserted = false;
    try {
      target_inserted = m_activeTargets.insert(key).second;
      if (!target_inserted) {
        ReleasePath(work.filepath);
        r_error = "this Save/Load Game node already has an operation pending";
        return false;
      }
      if (!EnsureWorker(r_error)) {
        m_activeTargets.erase(key);
        ReleasePath(work.filepath);
        return false;
      }
      work.epoch = m_epoch;
      m_pending.push_back(std::move(work));
      if (!m_workerBusy) {
        Result failure;
        if (StartNext(nullptr, failure)) {
          m_activeTargets.erase(target_key(failure.target));
          ReleasePath(failure.filepath);
          r_error = failure.error;
          return false;
        }
      }
    }
    catch (const std::exception &exception) {
      if (target_inserted) {
        m_activeTargets.erase(key);
      }
      ReleasePath(work.filepath);
      r_error = std::string("could not queue Save/Load Game operation: ") + exception.what();
      return false;
    }
    catch (...) {
      if (target_inserted) {
        m_activeTargets.erase(key);
      }
      ReleasePath(work.filepath);
      r_error = "could not queue Save/Load Game operation";
      return false;
    }
    return true;
  }

  std::optional<Result> TakeResult()
  {
    std::optional<Result> result;
    {
      std::lock_guard lock(m_workerState->mutex);
      if (!m_workerState->result) {
        return std::nullopt;
      }
      result = std::move(m_workerState->result);
      m_workerState->result.reset();
    }
    m_workerBusy = false;
    m_active.reset();
    return result;
  }

  bool StartNext(KX_Scene *scene, Result &r_immediate_failure)
  {
    if (m_workerBusy || m_pending.empty()) {
      return false;
    }

    WorkItem work = std::move(m_pending.front());
    m_pending.pop_front();
    try {
      if (work.operation == LN_GameStateOperation::Save && work.capture_deferred) {
        std::string error;
        LN_GameStateResult capture_result = LN_GameStateResult::InternalError;
        try {
          if (scene != nullptr) {
            capture_result = CaptureCheckpoint(*scene, work.checkpoint, error);
          }
        }
        catch (const std::exception &exception) {
          error = std::string("could not capture Save Game data: ") + exception.what();
        }
        catch (...) {
          error = "could not capture Save Game data";
        }
        if (!LN_GameStateSucceeded(capture_result)) {
          r_immediate_failure.epoch = work.epoch;
          r_immediate_failure.operation = work.operation;
          r_immediate_failure.filepath = std::move(work.filepath);
          r_immediate_failure.target = work.target;
          r_immediate_failure.status = capture_result;
          r_immediate_failure.error =
              error.empty() ? "could not capture Save Game data after the pending load" :
                              std::move(error);
          return true;
        }
      }

      work.request_id = next_request_id();
      ActiveOperation active;
      active.operation = work.operation;
      active.filepath = work.filepath;
      active.target = work.target;
      {
        std::lock_guard lock(m_workerState->mutex);
        if (m_workerState->stopping.load(std::memory_order_acquire) || m_workerState->work ||
            m_workerState->result)
        {
          r_immediate_failure.epoch = work.epoch;
          r_immediate_failure.operation = work.operation;
          r_immediate_failure.filepath = std::move(work.filepath);
          r_immediate_failure.target = work.target;
          r_immediate_failure.error = "Save Game worker is unavailable";
          return true;
        }
        m_workerState->work = std::move(work);
      }
      m_active = std::move(active);
      m_workerBusy = true;
      m_workerState->condition.notify_one();
      return false;
    }
    catch (const std::exception &exception) {
      r_immediate_failure.epoch = work.epoch;
      r_immediate_failure.operation = work.operation;
      r_immediate_failure.filepath = std::move(work.filepath);
      r_immediate_failure.target = work.target;
      r_immediate_failure.error = std::string("could not start Save/Load Game operation: ") +
                                  exception.what();
      return true;
    }
    catch (...) {
      r_immediate_failure.epoch = work.epoch;
      r_immediate_failure.operation = work.operation;
      r_immediate_failure.filepath = std::move(work.filepath);
      r_immediate_failure.target = work.target;
      r_immediate_failure.error = "could not start Save/Load Game operation";
      return true;
    }
  }

  std::vector<Result> CancelPending()
  {
    std::vector<Result> cancelled;
    cancelled.reserve(m_pending.size() + size_t(m_active.has_value()));
    auto append_cancelled = [&cancelled](const LN_GameStateOperation operation,
                                         const std::string &filepath,
                                         const LN_GameStateCompletionTarget &target) {
      Result result;
      result.operation = operation;
      result.filepath = filepath;
      result.target = target;
      result.status = LN_GameStateResult::Cancelled;
      result.error = "Save/Load Game operation was cancelled";
      cancelled.push_back(std::move(result));
    };
    if (m_active) {
      append_cancelled(m_active->operation, m_active->filepath, m_active->target);
    }
    for (const WorkItem &work : m_pending) {
      append_cancelled(work.operation, work.filepath, work.target);
    }

    m_epoch++;
    {
      std::lock_guard lock(m_workerState->mutex);
      m_workerState->epoch.store(m_epoch, std::memory_order_release);
    }
    for (const WorkItem &work : m_pending) {
      ReleasePath(work.filepath);
    }
    m_pending.clear();
    m_activeTargets.clear();
    m_active.reset();
    return cancelled;
  }

  uint64_t Epoch() const
  {
    return m_epoch;
  }

  void RemoveActiveTarget(const LN_GameStateCompletionTarget &target)
  {
    m_activeTargets.erase(target_key(target));
  }

  bool ReservePath(const std::string &filepath, std::string &r_error)
  {
    if (!reserve_process_path(filepath, this, r_error)) {
      return false;
    }
    try {
      m_reservedPaths[filepath]++;
    }
    catch (...) {
      release_process_path(filepath, this);
      r_error = "could not track the reserved save path";
      return false;
    }
    return true;
  }

  void ReleasePath(const std::string &filepath)
  {
    const auto found = m_reservedPaths.find(filepath);
    if (found == m_reservedPaths.end()) {
      return;
    }
    release_process_path(filepath, this);
    if (--found->second == 0) {
      m_reservedPaths.erase(found);
    }
  }

  void ReleaseAllPaths()
  {
    for (const auto &item : m_reservedPaths) {
      release_process_path(item.first, this, item.second);
    }
    m_reservedPaths.clear();
  }

 private:
  bool EnsureWorker(std::string &r_error)
  {
    if (m_worker.joinable()) {
      return true;
    }
    try {
      const std::shared_ptr<WorkerState> state = m_workerState;
      m_worker = std::thread([state]() { WorkerMain(state); });
    }
    catch (const std::exception &exception) {
      r_error = std::string("could not start Save Game worker: ") + exception.what();
      return false;
    }
    catch (...) {
      r_error = "could not start Save Game worker";
      return false;
    }
    return true;
  }

  static void WorkerMain(const std::shared_ptr<WorkerState> &state)
  {
    while (true) {
      WorkItem work;
      {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&state]() {
          return state->stopping.load(std::memory_order_acquire) || state->work;
        });
        if (state->stopping.load(std::memory_order_acquire)) {
          break;
        }
        work = std::move(*state->work);
        state->work.reset();
      }

      Result result;
      result.epoch = work.epoch;
      result.operation = work.operation;
      result.filepath = work.filepath;
      result.target = work.target;
      try {
        if (work.operation == LN_GameStateOperation::Save) {
          result.status = write_checkpoint_atomic(
              work.filepath,
              work.checkpoint,
              work.request_id,
              [state, epoch = work.epoch](const std::string &temporary_path,
                                          const std::string &destination_path,
                                          std::string &r_error) -> LN_GameStateResult {
                /* Serialize the final epoch check with cancellation. Once CancelPending returns,
                 * an operation that had not committed can no longer publish its temporary file. */
                std::lock_guard lock(state->mutex);
                if (state->stopping.load(std::memory_order_acquire) ||
                    state->epoch.load(std::memory_order_acquire) != epoch)
                {
                  r_error = "Save Game operation was cancelled";
                  return LN_GameStateResult::Cancelled;
                }
                return commit_checkpoint_temporary(temporary_path, destination_path, r_error);
              },
              result.error);
        }
        else {
          result.status = read_checkpoint(work.filepath, result.checkpoint, result.error);
        }
      }
      catch (const std::exception &exception) {
        result.error = std::string("Save Game I/O failed: ") + exception.what();
      }
      catch (...) {
        result.error = "Save Game I/O failed with an unknown error";
      }

      {
        std::lock_guard lock(state->mutex);
        if (state->stopping.load(std::memory_order_acquire)) {
          break;
        }
        state->result = std::move(result);
      }
    }
    {
      std::lock_guard lock(state->mutex);
      state->exited = true;
    }
    state->exit_condition.notify_all();
  }

  std::shared_ptr<WorkerState> m_workerState;
  KX_Scene *m_scene = nullptr;
  blender::UUID m_sceneUuid;
  bool m_registryValid = false;
  bool m_identityCatalogValid = false;
  std::string m_identityCatalogError;
  std::unordered_map<blender::UUID, AuthoredEntry, UuidHash> m_authored;
  std::unordered_map<KX_GameObject *, blender::UUID> m_authoredByObject;
  std::unordered_map<KX_GameObject *, SourceEntry> m_sourcesByObject;
  std::unordered_map<blender::UUID, KX_GameObject *, UuidHash> m_sourcesByUuid;
  std::unordered_set<KX_GameObject *> m_duplicateIdentityObjects;
  std::unordered_set<blender::UUID, UuidHash> m_authoredAndSourceUuids;
  MaterialAssetCatalog m_materialAssets;
  GeometryNodesAssetCatalog m_geometryNodesAssets;
  std::unordered_map<KX_GameObject *, SpawnEntry> m_spawns;
  std::unordered_map<KX_GameObject *, KX_GameObject *> m_spawnRootByMember;
  std::unordered_map<blender::UUID, KX_GameObject *, UuidHash> m_spawnByInstanceUuid;
  std::unordered_set<blender::UUID, UuidHash> m_usedInstanceUuids;
  std::deque<WorkItem> m_pending;
  std::unordered_set<TargetKey, TargetKeyHash> m_activeTargets;
  std::unordered_map<std::string, size_t> m_reservedPaths;
  std::optional<ActiveOperation> m_active;
  uint64_t m_epoch = 1;
  bool m_workerBusy = false;
  bool m_reconciling = false;
  std::thread m_worker;
};

LN_GameStateService::LN_GameStateService() : m_impl(std::make_unique<Impl>()) {}

LN_GameStateService::~LN_GameStateService() = default;

LN_GameStateLoadPayload::LN_GameStateLoadPayload(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

LN_GameStateLoadPayload::~LN_GameStateLoadPayload() = default;

LN_GameStateLoadPayload::LN_GameStateLoadPayload(LN_GameStateLoadPayload &&) noexcept = default;

LN_GameStateLoadPayload &LN_GameStateLoadPayload::operator=(
    LN_GameStateLoadPayload &&) noexcept = default;

const std::string &LN_GameStateLoadPayload::GetSceneUuid() const
{
  static const std::string empty;
  return m_impl ? m_impl->checkpoint.scene_uuid : empty;
}

const std::string &LN_GameStateLoadPayload::GetSceneName() const
{
  static const std::string empty;
  return m_impl ? m_impl->checkpoint.scene_name : empty;
}

LN_GameStateResult LN_GameStateService::InitializeRegistry(KX_Scene &scene, std::string &r_error)
{
  return m_impl->InitializeRegistry(scene, r_error);
}

LN_GameStateResult LN_GameStateService::PreflightAddObject(KX_Scene &scene,
                                                           KX_GameObject &source,
                                                           const float life_seconds,
                                                           const bool full_copy,
                                                           std::string &r_error) const
{
  return m_impl->PreflightAddObject(scene, source, life_seconds, full_copy, r_error);
}

LN_GameStateResult LN_GameStateService::RegisterAddedObject(
    KX_Scene &scene,
    KX_GameObject &source,
    KX_GameObject &root,
    const std::vector<KX_GameObject *> &members,
    const float life_seconds,
    const bool full_copy,
    std::string &r_error)
{
  return m_impl->RegisterAddedObject(
      scene, source, root, members, life_seconds, full_copy, r_error);
}

LN_GameStateResult LN_GameStateService::SetSaveExistence(KX_Scene &scene,
                                                         KX_GameObject *root,
                                                         const bool enabled,
                                                         std::string &r_error)
{
  return m_impl->SetSaveExistence(scene, root, enabled, r_error);
}

void LN_GameStateService::NotifyObjectRemoved(KX_GameObject &object,
                                              const KX_ObjectRemovalReason reason)
{
  m_impl->NotifyObjectRemoved(object, reason);
}

bool LN_GameStateService::ResolveFilePath(const int32_t slot,
                                          const std::string &path,
                                          const std::string &scene_uuid,
                                          const std::string &user_data_root,
                                          std::string &r_filepath,
                                          std::string &r_error)
{
  r_filepath.clear();
  r_error.clear();
  if (slot < 0) {
    r_error = "save slot must be zero or greater";
    return false;
  }
  if (path.find('\0') != std::string::npos) {
    r_error = "save path contains an embedded null byte";
    return false;
  }

  std::string requested_string = path;
  if (requested_string.empty()) {
    if (!is_canonical_uuid(scene_uuid)) {
      r_error = "default save path requires a durable checkpoint Scene UUID";
      return false;
    }
    std::string root = user_data_root;
    if (root.empty()) {
      const std::optional<std::string> platform_root =
          blender::BKE_appdir_folder_id_user_notest(blender::BLENDER_USER_DATAFILES, nullptr);
      if (!platform_root || platform_root->empty()) {
        r_error = "platform user-data directory is unavailable";
        return false;
      }
      root = *platform_root;
    }
    std::filesystem::path default_path = std::filesystem::path(root) / "upbge" / "savegames" /
                                         scene_uuid /
                                         ("save" + std::to_string(slot) + ".json");
    requested_string = default_path.string();
  }
  if (requested_string.size() >= FILE_MAX) {
    r_error = "save path exceeds the platform path limit";
    return false;
  }
  if (!blender::BLI_path_extension_check_n(requested_string.c_str(), ".json", ".sav", nullptr)) {
    const std::string filename = "save" + std::to_string(slot) + ".json";
    const bool needs_separator = requested_string.back() != '/' && requested_string.back() != '\\';
    if (requested_string.size() + size_t(needs_separator) + filename.size() >= FILE_MAX) {
      r_error = "save path exceeds the platform path limit";
      return false;
    }
    if (needs_separator) {
      requested_string.push_back('/');
    }
    requested_string += filename;
  }
  if (!blender::BLI_path_is_rel(requested_string.c_str()) &&
      !blender::BLI_path_is_abs_from_cwd(requested_string.c_str()))
  {
    requested_string.insert(0, "//");
  }
  if (requested_string.size() >= FILE_MAX) {
    r_error = "save path exceeds the platform path limit";
    return false;
  }

  char expanded[FILE_MAX];
  std::snprintf(expanded, sizeof(expanded), "%s", requested_string.c_str());
  if (blender::BLI_path_is_rel(expanded)) {
    const std::string &runtime_path = KX_GetMainPath().empty() ? KX_GetOrigPath() :
                                                                 KX_GetMainPath();
    if (runtime_path.size() >= FILE_MAX ||
        (!runtime_path.empty() && runtime_path.size() + requested_string.size() >= FILE_MAX))
    {
      r_error = "resolved save path exceeds the platform path limit";
      return false;
    }
    char base_path[FILE_MAX];
    std::snprintf(base_path, sizeof(base_path), "%s", runtime_path.c_str());
    if (!blender::BLI_path_is_rel(base_path) && base_path[0] != '\0') {
      if (!blender::BLI_path_is_abs_from_cwd(base_path)) {
        blender::BLI_path_abs_from_cwd(base_path, sizeof(base_path));
      }
      if (!blender::BLI_path_is_abs_from_cwd(base_path)) {
        r_error = "runtime project path could not be resolved";
        return false;
      }
      blender::BLI_path_abs(expanded, base_path);
    }
    else {
      std::memmove(expanded, expanded + 2, std::strlen(expanded + 2) + 1);
      blender::BLI_path_abs_from_cwd(expanded, sizeof(expanded));
    }
  }
  blender::BLI_path_normalize(expanded);
  if (expanded[0] == '\0' || !blender::BLI_path_is_abs_from_cwd(expanded)) {
    r_error = "save path could not be resolved";
    return false;
  }
  blender::BLI_path_canonicalize_native(expanded, sizeof(expanded));
  std::error_code canonical_error;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(std::filesystem::path(expanded), canonical_error);
  r_filepath = canonical_error ? std::string(expanded) : canonical.string();
  return true;
}

bool LN_GameStateService::RequestSave(KX_Scene &scene,
                                      const int32_t slot,
                                      const std::string &path,
                                      const LN_GameStateCompletionTarget &target,
                                      std::string &r_error)
{
  if (!target.runtime_tree_id.IsValid() || target.expression_index == LN_INVALID_INDEX) {
    r_error = "Save Game completion target is invalid";
    return false;
  }

  Impl::WorkItem work;
  work.operation = LN_GameStateOperation::Save;
  work.target = target;
  if (!ResolveFilePath(
          slot, path, m_impl->SceneUuidText(), std::string(), work.filepath, r_error) ||
      !m_impl->CanEnqueue(target, r_error))
  {
    return false;
  }
  work.capture_deferred = m_impl->HasUncommittedLoad();
  if (!work.capture_deferred) {
    try {
      if (!LN_GameStateSucceeded(
              m_impl->CaptureCheckpoint(scene, work.checkpoint, r_error)))
      {
        return false;
      }
    }
    catch (const std::exception &exception) {
      r_error = std::string("could not capture Save Game data: ") + exception.what();
      return false;
    }
    catch (...) {
      r_error = "could not capture Save Game data";
      return false;
    }
  }
  return m_impl->Enqueue(std::move(work), r_error);
}

bool LN_GameStateService::RequestLoad(const int32_t slot,
                                      const std::string &path,
                                      const LN_GameStateCompletionTarget &target,
                                      std::string &r_error)
{
  if (!target.runtime_tree_id.IsValid() || target.expression_index == LN_INVALID_INDEX) {
    r_error = "Load Game completion target is invalid";
    return false;
  }

  Impl::WorkItem work;
  work.operation = LN_GameStateOperation::Load;
  work.target = target;
  if (!ResolveFilePath(
          slot, path, m_impl->SceneUuidText(), std::string(), work.filepath, r_error) ||
      !m_impl->CanEnqueue(target, r_error))
  {
    return false;
  }
  return m_impl->Enqueue(std::move(work), r_error);
}

void LN_GameStateService::Poll(KX_Scene &scene,
                               const CompletionFn &completion_fn,
                               const SceneTransitionFn &scene_transition_fn)
{
  while (true) {
    std::optional<Impl::Result> result = m_impl->TakeResult();
    if (result) {
      bool transferred = false;
      if (result->epoch == m_impl->Epoch()) {
        m_impl->RemoveActiveTarget(result->target);
        if (LN_GameStateSucceeded(result->status) &&
            result->operation == LN_GameStateOperation::Load)
        {
          if (scene_transition_fn &&
              result->checkpoint.scene_uuid != m_impl->SceneUuidText())
          {
            try {
              std::unique_ptr<LN_GameStateLoadPayload> payload(
                  new LN_GameStateLoadPayload(std::make_unique<LN_GameStateLoadPayload::Impl>(
                      std::move(result->checkpoint))));
              result->status = scene_transition_fn(
                  result->target, result->filepath, std::move(payload), result->error);
              transferred = LN_GameStateSucceeded(result->status);
            }
            catch (const std::exception &exception) {
              result->status = LN_GameStateResult::InternalError;
              result->error =
                  std::string("could not transfer Save Game data: ") + exception.what();
            }
            catch (...) {
              result->status = LN_GameStateResult::InternalError;
              result->error = "could not transfer Save Game data";
            }
          }
          else {
            try {
              result->status = m_impl->ApplyCheckpoint(
                  scene, result->checkpoint, result->error);
            }
            catch (const std::exception &exception) {
              result->status = LN_GameStateResult::InternalError;
              result->error =
                  std::string("could not apply Save Game data: ") + exception.what();
            }
            catch (...) {
              result->status = LN_GameStateResult::InternalError;
              result->error = "could not apply Save Game data";
            }
          }
        }
        if (!transferred) {
          completion_fn(
              result->target, result->operation, result->filepath, result->status, result->error);
        }
      }
      m_impl->ReleasePath(result->filepath);
      if (transferred) {
        /* Scene replacement is a terminal ordering barrier for this scene-owned service. A
         * deferred Save after the Load must never capture the source scene while the decoded
         * checkpoint is waiting for its destination scene to be constructed. */
        for (Impl::Result &cancelled : m_impl->CancelPending()) {
          completion_fn(cancelled.target,
                        cancelled.operation,
                        cancelled.filepath,
                        cancelled.status,
                        cancelled.error);
        }
        return;
      }
      continue;
    }

    Impl::Result immediate_failure;
    if (m_impl->StartNext(&scene, immediate_failure)) {
      m_impl->RemoveActiveTarget(immediate_failure.target);
      m_impl->ReleasePath(immediate_failure.filepath);
      completion_fn(immediate_failure.target,
                    immediate_failure.operation,
                    immediate_failure.filepath,
                    immediate_failure.status,
                    immediate_failure.error);
      continue;
    }
    break;
  }
}

LN_GameStateResult LN_GameStateService::ApplyTransferredLoad(
    KX_Scene &scene, const LN_GameStateLoadPayload &payload, std::string &r_error)
{
  if (!payload.m_impl) {
    r_error = "transferred Save Game payload is empty";
    return LN_GameStateResult::InvalidRequest;
  }
  try {
    return m_impl->ApplyCheckpoint(scene, payload.m_impl->checkpoint, r_error);
  }
  catch (const std::exception &exception) {
    r_error = std::string("could not apply Save Game data: ") + exception.what();
  }
  catch (...) {
    r_error = "could not apply Save Game data";
  }
  return LN_GameStateResult::InternalError;
}

void LN_GameStateService::CancelPending(const CompletionFn &completion_fn)
{
  std::vector<Impl::Result> cancelled = m_impl->CancelPending();
  if (!completion_fn) {
    return;
  }
  for (const Impl::Result &result : cancelled) {
    completion_fn(result.target,
                  result.operation,
                  result.filepath,
                  LN_GameStateResult::Cancelled,
                  result.error);
  }
}
