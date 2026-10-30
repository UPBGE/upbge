/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateMaterial.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameStateMaterial.h"

#include "LN_GameStateInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BLI_assert.h"
#include "BLI_hash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DEG_depsgraph.hh"
#include "../../../blender/gpu/GPU_material.hh"
#include "KX_GameObject.h"
#include "KX_Scene.h"

namespace upbge::logic_nodes::game_state {

namespace {

bool values_are_finite(const std::array<float, 4> &values)
{
  return std::all_of(values.begin(), values.end(), [](const float value) {
    return std::isfinite(value);
  });
}

bool material_slot_identity(const blender::Material &material, MaterialSlotState &r_identity)
{
  r_identity.has_material = true;
  r_identity.material_name = material.id.name + 2;
  r_identity.library_path = material.id.lib ? material.id.lib->filepath : std::string();
  return is_valid_checkpoint_text(r_identity.material_name, kMaxNameBytes, false) &&
         is_valid_checkpoint_text(r_identity.library_path, kMaxIdentityBytes);
}

bool material_identity_matches(const blender::Material &material,
                               const MaterialSlotState &identity)
{
  MaterialSlotState current;
  return material_slot_identity(material, current) &&
         current.material_name == identity.material_name &&
         current.library_path == identity.library_path;
}

std::string material_identity_key(const MaterialSlotState &identity)
{
  std::string key;
  key.reserve(identity.material_name.size() + identity.library_path.size() + 1);
  key.append(identity.material_name);
  key.push_back('\0');
  key.append(identity.library_path);
  return key;
}

bool object_material_topology_matches(blender::Object &object, const size_t expected_slot_count)
{
  if (object.totcol < 0 || size_t(object.totcol) != expected_slot_count) {
    return false;
  }
  blender::Material ***data_materials = BKE_object_material_array_p(&object);
  const short *data_slot_count = BKE_object_material_len_p(&object);
  if (expected_slot_count == 0) {
    return data_slot_count == nullptr || *data_slot_count == 0;
  }
  return data_materials != nullptr && *data_materials != nullptr &&
         data_slot_count != nullptr && *data_slot_count >= 0 &&
         size_t(*data_slot_count) == expected_slot_count;
}

blender::Material *data_material_at(blender::Object &object, const size_t slot_index)
{
  blender::Material ***materials = BKE_object_material_array_p(&object);
  const short *slot_count = BKE_object_material_len_p(&object);
  if (materials == nullptr || *materials == nullptr || slot_count == nullptr ||
      slot_index >= size_t(std::max<short>(*slot_count, 0)))
  {
    return nullptr;
  }
  return (*materials)[slot_index];
}

/**
 * Match evaluated rendering semantics: an object-linked null pointer falls back to the material
 * on the object data. The non-evaluated BKE getter intentionally reports that null override.
 */
blender::Material *rendered_material_at(blender::Object &object, const size_t slot_index)
{
  if (slot_index < size_t(std::max<short>(object.totcol, 0)) && object.matbits != nullptr &&
      object.matbits[slot_index] != 0 && object.mat != nullptr &&
      object.mat[slot_index] != nullptr)
  {
    return object.mat[slot_index];
  }
  return data_material_at(object, slot_index);
}

blender::bNodeSocket *find_output_socket(blender::bNode &node, const char *name)
{
  for (blender::bNodeSocket &socket : node.outputs) {
    if (STREQ(socket.identifier, name) || STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
}

blender::bNodeSocket *attribute_output_for_socket(blender::bNode &attribute_node,
                                                  const blender::bNodeSocket &target_socket)
{
  switch (target_socket.type) {
    case blender::SOCK_RGBA:
      return find_output_socket(attribute_node, "Color");
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION:
      return find_output_socket(attribute_node, "Vector");
    case blender::SOCK_BOOLEAN:
    case blender::SOCK_INT:
    case blender::SOCK_FLOAT:
      if (blender::bNodeSocket *socket = find_output_socket(attribute_node, "Fac")) {
        return socket;
      }
      return find_output_socket(attribute_node, "Factor");
    default:
      return nullptr;
  }
}

blender::bNode *find_object_attribute_node(blender::bNodeTree &ntree,
                                           const std::string &attribute_name)
{
  for (blender::bNode &node : ntree.nodes) {
    if (node.type_legacy != SH_NODE_ATTRIBUTE || node.storage == nullptr) {
      continue;
    }
    const auto *storage = static_cast<const blender::NodeShaderAttribute *>(node.storage);
    if (storage->type == blender::SHD_ATTRIBUTE_OBJECT && attribute_name == storage->name) {
      return &node;
    }
  }
  return nullptr;
}

bool socket_is_bound_to_attribute(blender::bNodeTree &ntree,
                                  blender::bNode &target_node,
                                  blender::bNodeSocket &target_socket,
                                  blender::bNode &attribute_node,
                                  blender::bNodeSocket &attribute_output)
{
  bool found = false;
  for (blender::bNodeLink &link : ntree.links) {
    if (link.tonode != &target_node || link.tosock != &target_socket) {
      continue;
    }
    if (link.fromnode != &attribute_node || link.fromsock != &attribute_output || found) {
      return false;
    }
    found = true;
  }
  return found;
}

bool socket_default_values(const blender::bNodeSocket &socket,
                           std::array<float, 4> &r_values)
{
  if (socket.default_value == nullptr) {
    r_values = {0.0f, 0.0f, 0.0f, 1.0f};
    return material_parameter_socket_supports_object_attribute(socket);
  }

  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      const auto *value = static_cast<const blender::bNodeSocketValueBoolean *>(
          socket.default_value);
      const float scalar = value->value ? 1.0f : 0.0f;
      r_values = {scalar, scalar, scalar, 1.0f};
      break;
    }
    case blender::SOCK_INT: {
      const auto *value = static_cast<const blender::bNodeSocketValueInt *>(
          socket.default_value);
      const float scalar = float(value->value);
      r_values = {scalar, scalar, scalar, 1.0f};
      break;
    }
    case blender::SOCK_FLOAT: {
      const auto *value = static_cast<const blender::bNodeSocketValueFloat *>(
          socket.default_value);
      r_values = {value->value, value->value, value->value, 1.0f};
      break;
    }
    case blender::SOCK_VECTOR: {
      const auto *value = static_cast<const blender::bNodeSocketValueVector *>(
          socket.default_value);
      r_values = {value->value[0], value->value[1], value->value[2], 1.0f};
      break;
    }
    case blender::SOCK_ROTATION: {
      const auto *value = static_cast<const blender::bNodeSocketValueRotation *>(
          socket.default_value);
      r_values = {
          value->value_euler[0], value->value_euler[1], value->value_euler[2], 1.0f};
      break;
    }
    case blender::SOCK_RGBA: {
      const auto *value = static_cast<const blender::bNodeSocketValueRGBA *>(
          socket.default_value);
      r_values = {value->value[0], value->value[1], value->value[2], value->value[3]};
      break;
    }
    default:
      return false;
  }
  return values_are_finite(r_values);
}

bool shared_material_socket_type(const blender::bNodeSocket &socket,
                                 MaterialSocketValueType &r_type)
{
  switch (socket.type) {
    case blender::SOCK_BOOLEAN:
      r_type = MaterialSocketValueType::Bool;
      return true;
    case blender::SOCK_INT:
      r_type = MaterialSocketValueType::Int;
      return true;
    case blender::SOCK_MENU: {
      const auto *value = static_cast<const blender::bNodeSocketValueMenu *>(
          socket.default_value);
      if (value == nullptr || value->enum_items == nullptr || value->has_conflict() ||
          value->enum_items->find_item_by_identifier(value->value) == nullptr)
      {
        return false;
      }
      r_type = MaterialSocketValueType::Menu;
      return true;
    }
    case blender::SOCK_INT_VECTOR:
      r_type = MaterialSocketValueType::IntVector;
      return true;
    case blender::SOCK_FLOAT:
      r_type = MaterialSocketValueType::Float;
      return true;
    case blender::SOCK_VECTOR:
      r_type = MaterialSocketValueType::Vector;
      return true;
    case blender::SOCK_ROTATION:
      r_type = MaterialSocketValueType::Rotation;
      return true;
    case blender::SOCK_RGBA:
      r_type = MaterialSocketValueType::Color;
      return true;
    case blender::SOCK_STRING:
      r_type = MaterialSocketValueType::String;
      return true;
    case blender::SOCK_OBJECT:
      r_type = MaterialSocketValueType::Object;
      return true;
    case blender::SOCK_IMAGE:
      r_type = MaterialSocketValueType::Image;
      return true;
    case blender::SOCK_COLLECTION:
      r_type = MaterialSocketValueType::Collection;
      return true;
    case blender::SOCK_TEXTURE:
      r_type = MaterialSocketValueType::Texture;
      return true;
    case blender::SOCK_MATERIAL:
      r_type = MaterialSocketValueType::Material;
      return true;
    case blender::SOCK_FONT:
      r_type = MaterialSocketValueType::Font;
      return true;
    case blender::SOCK_SCENE:
      r_type = MaterialSocketValueType::Scene;
      return true;
    case blender::SOCK_TEXT_ID:
      r_type = MaterialSocketValueType::Text;
      return true;
    case blender::SOCK_MASK:
      r_type = MaterialSocketValueType::Mask;
      return true;
    case blender::SOCK_SOUND:
      r_type = MaterialSocketValueType::Sound;
      return true;
    default:
      return false;
  }
}

int16_t datablock_type_for_material_socket(const MaterialSocketValueType type)
{
  switch (type) {
    case MaterialSocketValueType::Object:
      return int16_t(blender::ID_OB);
    case MaterialSocketValueType::Image:
      return int16_t(blender::ID_IM);
    case MaterialSocketValueType::Collection:
      return int16_t(blender::ID_GR);
    case MaterialSocketValueType::Texture:
      return int16_t(blender::ID_TE);
    case MaterialSocketValueType::Material:
      return int16_t(blender::ID_MA);
    case MaterialSocketValueType::Font:
      return int16_t(blender::ID_VF);
    case MaterialSocketValueType::Scene:
      return int16_t(blender::ID_SCE);
    case MaterialSocketValueType::Text:
      return int16_t(blender::ID_TXT);
    case MaterialSocketValueType::Mask:
      return int16_t(blender::ID_MSK);
    case MaterialSocketValueType::Sound:
      return int16_t(blender::ID_SO);
    default:
      return 0;
  }
}

blender::ID *material_socket_datablock(const blender::bNodeSocket &socket)
{
  if (socket.default_value == nullptr) {
    return nullptr;
  }
  switch (socket.type) {
    case blender::SOCK_OBJECT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueObject *>(socket.default_value)->value);
    case blender::SOCK_IMAGE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueImage *>(socket.default_value)->value);
    case blender::SOCK_COLLECTION:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueCollection *>(socket.default_value)->value);
    case blender::SOCK_TEXTURE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueTexture *>(socket.default_value)->value);
    case blender::SOCK_MATERIAL:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMaterial *>(socket.default_value)->value);
    case blender::SOCK_FONT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueFont *>(socket.default_value)->value);
    case blender::SOCK_SCENE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueScene *>(socket.default_value)->value);
    case blender::SOCK_TEXT_ID:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueText *>(socket.default_value)->value);
    case blender::SOCK_MASK:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMask *>(socket.default_value)->value);
    case blender::SOCK_SOUND:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueSound *>(socket.default_value)->value);
    default:
      return nullptr;
  }
}

bool capture_datablock_reference(const blender::bNodeSocket &socket,
                                 const MaterialSocketValueType type,
                                 DatablockReferenceState &r_reference,
                                 std::string &r_error)
{
  blender::ID *id = material_socket_datablock(socket);
  if (id == nullptr) {
    return true;
  }
  if (*reinterpret_cast<const int16_t *>(id->name) !=
          datablock_type_for_material_socket(type) ||
      id->session_uid == 0 ||
      (id->tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                  blender::ID_TAG_NO_MAIN | blender::ID_TAG_TEMP_MAIN |
                  blender::ID_TAG_COPIED_ON_EVAL |
                  blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) != 0)
  {
    r_error = "shared material input references a runtime-only or invalid datablock";
    return false;
  }
  r_reference.has_value = true;
  r_reference.name = id->name + 2;
  r_reference.library_path = id->lib ? id->lib->filepath : std::string();
  if (!is_valid_checkpoint_text(r_reference.name, kMaxNameBytes, false) ||
      !is_valid_checkpoint_text(r_reference.library_path, kMaxIdentityBytes))
  {
    r_error = "shared material input references a datablock with an invalid identity";
    return false;
  }
  return true;
}

bool capture_shared_material_socket(const blender::bNodeSocket &socket,
                                    MaterialSocketValueState &r_state,
                                    std::string &r_error)
{
  if (socket.default_value == nullptr || (socket.flag & blender::SOCK_UNAVAIL) != 0 ||
      !shared_material_socket_type(socket, r_state.type))
  {
    return false;
  }
  r_state.socket_identifier = socket.identifier;
  if (!is_valid_checkpoint_text(r_state.socket_identifier, kMaxNameBytes, false)) {
    r_error = "shared material has a runtime-editable input without a stable identifier";
    return false;
  }

  switch (r_state.type) {
    case MaterialSocketValueType::Bool:
      r_state.bool_value =
          static_cast<const blender::bNodeSocketValueBoolean *>(socket.default_value)->value;
      return true;
    case MaterialSocketValueType::Int:
      r_state.int_value =
          static_cast<const blender::bNodeSocketValueInt *>(socket.default_value)->value;
      return true;
    case MaterialSocketValueType::Menu:
      r_state.int_value =
          static_cast<const blender::bNodeSocketValueMenu *>(socket.default_value)->value;
      return true;
    case MaterialSocketValueType::IntVector: {
      const auto &value = *static_cast<const blender::bNodeSocketValueIntVector *>(
          socket.default_value);
      if (value.dimensions < 2 || value.dimensions > 3) {
        r_error = "shared material has an integer-vector input with invalid dimensions";
        return false;
      }
      r_state.dimensions = uint8_t(value.dimensions);
      std::copy_n(value.value, value.dimensions, r_state.int_values.begin());
      return true;
    }
    case MaterialSocketValueType::Float: {
      const float value =
          static_cast<const blender::bNodeSocketValueFloat *>(socket.default_value)->value;
      if (!std::isfinite(value)) {
        r_error = "shared material has a non-finite float input";
        return false;
      }
      r_state.float_values[0] = value;
      return true;
    }
    case MaterialSocketValueType::Vector: {
      const auto &value = *static_cast<const blender::bNodeSocketValueVector *>(
          socket.default_value);
      if (value.dimensions < 2 || value.dimensions > 4) {
        r_error = "shared material has a vector input with invalid dimensions";
        return false;
      }
      r_state.dimensions = uint8_t(value.dimensions);
      std::copy_n(value.value, value.dimensions, r_state.float_values.begin());
      break;
    }
    case MaterialSocketValueType::Rotation: {
      const auto &value = *static_cast<const blender::bNodeSocketValueRotation *>(
          socket.default_value);
      std::copy_n(value.value_euler, 3, r_state.float_values.begin());
      break;
    }
    case MaterialSocketValueType::Color: {
      const auto &value = *static_cast<const blender::bNodeSocketValueRGBA *>(
          socket.default_value);
      std::copy_n(value.value, 4, r_state.float_values.begin());
      break;
    }
    case MaterialSocketValueType::String: {
      const auto &value = *static_cast<const blender::bNodeSocketValueString *>(
          socket.default_value);
      r_state.string_value = value.value;
      if (!is_valid_checkpoint_text(r_state.string_value, kMaxMaterialStringBytes)) {
        r_error = "shared material has an invalid or oversized string input";
        return false;
      }
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
    case MaterialSocketValueType::Sound:
      return capture_datablock_reference(
          socket, r_state.type, r_state.datablock_value, r_error);
  }
  return values_are_finite(r_state.float_values);
}

struct RuntimeBinding {
  blender::Material *material = nullptr;
  int32_t node_identifier = 0;
  std::string socket_identifier;
  std::string attribute_name;
  std::array<float, 4> default_values = {0.0f, 0.0f, 0.0f, 1.0f};
};

bool same_binding(const RuntimeBinding &a, const RuntimeBinding &b)
{
  return a.material == b.material && a.node_identifier == b.node_identifier &&
         a.socket_identifier == b.socket_identifier;
}

struct BindingScan {
  bool valid = false;
  std::vector<RuntimeBinding> bindings;
  std::string error;
};

BindingScan scan_material_bindings(blender::Material &material)
{
  BindingScan scan;
  scan.valid = true;
  blender::bNodeTree *ntree = material.nodetree;
  if (ntree == nullptr) {
    return scan;
  }

  try {
    std::unordered_map<std::string, RuntimeBinding> by_attribute;
    for (blender::bNode &node : ntree->nodes) {
      for (blender::bNodeSocket &socket : node.inputs) {
        if (!material_parameter_socket_supports_object_attribute(socket)) {
          continue;
        }
        const std::string attribute_name = material_parameter_attribute_name(
            &material, node, socket);
        blender::bNode *attribute_node = find_object_attribute_node(*ntree, attribute_name);
        blender::bNodeSocket *attribute_output =
            attribute_node ? attribute_output_for_socket(*attribute_node, socket) : nullptr;
        if (attribute_node == nullptr || attribute_output == nullptr ||
            !socket_is_bound_to_attribute(
                *ntree, node, socket, *attribute_node, *attribute_output))
        {
          continue;
        }

        RuntimeBinding binding;
        binding.material = &material;
        binding.node_identifier = node.identifier;
        binding.socket_identifier = socket.identifier;
        binding.attribute_name = attribute_name;
        if (!is_valid_checkpoint_text(binding.attribute_name, MAX_IDPROP_NAME - 1, false) ||
            !is_valid_checkpoint_text(binding.socket_identifier, kMaxNameBytes, false) ||
            !socket_default_values(socket, binding.default_values))
        {
          scan.valid = false;
          scan.error = "material '" + std::string(material.id.name + 2) +
                       "' has an invalid C++ Logic Nodes material-parameter binding";
          return scan;
        }
        const auto inserted = by_attribute.emplace(binding.attribute_name, binding);
        if (!inserted.second && !same_binding(inserted.first->second, binding)) {
          scan.valid = false;
          scan.error = "material '" + std::string(material.id.name + 2) +
                       "' has colliding C++ Logic Nodes material-parameter keys";
          return scan;
        }
      }
    }
    scan.bindings.reserve(by_attribute.size());
    for (auto &item : by_attribute) {
      scan.bindings.push_back(std::move(item.second));
    }
    std::sort(scan.bindings.begin(),
              scan.bindings.end(),
              [](const RuntimeBinding &a, const RuntimeBinding &b) {
                return a.attribute_name < b.attribute_name;
              });
  }
  catch (...) {
    scan.valid = false;
    scan.bindings.clear();
    scan.error = "could not inspect material parameter bindings";
  }
  return scan;
}

blender::IDProperty *material_parameter_property(blender::Object &object,
                                                 const std::string &name)
{
  blender::IDProperty *properties = blender::IDP_GetProperties(&object.id);
  return properties ? blender::IDP_GetPropertyFromGroup_null(properties, name) : nullptr;
}

bool material_parameter_property_is_valid(const blender::IDProperty *property)
{
  return property != nullptr && property->type == blender::IDP_ARRAY &&
         property->subtype == blender::IDP_FLOAT && property->len == 4 &&
         property->data.pointer != nullptr;
}

blender::IDProperty *create_material_parameter_property(
    blender::Object &object,
    const std::string &name,
    const std::array<float, 4> &values)
{
  blender::IDProperty *properties = blender::IDP_EnsureProperties(&object.id);
  if (properties == nullptr) {
    return nullptr;
  }
  blender::IDPropertyTemplate template_value{};
  template_value.array.len = 4;
  template_value.array.type = blender::IDP_FLOAT;
  blender::IDProperty *property = blender::IDP_New(
      blender::IDP_ARRAY, &template_value, name);
  if (property == nullptr || !blender::IDP_AddToGroup(properties, property)) {
    if (property != nullptr) {
      blender::IDP_FreeProperty(property);
    }
    return nullptr;
  }
  float *data = static_cast<float *>(property->data.pointer);
  std::copy(values.begin(), values.end(), data);
  return property;
}

bool append_selected_bindings(
    blender::Material &material,
    std::unordered_map<blender::Material *, BindingScan> &scan_cache,
    std::unordered_map<std::string, RuntimeBinding> &r_bindings,
    std::string &r_error)
{
  auto [scan_it, inserted] = scan_cache.try_emplace(&material);
  if (inserted) {
    scan_it->second = scan_material_bindings(material);
  }
  const BindingScan &scan = scan_it->second;
  if (!scan.valid) {
    r_error = scan.error;
    return false;
  }
  for (const RuntimeBinding &binding : scan.bindings) {
    const auto binding_result = r_bindings.emplace(binding.attribute_name, binding);
    if (!binding_result.second && !same_binding(binding_result.first->second, binding)) {
      r_error = "assigned materials produce a colliding C++ Logic Nodes material-parameter key";
      return false;
    }
  }
  return true;
}

}  // namespace

bool material_parameter_socket_supports_object_attribute(const blender::bNodeSocket &socket)
{
  return ELEM(socket.type,
              blender::SOCK_BOOLEAN,
              blender::SOCK_INT,
              blender::SOCK_FLOAT,
              blender::SOCK_VECTOR,
              blender::SOCK_ROTATION,
              blender::SOCK_RGBA);
}

std::string material_parameter_attribute_name(const blender::Material *material,
                                              const blender::bNode &node,
                                              const blender::bNodeSocket &socket)
{
  const uint32_t material_hash = blender::BLI_hash_string(material ? material->id.name : "");
  uint32_t socket_hash = uint32_t(node.identifier);
  socket_hash = blender::BLI_hash_int_2d(socket_hash,
                                         blender::BLI_hash_string(socket.identifier));
  char name[MAX_IDPROP_NAME];
  std::snprintf(name, sizeof(name), "_LNMP_%08x_%08x", material_hash, socket_hash);
  return name;
}

bool read_object_material_parameter_values(const blender::Object *object,
                                           const std::string &property_name,
                                           std::array<float, 4> &r_values)
{
  if (object == nullptr || property_name.empty()) {
    return false;
  }
  blender::IDProperty *property = material_parameter_property(
      *const_cast<blender::Object *>(object), property_name);
  if (!material_parameter_property_is_valid(property)) {
    return false;
  }
  const float *data = static_cast<const float *>(property->data.pointer);
  r_values = {data[0], data[1], data[2], data[3]};
  return values_are_finite(r_values);
}

struct MaterialAssetCatalog::Impl {
  std::unordered_map<uint32_t, MaterialSlotState> by_session_uid;
  std::unordered_map<std::string, uint32_t> session_uid_by_identity;
};

MaterialAssetCatalog::MaterialAssetCatalog() : m_impl(std::make_unique<Impl>()) {}
MaterialAssetCatalog::~MaterialAssetCatalog() = default;
MaterialAssetCatalog::MaterialAssetCatalog(MaterialAssetCatalog &&) noexcept = default;
MaterialAssetCatalog &MaterialAssetCatalog::operator=(MaterialAssetCatalog &&) noexcept = default;

LN_GameStateResult build_material_asset_catalog(blender::Main &bmain,
                                                MaterialAssetCatalog &r_catalog,
                                                std::string &r_error)
{
  r_error.clear();
  MaterialAssetCatalog catalog;
  try {
    for (blender::Material &material : bmain.materials) {
      MaterialSlotState identity;
      if (material.id.session_uid == 0 || !material_slot_identity(material, identity)) {
        r_error = "runtime material catalog contains an invalid asset identity";
        return LN_GameStateResult::UnsupportedContent;
      }
      const std::string key = material_identity_key(identity);
      if (!catalog.m_impl->by_session_uid.emplace(material.id.session_uid, identity).second ||
          !catalog.m_impl->session_uid_by_identity.emplace(key, material.id.session_uid).second)
      {
        r_error = "runtime material catalog contains a duplicate asset identity";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
  }
  catch (...) {
    r_error = "could not allocate the durable runtime material catalog";
    return LN_GameStateResult::InternalError;
  }
  r_catalog = std::move(catalog);
  return LN_GameStateResult::Success;
}

struct MaterialCaptureContext::Impl {
  explicit Impl(const MaterialAssetCatalog &catalog) : catalog(catalog.m_impl.get()) {}

  const MaterialAssetCatalog::Impl *catalog = nullptr;
  std::unordered_map<blender::Material *, BindingScan> binding_scans;
  std::unordered_set<blender::Material *> captured_materials;
  std::vector<SharedMaterialState> shared_materials;

  LN_GameStateResult capture_shared_material(blender::Material &material,
                                             size_t &r_budget,
                                             std::string &r_error)
  {
    if (captured_materials.find(&material) != captured_materials.end()) {
      return LN_GameStateResult::Success;
    }
    if (shared_materials.size() >= kMaxSharedMaterials) {
      r_error = "Save Game exceeds the shared material limit";
      return LN_GameStateResult::UnsupportedContent;
    }

    const auto asset = catalog->by_session_uid.find(material.id.session_uid);
    if (asset == catalog->by_session_uid.end() ||
        !material_identity_matches(material, asset->second))
    {
      r_error = "runtime-created or renamed material cannot be loaded in a future game session";
      return LN_GameStateResult::UnsupportedContent;
    }

    SharedMaterialState material_state;
    material_state.material = asset->second;
    if (!consume_budget(r_budget,
                        sizeof(SharedMaterialState) +
                            material_state.material.material_name.size() +
                            material_state.material.library_path.size() + 2u))
    {
      r_error = "Save Game shared material state exceeds the memory budget";
      return LN_GameStateResult::UnsupportedContent;
    }

    blender::bNodeTree *ntree = material.nodetree;
    if (ntree != nullptr) {
      ntree->ensure_topology_cache();
      for (blender::bNode &node : ntree->nodes) {
        SharedMaterialNodeState node_state;
        node_state.node_identifier = node.identifier;
        node_state.node_type = node.idname;
        for (blender::bNodeSocket &socket : node.inputs) {
          if (socket.is_directly_linked()) {
            continue;
          }
          MaterialSocketValueState input_state;
          std::string socket_error;
          if (!capture_shared_material_socket(socket, input_state, socket_error)) {
            if (!socket_error.empty()) {
              r_error = "material '" + material_state.material.material_name + "': " +
                        socket_error;
              return LN_GameStateResult::UnsupportedContent;
            }
            continue;
          }
          if (input_state.type == MaterialSocketValueType::Material &&
              input_state.datablock_value.has_value)
          {
            blender::ID *id = material_socket_datablock(socket);
            blender::Material *referenced = reinterpret_cast<blender::Material *>(id);
            const auto referenced_asset =
                referenced ? catalog->by_session_uid.find(referenced->id.session_uid) :
                             catalog->by_session_uid.end();
            if (referenced_asset == catalog->by_session_uid.end() ||
                !material_identity_matches(*referenced, referenced_asset->second))
            {
              r_error = "material '" + material_state.material.material_name +
                        "' references a runtime-created or renamed material";
              return LN_GameStateResult::UnsupportedContent;
            }
          }
          if (node_state.inputs.size() >= kMaxMaterialInputsPerNode ||
              !consume_budget(r_budget,
                              sizeof(MaterialSocketValueState) +
                                  input_state.socket_identifier.size() +
                                  input_state.string_value.size() +
                                  input_state.datablock_value.name.size() +
                                  input_state.datablock_value.library_path.size() + 4u))
          {
            r_error = "material '" + material_state.material.material_name +
                      "' exceeds the shared input persistence limit";
            return LN_GameStateResult::UnsupportedContent;
          }
          node_state.inputs.push_back(std::move(input_state));
        }
        if (node_state.inputs.empty()) {
          continue;
        }
        if (!is_valid_checkpoint_text(node_state.node_type, kMaxNameBytes, false) ||
            material_state.nodes.size() >= kMaxMaterialNodesPerAsset ||
            !consume_budget(
                r_budget, sizeof(SharedMaterialNodeState) + node_state.node_type.size() + 1u))
        {
          r_error = "material '" + material_state.material.material_name +
                    "' exceeds the shared node persistence limit";
          return LN_GameStateResult::UnsupportedContent;
        }
        std::sort(node_state.inputs.begin(),
                  node_state.inputs.end(),
                  [](const MaterialSocketValueState &a, const MaterialSocketValueState &b) {
                    return a.socket_identifier < b.socket_identifier;
                  });
        material_state.nodes.push_back(std::move(node_state));
      }
      std::sort(material_state.nodes.begin(),
                material_state.nodes.end(),
                [](const SharedMaterialNodeState &a, const SharedMaterialNodeState &b) {
                  return a.node_identifier < b.node_identifier;
                });
    }
    shared_materials.push_back(std::move(material_state));
    captured_materials.insert(&material);
    return LN_GameStateResult::Success;
  }
};

MaterialCaptureContext::MaterialCaptureContext(const MaterialAssetCatalog &catalog)
    : m_impl(std::make_unique<Impl>(catalog))
{
}
MaterialCaptureContext::~MaterialCaptureContext() = default;
MaterialCaptureContext::MaterialCaptureContext(MaterialCaptureContext &&) noexcept = default;
MaterialCaptureContext &MaterialCaptureContext::operator=(MaterialCaptureContext &&) noexcept =
    default;

LN_GameStateResult capture_material_state(KX_GameObject &game_object,
                                          const size_t expected_slot_count,
                                          MaterialCaptureContext &context,
                                          ObjectState &r_state,
                                          size_t &r_budget,
                                          std::string &r_error)
{
  blender::Object *object = game_object.GetBlenderObject();
  if (object == nullptr ||
      !object_material_topology_matches(*object, expected_slot_count) ||
      expected_slot_count > kMaxMaterialSlotsPerObject || context.m_impl == nullptr ||
      context.m_impl->catalog == nullptr)
  {
    r_error = "material slot topology changed for object '" + game_object.GetName() + "'";
    return LN_GameStateResult::UnsupportedContent;
  }

  try {
    r_state.has_material_state = true;
    r_state.material_slots.clear();
    r_state.material_parameters.clear();
    r_state.material_slots.reserve(expected_slot_count);
    std::unordered_map<std::string, RuntimeBinding> bindings;
    for (size_t slot_index = 0; slot_index < expected_slot_count; ++slot_index) {
      blender::Material *material = rendered_material_at(*object, slot_index);
      MaterialSlotState slot;
      if (material != nullptr) {
        const auto asset = context.m_impl->catalog->by_session_uid.find(
            material->id.session_uid);
        if (asset == context.m_impl->catalog->by_session_uid.end() ||
            !material_identity_matches(*material, asset->second))
        {
          r_error = "object '" + game_object.GetName() +
                    "' uses a runtime-created or renamed material that cannot be loaded in a "
                    "future game session";
          return LN_GameStateResult::UnsupportedContent;
        }
        slot = asset->second;
        const LN_GameStateResult shared_result =
            context.m_impl->capture_shared_material(*material, r_budget, r_error);
        if (!LN_GameStateSucceeded(shared_result)) {
          r_error = "object '" + game_object.GetName() + "': " + r_error;
          return shared_result;
        }
        if (!append_selected_bindings(
                *material, context.m_impl->binding_scans, bindings, r_error))
        {
          r_error = "object '" + game_object.GetName() + "': " + r_error;
          return LN_GameStateResult::UnsupportedContent;
        }
      }
      if (!consume_budget(r_budget,
                          sizeof(MaterialSlotState) + slot.material_name.size() +
                              slot.library_path.size() + 2u))
      {
        r_error = "Save Game material state exceeds the memory budget";
        return LN_GameStateResult::UnsupportedContent;
      }
      r_state.material_slots.push_back(std::move(slot));
    }

    if (bindings.size() > kMaxMaterialParametersPerObject) {
      r_error = "object '" + game_object.GetName() +
                "' exceeds the material-parameter persistence limit";
      return LN_GameStateResult::UnsupportedContent;
    }
    std::vector<const RuntimeBinding *> sorted_bindings;
    sorted_bindings.reserve(bindings.size());
    for (const auto &item : bindings) {
      sorted_bindings.push_back(&item.second);
    }
    std::sort(sorted_bindings.begin(),
              sorted_bindings.end(),
              [](const RuntimeBinding *a, const RuntimeBinding *b) {
                return a->attribute_name < b->attribute_name;
              });
    r_state.material_parameters.reserve(sorted_bindings.size());
    for (const RuntimeBinding *binding : sorted_bindings) {
      MaterialParameterState parameter;
      parameter.attribute_name = binding->attribute_name;
      blender::IDProperty *property = material_parameter_property(
          *object, parameter.attribute_name);
      if (property != nullptr && !material_parameter_property_is_valid(property)) {
        r_error = "object '" + game_object.GetName() +
                  "' has an incompatible C++ Logic Nodes material-parameter property";
        return LN_GameStateResult::UnsupportedContent;
      }
      if (property == nullptr) {
        parameter.values = binding->default_values;
      }
      else if (!read_object_material_parameter_values(
                   object, parameter.attribute_name, parameter.values))
      {
        r_error = "object '" + game_object.GetName() +
                  "' has a non-finite C++ Logic Nodes material-parameter value";
        return LN_GameStateResult::UnsupportedContent;
      }
      if (!values_are_finite(parameter.values) ||
          !consume_budget(r_budget,
                          sizeof(MaterialParameterState) + parameter.attribute_name.size() + 1u))
      {
        r_error = "object '" + game_object.GetName() +
                  "' has invalid or oversized material parameter state";
        return LN_GameStateResult::UnsupportedContent;
      }
      r_state.material_parameters.push_back(std::move(parameter));
    }
  }
  catch (...) {
    r_error = "could not capture material state for object '" + game_object.GetName() + "'";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

LN_GameStateResult finalize_material_capture(MaterialCaptureContext &context,
                                             Checkpoint &r_checkpoint,
                                             std::string &r_error)
{
  if (context.m_impl == nullptr) {
    r_error = "material capture context is unavailable";
    return LN_GameStateResult::InternalError;
  }
  try {
    std::sort(context.m_impl->shared_materials.begin(),
              context.m_impl->shared_materials.end(),
              [](const SharedMaterialState &a, const SharedMaterialState &b) {
                return material_identity_key(a.material) < material_identity_key(b.material);
              });
    r_checkpoint.shared_materials = std::move(context.m_impl->shared_materials);
  }
  catch (...) {
    r_error = "could not finalize shared material state";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

namespace {

bool material_socket_matches_state(const blender::bNodeSocket &socket,
                                   const MaterialSocketValueState &state)
{
  MaterialSocketValueType current_type;
  if (!shared_material_socket_type(socket, current_type) || current_type != state.type) {
    return false;
  }
  if (state.type == MaterialSocketValueType::IntVector) {
    const auto &value = *static_cast<const blender::bNodeSocketValueIntVector *>(
        socket.default_value);
    return value.dimensions == state.dimensions;
  }
  if (state.type == MaterialSocketValueType::Vector) {
    const auto &value = *static_cast<const blender::bNodeSocketValueVector *>(
        socket.default_value);
    return value.dimensions == state.dimensions;
  }
  if (state.type == MaterialSocketValueType::Menu) {
    const auto &value = *static_cast<const blender::bNodeSocketValueMenu *>(
        socket.default_value);
    return value.enum_items != nullptr && !value.has_conflict() &&
           value.enum_items->find_item_by_identifier(state.int_value) != nullptr;
  }
  return true;
}

blender::bNodeSocket *find_material_input(blender::bNode &node,
                                         const std::string &identifier)
{
  blender::bNodeSocket *found = nullptr;
  for (blender::bNodeSocket &socket : node.inputs) {
    if (identifier != socket.identifier) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &socket;
  }
  return found;
}

bool datablock_identity_matches(const blender::ID &id,
                                const int16_t expected_type,
                                const DatablockReferenceState &reference)
{
  return *reinterpret_cast<const int16_t *>(id.name) == expected_type &&
         reference.has_value &&
         reference.name == id.name + 2 &&
         reference.library_path == (id.lib ? id.lib->filepath : std::string()) &&
         (id.tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                    blender::ID_TAG_NO_MAIN |
                    blender::ID_TAG_TEMP_MAIN | blender::ID_TAG_COPIED_ON_EVAL |
                    blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) == 0;
}

blender::Library *find_library_by_path(blender::Main &bmain,
                                       const std::string &library_path)
{
  blender::Library *found = nullptr;
  for (blender::Library &library : bmain.libraries) {
    if (library_path != library.filepath) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &library;
  }
  return found;
}

bool set_material_socket_datablock(blender::bNodeSocket &socket, blender::ID *desired)
{
  blender::ID *current = material_socket_datablock(socket);
  if (current == desired) {
    return false;
  }
  if (desired != nullptr) {
    blender::id_us_plus(desired);
  }
  if (current != nullptr) {
    blender::id_us_min(current);
  }
  switch (socket.type) {
    case blender::SOCK_OBJECT:
      static_cast<blender::bNodeSocketValueObject *>(socket.default_value)->value =
          reinterpret_cast<blender::Object *>(desired);
      break;
    case blender::SOCK_IMAGE:
      static_cast<blender::bNodeSocketValueImage *>(socket.default_value)->value =
          reinterpret_cast<blender::Image *>(desired);
      break;
    case blender::SOCK_COLLECTION:
      static_cast<blender::bNodeSocketValueCollection *>(socket.default_value)->value =
          reinterpret_cast<blender::Collection *>(desired);
      break;
    case blender::SOCK_TEXTURE:
      static_cast<blender::bNodeSocketValueTexture *>(socket.default_value)->value =
          reinterpret_cast<blender::Tex *>(desired);
      break;
    case blender::SOCK_MATERIAL:
      static_cast<blender::bNodeSocketValueMaterial *>(socket.default_value)->value =
          reinterpret_cast<blender::Material *>(desired);
      break;
    case blender::SOCK_FONT:
      static_cast<blender::bNodeSocketValueFont *>(socket.default_value)->value =
          reinterpret_cast<blender::VFont *>(desired);
      break;
    case blender::SOCK_SCENE:
      static_cast<blender::bNodeSocketValueScene *>(socket.default_value)->value =
          reinterpret_cast<blender::Scene *>(desired);
      break;
    case blender::SOCK_TEXT_ID:
      static_cast<blender::bNodeSocketValueText *>(socket.default_value)->value =
          reinterpret_cast<blender::Text *>(desired);
      break;
    case blender::SOCK_MASK:
      static_cast<blender::bNodeSocketValueMask *>(socket.default_value)->value =
          reinterpret_cast<blender::Mask *>(desired);
      break;
    case blender::SOCK_SOUND:
      static_cast<blender::bNodeSocketValueSound *>(socket.default_value)->value =
          reinterpret_cast<blender::bSound *>(desired);
      break;
    default:
      BLI_assert_unreachable();
      break;
  }
  return true;
}

bool apply_material_socket_value(blender::bNodeSocket &socket,
                                 const MaterialSocketValueState &state,
                                 blender::ID *datablock)
{
  switch (state.type) {
    case MaterialSocketValueType::Bool: {
      auto &value = *static_cast<blender::bNodeSocketValueBoolean *>(socket.default_value);
      if (bool(value.value) == state.bool_value) {
        return false;
      }
      value.value = state.bool_value;
      return true;
    }
    case MaterialSocketValueType::Int: {
      auto &value = *static_cast<blender::bNodeSocketValueInt *>(socket.default_value);
      if (value.value == state.int_value) {
        return false;
      }
      value.value = state.int_value;
      return true;
    }
    case MaterialSocketValueType::Menu: {
      auto &value = *static_cast<blender::bNodeSocketValueMenu *>(socket.default_value);
      if (value.value == state.int_value) {
        return false;
      }
      value.value = state.int_value;
      return true;
    }
    case MaterialSocketValueType::IntVector: {
      auto &value = *static_cast<blender::bNodeSocketValueIntVector *>(
          socket.default_value);
      if (std::equal(state.int_values.begin(),
                     state.int_values.begin() + state.dimensions,
                     value.value))
      {
        return false;
      }
      std::copy_n(state.int_values.begin(), state.dimensions, value.value);
      return true;
    }
    case MaterialSocketValueType::Float: {
      auto &value = *static_cast<blender::bNodeSocketValueFloat *>(socket.default_value);
      if (value.value == state.float_values[0]) {
        return false;
      }
      value.value = state.float_values[0];
      return true;
    }
    case MaterialSocketValueType::Vector: {
      auto &value = *static_cast<blender::bNodeSocketValueVector *>(socket.default_value);
      if (std::equal(state.float_values.begin(),
                     state.float_values.begin() + state.dimensions,
                     value.value))
      {
        return false;
      }
      std::copy_n(state.float_values.begin(), state.dimensions, value.value);
      return true;
    }
    case MaterialSocketValueType::Rotation: {
      auto &value = *static_cast<blender::bNodeSocketValueRotation *>(
          socket.default_value);
      if (std::equal(state.float_values.begin(), state.float_values.begin() + 3, value.value_euler))
      {
        return false;
      }
      std::copy_n(state.float_values.begin(), 3, value.value_euler);
      return true;
    }
    case MaterialSocketValueType::Color: {
      auto &value = *static_cast<blender::bNodeSocketValueRGBA *>(socket.default_value);
      if (std::equal(state.float_values.begin(), state.float_values.end(), value.value)) {
        return false;
      }
      std::copy(state.float_values.begin(), state.float_values.end(), value.value);
      return true;
    }
    case MaterialSocketValueType::String: {
      auto &value = *static_cast<blender::bNodeSocketValueString *>(socket.default_value);
      if (STREQ(value.value, state.string_value.c_str())) {
        return false;
      }
      blender::BLI_strncpy(value.value, state.string_value.c_str(), sizeof(value.value));
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
    case MaterialSocketValueType::Sound:
      return set_material_socket_datablock(socket, datablock);
  }
  BLI_assert_unreachable();
  return false;
}

}  // namespace

struct PreparedMaterialRestore::Impl {
  struct SharedInput {
    blender::bNodeSocket *socket = nullptr;
    MaterialSocketValueState state;
    blender::ID *datablock = nullptr;
  };

  struct SharedMaterialPlan {
    blender::Material *material = nullptr;
    blender::bNodeTree *ntree = nullptr;
    std::vector<SharedInput> inputs;
  };

  struct Parameter {
    blender::IDProperty *property = nullptr;
    std::array<float, 4> values = {0.0f, 0.0f, 0.0f, 1.0f};
  };

  struct ObjectPlan {
    KX_GameObject *game_object = nullptr;
    blender::Object *object = nullptr;
    std::vector<blender::Material *> slots;
    std::vector<Parameter> parameters;
    std::vector<blender::IDProperty *> created_properties;
    bool properties_were_null = false;
  };

  blender::Main *bmain = nullptr;
  std::vector<SharedMaterialPlan> shared_materials;
  std::vector<ObjectPlan> objects;
  bool committed = false;

  void rollback()
  {
    if (committed) {
      return;
    }
    for (ObjectPlan &plan : objects) {
      blender::IDProperty *properties = plan.object ? plan.object->id.properties : nullptr;
      if (properties == nullptr) {
        continue;
      }
      for (blender::IDProperty *property : plan.created_properties) {
        if (property != nullptr &&
            blender::IDP_GetPropertyFromGroup_null(properties, property->name) == property)
        {
          blender::IDP_FreeFromGroup(properties, property);
        }
      }
      if (plan.properties_were_null && properties->len == 0) {
        plan.object->id.properties = nullptr;
        blender::IDP_FreeProperty(properties);
      }
      plan.created_properties.clear();
    }
    shared_materials.clear();
    objects.clear();
  }

  ~Impl()
  {
    rollback();
  }
};

PreparedMaterialRestore::PreparedMaterialRestore() : m_impl(std::make_unique<Impl>()) {}
PreparedMaterialRestore::~PreparedMaterialRestore() = default;
PreparedMaterialRestore::PreparedMaterialRestore(PreparedMaterialRestore &&) noexcept = default;
PreparedMaterialRestore &PreparedMaterialRestore::operator=(PreparedMaterialRestore &&) noexcept =
    default;

void PreparedMaterialRestore::ReserveCommitUpdates(KX_Scene &scene) const
{
  BLI_assert(m_impl != nullptr && !m_impl->committed);
  if (m_impl->shared_materials.size() > std::numeric_limits<size_t>::max() / 2u) {
    throw std::length_error("shared material update reservation exceeds the scene limit");
  }
  size_t all_render_pass_updates = m_impl->shared_materials.size() * 2u;
  size_t overlay_updates = 0;
  for (const Impl::ObjectPlan &plan : m_impl->objects) {
    BLI_assert(plan.object != nullptr);
    const size_t maximum_updates = plan.slots.size() * 2u + 1u;
    size_t &updates = (plan.object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0 ?
                          overlay_updates :
                          all_render_pass_updates;
    if (maximum_updates > std::numeric_limits<size_t>::max() - updates) {
      throw std::length_error("material update reservation exceeds the scene limit");
    }
    updates += maximum_updates;
  }
  scene.ReserveIdsToUpdate(all_render_pass_updates, overlay_updates);
}

void PreparedMaterialRestore::Rollback()
{
  if (m_impl != nullptr) {
    m_impl->rollback();
  }
  m_impl = std::make_unique<Impl>();
}

void PreparedMaterialRestore::Commit(KX_Scene &scene)
{
  BLI_assert(m_impl != nullptr && !m_impl->committed && m_impl->bmain != nullptr);
  for (Impl::SharedMaterialPlan &plan : m_impl->shared_materials) {
    BLI_assert(plan.material != nullptr && plan.ntree != nullptr);
    bool changed = false;
    for (Impl::SharedInput &input : plan.inputs) {
      BLI_assert(input.socket != nullptr);
      if (!apply_material_socket_value(*input.socket, input.state, input.datablock)) {
        continue;
      }
      BKE_ntree_update_tag_socket_property(plan.ntree, input.socket);
      changed = true;
    }
    if (!changed) {
      continue;
    }
    BKE_main_ensure_invariants(*m_impl->bmain, plan.ntree->id);
    GPU_material_free(&plan.material->gpumaterial);
    DEG_id_tag_update(&plan.material->id, blender::ID_RECALC_SHADING);
    scene.AppendToIdsToUpdate(&plan.material->id, blender::ID_RECALC_SHADING, false);
    scene.AppendToIdsToUpdate(
        &plan.ntree->id, static_cast<blender::IDRecalcFlag>(0), false);
  }
  for (Impl::ObjectPlan &plan : m_impl->objects) {
    bool slots_changed = false;
    bool parameters_changed = false;
    const bool overlay_only =
        (plan.object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
    for (size_t slot_index = 0; slot_index < plan.slots.size(); ++slot_index) {
      blender::Material *desired = plan.slots[slot_index];
      blender::Material *current = BKE_object_material_get(
          plan.object, short(slot_index + 1));
      if (current == desired) {
        continue;
      }
      BKE_object_material_assign(m_impl->bmain,
                                 plan.object,
                                 desired,
                                 short(slot_index + 1),
                                 blender::BKE_MAT_ASSIGN_OBJECT);
      BLI_assert(BKE_object_material_get(plan.object, short(slot_index + 1)) == desired);
      BLI_assert(rendered_material_at(*plan.object, slot_index) == desired);
      slots_changed = true;
      if (current != nullptr) {
        scene.AppendToIdsToUpdate(
            &current->id, blender::ID_RECALC_SHADING, overlay_only);
      }
      if (desired != nullptr) {
        scene.AppendToIdsToUpdate(
            &desired->id, blender::ID_RECALC_SHADING, overlay_only);
      }
    }
    for (const Impl::Parameter &parameter : plan.parameters) {
      BLI_assert(material_parameter_property_is_valid(parameter.property));
      float *data = static_cast<float *>(parameter.property->data.pointer);
      if (!std::equal(parameter.values.begin(), parameter.values.end(), data)) {
        std::copy(parameter.values.begin(), parameter.values.end(), data);
        parameters_changed = true;
      }
    }
    if (slots_changed || parameters_changed) {
      const blender::IDRecalcFlag flags = static_cast<blender::IDRecalcFlag>(
          blender::ID_RECALC_SHADING |
          (slots_changed ? blender::ID_RECALC_GEOMETRY : blender::ID_RECALC_TRANSFORM));
      scene.AppendToIdsToUpdate(&plan.object->id, flags, overlay_only);
    }
    plan.created_properties.clear();
  }
  m_impl->committed = true;
}

LN_GameStateResult prepare_material_restore(
    blender::Main &bmain,
    const MaterialAssetCatalog &catalog,
    const Checkpoint &checkpoint,
    const std::vector<MaterialRestoreTarget> &targets,
    PreparedMaterialRestore &r_prepared,
    std::string &r_error)
{
  r_error.clear();
  r_prepared.Rollback();
  r_prepared.m_impl->bmain = &bmain;
  if (catalog.m_impl == nullptr) {
    r_error = "durable material catalog is unavailable";
    return LN_GameStateResult::InternalError;
  }

  std::unordered_map<blender::Material *, BindingScan> binding_scans;
  std::unordered_map<uint32_t, blender::Material *> live_materials;
  std::unordered_map<std::string, blender::Material *> desired_shared_materials;
  std::unordered_set<KX_GameObject *> seen_objects;
  try {
    live_materials.reserve(catalog.m_impl->by_session_uid.size());
    for (blender::Material &material : bmain.materials) {
      if (material.id.session_uid == 0 ||
          !live_materials.emplace(material.id.session_uid, &material).second)
      {
        r_error = "runtime Main contains an invalid or duplicate material session identity";
        r_prepared.Rollback();
        return LN_GameStateResult::InternalError;
      }
    }
    r_prepared.m_impl->objects.reserve(targets.size());
    desired_shared_materials.reserve(checkpoint.shared_materials.size());
    seen_objects.reserve(targets.size());
    for (const MaterialRestoreTarget &target : targets) {
      if (target.object == nullptr || target.saved == nullptr ||
          !target.saved->has_material_state || !seen_objects.insert(target.object).second)
      {
        r_error = "Load Game material plan contains an invalid or duplicate object";
        r_prepared.Rollback();
        return LN_GameStateResult::InternalError;
      }
      blender::Object *object = target.object->GetBlenderObject();
      const ObjectState &saved = *target.saved;
      if (object == nullptr ||
          !object_material_topology_matches(*object, saved.material_slots.size()) ||
          saved.material_slots.size() > kMaxMaterialSlotsPerObject)
      {
        r_error = "material slot topology no longer matches object '" + target.object->GetName() +
                  "'";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }

      r_prepared.m_impl->objects.emplace_back();
      PreparedMaterialRestore::Impl::ObjectPlan &plan =
          r_prepared.m_impl->objects.back();
      plan.game_object = target.object;
      plan.object = object;
      plan.slots.reserve(saved.material_slots.size());
      std::unordered_map<std::string, RuntimeBinding> bindings;
      for (size_t slot_index = 0; slot_index < saved.material_slots.size(); ++slot_index) {
        const MaterialSlotState &slot = saved.material_slots[slot_index];
        blender::Material *material = nullptr;
        if (slot.has_material) {
          const auto catalog_asset = catalog.m_impl->session_uid_by_identity.find(
              material_identity_key(slot));
          const auto live_asset =
              catalog_asset == catalog.m_impl->session_uid_by_identity.end() ?
                  live_materials.end() :
                  live_materials.find(catalog_asset->second);
          if (catalog_asset == catalog.m_impl->session_uid_by_identity.end() ||
              live_asset == live_materials.end() ||
              !material_identity_matches(*live_asset->second, slot))
          {
            r_error = "saved material '" + slot.material_name +
                      "' is missing or was renamed in the current game build";
            r_prepared.Rollback();
            return LN_GameStateResult::Incompatible;
          }
          material = live_asset->second;
          const auto desired_shared = desired_shared_materials.emplace(
              material_identity_key(slot), material);
          if (!desired_shared.second && desired_shared.first->second != material) {
            r_error = "saved material identity resolves to multiple runtime materials";
            r_prepared.Rollback();
            return LN_GameStateResult::InternalError;
          }
          if (!append_selected_bindings(*material, binding_scans, bindings, r_error)) {
            r_error = "object '" + target.object->GetName() + "': " + r_error;
            r_prepared.Rollback();
            return LN_GameStateResult::Incompatible;
          }
        }
        else if (data_material_at(*object, slot_index) != nullptr) {
          r_error = "saved empty material slot " + std::to_string(slot_index) + " on object '" +
                    target.object->GetName() +
                    "' cannot be restored without changing shared object data";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        plan.slots.push_back(material);
      }

      if (bindings.size() > kMaxMaterialParametersPerObject ||
          saved.material_parameters.size() > kMaxMaterialParametersPerObject)
      {
        r_error = "material parameter state exceeds the Load Game limit";
        r_prepared.Rollback();
        return LN_GameStateResult::CorruptData;
      }
      std::unordered_map<std::string, std::array<float, 4>> desired_values;
      desired_values.reserve(bindings.size());
      for (const auto &item : bindings) {
        desired_values.emplace(item.first, item.second.default_values);
      }
      for (const MaterialParameterState &parameter : saved.material_parameters) {
        const auto desired = desired_values.find(parameter.attribute_name);
        if (desired == desired_values.end()) {
          r_error = "saved material parameter '" + parameter.attribute_name +
                    "' no longer exists on object '" + target.object->GetName() + "'";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        desired->second = parameter.values;
      }

      plan.parameters.reserve(desired_values.size());
      plan.created_properties.reserve(desired_values.size());
      std::vector<std::pair<std::string, std::array<float, 4>>> sorted_values;
      sorted_values.reserve(desired_values.size());
      for (const auto &item : desired_values) {
        sorted_values.push_back(item);
      }
      std::sort(sorted_values.begin(),
                sorted_values.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
      for (const auto &item : sorted_values) {
        blender::IDProperty *property = material_parameter_property(*object, item.first);
        if (property != nullptr && !material_parameter_property_is_valid(property)) {
          r_error = "runtime property for material parameter '" + item.first +
                    "' has an incompatible type";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        if (property == nullptr) {
          plan.properties_were_null |= object->id.properties == nullptr;
          property = create_material_parameter_property(
              *object, item.first, bindings.find(item.first)->second.default_values);
          if (property == nullptr) {
            r_error = "could not prepare material parameter '" + item.first + "' for object '" +
                      target.object->GetName() + "'";
            r_prepared.Rollback();
            return LN_GameStateResult::InternalError;
          }
          plan.created_properties.push_back(property);
        }
        plan.parameters.push_back({property, item.second});
      }
    }

    {
      std::unordered_map<std::string, const SharedMaterialState *> saved_shared_materials;
      saved_shared_materials.reserve(checkpoint.shared_materials.size());
      for (const SharedMaterialState &state : checkpoint.shared_materials) {
        if (!state.material.has_material ||
            !saved_shared_materials.emplace(material_identity_key(state.material), &state).second)
        {
          r_error = "checkpoint contains an invalid or duplicate shared material";
          r_prepared.Rollback();
          return LN_GameStateResult::CorruptData;
        }
      }

      std::vector<std::pair<std::string, blender::Material *>> sorted_materials;
      sorted_materials.reserve(desired_shared_materials.size());
      for (const auto &item : desired_shared_materials) {
        sorted_materials.push_back(item);
      }
      std::sort(sorted_materials.begin(),
                sorted_materials.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
      r_prepared.m_impl->shared_materials.reserve(sorted_materials.size());

      for (const auto &[identity_key, material] : sorted_materials) {
        const auto saved_it = saved_shared_materials.find(identity_key);
        if (saved_it == saved_shared_materials.end()) {
          r_error = "checkpoint is missing shared values for material '" +
                    std::string(material->id.name + 2) + "'";
          r_prepared.Rollback();
          return LN_GameStateResult::CorruptData;
        }
        const SharedMaterialState &saved_material = *saved_it->second;
        if (!material_identity_matches(*material, saved_material.material)) {
          r_error = "shared material identity no longer matches the current game build";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        if (saved_material.nodes.empty()) {
          continue;
        }
        blender::bNodeTree *ntree = material->nodetree;
        if (ntree == nullptr) {
          r_error = "saved material '" + saved_material.material.material_name +
                    "' no longer has a shader node tree";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        ntree->ensure_topology_cache();

        r_prepared.m_impl->shared_materials.emplace_back();
        PreparedMaterialRestore::Impl::SharedMaterialPlan &material_plan =
            r_prepared.m_impl->shared_materials.back();
        material_plan.material = material;
        material_plan.ntree = ntree;
        size_t input_count = 0;
        for (const SharedMaterialNodeState &node : saved_material.nodes) {
          if (node.inputs.size() > std::numeric_limits<size_t>::max() - input_count) {
            r_error = "shared material input count exceeds the Load Game limit";
            r_prepared.Rollback();
            return LN_GameStateResult::CorruptData;
          }
          input_count += node.inputs.size();
        }
        material_plan.inputs.reserve(input_count);

        for (const SharedMaterialNodeState &saved_node : saved_material.nodes) {
          blender::bNode *node = ntree->node_by_id(saved_node.node_identifier);
          if (node == nullptr || saved_node.node_type != node->idname) {
            r_error = "shader graph changed for saved material '" +
                      saved_material.material.material_name + "'";
            r_prepared.Rollback();
            return LN_GameStateResult::Incompatible;
          }
          for (const MaterialSocketValueState &saved_input : saved_node.inputs) {
            blender::bNodeSocket *socket = find_material_input(
                *node, saved_input.socket_identifier);
            if (socket == nullptr || socket->default_value == nullptr ||
                (socket->flag & blender::SOCK_UNAVAIL) != 0 ||
                socket->is_directly_linked() ||
                !material_socket_matches_state(*socket, saved_input))
            {
              r_error = "shader input '" + saved_input.socket_identifier +
                        "' changed for saved material '" +
                        saved_material.material.material_name + "'";
              r_prepared.Rollback();
              return LN_GameStateResult::Incompatible;
            }

            blender::ID *datablock = nullptr;
            const int16_t datablock_type =
                datablock_type_for_material_socket(saved_input.type);
            if (saved_input.datablock_value.has_value) {
              if (datablock_type == int16_t(blender::ID_MA)) {
                MaterialSlotState identity;
                identity.has_material = true;
                identity.material_name = saved_input.datablock_value.name;
                identity.library_path = saved_input.datablock_value.library_path;
                const auto catalog_asset = catalog.m_impl->session_uid_by_identity.find(
                    material_identity_key(identity));
                const auto live_asset =
                    catalog_asset == catalog.m_impl->session_uid_by_identity.end() ?
                        live_materials.end() :
                        live_materials.find(catalog_asset->second);
                if (catalog_asset != catalog.m_impl->session_uid_by_identity.end() &&
                    live_asset != live_materials.end())
                {
                  datablock = &live_asset->second->id;
                }
              }
              else if (datablock_type != 0) {
                blender::Library *library = nullptr;
                if (!saved_input.datablock_value.library_path.empty()) {
                  library = find_library_by_path(
                      bmain, saved_input.datablock_value.library_path);
                  if (library == nullptr) {
                    r_error = "linked datablock library is missing for material input '" +
                              saved_input.socket_identifier + "'";
                    r_prepared.Rollback();
                    return LN_GameStateResult::Incompatible;
                  }
                }
                datablock = BKE_libblock_find_name(&bmain,
                                                   datablock_type,
                                                   saved_input.datablock_value.name.c_str(),
                                                   library);
              }
              if (datablock == nullptr ||
                  !datablock_identity_matches(
                      *datablock, datablock_type, saved_input.datablock_value))
              {
                r_error = "datablock for material input '" +
                          saved_input.socket_identifier +
                          "' is missing or changed in the current game build";
                r_prepared.Rollback();
                return LN_GameStateResult::Incompatible;
              }
            }
            material_plan.inputs.push_back({socket, saved_input, datablock});
          }
        }
      }
    }
  }
  catch (...) {
    r_error = "could not prepare Load Game material state";
    r_prepared.Rollback();
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

}  // namespace upbge::logic_nodes::game_state
