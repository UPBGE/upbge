/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateCodec.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameStateInternal.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"

#include "BLI_hash_md5.hh"
#include "BLI_serialize.hh"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "DNA_ID.h"

namespace upbge::logic_nodes::game_state {

using blender::io::serialize::ArrayValue;
using blender::io::serialize::BooleanValue;
using blender::io::serialize::DictionaryValue;
using blender::io::serialize::DoubleValue;
using blender::io::serialize::IntValue;
using blender::io::serialize::JsonFormatter;
using blender::io::serialize::NullValue;
using blender::io::serialize::StringValue;
using blender::io::serialize::Value;

bool is_valid_orientation(const MT_Matrix3x3 &value)
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (!std::isfinite(value[row][column])) {
        return false;
      }
    }
    if (std::abs(value[row].dot(value[row]) - 1.0f) > kOrientationTolerance) {
      return false;
    }
    for (int other = row + 1; other < 3; other++) {
      if (std::abs(value[row].dot(value[other])) > kOrientationTolerance) {
        return false;
      }
    }
  }
  return std::abs(value.determinant() - 1.0f) <= kOrientationTolerance;
}

bool is_nonsingular_scale(const MT_Vector3 &value)
{
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(value[axis]) ||
        std::abs(value[axis]) < std::numeric_limits<float>::epsilon())
    {
      return false;
    }
  }
  return true;
}

bool contains_embedded_null(const std::string &value)
{
  return value.find('\0') != std::string::npos;
}

bool is_valid_checkpoint_text(const std::string &value,
                              const size_t maximum,
                              const bool allow_empty)
{
  return (allow_empty || !value.empty()) && value.size() <= maximum &&
         !contains_embedded_null(value) &&
         blender::BLI_str_utf8_invalid_byte(value.data(), value.size()) == -1;
}

bool consume_budget(size_t &remaining, const size_t bytes)
{
  if (bytes > remaining) {
    return false;
  }
  remaining -= bytes;
  return true;
}

namespace {

bool has_safe_json_nesting(const std::string &contents)
{
  size_t depth = 0;
  bool inside_string = false;
  bool escaped = false;
  for (const char character : contents) {
    if (inside_string) {
      if (escaped) {
        escaped = false;
      }
      else if (character == '\\') {
        escaped = true;
      }
      else if (character == '"') {
        inside_string = false;
      }
      continue;
    }
    if (character == '"') {
      inside_string = true;
    }
    else if (character == '{' || character == '[') {
      if (++depth > kMaxJsonNestingDepth) {
        return false;
      }
    }
    else if (character == '}' || character == ']') {
      if (depth == 0) {
        return false;
      }
      depth--;
    }
  }
  return !inside_string && depth == 0;
}

class StrictJsonSax : public nlohmann::json_sax<nlohmann::ordered_json> {
 public:
  using Json = nlohmann::ordered_json;
  using string_t = Json::string_t;
  using binary_t = Json::binary_t;
  using number_integer_t = Json::number_integer_t;
  using number_unsigned_t = Json::number_unsigned_t;
  using number_float_t = Json::number_float_t;

  bool null() override
  {
    return consume_memory(kScalarMemoryBytes);
  }
  bool boolean(bool) override
  {
    return consume_memory(kScalarMemoryBytes);
  }
  bool number_integer(number_integer_t) override
  {
    return consume_memory(kScalarMemoryBytes);
  }
  bool number_unsigned(number_unsigned_t) override
  {
    return consume_memory(kScalarMemoryBytes);
  }
  bool number_float(number_float_t, const string_t &) override
  {
    return consume_memory(kScalarMemoryBytes);
  }
  bool string(string_t &value) override
  {
    return consume_text(value) && consume_memory(kValueMemoryBytes + value.size());
  }
  bool binary(binary_t &) override
  {
    error = "binary values are not valid checkpoint JSON";
    return false;
  }
  bool start_object(std::size_t) override
  {
    return push(true);
  }
  bool key(string_t &value) override
  {
    if (frames.empty() || !frames.back().object) {
      error = "JSON key appeared outside an object";
      return false;
    }
    if (!consume_text(value)) {
      return false;
    }
    if (!consume_memory(kKeyMemoryBytes + value.size())) {
      return false;
    }
    if (!frames.back().keys.insert(value).second) {
      error = "duplicate JSON key '" + value + "'";
      return false;
    }
    return true;
  }
  bool end_object() override
  {
    return pop(true);
  }
  bool start_array(std::size_t) override
  {
    return push(false);
  }
  bool end_array() override
  {
    return pop(false);
  }
  bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &exception)
      override
  {
    error = exception.what();
    return false;
  }

  std::string error;

 private:
  struct Frame {
    explicit Frame(const bool is_object) : object(is_object) {}
    bool object;
    std::unordered_set<std::string> keys;
  };

  static constexpr size_t kScalarMemoryBytes = 64u;
  static constexpr size_t kValueMemoryBytes = 96u;
  static constexpr size_t kKeyMemoryBytes = 96u;
  static constexpr size_t kContainerMemoryBytes = 128u;

  bool consume_text(const std::string &value)
  {
    if (value.size() > kMaxStringPropertyBytes || contains_embedded_null(value) ||
        blender::BLI_str_utf8_invalid_byte(value.data(), value.size()) != -1 ||
        decoded_text_bytes > kMaxFileBytes - value.size())
    {
      error = "decoded JSON text exceeds the checkpoint limit";
      return false;
    }
    decoded_text_bytes += value.size();
    return true;
  }

  bool consume_memory(const size_t bytes)
  {
    if (bytes > kMaxDecodedMemoryBytes - decoded_memory_bytes) {
      error = "decoded JSON structure exceeds the checkpoint memory limit";
      return false;
    }
    decoded_memory_bytes += bytes;
    return true;
  }

  bool push(const bool object)
  {
    if (frames.size() >= kMaxJsonNestingDepth) {
      error = "JSON nesting exceeds the checkpoint limit";
      return false;
    }
    if (!consume_memory(kContainerMemoryBytes)) {
      return false;
    }
    frames.emplace_back(object);
    return true;
  }

  bool pop(const bool object)
  {
    if (frames.empty() || frames.back().object != object) {
      error = "invalid JSON container nesting";
      return false;
    }
    frames.pop_back();
    return true;
  }

  std::vector<Frame> frames;
  size_t decoded_text_bytes = 0;
  size_t decoded_memory_bytes = 0;
};

bool validate_strict_json(const std::string &contents, std::string &r_error)
{
  StrictJsonSax sax;
  try {
    if (!nlohmann::ordered_json::sax_parse(contents.begin(),
                                           contents.end(),
                                           &sax,
                                           nlohmann::json::input_format_t::json,
                                           true,
                                           false))
    {
      r_error = sax.error.empty() ? "save file contains malformed JSON" : std::move(sax.error);
      return false;
    }
  }
  catch (const std::exception &exception) {
    r_error = std::string("save file contains malformed JSON: ") + exception.what();
    return false;
  }
  return true;
}

}  // namespace

std::string uuid_string(const blender::bUUID &uuid)
{
  char text[UUID_STRING_SIZE];
  blender::BLI_uuid_format(text, uuid);
  return text;
}

namespace {

void append_vector3(ArrayValue &array, const MT_Vector3 &value)
{
  array.append_double(value.x());
  array.append_double(value.y());
  array.append_double(value.z());
}

const char *material_socket_value_type_name(const MaterialSocketValueType type)
{
  switch (type) {
    case MaterialSocketValueType::Bool:
      return "bool";
    case MaterialSocketValueType::Int:
      return "int";
    case MaterialSocketValueType::Menu:
      return "menu";
    case MaterialSocketValueType::IntVector:
      return "int_vector";
    case MaterialSocketValueType::Float:
      return "float";
    case MaterialSocketValueType::Vector:
      return "vector";
    case MaterialSocketValueType::Rotation:
      return "rotation";
    case MaterialSocketValueType::Color:
      return "color";
    case MaterialSocketValueType::String:
      return "string";
    case MaterialSocketValueType::Object:
      return "object";
    case MaterialSocketValueType::Image:
      return "image";
    case MaterialSocketValueType::Collection:
      return "collection";
    case MaterialSocketValueType::Texture:
      return "texture";
    case MaterialSocketValueType::Material:
      return "material";
    case MaterialSocketValueType::Font:
      return "font";
    case MaterialSocketValueType::Scene:
      return "scene";
    case MaterialSocketValueType::Text:
      return "text";
    case MaterialSocketValueType::Mask:
      return "mask";
    case MaterialSocketValueType::Sound:
      return "sound";
  }
  return "";
}

void append_material_socket_value(DictionaryValue &input,
                                  const MaterialSocketValueState &state)
{
  input.append_str("socket", state.socket_identifier);
  input.append_str("type", material_socket_value_type_name(state.type));
  switch (state.type) {
    case MaterialSocketValueType::Bool:
      input.append("value", std::make_shared<BooleanValue>(state.bool_value));
      break;
    case MaterialSocketValueType::Int:
    case MaterialSocketValueType::Menu:
      input.append_int("value", state.int_value);
      break;
    case MaterialSocketValueType::IntVector: {
      input.append_int("dimensions", state.dimensions);
      std::shared_ptr<ArrayValue> values = input.append_array("value");
      for (size_t index = 0; index < state.dimensions; ++index) {
        values->append_int(state.int_values[index]);
      }
      break;
    }
    case MaterialSocketValueType::Float:
      input.append_double("value", state.float_values[0]);
      break;
    case MaterialSocketValueType::Vector:
      input.append_int("dimensions", state.dimensions);
      [[fallthrough]];
    case MaterialSocketValueType::Rotation:
    case MaterialSocketValueType::Color: {
      std::shared_ptr<ArrayValue> values = input.append_array("value");
      const size_t count = state.type == MaterialSocketValueType::Color ?
                               4u :
                           state.type == MaterialSocketValueType::Vector ?
                               state.dimensions :
                               3u;
      for (size_t index = 0; index < count; ++index) {
        values->append_double(state.float_values[index]);
      }
      break;
    }
    case MaterialSocketValueType::String:
      input.append_str("value", state.string_value);
      break;
    case MaterialSocketValueType::Object:
    case MaterialSocketValueType::Image:
    case MaterialSocketValueType::Collection:
    case MaterialSocketValueType::Texture:
    case MaterialSocketValueType::Material:
    case MaterialSocketValueType::Font:
    case MaterialSocketValueType::Scene:
    case MaterialSocketValueType::Text:
    case MaterialSocketValueType::Mask:
    case MaterialSocketValueType::Sound:
      if (!state.datablock_value.has_value) {
        input.append("value", std::make_shared<NullValue>());
      }
      else {
        std::shared_ptr<DictionaryValue> value = input.append_dict("value");
        value->append_str("name", state.datablock_value.name);
        value->append_str("library", state.datablock_value.library_path);
      }
      break;
  }
}

const char *runtime_node_tree_type_name(const RuntimeNodeTreeType type)
{
  switch (type) {
    case RuntimeNodeTreeType::Geometry:
      return "geometry";
    case RuntimeNodeTreeType::Compositor:
      return "compositor";
  }
  return "";
}

void append_runtime_node_tree_state(DictionaryValue &root, const Checkpoint &checkpoint)
{
  std::shared_ptr<ArrayValue> trees = root.append_array("runtime_node_trees");
  for (const RuntimeNodeTreeState &tree_state : checkpoint.runtime_node_trees) {
    std::shared_ptr<DictionaryValue> tree = trees->append_dict();
    tree->append_str("type", runtime_node_tree_type_name(tree_state.type));
    tree->append_str("name", tree_state.name);
    tree->append_str("library", tree_state.library_path);
    tree->append_str("fingerprint", tree_state.structural_fingerprint);
    std::shared_ptr<ArrayValue> nodes = tree->append_array("nodes");
    for (const RuntimeNodeState &node_state : tree_state.nodes) {
      std::shared_ptr<DictionaryValue> node = nodes->append_dict();
      node->append_int("id", node_state.node_identifier);
      node->append_str("type", node_state.node_type);
      node->append("muted", std::make_shared<BooleanValue>(node_state.muted));
      std::shared_ptr<ArrayValue> inputs = node->append_array("inputs");
      for (const MaterialSocketValueState &input_state : node_state.inputs) {
        std::shared_ptr<DictionaryValue> input = inputs->append_dict();
        append_material_socket_value(*input, input_state);
      }
    }
  }

  std::shared_ptr<DictionaryValue> compositor = root.append_dict("compositor_state");
  compositor->append(
      "has_node_tree", std::make_shared<BooleanValue>(checkpoint.compositor.has_node_tree));
  if (checkpoint.compositor.has_node_tree) {
    compositor->append_str("name", checkpoint.compositor.node_tree_name);
    compositor->append_str("library", checkpoint.compositor.node_tree_library_path);
    compositor->append_str(
        "fingerprint", checkpoint.compositor.node_tree_structural_fingerprint);
  }
}

const char *geometry_nodes_input_mode_name(const GeometryNodesInputMode mode)
{
  switch (mode) {
    case GeometryNodesInputMode::Fallback:
      return "fallback";
    case GeometryNodesInputMode::Value:
      return "value";
    case GeometryNodesInputMode::Attribute:
      return "attribute";
    case GeometryNodesInputMode::Layer:
      return "layer";
  }
  return "";
}

void append_runtime_structure_state(DictionaryValue &object, const ObjectState &state)
{
  std::shared_ptr<ArrayValue> logic_trees = object.append_array("logic_trees");
  for (const LogicTreeAttachmentState &tree_state : state.logic_trees) {
    std::shared_ptr<DictionaryValue> tree = logic_trees->append_dict();
    tree->append_str("name", tree_state.tree_name);
    tree->append_str("library", tree_state.library_path);
    tree->append_int("applied_index", tree_state.applied_tree_index);
    tree->append("enabled", std::make_shared<BooleanValue>(tree_state.enabled));
  }

  std::shared_ptr<DictionaryValue> modifiers = object.append_dict("modifier_state");
  std::shared_ptr<ArrayValue> opaque = modifiers->append_array("authored");
  for (const NonGeometryModifierState &modifier_state : state.non_geometry_modifiers) {
    std::shared_ptr<DictionaryValue> modifier = opaque->append_dict();
    modifier->append_str("name", modifier_state.name);
    modifier->append_int("type", modifier_state.type);
    modifier->append_int("persistent_uid", modifier_state.persistent_uid);
    modifier->append_int("mode", modifier_state.mode);
  }

  std::shared_ptr<ArrayValue> geometry_nodes = modifiers->append_array("geometry_nodes");
  for (const GeometryNodesModifierState &modifier_state : state.geometry_nodes_modifiers) {
    std::shared_ptr<DictionaryValue> modifier = geometry_nodes->append_dict();
    modifier->append_str("name", modifier_state.name);
    modifier->append_int("persistent_uid", modifier_state.persistent_uid);
    modifier->append_int("stack_index", modifier_state.stack_index);
    modifier->append_int("mode", modifier_state.mode);
    std::shared_ptr<DictionaryValue> node_group = modifier->append_dict("node_group");
    node_group->append_str("name", modifier_state.node_group_name);
    node_group->append_str("library", modifier_state.node_group_library_path);
    node_group->append_str("fingerprint", modifier_state.node_group_fingerprint);
    std::shared_ptr<ArrayValue> inputs = modifier->append_array("inputs");
    for (const GeometryNodesInputState &input_state : modifier_state.inputs) {
      std::shared_ptr<DictionaryValue> input = inputs->append_dict();
      input->append_str("socket", input_state.socket_identifier);
      input->append_str("mode", geometry_nodes_input_mode_name(input_state.mode));
      switch (input_state.mode) {
        case GeometryNodesInputMode::Value: {
          std::shared_ptr<DictionaryValue> value = input->append_dict("value");
          append_material_socket_value(*value, input_state.value);
          break;
        }
        case GeometryNodesInputMode::Attribute:
          input->append_str("attribute", input_state.attribute_name);
          break;
        case GeometryNodesInputMode::Layer:
          input->append_str("layer", input_state.layer_name);
          break;
        case GeometryNodesInputMode::Fallback:
          break;
      }
    }
    std::shared_ptr<ArrayValue> outputs = modifier->append_array("outputs");
    for (const GeometryNodesOutputState &output_state : modifier_state.outputs) {
      std::shared_ptr<DictionaryValue> output = outputs->append_dict();
      output->append_str("socket", output_state.socket_identifier);
      output->append_str("attribute", output_state.attribute_name);
    }
  }
}

const char *rigid_body_mode_name(const RigidBodyMode mode)
{
  switch (mode) {
    case RigidBodyMode::Dynamic:
      return "dynamic";
    case RigidBodyMode::StaticCollider:
      return "static_collider";
    case RigidBodyMode::Ghost:
      return "ghost";
    case RigidBodyMode::NoCollision:
      return "no_collision";
  }
  return "";
}

const char *rigid_body_runtime_motion_type_name(const RigidBodyRuntimeMotionType type)
{
  switch (type) {
    case RigidBodyRuntimeMotionType::Authored:
      return "authored";
    case RigidBodyRuntimeMotionType::Dynamic:
      return "dynamic";
    case RigidBodyRuntimeMotionType::Kinematic:
      return "kinematic";
  }
  return "";
}

/** Plain values as JSON; vectors, objects and dictionaries as `{"<kind>": ...}`, lists as arrays. */
std::shared_ptr<Value> encode_property_data(const PropertyData &data)
{
  switch (data.kind) {
    case PropertyData::Kind::Bool:
      return std::make_shared<BooleanValue>(data.bool_value);
    case PropertyData::Kind::Int:
      return std::make_shared<IntValue>(data.int_value);
    case PropertyData::Kind::Float:
      return std::make_shared<DoubleValue>(data.float_value);
    case PropertyData::Kind::String:
      return std::make_shared<StringValue>(data.text);
    case PropertyData::Kind::Vector3:
    case PropertyData::Kind::Vector4: {
      const bool vector3 = data.kind == PropertyData::Kind::Vector3;
      std::shared_ptr<DictionaryValue> tagged = std::make_shared<DictionaryValue>();
      std::shared_ptr<ArrayValue> vector = tagged->append_array(vector3 ? "vector3" : "vector4");
      for (int axis = 0; axis < (vector3 ? 3 : 4); axis++) {
        vector->append_double(data.vector[axis]);
      }
      return tagged;
    }
    case PropertyData::Kind::Object: {
      std::shared_ptr<DictionaryValue> tagged = std::make_shared<DictionaryValue>();
      if (data.text.empty()) {
        tagged->append("object", std::make_shared<NullValue>());
      }
      else {
        tagged->append_str(data.object_by_name ? "object_name" : "object", data.text);
      }
      return tagged;
    }
    case PropertyData::Kind::List: {
      std::shared_ptr<ArrayValue> list = std::make_shared<ArrayValue>();
      for (const PropertyData &item : data.items) {
        list->append(encode_property_data(item));
      }
      return list;
    }
    case PropertyData::Kind::Dict: {
      std::shared_ptr<DictionaryValue> tagged = std::make_shared<DictionaryValue>();
      std::shared_ptr<DictionaryValue> dict = tagged->append_dict("dict");
      for (const auto &entry : data.entries) {
        dict->append(entry.first, encode_property_data(entry.second));
      }
      return tagged;
    }
  }
  return std::make_shared<NullValue>();
}

const char *structured_property_type_name(const PropertyType type)
{
  switch (type) {
    case PropertyType::Vector3:
      return "vector3";
    case PropertyType::Vector4:
      return "vector4";
    case PropertyType::Object:
      return "object";
    case PropertyType::List:
      return "list";
    case PropertyType::Dict:
      return "dict";
    default:
      return "";
  }
}

std::shared_ptr<DictionaryValue> encode_checkpoint_root(const Checkpoint &checkpoint)
{
  auto root = std::make_shared<DictionaryValue>();
  root->append_str("format", kFormatName);
  root->append_str("scene_uuid", checkpoint.scene_uuid);
  if (!checkpoint.scene_name.empty()) {
    root->append_str("scene_name", checkpoint.scene_name);
  }
  auto append_transform = [](DictionaryValue &object,
                             const char *key,
                             const MT_Vector3 &position_value,
                             const MT_Matrix3x3 &orientation_value,
                             const MT_Vector3 &scale_value) {
    std::shared_ptr<DictionaryValue> transform = object.append_dict(key);
    std::shared_ptr<ArrayValue> position = transform->append_array("position");
    append_vector3(*position, position_value);
    std::shared_ptr<ArrayValue> orientation = transform->append_array("orientation");
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        orientation->append_double(orientation_value[row][column]);
      }
    }
    std::shared_ptr<ArrayValue> scale = transform->append_array("scale");
    append_vector3(*scale, scale_value);
  };
  auto append_properties = [](DictionaryValue &object, const ObjectState &state) {
    std::shared_ptr<ArrayValue> properties = object.append_array("properties");
    for (const PropertyState &state_property : state.properties) {
      std::shared_ptr<DictionaryValue> property = properties->append_dict();
      property->append_str("name", state_property.name);
      property->append("exists", std::make_shared<BooleanValue>(state_property.exists));
      property->append("timer", std::make_shared<BooleanValue>(state_property.timer));
      switch (state_property.type) {
        case PropertyType::Bool:
          property->append_str("type", "bool");
          if (state_property.exists) {
            property->append("value", std::make_shared<BooleanValue>(state_property.bool_value));
          }
          break;
        case PropertyType::Int:
          property->append_str("type", "int");
          if (state_property.exists) {
            property->append_int("value", state_property.int_value);
          }
          break;
        case PropertyType::Float:
          property->append_str("type", "float");
          if (state_property.exists) {
            property->append_double("value", state_property.float_value);
          }
          break;
        case PropertyType::String:
          property->append_str("type", "string");
          if (state_property.exists) {
            property->append_str("value", state_property.string_value);
          }
          break;
        case PropertyType::Vector3:
        case PropertyType::Vector4:
        case PropertyType::Object:
        case PropertyType::List:
        case PropertyType::Dict:
          property->append_str("type", structured_property_type_name(state_property.type));
          if (state_property.exists) {
            property->append("value", encode_property_data(state_property.data));
          }
          break;
      }
    }
  };
  auto append_material_state = [](DictionaryValue &object, const ObjectState &state) {
    std::shared_ptr<DictionaryValue> material_state = object.append_dict("material_state");
    std::shared_ptr<ArrayValue> slots = material_state->append_array("slots");
    for (const MaterialSlotState &slot_state : state.material_slots) {
      if (!slot_state.has_material) {
        slots->append_null();
        continue;
      }
      std::shared_ptr<DictionaryValue> slot = slots->append_dict();
      slot->append_str("name", slot_state.material_name);
      slot->append_str("library", slot_state.library_path);
    }
    std::shared_ptr<ArrayValue> parameters = material_state->append_array("parameters");
    for (const MaterialParameterState &parameter_state : state.material_parameters) {
      std::shared_ptr<DictionaryValue> parameter = parameters->append_dict();
      parameter->append_str("attribute", parameter_state.attribute_name);
      std::shared_ptr<ArrayValue> values = parameter->append_array("value");
      for (const float value : parameter_state.values) {
        values->append_double(value);
      }
    }
  };
  auto append_hierarchy_state = [](DictionaryValue &object, const ObjectState &state) {
    std::shared_ptr<DictionaryValue> hierarchy = object.append_dict("hierarchy");
    if (state.hierarchy.relation == HierarchyRelation::Root) {
      hierarchy->append("parent", std::make_shared<NullValue>());
      hierarchy->append_str("relation", "root");
    }
    else {
      hierarchy->append_str("parent", state.hierarchy.parent_id);
      hierarchy->append_str("relation", "normal");
    }
    hierarchy->append("compound", std::make_shared<BooleanValue>(state.hierarchy.compound));
    hierarchy->append("ghost", std::make_shared<BooleanValue>(state.hierarchy.ghost));
  };
  /* Set Collection Member's changes; written only when there are some (older files have none). */
  auto append_collection_memberships = [](DictionaryValue &object, const ObjectState &state) {
    if (state.collection_memberships.empty()) {
      return;
    }
    std::shared_ptr<ArrayValue> collections = object.append_array("collections");
    for (const CollectionMembershipState &membership : state.collection_memberships) {
      std::shared_ptr<DictionaryValue> entry = collections->append_dict();
      entry->append_str("name", membership.collection_name);
      entry->append_str("library", membership.library_path);
      entry->append("member", std::make_shared<BooleanValue>(membership.member));
    }
  };
  auto append_rigid_body_state = [](DictionaryValue &object, const ObjectState &state) {
    if (!state.has_rigid_body_state) {
      object.append("physics", std::make_shared<NullValue>());
      return;
    }
    const RigidBodyState &saved = state.rigid_body;
    std::shared_ptr<DictionaryValue> physics = object.append_dict("physics");
    physics->append("dynamic_body", std::make_shared<BooleanValue>(saved.dynamic_body));
    physics->append_str("mode", rigid_body_mode_name(saved.mode));
    physics->append_str("runtime_motion_type",
                        rigid_body_runtime_motion_type_name(saved.runtime_motion_type));
    physics->append("ghost", std::make_shared<BooleanValue>(saved.ghost));
    physics->append(
        "physics_suspended", std::make_shared<BooleanValue>(saved.physics_suspended));
    physics->append("awake", std::make_shared<BooleanValue>(saved.awake));
    auto append_physics_vector = [&physics](const char *name, const MT_Vector3 &value) {
      std::shared_ptr<ArrayValue> array = physics->append_array(name);
      append_vector3(*array, value);
    };
    append_physics_vector("linear_velocity", saved.linear_velocity);
    append_physics_vector("angular_velocity", saved.angular_velocity);
    append_physics_vector("pending_force", saved.pending_force);
    append_physics_vector("pending_torque", saved.pending_torque);
    physics->append_double("mass", saved.mass);
    physics->append_double("friction", saved.friction);
    physics->append_double("restitution", saved.restitution);
    physics->append_double("linear_damping", saved.linear_damping);
    physics->append_double("angular_damping", saved.angular_damping);
    physics->append_double("linear_velocity_min", saved.linear_velocity_min);
    physics->append_double("linear_velocity_max", saved.linear_velocity_max);
    physics->append_double("angular_velocity_min", saved.angular_velocity_min);
    physics->append_double("angular_velocity_max", saved.angular_velocity_max);
    physics->append_double("gravity_factor", saved.gravity_factor);
    physics->append(
        "allow_sleeping", std::make_shared<BooleanValue>(saved.allow_sleeping));
    physics->append(
        "rotation_enabled", std::make_shared<BooleanValue>(saved.rotation_enabled));
    physics->append("ccd_enabled", std::make_shared<BooleanValue>(saved.ccd_enabled));
    physics->append(
        "dynamic_ccd_enabled", std::make_shared<BooleanValue>(saved.dynamic_ccd_enabled));
    std::shared_ptr<ArrayValue> locks = physics->append_array("axis_locks");
    for (const bool lock : saved.axis_locks) {
      locks->append(std::make_shared<BooleanValue>(lock));
    }
    physics->append_int("collision_group", saved.collision_group);
    physics->append_int("collision_mask", saved.collision_mask);
  };

  std::shared_ptr<ArrayValue> authored = root->append_array("authored_objects");
  for (const ObjectState &state : checkpoint.authored_objects) {
    std::shared_ptr<DictionaryValue> object = authored->append_dict();
    object->append_str("uuid", state.persistence_id);
    if (!state.name.empty()) {
      object->append_str("name", state.name);
    }
    if (state.has_existence) {
      object->append("exists", std::make_shared<BooleanValue>(state.exists));
    }
    if (state.has_transform) {
      append_transform(*object,
                       "transform",
                       state.world_position,
                       state.world_orientation,
                       state.world_scale);
    }
    if (state.has_material_state) {
      append_material_state(*object, state);
    }
    if (state.has_runtime_structure_state) {
      append_runtime_structure_state(*object, state);
    }
    if (state.has_hierarchy_state) {
      append_hierarchy_state(*object, state);
      append_rigid_body_state(*object, state);
      append_collection_memberships(*object, state);
    }
    append_properties(*object, state);
  }

  std::shared_ptr<ArrayValue> spawned = root->append_array("spawned_instances");
  for (const ObjectState &state : checkpoint.spawned_instances) {
    std::shared_ptr<DictionaryValue> object = spawned->append_dict();
    object->append_str("instance_uuid", state.persistence_id);
    object->append_str("source_uuid", state.source_id);
    if (!state.name.empty()) {
      object->append_str("name", state.name);
    }
    append_transform(*object,
                     "creation_transform",
                     state.creation_position,
                     state.creation_orientation,
                     state.creation_scale);
    if (state.has_transform) {
      append_transform(*object,
                       "transform",
                       state.world_position,
                       state.world_orientation,
                       state.world_scale);
    }
    if (state.has_material_state) {
      append_material_state(*object, state);
    }
    if (state.has_runtime_structure_state) {
      append_runtime_structure_state(*object, state);
    }
    if (state.has_hierarchy_state) {
      append_hierarchy_state(*object, state);
      append_rigid_body_state(*object, state);
      append_collection_memberships(*object, state);
    }
    append_properties(*object, state);
  }

  std::shared_ptr<ArrayValue> materials = root->append_array("shared_materials");
  for (const SharedMaterialState &material_state : checkpoint.shared_materials) {
    std::shared_ptr<DictionaryValue> material = materials->append_dict();
    material->append_str("name", material_state.material.material_name);
    material->append_str("library", material_state.material.library_path);
    std::shared_ptr<ArrayValue> nodes = material->append_array("nodes");
    for (const SharedMaterialNodeState &node_state : material_state.nodes) {
      std::shared_ptr<DictionaryValue> node = nodes->append_dict();
      node->append_int("id", node_state.node_identifier);
      node->append_str("type", node_state.node_type);
      std::shared_ptr<ArrayValue> inputs = node->append_array("inputs");
      for (const MaterialSocketValueState &input_state : node_state.inputs) {
        std::shared_ptr<DictionaryValue> input = inputs->append_dict();
        append_material_socket_value(*input, input_state);
      }
    }
  }
  append_runtime_node_tree_state(*root, checkpoint);
  return root;
}

bool serialize_json(const Value &value, std::string &r_contents, std::string &r_error)
{
  JsonFormatter formatter;
  formatter.indentation_len = 0;
  std::ostringstream stream;
  try {
    formatter.serialize(stream, value);
  }
  catch (...) {
    r_error = "Save Game data contains a value that JSON cannot serialize";
    return false;
  }
  if (!stream.good()) {
    r_error = "could not serialize Save Game data";
    return false;
  }
  r_contents = std::move(stream).str();
  return true;
}

std::string checkpoint_checksum(const std::string &contents)
{
  unsigned char digest[16];
  char hexadecimal[33];
  blender::BLI_hash_md5_buffer(contents.data(), contents.size(), digest);
  blender::BLI_hash_md5_to_hexdigest(digest, hexadecimal);
  return hexadecimal;
}

const Value *lookup_value(const DictionaryValue &dictionary, const char *name)
{
  const std::shared_ptr<Value> *value = dictionary.lookup(name);
  return value ? value->get() : nullptr;
}

bool has_exact_keys(const DictionaryValue &dictionary,
                    const std::initializer_list<const char *> required,
                    const std::initializer_list<const char *> optional,
                    std::string &r_error)
{
  std::unordered_set<std::string> allowed;
  allowed.reserve(required.size() + optional.size());
  for (const char *key : required) {
    allowed.emplace(key);
  }
  for (const char *key : optional) {
    allowed.emplace(key);
  }
  std::unordered_set<std::string> present;
  present.reserve(dictionary.elements().size());
  for (const DictionaryValue::Item &item : dictionary.elements()) {
    if (allowed.find(item.first) == allowed.end()) {
      r_error = "unexpected field '" + item.first + "'";
      return false;
    }
    if (!present.insert(item.first).second) {
      r_error = "duplicate field '" + item.first + "'";
      return false;
    }
  }
  for (const char *key : required) {
    if (present.find(key) == present.end()) {
      r_error = std::string("missing field '") + key + "'";
      return false;
    }
  }
  return true;
}

}  // namespace

bool is_canonical_uuid(const std::string &text)
{
  if (text.size() != UUID_STRING_SIZE - 1 || contains_embedded_null(text)) {
    return false;
  }
  blender::bUUID uuid;
  if (!blender::BLI_uuid_parse_string(&uuid, text.c_str()) || blender::BLI_uuid_is_nil(uuid)) {
    return false;
  }
  return uuid_string(uuid) == text;
}

bool parse_canonical_uuid(const std::string &text, blender::UUID &r_uuid)
{
  blender::bUUID uuid;
  if (!is_canonical_uuid(text) || !blender::BLI_uuid_parse_string(&uuid, text.c_str())) {
    return false;
  }
  r_uuid = blender::UUID(uuid);
  return true;
}

namespace {

bool read_string(const DictionaryValue &dictionary,
                 const char *name,
                 std::string &r_value,
                 std::string &r_error)
{
  const Value *value = lookup_value(dictionary, name);
  const StringValue *string = value ? value->as_string_value() : nullptr;
  if (string == nullptr) {
    r_error = std::string("missing or invalid string '") + name + "'";
    return false;
  }
  r_value = string->value();
  return true;
}

bool read_bool(const DictionaryValue &dictionary,
               const char *name,
               bool &r_value,
               std::string &r_error)
{
  const Value *value = lookup_value(dictionary, name);
  const BooleanValue *boolean = value ? value->as_boolean_value() : nullptr;
  if (boolean == nullptr) {
    r_error = std::string("missing or invalid boolean '") + name + "'";
    return false;
  }
  r_value = boolean->value();
  return true;
}

bool read_number(const Value *value, double &r_value)
{
  if (const DoubleValue *floating = value ? value->as_double_value() : nullptr) {
    r_value = floating->value();
    return std::isfinite(r_value);
  }
  if (const IntValue *integer = value ? value->as_int_value() : nullptr) {
    r_value = double(integer->value());
    return std::isfinite(r_value);
  }
  return false;
}

bool read_vector3(const DictionaryValue &dictionary,
                  const char *name,
                  MT_Vector3 &r_value,
                  std::string &r_error)
{
  const ArrayValue *array = dictionary.lookup_array(name);
  if (array == nullptr || array->elements().size() != 3) {
    r_error = std::string("missing or invalid vector '") + name + "'";
    return false;
  }
  double values[3];
  for (int index = 0; index < 3; index++) {
    if (!read_number(array->elements()[index].get(), values[index]) ||
        std::abs(values[index]) > std::numeric_limits<float>::max())
    {
      r_error = std::string("invalid component in vector '") + name + "'";
      return false;
    }
  }
  r_value = MT_Vector3(float(values[0]), float(values[1]), float(values[2]));
  return true;
}

bool read_matrix3(const DictionaryValue &dictionary,
                  const char *name,
                  MT_Matrix3x3 &r_value,
                  std::string &r_error)
{
  const ArrayValue *array = dictionary.lookup_array(name);
  if (array == nullptr || array->elements().size() != 9) {
    r_error = std::string("missing or invalid matrix '") + name + "'";
    return false;
  }
  float values[9];
  for (int index = 0; index < 9; index++) {
    double value;
    if (!read_number(array->elements()[index].get(), value) ||
        std::abs(value) > std::numeric_limits<float>::max())
    {
      r_error = std::string("invalid component in matrix '") + name + "'";
      return false;
    }
    values[index] = float(value);
  }
  r_value = MT_Matrix3x3(values[0],
                         values[1],
                         values[2],
                         values[3],
                         values[4],
                         values[5],
                         values[6],
                         values[7],
                         values[8]);
  if (!is_valid_orientation(r_value)) {
    r_error = std::string("matrix '") + name + "' is not a valid rotation";
    return false;
  }
  return true;
}

bool read_float4(const DictionaryValue &dictionary,
                 const char *name,
                 std::array<float, 4> &r_value,
                 std::string &r_error)
{
  const ArrayValue *array = dictionary.lookup_array(name);
  if (array == nullptr || array->elements().size() != 4) {
    r_error = std::string("missing or invalid float array '") + name + "'";
    return false;
  }
  for (int index = 0; index < 4; ++index) {
    double value;
    if (!read_number(array->elements()[index].get(), value) ||
        std::abs(value) > std::numeric_limits<float>::max())
    {
      r_error = std::string("invalid component in float array '") + name + "'";
      return false;
    }
    r_value[index] = float(value);
  }
  return true;
}

bool read_int32(const DictionaryValue &dictionary,
                const char *name,
                int32_t &r_value,
                std::string &r_error)
{
  const Value *value = lookup_value(dictionary, name);
  const IntValue *integer = value ? value->as_int_value() : nullptr;
  if (integer == nullptr || integer->value() < std::numeric_limits<int32_t>::min() ||
      integer->value() > std::numeric_limits<int32_t>::max())
  {
    r_error = std::string("missing or invalid integer '") + name + "'";
    return false;
  }
  r_value = int32_t(integer->value());
  return true;
}

bool read_uint32(const DictionaryValue &dictionary,
                 const char *name,
                 uint32_t &r_value,
                 std::string &r_error)
{
  const Value *value = lookup_value(dictionary, name);
  const IntValue *integer = value ? value->as_int_value() : nullptr;
  if (integer == nullptr || integer->value() < 0 ||
      uint64_t(integer->value()) > std::numeric_limits<uint32_t>::max())
  {
    r_error = std::string("missing or invalid unsigned integer '") + name + "'";
    return false;
  }
  r_value = uint32_t(integer->value());
  return true;
}

bool read_material_float_values(const DictionaryValue &dictionary,
                                const size_t count,
                                std::array<float, 4> &r_values,
                                std::string &r_error)
{
  const ArrayValue *array = dictionary.lookup_array("value");
  if (array == nullptr || array->elements().size() != count) {
    r_error = "missing or invalid material input value";
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    double value;
    if (!read_number(array->elements()[index].get(), value) ||
        std::abs(value) > std::numeric_limits<float>::max())
    {
      r_error = "material input contains a non-finite or out-of-range value";
      return false;
    }
    r_values[index] = float(value);
  }
  return true;
}

bool material_socket_value_type_from_name(const std::string &name,
                                          MaterialSocketValueType &r_type)
{
  constexpr MaterialSocketValueType types[] = {
      MaterialSocketValueType::Bool,
      MaterialSocketValueType::Int,
      MaterialSocketValueType::Menu,
      MaterialSocketValueType::IntVector,
      MaterialSocketValueType::Float,
      MaterialSocketValueType::Vector,
      MaterialSocketValueType::Rotation,
      MaterialSocketValueType::Color,
      MaterialSocketValueType::String,
      MaterialSocketValueType::Object,
      MaterialSocketValueType::Image,
      MaterialSocketValueType::Collection,
      MaterialSocketValueType::Texture,
      MaterialSocketValueType::Material,
      MaterialSocketValueType::Font,
      MaterialSocketValueType::Scene,
      MaterialSocketValueType::Text,
      MaterialSocketValueType::Mask,
      MaterialSocketValueType::Sound,
  };
  for (const MaterialSocketValueType type : types) {
    if (name == material_socket_value_type_name(type)) {
      r_type = type;
      return true;
    }
  }
  return false;
}

bool decode_material_socket_value(const DictionaryValue &dictionary,
                                  MaterialSocketValueState &r_state,
                                  std::string &r_error)
{
  std::string type_name;
  if (!read_string(dictionary, "socket", r_state.socket_identifier, r_error) ||
      !read_string(dictionary, "type", type_name, r_error) ||
      !is_valid_checkpoint_text(r_state.socket_identifier, kMaxNameBytes, false) ||
      !material_socket_value_type_from_name(type_name, r_state.type))
  {
    r_error = "invalid shared material input identity";
    return false;
  }

  const bool has_dimensions = r_state.type == MaterialSocketValueType::IntVector ||
                              r_state.type == MaterialSocketValueType::Vector;
  if (!has_exact_keys(dictionary,
                      {"socket", "type", "value"},
                      has_dimensions ? std::initializer_list<const char *>{"dimensions"} :
                                       std::initializer_list<const char *>{},
                      r_error))
  {
    return false;
  }
  if (has_dimensions) {
    int32_t dimensions;
    if (!read_int32(dictionary, "dimensions", dimensions, r_error) ||
        (r_state.type == MaterialSocketValueType::IntVector &&
         (dimensions < 2 || dimensions > 3)) ||
        (r_state.type == MaterialSocketValueType::Vector &&
         (dimensions < 2 || dimensions > 4)))
    {
      r_error = "invalid shared material input dimensions";
      return false;
    }
    r_state.dimensions = uint8_t(dimensions);
  }

  const Value *value = lookup_value(dictionary, "value");
  switch (r_state.type) {
    case MaterialSocketValueType::Bool: {
      const BooleanValue *boolean = value ? value->as_boolean_value() : nullptr;
      if (boolean == nullptr) {
        r_error = "invalid shared material boolean value";
        return false;
      }
      r_state.bool_value = boolean->value();
      return true;
    }
    case MaterialSocketValueType::Int:
    case MaterialSocketValueType::Menu:
      return read_int32(dictionary, "value", r_state.int_value, r_error);
    case MaterialSocketValueType::IntVector: {
      const ArrayValue *array = dictionary.lookup_array("value");
      if (array == nullptr || array->elements().size() != r_state.dimensions) {
        r_error = "invalid shared material integer-vector value";
        return false;
      }
      for (size_t index = 0; index < r_state.dimensions; ++index) {
        const IntValue *integer = array->elements()[index] ?
                                      array->elements()[index]->as_int_value() :
                                      nullptr;
        if (integer == nullptr ||
            integer->value() < std::numeric_limits<int32_t>::min() ||
            integer->value() > std::numeric_limits<int32_t>::max())
        {
          r_error = "invalid shared material integer-vector component";
          return false;
        }
        r_state.int_values[index] = int32_t(integer->value());
      }
      return true;
    }
    case MaterialSocketValueType::Float: {
      double floating;
      if (!read_number(value, floating) ||
          std::abs(floating) > std::numeric_limits<float>::max())
      {
        r_error = "invalid shared material float value";
        return false;
      }
      r_state.float_values[0] = float(floating);
      return true;
    }
    case MaterialSocketValueType::Vector:
      return read_material_float_values(
          dictionary, r_state.dimensions, r_state.float_values, r_error);
    case MaterialSocketValueType::Rotation:
      return read_material_float_values(dictionary, 3, r_state.float_values, r_error);
    case MaterialSocketValueType::Color:
      return read_material_float_values(dictionary, 4, r_state.float_values, r_error);
    case MaterialSocketValueType::String: {
      const StringValue *string = value ? value->as_string_value() : nullptr;
      if (string == nullptr ||
          !is_valid_checkpoint_text(string->value(), kMaxMaterialStringBytes))
      {
        r_error = "invalid or oversized shared material string value";
        return false;
      }
      r_state.string_value = string->value();
      return true;
    }
    case MaterialSocketValueType::Object:
    case MaterialSocketValueType::Image:
    case MaterialSocketValueType::Collection:
    case MaterialSocketValueType::Texture:
    case MaterialSocketValueType::Material:
    case MaterialSocketValueType::Font:
    case MaterialSocketValueType::Scene:
    case MaterialSocketValueType::Text:
    case MaterialSocketValueType::Mask:
    case MaterialSocketValueType::Sound: {
      if (value == nullptr) {
        r_error = "missing shared material datablock value";
        return false;
      }
      if (value->type() == blender::io::serialize::eValueType::Null) {
        return true;
      }
      const DictionaryValue *reference = value->as_dictionary_value();
      if (reference == nullptr ||
          !has_exact_keys(*reference, {"name", "library"}, {}, r_error) ||
          !read_string(*reference, "name", r_state.datablock_value.name, r_error) ||
          !read_string(
              *reference, "library", r_state.datablock_value.library_path, r_error) ||
          !is_valid_checkpoint_text(r_state.datablock_value.name, kMaxNameBytes, false) ||
          !is_valid_checkpoint_text(r_state.datablock_value.library_path, kMaxIdentityBytes))
      {
        r_error = "invalid shared material datablock reference";
        return false;
      }
      r_state.datablock_value.has_value = true;
      return true;
    }
  }
  r_error = "unsupported shared material input type";
  return false;
}

bool geometry_nodes_input_mode_from_name(const std::string &name,
                                         GeometryNodesInputMode &r_mode)
{
  constexpr GeometryNodesInputMode modes[] = {
      GeometryNodesInputMode::Fallback,
      GeometryNodesInputMode::Value,
      GeometryNodesInputMode::Attribute,
      GeometryNodesInputMode::Layer,
  };
  for (const GeometryNodesInputMode mode : modes) {
    if (name == geometry_nodes_input_mode_name(mode)) {
      r_mode = mode;
      return true;
    }
  }
  return false;
}

bool decode_runtime_structure_state(const DictionaryValue &dictionary,
                                    const std::string &object_name,
                                    ObjectState &r_object,
                                    std::string &r_error)
{
  const ArrayValue *logic_trees = dictionary.lookup_array("logic_trees");
  const DictionaryValue *modifiers = dictionary.lookup_dict("modifier_state");
  if (logic_trees == nullptr || logic_trees->elements().size() > kMaxLogicTreesPerObject ||
      modifiers == nullptr ||
      !has_exact_keys(*modifiers, {"authored", "geometry_nodes"}, {}, r_error))
  {
    r_error = "invalid runtime structure state for object '" + object_name + "': " + r_error;
    return false;
  }
  const ArrayValue *authored = modifiers->lookup_array("authored");
  const ArrayValue *geometry_nodes = modifiers->lookup_array("geometry_nodes");
  if (authored == nullptr || geometry_nodes == nullptr ||
      authored->elements().size() > kMaxModifiersPerObject ||
      geometry_nodes->elements().size() >
          kMaxModifiersPerObject - authored->elements().size())
  {
    r_error = "oversized modifier state for object '" + object_name + "'";
    return false;
  }

  std::unordered_set<uint32_t> applied_indices;
  r_object.logic_trees.reserve(logic_trees->elements().size());
  for (const std::shared_ptr<Value> &value : logic_trees->elements()) {
    const DictionaryValue *tree = value ? value->as_dictionary_value() : nullptr;
    LogicTreeAttachmentState state;
    if (tree == nullptr ||
        !has_exact_keys(*tree, {"name", "library", "applied_index", "enabled"}, {}, r_error) ||
        !read_string(*tree, "name", state.tree_name, r_error) ||
        !read_string(*tree, "library", state.library_path, r_error) ||
        !read_uint32(*tree, "applied_index", state.applied_tree_index, r_error) ||
        !read_bool(*tree, "enabled", state.enabled, r_error) ||
        !is_valid_checkpoint_text(state.tree_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(state.library_path, kMaxIdentityBytes))
    {
      r_error = "invalid C++ Logic Node attachment for object '" + object_name + "': " +
                r_error;
      return false;
    }
    if (!applied_indices.insert(state.applied_tree_index).second)
    {
      r_error = "duplicate C++ Logic Node attachment for object '" + object_name + "'";
      return false;
    }
    r_object.logic_trees.push_back(std::move(state));
  }
  if (!std::is_sorted(r_object.logic_trees.begin(),
                      r_object.logic_trees.end(),
                      [](const LogicTreeAttachmentState &a,
                         const LogicTreeAttachmentState &b) {
                        return a.applied_tree_index < b.applied_tree_index;
                      }))
  {
    r_error = "C++ Logic Node attachments are not in stable applied-index order";
    return false;
  }

  std::unordered_set<std::string> modifier_names;
  std::unordered_set<int32_t> modifier_uids;
  r_object.non_geometry_modifiers.reserve(authored->elements().size());
  for (const std::shared_ptr<Value> &value : authored->elements()) {
    const DictionaryValue *modifier = value ? value->as_dictionary_value() : nullptr;
    NonGeometryModifierState state;
    if (modifier == nullptr ||
        !has_exact_keys(
            *modifier, {"name", "type", "persistent_uid", "mode"}, {}, r_error) ||
        !read_string(*modifier, "name", state.name, r_error) ||
        !read_int32(*modifier, "type", state.type, r_error) ||
        !read_int32(*modifier, "persistent_uid", state.persistent_uid, r_error) ||
        !read_uint32(*modifier, "mode", state.mode, r_error) ||
        !is_valid_checkpoint_text(state.name, kMaxNameBytes, false) ||
        state.type < 0 || state.type >= int32_t(blender::NUM_MODIFIER_TYPES) ||
        state.persistent_uid <= 0 || (state.mode & ~kPersistentModifierModeMask) != 0 ||
        !modifier_names.insert(state.name).second ||
        !modifier_uids.insert(state.persistent_uid).second)
    {
      r_error = "invalid or duplicate authored modifier for object '" + object_name + "'";
      return false;
    }
    r_object.non_geometry_modifiers.push_back(std::move(state));
  }

  std::unordered_set<uint32_t> stack_indices;
  r_object.geometry_nodes_modifiers.reserve(geometry_nodes->elements().size());
  for (const std::shared_ptr<Value> &value : geometry_nodes->elements()) {
    const DictionaryValue *modifier = value ? value->as_dictionary_value() : nullptr;
    GeometryNodesModifierState state;
    const DictionaryValue *node_group =
        modifier ? modifier->lookup_dict("node_group") : nullptr;
    const ArrayValue *inputs = modifier ? modifier->lookup_array("inputs") : nullptr;
    const ArrayValue *outputs = modifier ? modifier->lookup_array("outputs") : nullptr;
    if (modifier == nullptr ||
        !has_exact_keys(*modifier,
                        {"name",
                         "persistent_uid",
                         "stack_index",
                         "mode",
                         "node_group",
                         "inputs",
                         "outputs"},
                        {},
                        r_error) ||
        node_group == nullptr ||
        !has_exact_keys(*node_group, {"name", "library", "fingerprint"}, {}, r_error) ||
        !read_string(*modifier, "name", state.name, r_error) ||
        !read_int32(*modifier, "persistent_uid", state.persistent_uid, r_error) ||
        !read_uint32(*modifier, "stack_index", state.stack_index, r_error) ||
        !read_uint32(*modifier, "mode", state.mode, r_error) ||
        !read_string(*node_group, "name", state.node_group_name, r_error) ||
        !read_string(
            *node_group, "library", state.node_group_library_path, r_error) ||
        !read_string(*node_group, "fingerprint", state.node_group_fingerprint, r_error) ||
        inputs == nullptr ||
        inputs->elements().size() > kMaxGeometryNodesInputsPerModifier ||
        outputs == nullptr ||
        outputs->elements().size() > kMaxGeometryNodesOutputsPerModifier ||
        !is_valid_checkpoint_text(state.name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(state.node_group_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(state.node_group_library_path, kMaxIdentityBytes) ||
        !is_valid_checkpoint_text(state.node_group_fingerprint, kMaxIdentityBytes, false) ||
        state.persistent_uid <= 0 ||
        (state.mode & ~kPersistentModifierModeMask) != 0 ||
        state.stack_index >= authored->elements().size() + geometry_nodes->elements().size() ||
        !modifier_names.insert(state.name).second ||
        !modifier_uids.insert(state.persistent_uid).second ||
        !stack_indices.insert(state.stack_index).second)
    {
      r_error = "invalid or duplicate Geometry Nodes modifier for object '" + object_name +
                "': " + r_error;
      return false;
    }

    std::unordered_set<std::string> input_identifiers;
    state.inputs.reserve(inputs->elements().size());
    for (const std::shared_ptr<Value> &input_value : inputs->elements()) {
      const DictionaryValue *input =
          input_value ? input_value->as_dictionary_value() : nullptr;
      GeometryNodesInputState input_state;
      std::string mode_name;
      if (input == nullptr ||
          !read_string(*input, "socket", input_state.socket_identifier, r_error) ||
          !read_string(*input, "mode", mode_name, r_error) ||
          !geometry_nodes_input_mode_from_name(mode_name, input_state.mode) ||
          !is_valid_checkpoint_text(input_state.socket_identifier, kMaxNameBytes, false) ||
          !input_identifiers.insert(input_state.socket_identifier).second)
      {
        r_error = "invalid Geometry Nodes input for object '" + object_name + "'";
        return false;
      }
      switch (input_state.mode) {
        case GeometryNodesInputMode::Value: {
          const DictionaryValue *saved_value = input->lookup_dict("value");
          if (!has_exact_keys(*input, {"socket", "mode", "value"}, {}, r_error) ||
              saved_value == nullptr ||
              !decode_material_socket_value(*saved_value, input_state.value, r_error) ||
              input_state.value.socket_identifier != input_state.socket_identifier)
          {
            r_error = "invalid Geometry Nodes value input for object '" + object_name + "': " +
                      r_error;
            return false;
          }
          break;
        }
        case GeometryNodesInputMode::Attribute:
          if (!has_exact_keys(*input, {"socket", "mode", "attribute"}, {}, r_error) ||
              !read_string(*input, "attribute", input_state.attribute_name, r_error) ||
              !is_valid_checkpoint_text(
                  input_state.attribute_name, kMaxStringPropertyBytes))
          {
            r_error = "invalid Geometry Nodes attribute input for object '" + object_name + "'";
            return false;
          }
          break;
        case GeometryNodesInputMode::Layer:
          if (!has_exact_keys(*input, {"socket", "mode", "layer"}, {}, r_error) ||
              !read_string(*input, "layer", input_state.layer_name, r_error) ||
              !is_valid_checkpoint_text(input_state.layer_name, kMaxStringPropertyBytes))
          {
            r_error = "invalid Geometry Nodes layer input for object '" + object_name + "'";
            return false;
          }
          break;
        case GeometryNodesInputMode::Fallback:
          if (!has_exact_keys(*input, {"socket", "mode"}, {}, r_error)) {
            r_error = "invalid Geometry Nodes fallback input for object '" + object_name + "'";
            return false;
          }
          break;
      }
      state.inputs.push_back(std::move(input_state));
    }

    std::unordered_set<std::string> output_identifiers;
    state.outputs.reserve(outputs->elements().size());
    for (const std::shared_ptr<Value> &output_value : outputs->elements()) {
      const DictionaryValue *output =
          output_value ? output_value->as_dictionary_value() : nullptr;
      GeometryNodesOutputState output_state;
      if (output == nullptr ||
          !has_exact_keys(*output, {"socket", "attribute"}, {}, r_error) ||
          !read_string(*output, "socket", output_state.socket_identifier, r_error) ||
          !read_string(*output, "attribute", output_state.attribute_name, r_error) ||
          !is_valid_checkpoint_text(
              output_state.socket_identifier, kMaxNameBytes, false) ||
          !is_valid_checkpoint_text(
              output_state.attribute_name, kMaxStringPropertyBytes) ||
          !output_identifiers.insert(output_state.socket_identifier).second)
      {
        r_error = "invalid Geometry Nodes output for object '" + object_name + "'";
        return false;
      }
      state.outputs.push_back(std::move(output_state));
    }
    r_object.geometry_nodes_modifiers.push_back(std::move(state));
  }
  if (!std::is_sorted(r_object.geometry_nodes_modifiers.begin(),
                      r_object.geometry_nodes_modifiers.end(),
                      [](const GeometryNodesModifierState &a,
                         const GeometryNodesModifierState &b) {
                        return a.stack_index < b.stack_index;
                      }))
  {
    r_error = "Geometry Nodes modifiers are not in stable stack order";
    return false;
  }
  r_object.has_runtime_structure_state = true;
  return true;
}

/** Reads what #encode_property_data wrote, refusing other shapes, deep nesting and large data. */
bool decode_property_data(const Value &value,
                          const int depth,
                          size_t &r_items,
                          PropertyData &r_data,
                          std::string &r_error)
{
  if (depth + 2 > kMaxPropertyDataDepth || ++r_items > kMaxPropertyDataItems) {
    r_error = "property value is nested too deep or holds too many values";
    return false;
  }
  switch (value.type()) {
    case blender::io::serialize::eValueType::Boolean:
      r_data.kind = PropertyData::Kind::Bool;
      r_data.bool_value = value.as_boolean_value()->value();
      return true;
    case blender::io::serialize::eValueType::Int:
      r_data.kind = PropertyData::Kind::Int;
      r_data.int_value = value.as_int_value()->value();
      return true;
    case blender::io::serialize::eValueType::Double:
      r_data.kind = PropertyData::Kind::Float;
      r_data.float_value = value.as_double_value()->value();
      if (!std::isfinite(r_data.float_value) ||
          std::abs(r_data.float_value) > std::numeric_limits<float>::max())
      {
        r_error = "invalid float in property value";
        return false;
      }
      return true;
    case blender::io::serialize::eValueType::String:
      r_data.kind = PropertyData::Kind::String;
      r_data.text = value.as_string_value()->value();
      if (!is_valid_checkpoint_text(r_data.text, kMaxStringPropertyBytes)) {
        r_error = "invalid or oversized text in property value";
        return false;
      }
      return true;
    case blender::io::serialize::eValueType::Array:
      r_data.kind = PropertyData::Kind::List;
      for (const std::shared_ptr<Value> &item : value.as_array_value()->elements()) {
        r_data.items.emplace_back();
        if (item == nullptr ||
            !decode_property_data(*item, depth + 1, r_items, r_data.items.back(), r_error))
        {
          if (r_error.empty()) {
            r_error = "invalid list item in property value";
          }
          return false;
        }
      }
      return true;
    case blender::io::serialize::eValueType::Dictionary:
      break;
    default:
      r_error = "unsupported value in property value";
      return false;
  }

  const DictionaryValue &tagged = *value.as_dictionary_value();
  if (tagged.elements().size() != 1 || tagged.elements()[0].second == nullptr) {
    r_error = "invalid tagged value in property value";
    return false;
  }
  const std::string &tag = tagged.elements()[0].first;
  const Value &inner = *tagged.elements()[0].second;
  if (tag == "vector3" || tag == "vector4") {
    const size_t size = tag == "vector3" ? 3 : 4;
    const ArrayValue *vector = inner.as_array_value();
    if (vector == nullptr || vector->elements().size() != size) {
      r_error = "invalid vector in property value";
      return false;
    }
    r_data.kind = size == 3 ? PropertyData::Kind::Vector3 : PropertyData::Kind::Vector4;
    for (size_t axis = 0; axis < size; axis++) {
      double component;
      if (!read_number(vector->elements()[axis].get(), component) ||
          std::abs(component) > std::numeric_limits<float>::max())
      {
        r_error = "invalid vector in property value";
        return false;
      }
      r_data.vector[axis] = float(component);
    }
    return true;
  }
  if (tag == "object" || tag == "object_name") {
    r_data.kind = PropertyData::Kind::Object;
    r_data.object_by_name = tag == "object_name";
    if (inner.type() == blender::io::serialize::eValueType::Null && !r_data.object_by_name) {
      return true;
    }
    const StringValue *text = inner.as_string_value();
    if (text == nullptr || text->value().empty() ||
        !is_valid_checkpoint_text(text->value(), kMaxNameBytes, false))
    {
      r_error = "invalid object in property value";
      return false;
    }
    r_data.text = text->value();
    return true;
  }
  if (tag == "dict") {
    const DictionaryValue *dict = inner.as_dictionary_value();
    if (dict == nullptr) {
      r_error = "invalid dictionary in property value";
      return false;
    }
    r_data.kind = PropertyData::Kind::Dict;
    std::unordered_set<std::string> keys;
    for (const auto &entry : dict->elements()) {
      if (entry.second == nullptr || !is_valid_checkpoint_text(entry.first, kMaxNameBytes, false) ||
          !keys.insert(entry.first).second)
      {
        r_error = "invalid or repeated dictionary key in property value";
        return false;
      }
      r_data.entries.emplace_back(entry.first, PropertyData());
      if (!decode_property_data(
              *entry.second, depth + 2, r_items, r_data.entries.back().second, r_error))
      {
        return false;
      }
    }
    return true;
  }
  r_error = "unsupported tagged value '" + tag + "' in property value";
  return false;
}

bool decode_property(const DictionaryValue &dictionary,
                     PropertyState &r_property,
                     std::string &r_error)
{
  std::string type;
  if (!has_exact_keys(dictionary, {"name", "type", "timer", "exists"}, {"value"}, r_error) ||
      !read_bool(dictionary, "exists", r_property.exists, r_error))
  {
    return false;
  }
  if (!read_string(dictionary, "name", r_property.name, r_error) ||
      !read_string(dictionary, "type", type, r_error) ||
      !read_bool(dictionary, "timer", r_property.timer, r_error))
  {
    return false;
  }
  if (!is_valid_checkpoint_text(r_property.name, kMaxNameBytes, false))
  {
    r_error = "invalid Save Game property name";
    return false;
  }

  const Value *value = lookup_value(dictionary, "value");
  if (r_property.exists != (value != nullptr)) {
    r_error = r_property.exists ? "present property is missing its value" :
                                  "property tombstone must not contain a value";
    return false;
  }
  if (type == "bool") {
    const BooleanValue *boolean = value ? value->as_boolean_value() : nullptr;
    if (r_property.exists && boolean == nullptr) {
      r_error = "invalid boolean property value";
      return false;
    }
    r_property.type = PropertyType::Bool;
    r_property.bool_value = boolean ? boolean->value() : false;
  }
  else if (type == "int") {
    const IntValue *integer = value ? value->as_int_value() : nullptr;
    if (r_property.exists && integer == nullptr) {
      r_error = "invalid integer property value";
      return false;
    }
    r_property.type = PropertyType::Int;
    r_property.int_value = integer ? integer->value() : 0;
  }
  else if (type == "float") {
    double floating;
    if (r_property.exists &&
        (!read_number(value, floating) || std::abs(floating) > std::numeric_limits<float>::max()))
    {
      r_error = "invalid float property value";
      return false;
    }
    r_property.type = PropertyType::Float;
    r_property.float_value = r_property.exists ? float(floating) : 0.0f;
  }
  else if (type == "string") {
    const StringValue *string = value ? value->as_string_value() : nullptr;
    if (r_property.exists &&
        (string == nullptr ||
         !is_valid_checkpoint_text(string->value(), kMaxStringPropertyBytes)))
    {
      r_error = "invalid or oversized string property value";
      return false;
    }
    r_property.type = PropertyType::String;
    r_property.string_value = string ? string->value() : std::string();
  }
  else if (type == "vector3" || type == "vector4" || type == "object" || type == "list" ||
           type == "dict")
  {
    static const std::pair<const char *, std::pair<PropertyType, PropertyData::Kind>> kinds[] = {
        {"vector3", {PropertyType::Vector3, PropertyData::Kind::Vector3}},
        {"vector4", {PropertyType::Vector4, PropertyData::Kind::Vector4}},
        {"object", {PropertyType::Object, PropertyData::Kind::Object}},
        {"list", {PropertyType::List, PropertyData::Kind::List}},
        {"dict", {PropertyType::Dict, PropertyData::Kind::Dict}}};
    for (const auto &[name, kind] : kinds) {
      if (type == name) {
        r_property.type = kind.first;
        r_property.data.kind = kind.second;
      }
    }
    if (r_property.exists) {
      const PropertyData::Kind declared_kind = r_property.data.kind;
      size_t items = 0;
      if (!decode_property_data(*value, 0, items, r_property.data, r_error)) {
        return false;
      }
      if (r_property.data.kind != declared_kind) {
        r_error = "property value does not match its type '" + type + "'";
        return false;
      }
    }
  }
  else {
    r_error = "unsupported Save Game property type '" + type + "'";
    return false;
  }
  if (r_property.timer && r_property.type != PropertyType::Float) {
    r_error = "timer property must be a float";
    return false;
  }
  return true;
}

bool decode_material_state(const DictionaryValue &dictionary,
                           const std::string &object_name,
                           ObjectState &r_object,
                           std::string &r_error)
{
  const DictionaryValue *material_state = dictionary.lookup_dict("material_state");
  if (material_state == nullptr ||
      !has_exact_keys(*material_state, {"slots", "parameters"}, {}, r_error))
  {
    r_error = "invalid material state for object '" + object_name + "': " + r_error;
    return false;
  }

  const ArrayValue *slots = material_state->lookup_array("slots");
  const ArrayValue *parameters = material_state->lookup_array("parameters");
  if (slots == nullptr || slots->elements().size() > kMaxMaterialSlotsPerObject ||
      parameters == nullptr ||
      parameters->elements().size() > kMaxMaterialParametersPerObject)
  {
    r_error = "missing or oversized material state for object '" + object_name + "'";
    return false;
  }

  r_object.has_material_state = true;
  r_object.material_slots.reserve(slots->elements().size());
  for (const std::shared_ptr<Value> &value : slots->elements()) {
    MaterialSlotState slot;
    if (value == nullptr) {
      r_error = "material slot entry is invalid";
      return false;
    }
    if (value->type() == blender::io::serialize::eValueType::Null) {
      r_object.material_slots.push_back(std::move(slot));
      continue;
    }
    const DictionaryValue *slot_dictionary = value->as_dictionary_value();
    if (slot_dictionary == nullptr ||
        !has_exact_keys(*slot_dictionary, {"name", "library"}, {}, r_error) ||
        !read_string(*slot_dictionary, "name", slot.material_name, r_error) ||
        !read_string(*slot_dictionary, "library", slot.library_path, r_error) ||
        !is_valid_checkpoint_text(slot.material_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(slot.library_path, kMaxIdentityBytes))
    {
      r_error = "invalid material slot for object '" + object_name + "': " + r_error;
      return false;
    }
    slot.has_material = true;
    r_object.material_slots.push_back(std::move(slot));
  }

  std::unordered_set<std::string> attributes;
  attributes.reserve(parameters->elements().size());
  r_object.material_parameters.reserve(parameters->elements().size());
  for (const std::shared_ptr<Value> &value : parameters->elements()) {
    const DictionaryValue *parameter_dictionary = value ? value->as_dictionary_value() : nullptr;
    MaterialParameterState parameter;
    if (parameter_dictionary == nullptr ||
        !has_exact_keys(*parameter_dictionary, {"attribute", "value"}, {}, r_error) ||
        !read_string(
            *parameter_dictionary, "attribute", parameter.attribute_name, r_error) ||
        !read_float4(*parameter_dictionary, "value", parameter.values, r_error) ||
        !is_valid_checkpoint_text(parameter.attribute_name, MAX_IDPROP_NAME - 1, false))
    {
      r_error = "invalid material parameter for object '" + object_name + "': " + r_error;
      return false;
    }
    if (!attributes.insert(parameter.attribute_name).second) {
      r_error = "duplicate material parameter '" + parameter.attribute_name + "' on object '" +
                object_name + "'";
      return false;
    }
    r_object.material_parameters.push_back(std::move(parameter));
  }
  return true;
}

std::string material_state_identity_key(const MaterialSlotState &identity)
{
  std::string key;
  key.reserve(identity.material_name.size() + identity.library_path.size() + 1u);
  key.append(identity.material_name);
  key.push_back('\0');
  key.append(identity.library_path);
  return key;
}

bool consume_material_socket_value_budget(size_t &remaining,
                                          const MaterialSocketValueState &state)
{
  return consume_budget(remaining, state.socket_identifier.size() + 1u) &&
         consume_budget(remaining, state.string_value.size() + 1u) &&
         consume_budget(remaining, state.datablock_value.name.size() + 1u) &&
         consume_budget(remaining, state.datablock_value.library_path.size() + 1u);
}

bool decode_shared_materials(const DictionaryValue &root,
                             Checkpoint &r_checkpoint,
                             size_t &r_budget,
                             std::string &r_error)
{
  const ArrayValue *materials = root.lookup_array("shared_materials");
  if (materials == nullptr || materials->elements().size() > kMaxSharedMaterials) {
    r_error = "missing or oversized shared material state";
    return false;
  }

  std::unordered_set<std::string> material_identities;
  material_identities.reserve(materials->elements().size());
  r_checkpoint.shared_materials.reserve(materials->elements().size());
  for (const std::shared_ptr<Value> &material_value : materials->elements()) {
    const DictionaryValue *material_dictionary =
        material_value ? material_value->as_dictionary_value() : nullptr;
    SharedMaterialState material;
    if (material_dictionary == nullptr ||
        !has_exact_keys(*material_dictionary, {"name", "library", "nodes"}, {}, r_error) ||
        !read_string(
            *material_dictionary, "name", material.material.material_name, r_error) ||
        !read_string(
            *material_dictionary, "library", material.material.library_path, r_error) ||
        !is_valid_checkpoint_text(material.material.material_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(material.material.library_path, kMaxIdentityBytes))
    {
      r_error = "invalid shared material identity: " + r_error;
      return false;
    }
    material.material.has_material = true;
    if (!material_identities.insert(material_state_identity_key(material.material)).second) {
      r_error = "duplicate shared material state for '" +
                material.material.material_name + "'";
      return false;
    }

    const ArrayValue *nodes = material_dictionary->lookup_array("nodes");
    if (nodes == nullptr || nodes->elements().size() > kMaxMaterialNodesPerAsset ||
        !consume_budget(r_budget, sizeof(SharedMaterialState)) ||
        !consume_budget(r_budget, material.material.material_name.size() + 1u) ||
        !consume_budget(r_budget, material.material.library_path.size() + 1u) ||
        !consume_budget(
            r_budget, nodes->elements().size() * sizeof(SharedMaterialNodeState)))
    {
      r_error = "shared material node state exceeds the Load Game limit";
      return false;
    }

    std::unordered_set<int32_t> node_identifiers;
    node_identifiers.reserve(nodes->elements().size());
    material.nodes.reserve(nodes->elements().size());
    for (const std::shared_ptr<Value> &node_value : nodes->elements()) {
      const DictionaryValue *node_dictionary =
          node_value ? node_value->as_dictionary_value() : nullptr;
      SharedMaterialNodeState node;
      if (node_dictionary == nullptr ||
          !has_exact_keys(*node_dictionary, {"id", "type", "inputs"}, {}, r_error) ||
          !read_int32(*node_dictionary, "id", node.node_identifier, r_error) ||
          !read_string(*node_dictionary, "type", node.node_type, r_error) ||
          !is_valid_checkpoint_text(node.node_type, kMaxNameBytes, false) ||
          !node_identifiers.insert(node.node_identifier).second)
      {
        r_error = "invalid or duplicate shared material node: " + r_error;
        return false;
      }
      if (!consume_budget(r_budget, node.node_type.size() + 1u)) {
        r_error = "shared material node state exceeds the Load Game memory limit";
        return false;
      }

      const ArrayValue *inputs = node_dictionary->lookup_array("inputs");
      if (inputs == nullptr || inputs->elements().size() > kMaxMaterialInputsPerNode ||
          !consume_budget(
              r_budget, inputs->elements().size() * sizeof(MaterialSocketValueState)))
      {
        r_error = "shared material input state exceeds the Load Game limit";
        return false;
      }
      std::unordered_set<std::string> socket_identifiers;
      socket_identifiers.reserve(inputs->elements().size());
      node.inputs.reserve(inputs->elements().size());
      for (const std::shared_ptr<Value> &input_value : inputs->elements()) {
        const DictionaryValue *input_dictionary =
            input_value ? input_value->as_dictionary_value() : nullptr;
        MaterialSocketValueState input;
        if (input_dictionary == nullptr ||
            !decode_material_socket_value(*input_dictionary, input, r_error) ||
            !socket_identifiers.insert(input.socket_identifier).second ||
            !consume_material_socket_value_budget(r_budget, input))
        {
          r_error = "invalid, duplicate, or oversized shared material input: " + r_error;
          return false;
        }
        node.inputs.push_back(std::move(input));
      }
      material.nodes.push_back(std::move(node));
    }
    r_checkpoint.shared_materials.push_back(std::move(material));
  }
  return true;
}

bool runtime_node_tree_type_from_name(const std::string &name,
                                      RuntimeNodeTreeType &r_type)
{
  constexpr RuntimeNodeTreeType types[] = {
      RuntimeNodeTreeType::Geometry,
      RuntimeNodeTreeType::Compositor,
  };
  for (const RuntimeNodeTreeType type : types) {
    if (name == runtime_node_tree_type_name(type)) {
      r_type = type;
      return true;
    }
  }
  return false;
}

std::string runtime_node_tree_identity_key(const RuntimeNodeTreeState &state)
{
  std::string key;
  key.reserve(state.name.size() + state.library_path.size() + 2u);
  key.push_back(char(state.type));
  key.append(state.name);
  key.push_back('\0');
  key.append(state.library_path);
  return key;
}

bool decode_runtime_node_tree_state(const DictionaryValue &root,
                                    Checkpoint &r_checkpoint,
                                    size_t &r_budget,
                                    std::string &r_error)
{
  const ArrayValue *trees = root.lookup_array("runtime_node_trees");
  if (trees == nullptr || trees->elements().size() > kMaxRuntimeNodeTrees ||
      !consume_budget(
          r_budget, trees ? trees->elements().size() * sizeof(RuntimeNodeTreeState) : 0u))
  {
    r_error = "missing or oversized runtime node-tree state";
    return false;
  }

  std::unordered_set<std::string> identities;
  identities.reserve(trees->elements().size());
  r_checkpoint.runtime_node_trees.reserve(trees->elements().size());
  std::string previous_identity;
  for (const std::shared_ptr<Value> &tree_value : trees->elements()) {
    const DictionaryValue *tree_dictionary =
        tree_value ? tree_value->as_dictionary_value() : nullptr;
    RuntimeNodeTreeState tree;
    std::string type_name;
    if (tree_dictionary == nullptr ||
        !has_exact_keys(
            *tree_dictionary, {"type", "name", "library", "fingerprint", "nodes"}, {}, r_error) ||
        !read_string(*tree_dictionary, "type", type_name, r_error) ||
        !runtime_node_tree_type_from_name(type_name, tree.type) ||
        !read_string(*tree_dictionary, "name", tree.name, r_error) ||
        !read_string(*tree_dictionary, "library", tree.library_path, r_error) ||
        !read_string(
            *tree_dictionary, "fingerprint", tree.structural_fingerprint, r_error) ||
        !is_valid_checkpoint_text(tree.name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(tree.library_path, kMaxIdentityBytes) ||
        !is_valid_checkpoint_text(
            tree.structural_fingerprint, kMaxIdentityBytes, false))
    {
      r_error = "invalid runtime node-tree identity: " + r_error;
      return false;
    }
    const std::string identity = runtime_node_tree_identity_key(tree);
    if (!identities.insert(identity).second ||
        (!previous_identity.empty() && identity <= previous_identity))
    {
      r_error = "runtime node-tree identities are duplicate or not canonical";
      return false;
    }
    previous_identity = identity;

    const ArrayValue *nodes = tree_dictionary->lookup_array("nodes");
    if (nodes == nullptr || nodes->elements().size() > kMaxRuntimeNodesPerTree ||
        !consume_budget(r_budget, tree.name.size() + tree.library_path.size() +
                                      tree.structural_fingerprint.size() + 3u) ||
        !consume_budget(
            r_budget, nodes ? nodes->elements().size() * sizeof(RuntimeNodeState) : 0u))
    {
      r_error = "runtime node-tree manifest exceeds the Load Game limit";
      return false;
    }
    tree.nodes.reserve(nodes->elements().size());
    int32_t previous_node_identifier = -1;
    for (const std::shared_ptr<Value> &node_value : nodes->elements()) {
      const DictionaryValue *node_dictionary =
          node_value ? node_value->as_dictionary_value() : nullptr;
      RuntimeNodeState node;
      if (node_dictionary == nullptr ||
          !has_exact_keys(
              *node_dictionary, {"id", "type", "muted", "inputs"}, {}, r_error) ||
          !read_int32(*node_dictionary, "id", node.node_identifier, r_error) ||
          node.node_identifier < 0 || node.node_identifier <= previous_node_identifier ||
          !read_string(*node_dictionary, "type", node.node_type, r_error) ||
          !is_valid_checkpoint_text(node.node_type, kMaxNameBytes, false) ||
          !read_bool(*node_dictionary, "muted", node.muted, r_error) ||
          !consume_budget(r_budget, node.node_type.size() + 1u))
      {
        r_error = "invalid or non-canonical runtime node state: " + r_error;
        return false;
      }
      previous_node_identifier = node.node_identifier;
      const ArrayValue *inputs = node_dictionary->lookup_array("inputs");
      if (inputs == nullptr ||
          inputs->elements().size() > kMaxRuntimeNodeInputsPerNode ||
          !consume_budget(
              r_budget,
              inputs ? inputs->elements().size() * sizeof(MaterialSocketValueState) : 0u))
      {
        r_error = "runtime node input manifest exceeds the Load Game limit";
        return false;
      }
      node.inputs.reserve(inputs->elements().size());
      std::string previous_socket;
      for (const std::shared_ptr<Value> &input_value : inputs->elements()) {
        const DictionaryValue *input_dictionary =
            input_value ? input_value->as_dictionary_value() : nullptr;
        MaterialSocketValueState input;
        if (input_dictionary == nullptr ||
            !decode_material_socket_value(*input_dictionary, input, r_error) ||
            (!previous_socket.empty() && input.socket_identifier <= previous_socket) ||
            !consume_material_socket_value_budget(r_budget, input))
        {
          r_error = "invalid or non-canonical runtime node input: " + r_error;
          return false;
        }
        previous_socket = input.socket_identifier;
        node.inputs.push_back(std::move(input));
      }
      tree.nodes.push_back(std::move(node));
    }
    r_checkpoint.runtime_node_trees.push_back(std::move(tree));
  }

  const DictionaryValue *compositor = root.lookup_dict("compositor_state");
  bool has_node_tree = false;
  if (compositor == nullptr ||
      !read_bool(*compositor, "has_node_tree", has_node_tree, r_error) ||
      !has_exact_keys(*compositor,
                      {"has_node_tree"},
                      has_node_tree ?
                          std::initializer_list<const char *>{
                              "name", "library", "fingerprint"} :
                          std::initializer_list<const char *>{},
                      r_error))
  {
    r_error = "invalid compositor state: " + r_error;
    return false;
  }
  r_checkpoint.compositor.has_node_tree = has_node_tree;
  if (has_node_tree) {
    if (!read_string(
            *compositor, "name", r_checkpoint.compositor.node_tree_name, r_error) ||
        !read_string(*compositor,
                     "library",
                     r_checkpoint.compositor.node_tree_library_path,
                     r_error) ||
        !read_string(*compositor,
                     "fingerprint",
                     r_checkpoint.compositor.node_tree_structural_fingerprint,
                     r_error) ||
        !is_valid_checkpoint_text(
            r_checkpoint.compositor.node_tree_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(
            r_checkpoint.compositor.node_tree_library_path, kMaxIdentityBytes) ||
        !is_valid_checkpoint_text(
            r_checkpoint.compositor.node_tree_structural_fingerprint,
            kMaxIdentityBytes,
            false) ||
        !consume_budget(
            r_budget,
            sizeof(CompositorState) + r_checkpoint.compositor.node_tree_name.size() +
                r_checkpoint.compositor.node_tree_library_path.size() +
                r_checkpoint.compositor.node_tree_structural_fingerprint.size() + 3u))
    {
      r_error = "invalid or oversized compositor identity: " + r_error;
      return false;
    }
    RuntimeNodeTreeState compositor_identity;
    compositor_identity.type = RuntimeNodeTreeType::Compositor;
    compositor_identity.name = r_checkpoint.compositor.node_tree_name;
    compositor_identity.library_path =
        r_checkpoint.compositor.node_tree_library_path;
    const auto found = identities.find(runtime_node_tree_identity_key(compositor_identity));
    if (found == identities.end()) {
      r_error = "compositor root has no matching runtime node-tree state";
      return false;
    }
    const auto matching_tree = std::find_if(
        r_checkpoint.runtime_node_trees.begin(),
        r_checkpoint.runtime_node_trees.end(),
        [&](const RuntimeNodeTreeState &tree) {
          return tree.type == RuntimeNodeTreeType::Compositor &&
                 tree.name == r_checkpoint.compositor.node_tree_name &&
                 tree.library_path ==
                     r_checkpoint.compositor.node_tree_library_path;
        });
    if (matching_tree == r_checkpoint.runtime_node_trees.end() ||
        matching_tree->structural_fingerprint !=
            r_checkpoint.compositor.node_tree_structural_fingerprint)
    {
      r_error = "compositor root fingerprint does not match its node-tree state";
      return false;
    }
  }
  else if (std::any_of(r_checkpoint.runtime_node_trees.begin(),
                       r_checkpoint.runtime_node_trees.end(),
                       [](const RuntimeNodeTreeState &tree) {
                         return tree.type == RuntimeNodeTreeType::Compositor;
                       }))
  {
    r_error = "compositor node-tree state has no saved compositor root";
    return false;
  }
  return true;
}

bool decode_properties(const DictionaryValue &dictionary,
                       const std::string &object_name,
                       ObjectState &r_object,
                       std::string &r_error)
{
  const ArrayValue *properties = dictionary.lookup_array("properties");
  if (properties == nullptr || properties->elements().size() > kMaxPropertiesPerObject) {
    r_error = "missing or oversized property list for object '" + object_name + "'";
    return false;
  }
  std::unordered_set<std::string> names;
  names.reserve(properties->elements().size());
  r_object.properties.reserve(properties->elements().size());
  for (const std::shared_ptr<Value> &value : properties->elements()) {
    const DictionaryValue *property_dictionary = value ? value->as_dictionary_value() : nullptr;
    if (property_dictionary == nullptr) {
      r_error = "property entry is not an object";
      return false;
    }
    PropertyState property;
    if (!decode_property(*property_dictionary, property, r_error)) {
      return false;
    }
    if (!names.insert(property.name).second) {
      r_error = "duplicate property '" + property.name + "' on object '" + object_name + "'";
      return false;
    }
    r_object.properties.push_back(std::move(property));
  }
  return true;
}

bool read_float(const DictionaryValue &dictionary,
                const char *name,
                float &r_value,
                std::string &r_error)
{
  double value;
  if (!read_number(lookup_value(dictionary, name), value) ||
      std::abs(value) > std::numeric_limits<float>::max())
  {
    r_error = std::string("missing or invalid float '") + name + "'";
    return false;
  }
  r_value = float(value);
  return true;
}

bool read_uint16(const DictionaryValue &dictionary,
                 const char *name,
                 uint16_t &r_value,
                 std::string &r_error)
{
  const Value *value = lookup_value(dictionary, name);
  const IntValue *integer = value ? value->as_int_value() : nullptr;
  if (integer == nullptr || integer->value() < 0 ||
      integer->value() > std::numeric_limits<uint16_t>::max())
  {
    r_error = std::string("missing or invalid unsigned integer '") + name + "'";
    return false;
  }
  r_value = uint16_t(integer->value());
  return true;
}

bool decode_hierarchy_state(const DictionaryValue &dictionary,
                            const std::string &object_name,
                            ObjectState &r_object,
                            std::string &r_error)
{
  const DictionaryValue *hierarchy = dictionary.lookup_dict("hierarchy");
  if (hierarchy == nullptr ||
      !has_exact_keys(
          *hierarchy, {"parent", "relation", "compound", "ghost"}, {}, r_error))
  {
    r_error = "invalid hierarchy for object '" + object_name + "': " + r_error;
    return false;
  }
  std::string relation;
  if (!read_string(*hierarchy, "relation", relation, r_error) ||
      !read_bool(*hierarchy, "compound", r_object.hierarchy.compound, r_error) ||
      !read_bool(*hierarchy, "ghost", r_object.hierarchy.ghost, r_error))
  {
    return false;
  }
  const Value *parent = lookup_value(*hierarchy, "parent");
  if (relation == "root") {
    if (parent == nullptr ||
        parent->type() != blender::io::serialize::eValueType::Null ||
        r_object.hierarchy.compound || r_object.hierarchy.ghost)
    {
      r_error = "root hierarchy for object '" + object_name +
                "' must have a null parent and no compound/ghost flags";
      return false;
    }
    r_object.hierarchy.relation = HierarchyRelation::Root;
  }
  else if (relation == "normal") {
    const StringValue *parent_uuid = parent ? parent->as_string_value() : nullptr;
    if (parent_uuid == nullptr || !is_canonical_uuid(parent_uuid->value()) ||
        (r_object.hierarchy.ghost && !r_object.hierarchy.compound))
    {
      r_error = "normal hierarchy for object '" + object_name +
                "' has an invalid parent or compound/ghost contract";
      return false;
    }
    r_object.hierarchy.relation = HierarchyRelation::Normal;
    r_object.hierarchy.parent_id = parent_uuid->value();
  }
  else {
    r_error = "unsupported hierarchy relation for object '" + object_name + "'";
    return false;
  }
  r_object.has_hierarchy_state = true;
  return true;
}

bool decode_rigid_body_state(const DictionaryValue &dictionary,
                             const std::string &object_name,
                             ObjectState &r_object,
                             std::string &r_error)
{
  const Value *physics_value = lookup_value(dictionary, "physics");
  if (physics_value == nullptr) {
    r_error = "missing physics state for object '" + object_name + "'";
    return false;
  }
  if (physics_value->type() == blender::io::serialize::eValueType::Null) {
    return true;
  }
  const DictionaryValue *physics = physics_value->as_dictionary_value();
  if (physics == nullptr ||
      !has_exact_keys(*physics,
                      {"dynamic_body",
                       "mode",
                       "ghost",
                       "physics_suspended",
                       "awake",
                       "linear_velocity",
                       "angular_velocity",
                       "pending_force",
                       "pending_torque",
                       "mass",
                       "friction",
                       "restitution",
                       "linear_damping",
                       "angular_damping",
                       "linear_velocity_min",
                       "linear_velocity_max",
                       "angular_velocity_min",
                       "angular_velocity_max",
                       "gravity_factor",
                       "allow_sleeping",
                       "rotation_enabled",
                       "ccd_enabled",
                       "axis_locks",
                       "collision_group",
                       "collision_mask"},
                      {"runtime_motion_type", "dynamic_ccd_enabled"},
                      r_error))
  {
    r_error = "invalid rigid-body state for object '" + object_name + "': " + r_error;
    return false;
  }

  RigidBodyState &state = r_object.rigid_body;
  std::string mode;
  if (!read_bool(*physics, "dynamic_body", state.dynamic_body, r_error) ||
      !read_string(*physics, "mode", mode, r_error) ||
      !read_bool(*physics, "ghost", state.ghost, r_error) ||
      !read_bool(*physics, "physics_suspended", state.physics_suspended, r_error) ||
      !read_bool(*physics, "awake", state.awake, r_error) ||
      !read_vector3(*physics, "linear_velocity", state.linear_velocity, r_error) ||
      !read_vector3(*physics, "angular_velocity", state.angular_velocity, r_error) ||
      !read_vector3(*physics, "pending_force", state.pending_force, r_error) ||
      !read_vector3(*physics, "pending_torque", state.pending_torque, r_error) ||
      !read_float(*physics, "mass", state.mass, r_error) ||
      !read_float(*physics, "friction", state.friction, r_error) ||
      !read_float(*physics, "restitution", state.restitution, r_error) ||
      !read_float(*physics, "linear_damping", state.linear_damping, r_error) ||
      !read_float(*physics, "angular_damping", state.angular_damping, r_error) ||
      !read_float(*physics, "linear_velocity_min", state.linear_velocity_min, r_error) ||
      !read_float(*physics, "linear_velocity_max", state.linear_velocity_max, r_error) ||
      !read_float(*physics, "angular_velocity_min", state.angular_velocity_min, r_error) ||
      !read_float(*physics, "angular_velocity_max", state.angular_velocity_max, r_error) ||
      !read_float(*physics, "gravity_factor", state.gravity_factor, r_error) ||
      !read_bool(*physics, "allow_sleeping", state.allow_sleeping, r_error) ||
      !read_bool(*physics, "rotation_enabled", state.rotation_enabled, r_error) ||
      !read_bool(*physics, "ccd_enabled", state.ccd_enabled, r_error) ||
      !read_uint16(*physics, "collision_group", state.collision_group, r_error) ||
      !read_uint16(*physics, "collision_mask", state.collision_mask, r_error))
  {
    r_error = "invalid rigid-body state for object '" + object_name + "': " + r_error;
    return false;
  }
  if (mode == "dynamic") {
    state.mode = RigidBodyMode::Dynamic;
  }
  else if (mode == "static_collider") {
    state.mode = RigidBodyMode::StaticCollider;
  }
  else if (mode == "ghost") {
    state.mode = RigidBodyMode::Ghost;
  }
  else if (mode == "no_collision") {
    state.mode = RigidBodyMode::NoCollision;
  }
  else {
    r_error = "invalid rigid-body mode for object '" + object_name + "'";
    return false;
  }

  if (const Value *runtime_motion_type_value = lookup_value(*physics, "runtime_motion_type")) {
    const StringValue *runtime_motion_type = runtime_motion_type_value->as_string_value();
    if (runtime_motion_type == nullptr) {
      r_error = "invalid rigid-body runtime motion type for object '" + object_name + "'";
      return false;
    }
    if (runtime_motion_type->value() == "authored") {
      state.runtime_motion_type = RigidBodyRuntimeMotionType::Authored;
    }
    else if (runtime_motion_type->value() == "dynamic") {
      state.runtime_motion_type = RigidBodyRuntimeMotionType::Dynamic;
    }
    else if (runtime_motion_type->value() == "kinematic") {
      state.runtime_motion_type = RigidBodyRuntimeMotionType::Kinematic;
    }
    else {
      r_error = "invalid rigid-body runtime motion type for object '" + object_name + "'";
      return false;
    }
  }

  /* Added after runtime Kinematic support. Falling back to the effective CCD
   * value preserves every legacy Dynamic state; older Kinematic data did not
   * contain a latent Dynamic preference, so false is the only safe default. */
  state.dynamic_ccd_enabled = state.ccd_enabled;
  if (lookup_value(*physics, "dynamic_ccd_enabled") &&
      !read_bool(*physics, "dynamic_ccd_enabled", state.dynamic_ccd_enabled, r_error))
  {
    r_error = "invalid rigid-body state for object '" + object_name + "': " + r_error;
    return false;
  }

  const ArrayValue *locks = physics->lookup_array("axis_locks");
  if (locks == nullptr || locks->elements().size() != state.axis_locks.size()) {
    r_error = "invalid rigid-body axis locks for object '" + object_name + "'";
    return false;
  }
  for (size_t index = 0; index < state.axis_locks.size(); ++index) {
    const BooleanValue *lock = locks->elements()[index] ?
                                   locks->elements()[index]->as_boolean_value() :
                                   nullptr;
    if (lock == nullptr) {
      r_error = "invalid rigid-body axis lock for object '" + object_name + "'";
      return false;
    }
    state.axis_locks[index] = lock->value();
  }

  const auto vector_is_zero = [](const MT_Vector3 &value) {
    constexpr float tolerance = 1.0e-6f;
    return std::abs(value[0]) <= tolerance && std::abs(value[1]) <= tolerance &&
           std::abs(value[2]) <= tolerance;
  };
  const auto velocity_fits = [](const MT_Vector3 &value, const float maximum) {
    const double length_squared = double(value[0]) * double(value[0]) +
                                  double(value[1]) * double(value[1]) +
                                  double(value[2]) * double(value[2]);
    const double tolerance = std::max(1.0, double(maximum)) * 1.0e-5;
    const double accepted_maximum = double(maximum) + tolerance;
    return length_squared <= accepted_maximum * accepted_maximum;
  };
  constexpr float default_linear_velocity_max = 500.0f;
  constexpr float default_angular_velocity_max =
      0.25f * 3.14159265358979323846f * 60.0f;
  const float effective_linear_max = state.linear_velocity_max > 0.0f ?
                                         state.linear_velocity_max :
                                         default_linear_velocity_max;
  const float effective_angular_max = state.angular_velocity_max > 0.0f ?
                                          state.angular_velocity_max :
                                          default_angular_velocity_max;
  const bool any_translation_dof =
      !state.axis_locks[0] || !state.axis_locks[1] || !state.axis_locks[2];
  const bool any_rotation_dof =
      state.rotation_enabled &&
      (!state.axis_locks[3] || !state.axis_locks[4] || !state.axis_locks[5]);
  const bool static_motion_state =
      !state.dynamic_body ||
      state.mode == RigidBodyMode::StaticCollider || state.mode == RigidBodyMode::Ghost;
  const bool zero_motion = vector_is_zero(state.linear_velocity) &&
                           vector_is_zero(state.angular_velocity) &&
                           vector_is_zero(state.pending_force) &&
                           vector_is_zero(state.pending_torque);
  if ((state.dynamic_body && state.mass <= 0.0f) || (!state.dynamic_body && state.mass < 0.0f) ||
      state.friction < 0.0f || state.restitution < 0.0f || state.restitution > 1.0f ||
      state.linear_damping < 0.0f || state.linear_damping > 1.0f ||
      state.angular_damping < 0.0f || state.angular_damping > 1.0f ||
      state.linear_velocity_min < -1.0f || state.linear_velocity_max < -1.0f ||
      state.angular_velocity_min < -1.0f || state.angular_velocity_max < -1.0f ||
      state.collision_group > 0x03ffu ||
      (state.runtime_motion_type == RigidBodyRuntimeMotionType::Dynamic && !state.dynamic_body) ||
      (state.runtime_motion_type == RigidBodyRuntimeMotionType::Kinematic &&
       (state.dynamic_body || state.rotation_enabled)) ||
      (!state.dynamic_body && state.ccd_enabled) ||
      (state.dynamic_body && state.ccd_enabled != state.dynamic_ccd_enabled) ||
      (state.dynamic_body && !any_translation_dof && !any_rotation_dof) ||
      !velocity_fits(state.linear_velocity, effective_linear_max) ||
      !velocity_fits(state.angular_velocity, effective_angular_max) ||
      (state.awake &&
       (state.physics_suspended || state.mode != RigidBodyMode::Dynamic)) ||
      (static_motion_state && (!zero_motion || state.awake)))
  {
    r_error = "rigid-body values are outside the supported range for object '" + object_name + "'";
    return false;
  }
  r_object.has_rigid_body_state = true;
  return true;
}

/** Set Collection Member's changes ("collections"), when the record has them. */
bool decode_collection_memberships(const DictionaryValue &dictionary,
                                   const std::string &object_name,
                                   ObjectState &r_object,
                                   std::string &r_error)
{
  if (lookup_value(dictionary, "collections") == nullptr) {
    return true;
  }
  const ArrayValue *collections = dictionary.lookup_array("collections");
  if (collections == nullptr || collections->elements().is_empty() ||
      collections->elements().size() > kMaxCollectionMembershipsPerObject)
  {
    r_error = "missing, empty or oversized collections for object '" + object_name + "'";
    return false;
  }
  std::unordered_set<std::string> seen;
  r_object.collection_memberships.reserve(collections->elements().size());
  for (const std::shared_ptr<Value> &value : collections->elements()) {
    const DictionaryValue *entry = value ? value->as_dictionary_value() : nullptr;
    CollectionMembershipState membership;
    if (entry == nullptr ||
        !has_exact_keys(*entry, {"name", "library", "member"}, {}, r_error) ||
        !read_string(*entry, "name", membership.collection_name, r_error) ||
        !read_string(*entry, "library", membership.library_path, r_error) ||
        !read_bool(*entry, "member", membership.member, r_error) ||
        !is_valid_checkpoint_text(membership.collection_name, kMaxNameBytes, false) ||
        !is_valid_checkpoint_text(membership.library_path, kMaxIdentityBytes))
    {
      r_error = "invalid collection membership for object '" + object_name + "': " + r_error;
      return false;
    }
    /* One entry per collection, as the object keeps them. */
    if (!seen.insert(membership.library_path + '\n' + membership.collection_name).second) {
      r_error = "object '" + object_name + "' lists a collection membership twice";
      return false;
    }
    r_object.collection_memberships.push_back(std::move(membership));
  }
  return true;
}

bool decode_object(const DictionaryValue &dictionary,
                   ObjectState &r_object,
                   std::string &r_error)
{
  if (!has_exact_keys(dictionary,
                      {"uuid", "properties"},
                      {"name",
                       "exists",
                       "transform",
                       "material_state",
                       "logic_trees",
                       "modifier_state",
                       "hierarchy",
                       "physics",
                       "collections"},
                      r_error) ||
      !read_string(dictionary, "uuid", r_object.persistence_id, r_error))
  {
    return false;
  }
  if (lookup_value(dictionary, "name") != nullptr &&
      !read_string(dictionary, "name", r_object.name, r_error))
  {
    return false;
  }
  if (!is_canonical_uuid(r_object.persistence_id) ||
      !is_valid_checkpoint_text(r_object.name, kMaxNameBytes))
  {
    r_error = "invalid Save Game object identity";
    return false;
  }

  if (lookup_value(dictionary, "exists") != nullptr) {
    if (!read_bool(dictionary, "exists", r_object.exists, r_error)) {
      return false;
    }
    r_object.has_existence = true;
  }

  if (const Value *transform_value = lookup_value(dictionary, "transform")) {
    const DictionaryValue *transform = transform_value->as_dictionary_value();
    if (transform == nullptr ||
        !has_exact_keys(*transform, {"position", "orientation", "scale"}, {}, r_error) ||
        !read_vector3(*transform, "position", r_object.world_position, r_error) ||
        !read_matrix3(*transform, "orientation", r_object.world_orientation, r_error) ||
        !read_vector3(*transform, "scale", r_object.world_scale, r_error))
    {
      r_error = "invalid transform for object '" + r_object.name + "': " + r_error;
      return false;
    }
    r_object.has_transform = true;
    if (!is_nonsingular_scale(r_object.world_scale)) {
      r_error = "object '" + r_object.name + "' has a singular transform";
      return false;
    }
  }

  if (lookup_value(dictionary, "material_state") != nullptr &&
      !decode_material_state(dictionary, r_object.name, r_object, r_error))
  {
    return false;
  }
  const bool has_logic_tree_field = lookup_value(dictionary, "logic_trees") != nullptr;
  const bool has_modifier_field = lookup_value(dictionary, "modifier_state") != nullptr;
  if (has_logic_tree_field != has_modifier_field) {
    r_error = "object runtime structure state is incomplete";
    return false;
  }
  if (has_logic_tree_field &&
      !decode_runtime_structure_state(dictionary, r_object.name, r_object, r_error))
  {
    return false;
  }
  if (lookup_value(dictionary, "hierarchy") != nullptr &&
      !decode_hierarchy_state(dictionary, r_object.name, r_object, r_error))
  {
    return false;
  }
  const bool has_physics_field = lookup_value(dictionary, "physics") != nullptr;
  if (has_physics_field &&
      !decode_rigid_body_state(dictionary, r_object.name, r_object, r_error))
  {
    return false;
  }
  if (!decode_collection_memberships(dictionary, r_object.name, r_object, r_error)) {
    return false;
  }
  /* Collection memberships belong to a live Save Object State record. */
  if (!r_object.collection_memberships.empty() &&
      !(r_object.has_existence && r_object.exists && r_object.has_hierarchy_state))
  {
    r_error = "object '" + r_object.name +
              "' saves collection memberships without its Save Object State";
    return false;
  }

  if (!decode_properties(dictionary, r_object.name, r_object, r_error)) {
    return false;
  }

  if (r_object.has_existence && !r_object.exists &&
      (r_object.has_transform || r_object.has_material_state || r_object.has_hierarchy_state ||
       r_object.has_runtime_structure_state || has_physics_field ||
       !r_object.properties.empty()))
  {
    r_error = "removed authored object contains runtime state";
    return false;
  }
  if (r_object.has_existence && r_object.exists &&
      (!r_object.has_transform || !r_object.has_material_state || !r_object.has_hierarchy_state ||
       !r_object.has_runtime_structure_state || !has_physics_field))
  {
    r_error = "live Save Object State record is incomplete";
    return false;
  }
  if (!r_object.has_existence &&
      (r_object.has_transform || r_object.has_material_state || r_object.has_hierarchy_state ||
       r_object.has_runtime_structure_state || has_physics_field))
  {
    r_error = "property-only object contains structural state";
    return false;
  }
  if (!r_object.has_existence && r_object.properties.empty())
  {
    r_error = "authored object record selects no state";
    return false;
  }
  return true;
}

bool decode_spawned_object(const DictionaryValue &dictionary,
                           ObjectState &r_object,
                           std::string &r_error)
{
  if (!has_exact_keys(dictionary,
                      {"instance_uuid",
                       "source_uuid",
                       "creation_transform",
                       "transform",
                       "material_state",
                       "logic_trees",
                       "modifier_state",
                       "hierarchy",
                       "physics",
                       "properties"},
                      {"name", "collections"},
                      r_error) ||
      !read_string(dictionary, "instance_uuid", r_object.persistence_id, r_error) ||
      !read_string(dictionary, "source_uuid", r_object.source_id, r_error))
  {
    return false;
  }
  r_object.spawned = true;
  r_object.has_existence = true;
  r_object.exists = true;
  if (!is_canonical_uuid(r_object.persistence_id) || !is_canonical_uuid(r_object.source_id)) {
    r_error = "invalid spawned instance identity";
    return false;
  }
  if (lookup_value(dictionary, "name") != nullptr &&
      !read_string(dictionary, "name", r_object.name, r_error))
  {
    return false;
  }
  if (!is_valid_checkpoint_text(r_object.name, kMaxNameBytes)) {
    r_error = "invalid spawned instance diagnostic name";
    return false;
  }
  const DictionaryValue *creation = dictionary.lookup_dict("creation_transform");
  if (creation == nullptr ||
      !has_exact_keys(*creation, {"position", "orientation", "scale"}, {}, r_error) ||
      !read_vector3(*creation, "position", r_object.creation_position, r_error) ||
      !read_matrix3(*creation, "orientation", r_object.creation_orientation, r_error) ||
      !read_vector3(*creation, "scale", r_object.creation_scale, r_error))
  {
    r_error = "invalid spawned instance creation transform: " + r_error;
    return false;
  }
  if (!is_nonsingular_scale(r_object.creation_scale)) {
    r_error = "spawned instance has a singular creation transform";
    return false;
  }
  const DictionaryValue *transform = dictionary.lookup_dict("transform");
  if (transform == nullptr ||
      !has_exact_keys(*transform, {"position", "orientation", "scale"}, {}, r_error) ||
      !read_vector3(*transform, "position", r_object.world_position, r_error) ||
      !read_matrix3(*transform, "orientation", r_object.world_orientation, r_error) ||
      !read_vector3(*transform, "scale", r_object.world_scale, r_error))
  {
    r_error = "invalid spawned instance transform: " + r_error;
    return false;
  }
  if (!is_nonsingular_scale(r_object.world_scale)) {
    r_error = "spawned instance has a singular transform";
    return false;
  }
  r_object.has_transform = true;
  if (!decode_material_state(dictionary, r_object.name, r_object, r_error) ||
      !decode_runtime_structure_state(dictionary, r_object.name, r_object, r_error) ||
      !decode_hierarchy_state(dictionary, r_object.name, r_object, r_error) ||
      !decode_rigid_body_state(dictionary, r_object.name, r_object, r_error) ||
      !decode_collection_memberships(dictionary, r_object.name, r_object, r_error))
  {
    return false;
  }
  return decode_properties(dictionary, r_object.name, r_object, r_error);
}

bool consume_object_state_budget(size_t &remaining, const ObjectState &object)
{
  if (!consume_budget(remaining, sizeof(ObjectState)) ||
      !consume_budget(remaining, object.persistence_id.size() + 1u) ||
      !consume_budget(remaining, object.source_id.size() + 1u) ||
      !consume_budget(remaining, object.name.size() + 1u) ||
      !consume_budget(remaining, object.hierarchy.parent_id.size() + 1u) ||
      !consume_budget(remaining, object.properties.size() * sizeof(PropertyState)) ||
      !consume_budget(remaining, object.material_slots.size() * sizeof(MaterialSlotState)) ||
      !consume_budget(
          remaining, object.material_parameters.size() * sizeof(MaterialParameterState)) ||
      !consume_budget(
          remaining, object.logic_trees.size() * sizeof(LogicTreeAttachmentState)) ||
      !consume_budget(remaining,
                      object.non_geometry_modifiers.size() *
                          sizeof(NonGeometryModifierState)) ||
      !consume_budget(remaining,
                      object.geometry_nodes_modifiers.size() *
                          sizeof(GeometryNodesModifierState)) ||
      !consume_budget(remaining,
                      object.collection_memberships.size() * sizeof(CollectionMembershipState)))
  {
    return false;
  }
  for (const CollectionMembershipState &membership : object.collection_memberships) {
    if (!consume_budget(remaining,
                        membership.collection_name.size() + membership.library_path.size() + 2u))
    {
      return false;
    }
  }
  for (const PropertyState &property : object.properties) {
    if (!consume_budget(remaining, property.name.size() + 1u) ||
        !consume_budget(remaining, property.string_value.size() + 1u) ||
        !consume_budget(remaining, property_data_bytes(property.data)))
    {
      return false;
    }
  }
  for (const MaterialSlotState &slot : object.material_slots) {
    if (!consume_budget(remaining, slot.material_name.size() + 1u) ||
        !consume_budget(remaining, slot.library_path.size() + 1u))
    {
      return false;
    }
  }
  for (const MaterialParameterState &parameter : object.material_parameters) {
    if (!consume_budget(remaining, parameter.attribute_name.size() + 1u)) {
      return false;
    }
  }
  for (const LogicTreeAttachmentState &tree : object.logic_trees) {
    if (!consume_budget(remaining, tree.tree_name.size() + 1u) ||
        !consume_budget(remaining, tree.library_path.size() + 1u))
    {
      return false;
    }
  }
  for (const NonGeometryModifierState &modifier : object.non_geometry_modifiers) {
    if (!consume_budget(remaining, modifier.name.size() + 1u)) {
      return false;
    }
  }
  for (const GeometryNodesModifierState &modifier : object.geometry_nodes_modifiers) {
    if (!consume_budget(remaining, modifier.name.size() + 1u) ||
        !consume_budget(remaining, modifier.node_group_name.size() + 1u) ||
        !consume_budget(remaining, modifier.node_group_library_path.size() + 1u) ||
        !consume_budget(remaining, modifier.node_group_fingerprint.size() + 1u) ||
        !consume_budget(
            remaining, modifier.inputs.size() * sizeof(GeometryNodesInputState)) ||
        !consume_budget(
            remaining, modifier.outputs.size() * sizeof(GeometryNodesOutputState)))
    {
      return false;
    }
    for (const GeometryNodesInputState &input : modifier.inputs) {
      if (!consume_budget(remaining, input.socket_identifier.size() + 1u) ||
          !consume_budget(remaining, input.attribute_name.size() + 1u) ||
          !consume_budget(remaining, input.layer_name.size() + 1u) ||
          !consume_material_socket_value_budget(remaining, input.value))
      {
        return false;
      }
    }
    for (const GeometryNodesOutputState &output : modifier.outputs) {
      if (!consume_budget(remaining, output.socket_identifier.size() + 1u) ||
          !consume_budget(remaining, output.attribute_name.size() + 1u))
      {
        return false;
      }
    }
  }
  return true;
}

LN_GameStateResult decode_checkpoint_root_data(const Value &root_value,
                                               Checkpoint &r_checkpoint,
                                               std::string &r_error)
{
  const DictionaryValue *root = root_value.as_dictionary_value();
  if (root == nullptr) {
    r_error = "Save Game root is not an object";
    return LN_GameStateResult::CorruptData;
  }
  if (!has_exact_keys(*root,
                      {"format",
                       "scene_uuid",
                       "authored_objects",
                       "spawned_instances",
                       "shared_materials",
                       "runtime_node_trees",
                       "compositor_state",
                       "checksum"},
                      {"scene_name"},
                      r_error))
  {
    return LN_GameStateResult::CorruptData;
  }
  std::string format;
  if (!read_string(*root, "format", format, r_error) ||
      !read_string(*root, "scene_uuid", r_checkpoint.scene_uuid, r_error))
  {
    return LN_GameStateResult::CorruptData;
  }
  if (lookup_value(*root, "scene_name") != nullptr &&
      !read_string(*root, "scene_name", r_checkpoint.scene_name, r_error))
  {
    return LN_GameStateResult::CorruptData;
  }
  if (format != kFormatName) {
    r_error = "not an UPBGE Save Game file";
    return LN_GameStateResult::Incompatible;
  }
  if (!is_canonical_uuid(r_checkpoint.scene_uuid) ||
      !is_valid_checkpoint_text(r_checkpoint.scene_name, kMaxNameBytes))
  {
    r_error = "scene name contains invalid or oversized text";
    return LN_GameStateResult::CorruptData;
  }

  const ArrayValue *objects = root->lookup_array("authored_objects");
  if (objects == nullptr || objects->elements().size() > kMaxObjects) {
    r_error = "missing or oversized Save Game object list";
    return LN_GameStateResult::CorruptData;
  }
  const ArrayValue *spawned = root->lookup_array("spawned_instances");
  if (spawned == nullptr || spawned->elements().size() > kMaxObjects ||
      objects->elements().size() > kMaxObjects - spawned->elements().size())
  {
    r_error = "missing or oversized spawned instance manifest";
    return LN_GameStateResult::CorruptData;
  }
  size_t decoded_budget = kMaxFileBytes;
  if (!consume_budget(decoded_budget, sizeof(Checkpoint)) ||
      !consume_budget(decoded_budget, r_checkpoint.scene_uuid.size() + 1u) ||
      !consume_budget(decoded_budget, r_checkpoint.scene_name.size() + 1u))
  {
    r_error = "decoded Save Game state exceeds the memory limit";
    return LN_GameStateResult::CorruptData;
  }
  std::unordered_set<std::string> identities;
  identities.reserve(objects->elements().size() + spawned->elements().size());
  r_checkpoint.authored_objects.reserve(objects->elements().size());
  for (const std::shared_ptr<Value> &value : objects->elements()) {
    const DictionaryValue *object_dictionary = value ? value->as_dictionary_value() : nullptr;
    if (object_dictionary == nullptr) {
      r_error = "Save Game object entry is not an object";
      return LN_GameStateResult::CorruptData;
    }
    ObjectState object;
    if (!decode_object(*object_dictionary, object, r_error))
    {
      return LN_GameStateResult::CorruptData;
    }
    if (!consume_object_state_budget(decoded_budget, object)) {
      r_error = "decoded Save Game state exceeds the memory limit";
      return LN_GameStateResult::CorruptData;
    }
    if (!identities.insert(object.persistence_id).second) {
      r_error = "duplicate Save Game object identity '" + object.name + "'";
      return LN_GameStateResult::CorruptData;
    }
    r_checkpoint.authored_objects.push_back(std::move(object));
  }
  r_checkpoint.spawned_instances.reserve(spawned->elements().size());
  for (const std::shared_ptr<Value> &value : spawned->elements()) {
    const DictionaryValue *object_dictionary = value ? value->as_dictionary_value() : nullptr;
    if (object_dictionary == nullptr) {
      r_error = "spawned instance entry is not an object";
      return LN_GameStateResult::CorruptData;
    }
    ObjectState object;
    if (!decode_spawned_object(*object_dictionary, object, r_error))
    {
      return LN_GameStateResult::CorruptData;
    }
    if (!consume_object_state_budget(decoded_budget, object)) {
      r_error = "decoded Save Game state exceeds the memory limit";
      return LN_GameStateResult::CorruptData;
    }
    if (!identities.insert(object.persistence_id).second) {
      r_error = "duplicate checkpoint instance UUID";
      return LN_GameStateResult::CorruptData;
    }
    r_checkpoint.spawned_instances.push_back(std::move(object));
  }
  std::unordered_map<std::string, const ObjectState *> structural_objects;
  structural_objects.reserve(r_checkpoint.authored_objects.size() +
                             r_checkpoint.spawned_instances.size());
  auto index_structural = [&structural_objects](const std::vector<ObjectState> &objects) {
    for (const ObjectState &object : objects) {
      if (object.has_existence && object.exists && object.has_hierarchy_state) {
        structural_objects.emplace(object.persistence_id, &object);
      }
    }
  };
  index_structural(r_checkpoint.authored_objects);
  index_structural(r_checkpoint.spawned_instances);

  std::unordered_map<std::string, uint8_t> hierarchy_color;
  hierarchy_color.reserve(structural_objects.size());
  for (const auto &item : structural_objects) {
    if (hierarchy_color[item.first] == 2) {
      continue;
    }
    std::vector<std::string> path;
    const ObjectState *cursor = item.second;
    while (cursor != nullptr) {
      uint8_t &color = hierarchy_color[cursor->persistence_id];
      if (color == 1) {
        r_error = "saved object hierarchy contains a cycle";
        return LN_GameStateResult::CorruptData;
      }
      if (color == 2) {
        break;
      }
      color = 1;
      path.push_back(cursor->persistence_id);
      if (cursor->hierarchy.relation == HierarchyRelation::Root) {
        break;
      }
      if (cursor->hierarchy.parent_id == cursor->persistence_id) {
        r_error = "saved object hierarchy contains a self-parent";
        return LN_GameStateResult::CorruptData;
      }
      const auto parent = structural_objects.find(cursor->hierarchy.parent_id);
      if (parent == structural_objects.end()) {
        r_error = "saved object hierarchy references an absent or non-persistent parent";
        return LN_GameStateResult::CorruptData;
      }
      cursor = parent->second;
    }
    for (const std::string &identity : path) {
      hierarchy_color[identity] = 2;
    }
  }

  if (!decode_shared_materials(*root, r_checkpoint, decoded_budget, r_error)) {
    return LN_GameStateResult::CorruptData;
  }
  if (!decode_runtime_node_tree_state(*root, r_checkpoint, decoded_budget, r_error)) {
    return LN_GameStateResult::CorruptData;
  }

  std::unordered_set<std::string> referenced_materials;
  auto append_references = [&referenced_materials](const std::vector<ObjectState> &objects) {
    for (const ObjectState &object : objects) {
      if (!object.has_material_state) {
        continue;
      }
      for (const MaterialSlotState &slot : object.material_slots) {
        if (slot.has_material) {
          referenced_materials.insert(material_state_identity_key(slot));
        }
      }
    }
  };
  append_references(r_checkpoint.authored_objects);
  append_references(r_checkpoint.spawned_instances);
  if (referenced_materials.size() != r_checkpoint.shared_materials.size()) {
    r_error = "shared material manifest does not match the saved object slots";
    return LN_GameStateResult::CorruptData;
  }
  for (const SharedMaterialState &material : r_checkpoint.shared_materials) {
    if (referenced_materials.find(material_state_identity_key(material.material)) ==
        referenced_materials.end())
    {
      r_error = "shared material manifest contains an unreferenced material";
      return LN_GameStateResult::CorruptData;
    }
  }
  return LN_GameStateResult::Success;
}

}  // namespace

namespace {

LN_GameStateResult encode_checkpoint_impl(const Checkpoint &checkpoint,
                                          std::string &r_contents,
                                          std::string &r_error)
{
  std::shared_ptr<DictionaryValue> root;
  try {
    root = encode_checkpoint_root(checkpoint);
  }
  catch (...) {
    r_error = "could not construct the Save Game document";
    return LN_GameStateResult::InternalError;
  }

  std::string canonical_contents;
  if (!serialize_json(*root, canonical_contents, r_error)) {
    return LN_GameStateResult::InternalError;
  }
  root->append_str("checksum", checkpoint_checksum(canonical_contents));
  if (!serialize_json(*root, r_contents, r_error)) {
    return LN_GameStateResult::InternalError;
  }
  if (r_contents.size() > kMaxFileBytes) {
    r_error = "serialized Save Game data exceeds the file-size limit";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

LN_GameStateResult parse_checkpoint_root(std::string contents,
                                         std::unique_ptr<Value> &r_root,
                                         std::string &r_error)
{
  if (!has_safe_json_nesting(contents)) {
    r_error = "save file exceeds the JSON nesting-depth limit or is structurally invalid";
    return LN_GameStateResult::CorruptData;
  }
  if (!validate_strict_json(contents, r_error)) {
    return LN_GameStateResult::CorruptData;
  }

  std::istringstream stream(std::move(contents));
  JsonFormatter formatter;
  r_root = formatter.deserialize(stream);
  if (r_root == nullptr) {
    r_error = "save file contains malformed JSON";
    return LN_GameStateResult::CorruptData;
  }
  stream >> std::ws;
  if (stream.peek() != std::char_traits<char>::eof()) {
    r_error = "save file contains trailing data after the JSON document";
    return LN_GameStateResult::CorruptData;
  }
  return LN_GameStateResult::Success;
}

LN_GameStateResult decode_checkpoint_checked(const Value &root,
                                             Checkpoint &r_checkpoint,
                                             std::string &r_error)
{
  Checkpoint decoded;
  const LN_GameStateResult decode_result = decode_checkpoint_root_data(root, decoded, r_error);
  if (!LN_GameStateSucceeded(decode_result)) {
    return decode_result;
  }
  const DictionaryValue *root_dictionary = root.as_dictionary_value();
  std::string stored_checksum;
  if (root_dictionary == nullptr ||
      !read_string(*root_dictionary, "checksum", stored_checksum, r_error) ||
      stored_checksum.size() != 32 ||
      !std::all_of(stored_checksum.begin(), stored_checksum.end(), [](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
      }))
  {
    r_error = "Save Game file is missing a valid integrity checksum";
    return LN_GameStateResult::CorruptData;
  }
  std::string canonical_contents;
  try {
    const std::shared_ptr<DictionaryValue> canonical_root = encode_checkpoint_root(decoded);
    if (!serialize_json(*canonical_root, canonical_contents, r_error)) {
      return LN_GameStateResult::InternalError;
    }
  }
  catch (...) {
    r_error = "could not validate Save Game integrity";
    return LN_GameStateResult::InternalError;
  }
  if (checkpoint_checksum(canonical_contents) != stored_checksum) {
    r_error = "Save Game integrity checksum does not match its contents";
    return LN_GameStateResult::CorruptData;
  }
  r_checkpoint = std::move(decoded);
  return LN_GameStateResult::Success;
}

}  // namespace

LN_GameStateResult encode_checkpoint(const Checkpoint &checkpoint,
                                     std::string &r_contents,
                                     std::string &r_error)
{
  return encode_checkpoint_impl(checkpoint, r_contents, r_error);
}

LN_GameStateResult decode_checkpoint(std::string contents,
                                     Checkpoint &r_checkpoint,
                                     std::string &r_error)
{
  std::unique_ptr<Value> root;
  const LN_GameStateResult parse_result =
      parse_checkpoint_root(std::move(contents), root, r_error);
  if (!LN_GameStateSucceeded(parse_result)) {
    return parse_result;
  }
  return decode_checkpoint_checked(*root, r_checkpoint, r_error);
}

}  // namespace upbge::logic_nodes::game_state
