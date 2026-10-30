/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateGeometryNodes.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameStateGeometryNodes.h"
#include "LN_GameStateNodeTrees.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "DNA_ID.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "modifiers/MOD_nodes.hh"
#include "NOD_geometry_nodes_srna.hh"
#include "NOD_socket.hh"
#include "RNA_access.hh"

#include "KX_GameObject.h"
#include "KX_Scene.h"

namespace blender {
extern StructRNA *RNA_NodesModifier;
}

namespace upbge::logic_nodes::game_state {

namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hash_bytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
}

template<typename T> void hash_value(uint64_t &hash, const T &value)
{
  hash_bytes(hash, &value, sizeof(value));
}

void hash_string(uint64_t &hash, const char *value)
{
  const char *safe = value ? value : "";
  hash_bytes(hash, safe, std::strlen(safe));
  const unsigned char terminator = 0;
  hash_bytes(hash, &terminator, 1);
}

void hash_id_identity(uint64_t &hash, const blender::ID *id)
{
  const bool has_id = id != nullptr;
  hash_value(hash, has_id);
  if (!has_id) {
    return;
  }
  hash_value(hash, *reinterpret_cast<const int16_t *>(id->name));
  hash_string(hash, id->name + 2);
  hash_string(hash, id->lib ? id->lib->filepath : "");
}

void hash_socket_default(uint64_t &hash,
                         const blender::eNodeSocketDatatype type,
                         const void *default_value)
{
  const bool has_default = default_value != nullptr;
  hash_value(hash, has_default);
  if (!has_default) {
    return;
  }

  switch (type) {
    case blender::SOCK_BOOLEAN: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueBoolean *>(default_value);
      hash_value(hash, value.value);
      break;
    }
    case blender::SOCK_INT: {
      const auto &value = *static_cast<const blender::bNodeSocketValueInt *>(default_value);
      hash_value(hash, value.subtype);
      hash_value(hash, value.value);
      hash_value(hash, value.min);
      hash_value(hash, value.max);
      break;
    }
    case blender::SOCK_MENU: {
      const auto &value = *static_cast<const blender::bNodeSocketValueMenu *>(default_value);
      hash_value(hash, value.value);
      break;
    }
    case blender::SOCK_INT_VECTOR: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueIntVector *>(default_value);
      hash_value(hash, value.subtype);
      hash_bytes(hash, value.value, sizeof(value.value));
      hash_value(hash, value.min);
      hash_value(hash, value.max);
      hash_value(hash, value.dimensions);
      break;
    }
    case blender::SOCK_FLOAT: {
      const auto &value = *static_cast<const blender::bNodeSocketValueFloat *>(default_value);
      hash_value(hash, value.subtype);
      hash_value(hash, value.value);
      hash_value(hash, value.min);
      hash_value(hash, value.max);
      break;
    }
    case blender::SOCK_VECTOR: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueVector *>(default_value);
      hash_value(hash, value.subtype);
      hash_bytes(hash, value.value, sizeof(value.value));
      hash_value(hash, value.min);
      hash_value(hash, value.max);
      hash_value(hash, value.dimensions);
      break;
    }
    case blender::SOCK_ROTATION: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueRotation *>(default_value);
      hash_bytes(hash, value.value_euler, sizeof(value.value_euler));
      break;
    }
    case blender::SOCK_RGBA: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueRGBA *>(default_value);
      hash_bytes(hash, value.value, sizeof(value.value));
      break;
    }
    case blender::SOCK_STRING: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueString *>(default_value);
      hash_value(hash, value.subtype);
      hash_string(hash, value.value);
      break;
    }
    case blender::SOCK_OBJECT:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueObject *>(default_value)->value));
      break;
    case blender::SOCK_IMAGE:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueImage *>(default_value)->value));
      break;
    case blender::SOCK_COLLECTION:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueCollection *>(default_value)
                  ->value));
      break;
    case blender::SOCK_TEXTURE:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueTexture *>(default_value)->value));
      break;
    case blender::SOCK_MATERIAL:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueMaterial *>(default_value)
                  ->value));
      break;
    case blender::SOCK_FONT:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueFont *>(default_value)->value));
      break;
    case blender::SOCK_SCENE:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueScene *>(default_value)->value));
      break;
    case blender::SOCK_TEXT_ID:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueText *>(default_value)->value));
      break;
    case blender::SOCK_MASK:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueMask *>(default_value)->value));
      break;
    case blender::SOCK_SOUND:
      hash_id_identity(
          hash,
          reinterpret_cast<const blender::ID *>(
              reinterpret_cast<const blender::bNodeSocketValueSound *>(default_value)->value));
      break;
    case blender::SOCK_CUSTOM:
    case blender::SOCK_SHADER:
    case blender::SOCK_GEOMETRY:
    case blender::SOCK_MATRIX:
    case blender::SOCK_BUNDLE:
    case blender::SOCK_CLOSURE:
      break;
  }
}

void hash_node_socket(uint64_t &hash,
                      const blender::bNodeSocket &socket,
                      const bool include_default_value)
{
  hash_string(hash, socket.identifier);
  hash_string(hash, socket.name);
  hash_string(hash, socket.idname);
  hash_value(hash, socket.type);
  hash_value(hash, socket.in_out);
  hash_value(hash, socket.limit);
  constexpr int semantic_flags = blender::SOCK_UNAVAIL | blender::SOCK_HIDE_VALUE |
                                 blender::SOCK_MULTI_INPUT |
                                 blender::SOCK_HIDE_IN_MODIFIER;
  hash_value(hash, int(socket.flag) & semantic_flags);
  hash_value(hash, socket.attribute_domain);
  hash_string(hash, socket.default_attribute_name);
  if (include_default_value) {
    hash_socket_default(hash, socket.type, socket.default_value);
  }
}

bool hash_interface_socket(uint64_t &hash,
                           const blender::bNodeTreeInterfaceSocket &socket)
{
  const blender::bke::bNodeSocketType *socket_type = socket.socket_typeinfo();
  if (socket_type == nullptr) {
    return false;
  }
  hash_string(hash, socket.identifier);
  hash_string(hash, socket.socket_type);
  constexpr int semantic_flags =
      blender::NODE_INTERFACE_SOCKET_INPUT | blender::NODE_INTERFACE_SOCKET_OUTPUT |
      blender::NODE_INTERFACE_SOCKET_HIDE_VALUE |
      blender::NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER |
      blender::NODE_INTERFACE_SOCKET_SINGLE_VALUE_ONLY_LEGACY |
      blender::NODE_INTERFACE_SOCKET_LAYER_SELECTION |
      blender::NODE_INTERFACE_SOCKET_PANEL_TOGGLE;
  hash_value(hash, int(socket.flag) & semantic_flags);
  hash_value(hash, socket.attribute_domain);
  hash_value(hash, socket.default_input);
  hash_value(hash, socket.structure_type);
  hash_string(hash, socket.default_attribute_name);
  hash_socket_default(
      hash, blender::eNodeSocketDatatype(socket_type->type), socket.socket_data);
  return true;
}

std::string node_group_identity_key(const std::string &name, const std::string &library_path)
{
  std::string key;
  key.reserve(name.size() + library_path.size() + 1u);
  key.append(name);
  key.push_back('\0');
  key.append(library_path);
  return key;
}

bool node_group_identity(const blender::bNodeTree &tree,
                         std::string &r_name,
                         std::string &r_library_path)
{
  r_name = tree.id.name + 2;
  r_library_path = tree.id.lib ? tree.id.lib->filepath : std::string();
  return ELEM(tree.type, blender::NTREE_GEOMETRY, blender::NTREE_COMPOSIT) &&
         tree.id.session_uid != 0 &&
         (tree.id.tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                         blender::ID_TAG_NO_MAIN | blender::ID_TAG_TEMP_MAIN |
                         blender::ID_TAG_COPIED_ON_EVAL |
                         blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) == 0 &&
         is_valid_checkpoint_text(r_name, kMaxNameBytes, false) &&
         is_valid_checkpoint_text(r_library_path, kMaxIdentityBytes);
}

bool fingerprint_node_group_recursive(const blender::bNodeTree &tree,
                                      std::unordered_set<const blender::bNodeTree *> &active,
                                      uint64_t &r_hash,
                                      bool &r_has_simulation)
{
  if (!active.insert(&tree).second) {
    return false;
  }
  hash_value(r_hash, tree.type);
  hash_string(r_hash, tree.idname);

  tree.ensure_interface_cache();
  for (const blender::bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    if (socket == nullptr || !hash_interface_socket(r_hash, *socket)) {
      active.erase(&tree);
      return false;
    }
  }
  for (const blender::bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    if (socket == nullptr || !hash_interface_socket(r_hash, *socket)) {
      active.erase(&tree);
      return false;
    }
  }
  for (const blender::bNode &node : tree.nodes) {
    hash_value(r_hash, node.identifier);
    hash_string(r_hash, node.idname);
    hash_value(r_hash, node.custom1);
    hash_value(r_hash, node.custom2);
    /* Mute and unlinked input defaults are explicit runtime checkpoint state. */
    hash_value(r_hash, node.flag & blender::NODE_DO_OUTPUT);
    for (const blender::bNodeSocket &socket : node.inputs) {
      hash_node_socket(r_hash, socket, false);
    }
    for (const blender::bNodeSocket &socket : node.outputs) {
      hash_node_socket(r_hash, socket, true);
    }
    if (STREQ(node.idname, "GeometryNodeSimulationInput") ||
        STREQ(node.idname, "GeometryNodeSimulationOutput") ||
        STREQ(node.idname, "GeometryNodeBake"))
    {
      r_has_simulation = true;
    }
    if (node.id != nullptr &&
        *reinterpret_cast<const int16_t *>(node.id->name) == blender::ID_NT)
    {
      const auto &nested = *reinterpret_cast<const blender::bNodeTree *>(node.id);
      std::string nested_name;
      std::string nested_library;
      if (!node_group_identity(nested, nested_name, nested_library)) {
        active.erase(&tree);
        return false;
      }
      hash_string(r_hash, nested_name.c_str());
      hash_string(r_hash, nested_library.c_str());
      if (!fingerprint_node_group_recursive(
              nested, active, r_hash, r_has_simulation))
      {
        active.erase(&tree);
        return false;
      }
    }
    else {
      hash_id_identity(r_hash, node.id);
    }
  }
  struct LinkIdentity {
    int32_t from_node = 0;
    std::string from_socket;
    int32_t to_node = 0;
    std::string to_socket;
    int32_t multi_input_sort_id = 0;
    int32_t semantic_flags = 0;

    bool operator<(const LinkIdentity &other) const
    {
      return std::tie(from_node,
                      from_socket,
                      to_node,
                      to_socket,
                      multi_input_sort_id,
                      semantic_flags) <
             std::tie(other.from_node,
                      other.from_socket,
                      other.to_node,
                      other.to_socket,
                      other.multi_input_sort_id,
                      other.semantic_flags);
    }
  };
  std::vector<LinkIdentity> links;
  links.reserve(blender::BLI_listbase_count(&tree.links));
  for (const blender::bNodeLink &link : tree.links) {
    if (link.fromnode == nullptr || link.fromsock == nullptr || link.tonode == nullptr ||
        link.tosock == nullptr)
    {
      active.erase(&tree);
      return false;
    }
    links.push_back({link.fromnode->identifier,
                     link.fromsock->identifier,
                     link.tonode->identifier,
                     link.tosock->identifier,
                     link.multi_input_sort_id,
                     int32_t(link.flag) &
                         int32_t(blender::NODE_LINK_VALID | blender::NODE_LINK_MUTED)});
  }
  std::sort(links.begin(), links.end());
  for (const LinkIdentity &link : links) {
    hash_value(r_hash, link.from_node);
    hash_string(r_hash, link.from_socket.c_str());
    hash_value(r_hash, link.to_node);
    hash_string(r_hash, link.to_socket.c_str());
    hash_value(r_hash, link.multi_input_sort_id);
    hash_value(r_hash, link.semantic_flags);
  }
  active.erase(&tree);
  return true;
}

bool node_group_fingerprint(const blender::bNodeTree &tree,
                            std::string &r_fingerprint,
                            bool &r_has_simulation)
{
  uint64_t hash = kFnvOffsetBasis;
  std::unordered_set<const blender::bNodeTree *> active;
  r_has_simulation = false;
  if (!fingerprint_node_group_recursive(tree, active, hash, r_has_simulation)) {
    return false;
  }
  std::ostringstream stream;
  stream << "nt-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  r_fingerprint = stream.str();
  return true;
}

int16_t datablock_type_for_value_type(const MaterialSocketValueType type)
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

bool value_type_for_datablock_type(const int16_t id_type, MaterialSocketValueType &r_type)
{
  switch (id_type) {
    case blender::ID_OB:
      r_type = MaterialSocketValueType::Object;
      return true;
    case blender::ID_IM:
      r_type = MaterialSocketValueType::Image;
      return true;
    case blender::ID_GR:
      r_type = MaterialSocketValueType::Collection;
      return true;
    case blender::ID_TE:
      r_type = MaterialSocketValueType::Texture;
      return true;
    case blender::ID_MA:
      r_type = MaterialSocketValueType::Material;
      return true;
    case blender::ID_VF:
      r_type = MaterialSocketValueType::Font;
      return true;
    case blender::ID_SCE:
      r_type = MaterialSocketValueType::Scene;
      return true;
    case blender::ID_TXT:
      r_type = MaterialSocketValueType::Text;
      return true;
    case blender::ID_MSK:
      r_type = MaterialSocketValueType::Mask;
      return true;
    case blender::ID_SO:
      r_type = MaterialSocketValueType::Sound;
      return true;
    default:
      return false;
  }
}

bool value_type_for_interface_socket(const blender::bNodeTreeInterfaceSocket &socket,
                                     MaterialSocketValueType &r_type)
{
  const blender::bke::bNodeSocketType *socket_type = socket.socket_typeinfo();
  if (socket_type == nullptr) {
    return false;
  }
  switch (socket_type->type) {
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
    default:
      return false;
  }
}

bool capture_datablock_reference(blender::ID *id,
                                 MaterialSocketValueState &r_state,
                                 std::string &r_error)
{
  if (id == nullptr) {
    return true;
  }
  MaterialSocketValueType actual_type;
  if (!value_type_for_datablock_type(
          *reinterpret_cast<const int16_t *>(id->name), actual_type) ||
      actual_type != r_state.type || id->session_uid == 0 ||
      (id->tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                  blender::ID_TAG_NO_MAIN | blender::ID_TAG_TEMP_MAIN |
                  blender::ID_TAG_COPIED_ON_EVAL |
                  blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) != 0)
  {
    r_error = "Geometry Nodes input references a runtime-only or invalid datablock";
    return false;
  }
  r_state.datablock_value.has_value = true;
  r_state.datablock_value.name = id->name + 2;
  r_state.datablock_value.library_path = id->lib ? id->lib->filepath : std::string();
  if (!is_valid_checkpoint_text(
          r_state.datablock_value.name, kMaxNameBytes, false) ||
      !is_valid_checkpoint_text(
          r_state.datablock_value.library_path, kMaxIdentityBytes))
  {
    r_error = "Geometry Nodes input has an invalid datablock identity";
    return false;
  }
  return true;
}

bool capture_rna_value(blender::PointerRNA &input_ptr,
                       blender::PropertyRNA &property,
                       const blender::bNodeTreeInterfaceSocket &interface_socket,
                       MaterialSocketValueState &r_state,
                       std::string &r_error)
{
  r_state.socket_identifier = interface_socket.identifier;
  const int array_length = blender::RNA_property_array_check(&property) ?
                               blender::RNA_property_array_length(&input_ptr, &property) :
                               0;
  switch (blender::RNA_property_type(&property)) {
    case blender::PROP_BOOLEAN:
      if (array_length != 0) {
        break;
      }
      r_state.type = MaterialSocketValueType::Bool;
      r_state.bool_value = blender::RNA_property_boolean_get(&input_ptr, &property);
      return true;
    case blender::PROP_INT:
      if (array_length == 0) {
        r_state.type = MaterialSocketValueType::Int;
        r_state.int_value = blender::RNA_property_int_get(&input_ptr, &property);
        return true;
      }
      if (array_length >= 2 && array_length <= 3) {
        r_state.type = MaterialSocketValueType::IntVector;
        r_state.dimensions = uint8_t(array_length);
        blender::RNA_property_int_get_array_at_most(
            &input_ptr, &property, r_state.int_values.data(), array_length);
        return true;
      }
      break;
    case blender::PROP_FLOAT:
      if (array_length == 0) {
        const float value = blender::RNA_property_float_get(&input_ptr, &property);
        if (!std::isfinite(value)) {
          break;
        }
        r_state.type = MaterialSocketValueType::Float;
        r_state.float_values[0] = value;
        return true;
      }
      if (array_length < 2 || array_length > 4) {
        break;
      }
      blender::RNA_property_float_get_array_at_most(
          &input_ptr, &property, r_state.float_values.data(), array_length);
      if (!std::all_of(r_state.float_values.begin(),
                       r_state.float_values.begin() + array_length,
                       [](const float value) { return std::isfinite(value); }))
      {
        break;
      }
      if (const blender::bke::bNodeSocketType *socket_type =
              interface_socket.socket_typeinfo())
      {
        if (socket_type->type == blender::SOCK_RGBA && array_length == 4) {
          r_state.type = MaterialSocketValueType::Color;
          return true;
        }
        if (socket_type->type == blender::SOCK_ROTATION && array_length == 3) {
          r_state.type = MaterialSocketValueType::Rotation;
          return true;
        }
      }
      r_state.type = MaterialSocketValueType::Vector;
      r_state.dimensions = uint8_t(array_length);
      return true;
    case blender::PROP_STRING:
      if (array_length != 0) {
        break;
      }
      r_state.type = MaterialSocketValueType::String;
      r_state.string_value = blender::RNA_property_string_get(&input_ptr, &property);
      if (is_valid_checkpoint_text(r_state.string_value, kMaxMaterialStringBytes)) {
        return true;
      }
      break;
    case blender::PROP_ENUM:
      if (array_length != 0) {
        break;
      }
      r_state.type = MaterialSocketValueType::Menu;
      r_state.int_value = blender::RNA_property_enum_get(&input_ptr, &property);
      return true;
    case blender::PROP_POINTER: {
      if (array_length != 0 ||
          !value_type_for_interface_socket(interface_socket, r_state.type))
      {
        break;
      }
      const blender::PointerRNA pointer =
          blender::RNA_property_pointer_get(&input_ptr, &property);
      return capture_datablock_reference(
          static_cast<blender::ID *>(pointer.data), r_state, r_error);
    }
    default:
      break;
  }
  r_error = "Geometry Nodes input '" + std::string(interface_socket.identifier) +
            "' has an unsupported or invalid Value type";
  return false;
}

bool modifier_input_pointer(blender::Object &object,
                            blender::NodesModifierData &modifier,
                            const char *identifier,
                            blender::PointerRNA &r_input)
{
  blender::PointerRNA modifier_ptr = blender::RNA_pointer_create_discrete(
      &object.id, blender::RNA_NodesModifier, &modifier);
  blender::PointerRNA properties = blender::RNA_pointer_get(&modifier_ptr, "properties");
  blender::PointerRNA inputs = blender::RNA_pointer_get(&properties, "inputs");
  r_input = blender::RNA_pointer_get(&inputs, identifier);
  return r_input.data != nullptr;
}

bool modifier_output_pointer(blender::Object &object,
                             blender::NodesModifierData &modifier,
                             const char *identifier,
                             blender::PointerRNA &r_output)
{
  blender::PointerRNA modifier_ptr = blender::RNA_pointer_create_discrete(
      &object.id, blender::RNA_NodesModifier, &modifier);
  blender::PointerRNA properties = blender::RNA_pointer_get(&modifier_ptr, "properties");
  blender::PointerRNA outputs = blender::RNA_pointer_get(&properties, "outputs");
  r_output = blender::RNA_pointer_get(&outputs, identifier);
  return r_output.data != nullptr;
}

bool capture_geometry_nodes_modifier(
    blender::Object &object,
    blender::NodesModifierData &modifier,
    const uint32_t stack_index,
    const GeometryNodesAssetCatalog &catalog,
    const bool reject_simulation,
    GeometryNodesModifierState &r_state,
    std::string &r_error);

bool non_geometry_modifier_state_equal(const NonGeometryModifierState &a,
                                       const NonGeometryModifierState &b)
{
  return a.name == b.name && a.type == b.type &&
         a.persistent_uid == b.persistent_uid && a.mode == b.mode;
}

LN_GameStateResult capture_current_modifier_state(
    blender::Object &object,
    const GeometryNodesAssetCatalog &catalog,
    const bool reject_simulation,
    std::vector<NonGeometryModifierState> &r_non_geometry,
    std::vector<GeometryNodesModifierState> &r_geometry_nodes,
    std::string &r_error)
{
  r_non_geometry.clear();
  r_geometry_nodes.clear();
  if (blender::BLI_listbase_count(&object.modifiers) > int(kMaxModifiersPerObject)) {
    r_error = "modifier stack exceeds the Save Game limit";
    return LN_GameStateResult::UnsupportedContent;
  }
  std::unordered_set<std::string> names;
  std::unordered_set<int32_t> persistent_uids;
  try {
    names.reserve(size_t(blender::BLI_listbase_count(&object.modifiers)));
    persistent_uids.reserve(size_t(blender::BLI_listbase_count(&object.modifiers)));
    uint32_t stack_index = 0;
    for (blender::ModifierData &modifier : object.modifiers) {
      const std::string name = modifier.name;
      if (!is_valid_checkpoint_text(name, kMaxNameBytes, false) ||
          modifier.persistent_uid <= 0 || !names.insert(name).second ||
          !persistent_uids.insert(modifier.persistent_uid).second)
      {
        r_error = "modifier stack contains an invalid or duplicate durable identity";
        return LN_GameStateResult::UnsupportedContent;
      }
      if (modifier.type == blender::eModifierType_Nodes) {
        r_geometry_nodes.emplace_back();
        if (!capture_geometry_nodes_modifier(
                object,
                reinterpret_cast<blender::NodesModifierData &>(modifier),
                stack_index,
                catalog,
                reject_simulation,
                r_geometry_nodes.back(),
                r_error))
        {
          r_geometry_nodes.pop_back();
          return LN_GameStateResult::UnsupportedContent;
        }
      }
      else {
        NonGeometryModifierState state;
        state.name = name;
        state.type = int32_t(modifier.type);
        state.persistent_uid = modifier.persistent_uid;
        state.mode = uint32_t(modifier.mode) & kPersistentModifierModeMask;
        r_non_geometry.push_back(std::move(state));
      }
      stack_index++;
    }
  }
  catch (...) {
    r_error = "could not capture the modifier stack";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

blender::Library *find_library(blender::Main &bmain, const std::string &path)
{
  blender::Library *found = nullptr;
  for (blender::Library &library : bmain.libraries) {
    if (path != library.filepath) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &library;
  }
  return found;
}

blender::ID *resolve_datablock(blender::Main &bmain,
                               const MaterialSocketValueState &state)
{
  if (!state.datablock_value.has_value) {
    return nullptr;
  }
  const int16_t type = datablock_type_for_value_type(state.type);
  if (type == 0) {
    return nullptr;
  }
  blender::Library *library = nullptr;
  if (!state.datablock_value.library_path.empty()) {
    library = find_library(bmain, state.datablock_value.library_path);
    if (library == nullptr) {
      return nullptr;
    }
  }
  blender::ID *id = BKE_libblock_find_name(
      &bmain, type, state.datablock_value.name.c_str(), library);
  if (id == nullptr || id->session_uid == 0 ||
      (id->tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                  blender::ID_TAG_NO_MAIN | blender::ID_TAG_TEMP_MAIN |
                  blender::ID_TAG_COPIED_ON_EVAL |
                  blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) != 0)
  {
    return nullptr;
  }
  return id;
}

bool apply_rna_value(blender::Main &bmain,
                     blender::PointerRNA &input_ptr,
                     blender::PropertyRNA &property,
                     const MaterialSocketValueState &state)
{
  const int array_length = blender::RNA_property_array_check(&property) ?
                               blender::RNA_property_array_length(&input_ptr, &property) :
                               0;
  switch (state.type) {
    case MaterialSocketValueType::Bool:
      if (blender::RNA_property_type(&property) != blender::PROP_BOOLEAN ||
          array_length != 0)
      {
        return false;
      }
      blender::RNA_property_boolean_set(&input_ptr, &property, state.bool_value);
      return blender::RNA_property_boolean_get(&input_ptr, &property) == state.bool_value;
    case MaterialSocketValueType::Int:
      if (blender::RNA_property_type(&property) != blender::PROP_INT ||
          array_length != 0)
      {
        return false;
      }
      blender::RNA_property_int_set(&input_ptr, &property, state.int_value);
      return blender::RNA_property_int_get(&input_ptr, &property) == state.int_value;
    case MaterialSocketValueType::Menu:
      if (blender::RNA_property_type(&property) != blender::PROP_ENUM ||
          array_length != 0)
      {
        return false;
      }
      blender::RNA_property_enum_set(&input_ptr, &property, state.int_value);
      return blender::RNA_property_enum_get(&input_ptr, &property) == state.int_value;
    case MaterialSocketValueType::IntVector: {
      if (blender::RNA_property_type(&property) != blender::PROP_INT ||
          array_length != state.dimensions)
      {
        return false;
      }
      blender::RNA_property_int_set_array_at_most(
          &input_ptr, &property, state.int_values.data(), state.dimensions);
      std::array<int32_t, 3> current{};
      blender::RNA_property_int_get_array_at_most(
          &input_ptr, &property, current.data(), state.dimensions);
      return std::equal(
          current.begin(), current.begin() + state.dimensions, state.int_values.begin());
    }
    case MaterialSocketValueType::Float:
      if (blender::RNA_property_type(&property) != blender::PROP_FLOAT ||
          array_length != 0)
      {
        return false;
      }
      blender::RNA_property_float_set(&input_ptr, &property, state.float_values[0]);
      return blender::RNA_property_float_get(&input_ptr, &property) ==
             state.float_values[0];
    case MaterialSocketValueType::Vector:
    case MaterialSocketValueType::Rotation:
    case MaterialSocketValueType::Color: {
      const int expected_length = state.type == MaterialSocketValueType::Vector ?
                                      state.dimensions :
                                  state.type == MaterialSocketValueType::Rotation ?
                                      3 :
                                      4;
      if (blender::RNA_property_type(&property) != blender::PROP_FLOAT ||
          array_length != expected_length)
      {
        return false;
      }
      blender::RNA_property_float_set_array_at_most(
          &input_ptr, &property, state.float_values.data(), expected_length);
      std::array<float, 4> current{};
      blender::RNA_property_float_get_array_at_most(
          &input_ptr, &property, current.data(), expected_length);
      return std::equal(
          current.begin(), current.begin() + expected_length, state.float_values.begin());
    }
    case MaterialSocketValueType::String:
      if (blender::RNA_property_type(&property) != blender::PROP_STRING ||
          array_length != 0)
      {
        return false;
      }
      blender::RNA_property_string_set(
          &input_ptr, &property, state.string_value.c_str());
      return blender::RNA_property_string_get(&input_ptr, &property) ==
             state.string_value;
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
      if (blender::RNA_property_type(&property) != blender::PROP_POINTER ||
          array_length != 0)
      {
        return false;
      }
      blender::ID *id = resolve_datablock(bmain, state);
      if (state.datablock_value.has_value && id == nullptr) {
        return false;
      }
      const blender::PointerRNA desired =
          id ? blender::RNA_id_pointer_create(id) : blender::PointerRNA_NULL;
      blender::RNA_property_pointer_set(&input_ptr, &property, desired, nullptr);
      return blender::RNA_property_pointer_get(&input_ptr, &property).data == id;
    }
  }
  return false;
}

bool apply_geometry_nodes_inputs(blender::Main &bmain,
                                 blender::Object &object,
                                 blender::NodesModifierData &modifier,
                                 const GeometryNodesModifierState &saved,
                                 std::string &r_error)
{
  blender::bNodeTree *tree = modifier.node_group;
  if (tree == nullptr) {
    r_error = "prepared Geometry Nodes modifier has no node group";
    return false;
  }
  tree->ensure_interface_cache();
  size_t saved_index = 0;
  for (const blender::bNodeTreeInterfaceSocket *interface_socket :
       tree->interface_inputs())
  {
    const blender::bke::bNodeSocketType *socket_type =
        interface_socket->socket_typeinfo();
    if (socket_type == nullptr ||
        socket_type->make_geometry_nodes_input_srna == nullptr)
    {
      continue;
    }
    blender::PointerRNA input_ptr;
    if (!modifier_input_pointer(
            object, modifier, interface_socket->identifier, input_ptr))
    {
      r_error = "Geometry Nodes input is unavailable for modifier '" + saved.name + "'";
      return false;
    }
    if (saved_index >= saved.inputs.size() ||
        saved.inputs[saved_index].socket_identifier != interface_socket->identifier)
    {
      r_error = "Geometry Nodes interface changed for modifier '" + saved.name + "'";
      return false;
    }
    const GeometryNodesInputState &input_state = saved.inputs[saved_index++];
    blender::PropertyRNA *type_property =
        blender::RNA_struct_find_property(&input_ptr, "type");
    if (type_property == nullptr ||
        blender::RNA_property_type(type_property) != blender::PROP_ENUM)
    {
      r_error = "Geometry Nodes input mode is unavailable for modifier '" + saved.name + "'";
      return false;
    }
    int mode = int(blender::nodes::GeometryNodesInputType::Fallback);
    switch (input_state.mode) {
      case GeometryNodesInputMode::Fallback:
        mode = int(blender::nodes::GeometryNodesInputType::Fallback);
        break;
      case GeometryNodesInputMode::Value:
        mode = int(blender::nodes::GeometryNodesInputType::Value);
        break;
      case GeometryNodesInputMode::Attribute:
        mode = int(blender::nodes::GeometryNodesInputType::Attribute);
        break;
      case GeometryNodesInputMode::Layer:
        mode = int(blender::nodes::GeometryNodesInputType::Layer);
        break;
    }
    blender::RNA_property_enum_set(&input_ptr, type_property, mode);
    if (blender::RNA_property_enum_get(&input_ptr, type_property) != mode) {
      r_error = "saved Geometry Nodes input mode is no longer supported";
      return false;
    }
    if (input_state.mode == GeometryNodesInputMode::Value) {
      blender::PropertyRNA *value_property =
          blender::RNA_struct_find_property(&input_ptr, "value");
      if (value_property == nullptr ||
          !apply_rna_value(
              bmain, input_ptr, *value_property, input_state.value))
      {
        r_error = "saved Geometry Nodes value type changed for input '" +
                  input_state.socket_identifier + "'";
        return false;
      }
    }
    else if (input_state.mode == GeometryNodesInputMode::Attribute) {
      blender::PropertyRNA *attribute =
          blender::RNA_struct_find_property(&input_ptr, "attribute_name");
      if (attribute == nullptr ||
          blender::RNA_property_type(attribute) != blender::PROP_STRING)
      {
        r_error = "saved Geometry Nodes attribute input is no longer supported";
        return false;
      }
      blender::RNA_property_string_set(
          &input_ptr, attribute, input_state.attribute_name.c_str());
    }
    else if (input_state.mode == GeometryNodesInputMode::Layer) {
      blender::PropertyRNA *layer =
          blender::RNA_struct_find_property(&input_ptr, "layer_name");
      if (layer == nullptr ||
          blender::RNA_property_type(layer) != blender::PROP_STRING)
      {
        r_error = "saved Geometry Nodes layer input is no longer supported";
        return false;
      }
      blender::RNA_property_string_set(
          &input_ptr, layer, input_state.layer_name.c_str());
    }
  }
  if (saved_index != saved.inputs.size()) {
    r_error = "Geometry Nodes interface changed for modifier '" + saved.name + "'";
    return false;
  }
  return true;
}

bool apply_geometry_nodes_outputs(blender::Object &object,
                                  blender::NodesModifierData &modifier,
                                  const GeometryNodesModifierState &saved,
                                  std::string &r_error)
{
  blender::bNodeTree *tree = modifier.node_group;
  if (tree == nullptr) {
    r_error = "prepared Geometry Nodes modifier has no node group";
    return false;
  }
  tree->ensure_interface_cache();
  size_t saved_index = 0;
  for (const blender::bNodeTreeInterfaceSocket *interface_socket :
       tree->interface_outputs())
  {
    const blender::bke::bNodeSocketType *socket_type =
        interface_socket->socket_typeinfo();
    if (socket_type == nullptr ||
        !blender::nodes::socket_type_supports_attributes(
            blender::eNodeSocketDatatype(socket_type->type)))
    {
      continue;
    }
    blender::PointerRNA output_ptr;
    if (!modifier_output_pointer(
            object, modifier, interface_socket->identifier, output_ptr))
    {
      r_error = "Geometry Nodes output is unavailable for modifier '" + saved.name + "'";
      return false;
    }
    if (saved_index >= saved.outputs.size() ||
        saved.outputs[saved_index].socket_identifier != interface_socket->identifier)
    {
      r_error = "Geometry Nodes output interface changed for modifier '" + saved.name + "'";
      return false;
    }
    const GeometryNodesOutputState &output_state = saved.outputs[saved_index++];
    blender::PropertyRNA *attribute =
        blender::RNA_struct_find_property(&output_ptr, "attribute_name");
    if (attribute == nullptr ||
        blender::RNA_property_type(attribute) != blender::PROP_STRING)
    {
      r_error = "saved Geometry Nodes output attribute is no longer supported";
      return false;
    }
    blender::RNA_property_string_set(
        &output_ptr, attribute, output_state.attribute_name.c_str());
    if (blender::RNA_property_string_get(&output_ptr, attribute) !=
        output_state.attribute_name)
    {
      r_error = "saved Geometry Nodes output attribute could not be restored";
      return false;
    }
  }
  if (saved_index != saved.outputs.size()) {
    r_error = "Geometry Nodes output interface changed for modifier '" + saved.name + "'";
    return false;
  }
  return true;
}

}  // namespace

struct GeometryNodesAssetCatalog::Impl {
  struct Asset {
    uint32_t session_uid = 0;
    RuntimeNodeTreeType type = RuntimeNodeTreeType::Geometry;
    std::string name;
    std::string library_path;
    std::string fingerprint;
  };

  std::unordered_map<std::string, Asset> by_identity;
  std::unordered_map<uint32_t, std::string> identity_by_session_uid;
};

GeometryNodesAssetCatalog::GeometryNodesAssetCatalog()
    : m_impl(std::make_unique<Impl>())
{
}

GeometryNodesAssetCatalog::~GeometryNodesAssetCatalog() = default;
GeometryNodesAssetCatalog::GeometryNodesAssetCatalog(
    GeometryNodesAssetCatalog &&) noexcept = default;
GeometryNodesAssetCatalog &GeometryNodesAssetCatalog::operator=(
    GeometryNodesAssetCatalog &&) noexcept = default;

bool GeometryNodesAssetCatalog::DescribeNodeGroup(
    const blender::bNodeTree &tree,
    std::string &r_name,
    std::string &r_library_path,
    std::string &r_fingerprint,
    bool &r_has_simulation) const
{
  if (tree.type != blender::NTREE_GEOMETRY) {
    return false;
  }
  return DescribeNodeTree(tree,
                          RuntimeNodeTreeType::Geometry,
                          r_name,
                          r_library_path,
                          r_fingerprint) &&
         node_group_fingerprint(tree, r_fingerprint, r_has_simulation);
}

bool GeometryNodesAssetCatalog::DescribeNodeTree(
    const blender::bNodeTree &tree,
    const RuntimeNodeTreeType expected_type,
    std::string &r_name,
    std::string &r_library_path,
    std::string &r_fingerprint) const
{
  if (m_impl == nullptr) {
    return false;
  }
  const auto identity = m_impl->identity_by_session_uid.find(tree.id.session_uid);
  if (identity == m_impl->identity_by_session_uid.end()) {
    return false;
  }
  const auto asset = m_impl->by_identity.find(identity->second);
  std::string current_name;
  std::string current_library;
  std::string current_fingerprint;
  bool has_simulation = false;
  if (asset == m_impl->by_identity.end() ||
      asset->second.type != expected_type ||
      tree.type != (expected_type == RuntimeNodeTreeType::Geometry ?
                         blender::NTREE_GEOMETRY :
                         blender::NTREE_COMPOSIT) ||
      !node_group_identity(tree, current_name, current_library) ||
      !node_group_fingerprint(
          tree, current_fingerprint, has_simulation) ||
      current_name != asset->second.name ||
      current_library != asset->second.library_path ||
      current_fingerprint != asset->second.fingerprint)
  {
    return false;
  }
  r_name = asset->second.name;
  r_library_path = asset->second.library_path;
  r_fingerprint = asset->second.fingerprint;
  return true;
}

blender::bNodeTree *GeometryNodesAssetCatalog::ResolveNodeGroup(
    blender::Main &bmain, const GeometryNodesModifierState &saved) const
{
  if (m_impl == nullptr) {
    return nullptr;
  }
  const std::string identity =
      node_group_identity_key(saved.node_group_name, saved.node_group_library_path);
  const auto asset = m_impl->by_identity.find(identity);
  if (asset == m_impl->by_identity.end() ||
      asset->second.fingerprint != saved.node_group_fingerprint)
  {
    return nullptr;
  }
  for (blender::bNodeTree &tree : bmain.nodetrees) {
    if (tree.id.session_uid != asset->second.session_uid) {
      continue;
    }
    std::string name;
    std::string library;
    std::string fingerprint;
    bool has_simulation = false;
    if (!DescribeNodeGroup(
            tree, name, library, fingerprint, has_simulation) ||
        has_simulation || name != saved.node_group_name ||
        library != saved.node_group_library_path ||
        fingerprint != saved.node_group_fingerprint)
    {
      return nullptr;
    }
    return &tree;
  }
  return nullptr;
}

blender::bNodeTree *GeometryNodesAssetCatalog::ResolveNodeTree(
    blender::Main &bmain, const RuntimeNodeTreeState &saved) const
{
  if (m_impl == nullptr) {
    return nullptr;
  }
  const std::string identity = node_group_identity_key(saved.name, saved.library_path);
  const auto asset = m_impl->by_identity.find(identity);
  if (asset == m_impl->by_identity.end() || asset->second.type != saved.type ||
      asset->second.fingerprint != saved.structural_fingerprint)
  {
    return nullptr;
  }
  for (blender::bNodeTree &tree : bmain.nodetrees) {
    if (tree.id.session_uid != asset->second.session_uid) {
      continue;
    }
    std::string name;
    std::string library;
    std::string fingerprint;
    if (!DescribeNodeTree(tree, saved.type, name, library, fingerprint) ||
        name != saved.name || library != saved.library_path ||
        fingerprint != saved.structural_fingerprint)
    {
      return nullptr;
    }
    return &tree;
  }
  return nullptr;
}

LN_GameStateResult build_geometry_nodes_asset_catalog(
    blender::Main &bmain,
    GeometryNodesAssetCatalog &r_catalog,
    std::string &r_error)
{
  r_error.clear();
  GeometryNodesAssetCatalog replacement;
  try {
    replacement.m_impl->by_identity.reserve(
        blender::BLI_listbase_count(&bmain.nodetrees));
    replacement.m_impl->identity_by_session_uid.reserve(
        blender::BLI_listbase_count(&bmain.nodetrees));
    for (blender::bNodeTree &tree : bmain.nodetrees) {
      if (!ELEM(tree.type, blender::NTREE_GEOMETRY, blender::NTREE_COMPOSIT)) {
        continue;
      }
      GeometryNodesAssetCatalog::Impl::Asset asset;
      bool has_simulation = false;
      if (!node_group_identity(tree, asset.name, asset.library_path) ||
          !node_group_fingerprint(
              tree, asset.fingerprint, has_simulation))
      {
        r_error = "Geometry Nodes asset catalog contains an invalid node group";
        return LN_GameStateResult::UnsupportedContent;
      }
      asset.session_uid = tree.id.session_uid;
      asset.type = tree.type == blender::NTREE_GEOMETRY ?
                       RuntimeNodeTreeType::Geometry :
                       RuntimeNodeTreeType::Compositor;
      const std::string identity =
          node_group_identity_key(asset.name, asset.library_path);
      if (!replacement.m_impl->by_identity.emplace(identity, asset).second ||
          !replacement.m_impl->identity_by_session_uid
               .emplace(asset.session_uid, identity)
               .second)
      {
        r_error = "Geometry Nodes asset catalog contains a duplicate durable identity";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
  }
  catch (...) {
    r_error = "could not allocate the Geometry Nodes asset catalog";
    return LN_GameStateResult::InternalError;
  }
  r_catalog = std::move(replacement);
  return LN_GameStateResult::Success;
}

namespace {

bool capture_geometry_nodes_modifier(
    blender::Object &object,
    blender::NodesModifierData &modifier,
    const uint32_t stack_index,
    const GeometryNodesAssetCatalog &catalog,
    const bool reject_simulation,
    GeometryNodesModifierState &r_state,
    std::string &r_error)
{
  blender::bNodeTree *tree = modifier.node_group;
  std::string asset_name;
  std::string asset_library_path;
  std::string asset_fingerprint;
  bool has_simulation = false;
  if (tree == nullptr || tree->type != blender::NTREE_GEOMETRY ||
      !catalog.DescribeNodeGroup(*tree,
                                 asset_name,
                                 asset_library_path,
                                 asset_fingerprint,
                                 has_simulation))
  {
    r_error =
        "Geometry Nodes modifier references a runtime-created or structurally edited node group";
    return false;
  }
  if (reject_simulation && has_simulation) {
    r_error = "Geometry Nodes simulation and bake state is not checkpoint-persistent";
    return false;
  }

  r_state.name = modifier.modifier.name;
  r_state.persistent_uid = modifier.modifier.persistent_uid;
  r_state.stack_index = stack_index;
  r_state.mode = uint32_t(modifier.modifier.mode) & kPersistentModifierModeMask;
  r_state.node_group_name = std::move(asset_name);
  r_state.node_group_library_path = std::move(asset_library_path);
  r_state.node_group_fingerprint = std::move(asset_fingerprint);

  tree->ensure_interface_cache();
  if (tree->interface_inputs().size() > kMaxGeometryNodesInputsPerModifier) {
    r_error = "Geometry Nodes interface input count exceeds the Save Game limit";
    return false;
  }
  r_state.inputs.reserve(tree->interface_inputs().size());
  for (const blender::bNodeTreeInterfaceSocket *interface_socket :
       tree->interface_inputs())
  {
    const blender::bke::bNodeSocketType *socket_type =
        interface_socket->socket_typeinfo();
    if (socket_type == nullptr ||
        socket_type->make_geometry_nodes_input_srna == nullptr)
    {
      continue;
    }
    blender::PointerRNA input_ptr;
    if (!modifier_input_pointer(
            object, modifier, interface_socket->identifier, input_ptr))
    {
      r_error = "Geometry Nodes modifier input is unavailable";
      return false;
    }
    blender::PropertyRNA *type_property =
        blender::RNA_struct_find_property(&input_ptr, "type");
    if (type_property == nullptr ||
        blender::RNA_property_type(type_property) != blender::PROP_ENUM)
    {
      r_error = "Geometry Nodes input has no deterministic input mode";
      return false;
    }
    GeometryNodesInputState state;
    state.socket_identifier = interface_socket->identifier;
    const int mode = blender::RNA_property_enum_get(&input_ptr, type_property);
    if (mode == int(blender::nodes::GeometryNodesInputType::Fallback)) {
      state.mode = GeometryNodesInputMode::Fallback;
    }
    else if (mode == int(blender::nodes::GeometryNodesInputType::Value)) {
      state.mode = GeometryNodesInputMode::Value;
      blender::PropertyRNA *value_property =
          blender::RNA_struct_find_property(&input_ptr, "value");
      if (value_property == nullptr ||
          !capture_rna_value(
              input_ptr, *value_property, *interface_socket, state.value, r_error))
      {
        return false;
      }
    }
    else if (mode == int(blender::nodes::GeometryNodesInputType::Attribute)) {
      state.mode = GeometryNodesInputMode::Attribute;
      blender::PropertyRNA *attribute =
          blender::RNA_struct_find_property(&input_ptr, "attribute_name");
      if (attribute == nullptr ||
          blender::RNA_property_type(attribute) != blender::PROP_STRING)
      {
        r_error = "Geometry Nodes attribute input is invalid";
        return false;
      }
      state.attribute_name =
          blender::RNA_property_string_get(&input_ptr, attribute);
      if (!is_valid_checkpoint_text(
              state.attribute_name, kMaxStringPropertyBytes))
      {
        r_error = "Geometry Nodes attribute name is invalid or oversized";
        return false;
      }
    }
    else if (mode == int(blender::nodes::GeometryNodesInputType::Layer)) {
      state.mode = GeometryNodesInputMode::Layer;
      blender::PropertyRNA *layer =
          blender::RNA_struct_find_property(&input_ptr, "layer_name");
      if (layer == nullptr ||
          blender::RNA_property_type(layer) != blender::PROP_STRING)
      {
        r_error = "Geometry Nodes layer input is invalid";
        return false;
      }
      state.layer_name = blender::RNA_property_string_get(&input_ptr, layer);
      if (!is_valid_checkpoint_text(state.layer_name, kMaxStringPropertyBytes)) {
        r_error = "Geometry Nodes layer name is invalid or oversized";
        return false;
      }
    }
    else {
      r_error = "Geometry Nodes input uses an unsupported runtime mode";
      return false;
    }
    r_state.inputs.push_back(std::move(state));
  }
  if (tree->interface_outputs().size() > kMaxGeometryNodesOutputsPerModifier) {
    r_error = "Geometry Nodes interface output count exceeds the Save Game limit";
    return false;
  }
  r_state.outputs.reserve(tree->interface_outputs().size());
  for (const blender::bNodeTreeInterfaceSocket *interface_socket :
       tree->interface_outputs())
  {
    const blender::bke::bNodeSocketType *socket_type =
        interface_socket->socket_typeinfo();
    if (socket_type == nullptr ||
        !blender::nodes::socket_type_supports_attributes(
            blender::eNodeSocketDatatype(socket_type->type)))
    {
      continue;
    }
    blender::PointerRNA output_ptr;
    if (!modifier_output_pointer(
            object, modifier, interface_socket->identifier, output_ptr))
    {
      r_error = "Geometry Nodes modifier output is unavailable";
      return false;
    }
    blender::PropertyRNA *attribute =
        blender::RNA_struct_find_property(&output_ptr, "attribute_name");
    if (attribute == nullptr ||
        blender::RNA_property_type(attribute) != blender::PROP_STRING)
    {
      r_error = "Geometry Nodes output has no deterministic attribute state";
      return false;
    }
    GeometryNodesOutputState state;
    state.socket_identifier = interface_socket->identifier;
    state.attribute_name =
        blender::RNA_property_string_get(&output_ptr, attribute);
    if (!is_valid_checkpoint_text(state.attribute_name, kMaxStringPropertyBytes)) {
      r_error = "Geometry Nodes output attribute name is invalid or oversized";
      return false;
    }
    r_state.outputs.push_back(std::move(state));
  }
  return true;
}

}  // namespace

LN_GameStateResult capture_modifier_baseline(
    const blender::Object &object,
    const GeometryNodesAssetCatalog &catalog,
    ModifierBaseline &r_baseline,
    std::string &r_error)
{
  return capture_current_modifier_state(
      const_cast<blender::Object &>(object),
      catalog,
      false,
      r_baseline.non_geometry_modifiers,
      r_baseline.geometry_nodes_modifiers,
      r_error);
}

LN_GameStateResult capture_modifier_state(
    KX_GameObject &game_object,
    const ModifierBaseline &baseline,
    const GeometryNodesAssetCatalog &catalog,
    RuntimeNodeTreeCapture &node_tree_capture,
    ObjectState &r_state,
    size_t &r_budget,
    std::string &r_error)
{
  blender::Object *object = game_object.GetBlenderObject();
  if (object == nullptr) {
    r_error = "object '" + game_object.GetName() + "' has no Blender modifier owner";
    return LN_GameStateResult::UnsupportedContent;
  }
  std::vector<NonGeometryModifierState> non_geometry;
  std::vector<GeometryNodesModifierState> geometry_nodes;
  const LN_GameStateResult result = capture_current_modifier_state(
      *object, catalog, true, non_geometry, geometry_nodes, r_error);
  if (!LN_GameStateSucceeded(result)) {
    r_error = "object '" + game_object.GetName() + "': " + r_error;
    return result;
  }
  if (non_geometry.size() != baseline.non_geometry_modifiers.size()) {
    r_error = "object '" + game_object.GetName() +
              "' changed its unsupported authored modifier stack at runtime";
    return LN_GameStateResult::UnsupportedContent;
  }
  for (size_t index = 0; index < non_geometry.size(); ++index) {
    if (!non_geometry_modifier_state_equal(
            non_geometry[index], baseline.non_geometry_modifiers[index]))
    {
      r_error = "object '" + game_object.GetName() +
                "' changed unsupported authored modifier state at runtime";
      return LN_GameStateResult::UnsupportedContent;
    }
  }
  for (blender::ModifierData &modifier : object->modifiers) {
    if (modifier.type != blender::eModifierType_Nodes) {
      continue;
    }
    auto &nodes_modifier = reinterpret_cast<blender::NodesModifierData &>(modifier);
    if (nodes_modifier.node_group == nullptr) {
      r_error = "object '" + game_object.GetName() +
                "' has a Geometry Nodes modifier without a node group";
      return LN_GameStateResult::UnsupportedContent;
    }
    const LN_GameStateResult node_tree_result = node_tree_capture.CaptureGeometryTree(
        *nodes_modifier.node_group, r_budget, r_error);
    if (!LN_GameStateSucceeded(node_tree_result)) {
      r_error = "object '" + game_object.GetName() + "': " + r_error;
      return node_tree_result;
    }
  }

  try {
    for (const NonGeometryModifierState &modifier : non_geometry) {
      if (!consume_budget(r_budget, sizeof(modifier) + modifier.name.size() + 1u)) {
        r_error = "modifier state exceeds the Save Game memory budget";
        return LN_GameStateResult::UnsupportedContent;
      }
    }
    for (const GeometryNodesModifierState &modifier : geometry_nodes) {
      if (!consume_budget(r_budget,
                          sizeof(modifier) + modifier.name.size() +
                              modifier.node_group_name.size() +
                              modifier.node_group_library_path.size() +
                              modifier.node_group_fingerprint.size() + 4u))
      {
        r_error = "Geometry Nodes state exceeds the Save Game memory budget";
        return LN_GameStateResult::UnsupportedContent;
      }
      for (const GeometryNodesInputState &input : modifier.inputs) {
        if (!consume_budget(r_budget,
                            sizeof(input) + input.socket_identifier.size() +
                                input.attribute_name.size() + input.layer_name.size() +
                                input.value.socket_identifier.size() +
                                input.value.string_value.size() +
                                input.value.datablock_value.name.size() +
                                input.value.datablock_value.library_path.size() + 8u))
        {
          r_error = "Geometry Nodes input state exceeds the Save Game memory budget";
          return LN_GameStateResult::UnsupportedContent;
        }
      }
      for (const GeometryNodesOutputState &output : modifier.outputs) {
        if (!consume_budget(r_budget,
                            sizeof(output) + output.socket_identifier.size() +
                                output.attribute_name.size() + 2u))
        {
          r_error = "Geometry Nodes output state exceeds the Save Game memory budget";
          return LN_GameStateResult::UnsupportedContent;
        }
      }
    }
    r_state.non_geometry_modifiers = std::move(non_geometry);
    r_state.geometry_nodes_modifiers = std::move(geometry_nodes);
    r_state.has_runtime_structure_state = true;
  }
  catch (...) {
    r_error = "could not retain modifier state for object '" + game_object.GetName() + "'";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

struct PreparedModifierRestore::Impl {
  struct ObjectPlan {
    KX_GameObject *game_object = nullptr;
    blender::Object *object = nullptr;
    std::vector<blender::ModifierData *> old_geometry_nodes;
    std::vector<blender::ModifierData *> prepared_geometry_nodes;
    std::vector<blender::ModifierData *> final_order;
  };

  blender::Main *bmain = nullptr;
  std::vector<ObjectPlan> objects;
  bool committed = false;
};

PreparedModifierRestore::PreparedModifierRestore()
    : m_impl(std::make_unique<Impl>())
{
}

PreparedModifierRestore::~PreparedModifierRestore()
{
  Rollback();
}

PreparedModifierRestore::PreparedModifierRestore(
    PreparedModifierRestore &&other) noexcept
    : m_impl(std::move(other.m_impl))
{
}
PreparedModifierRestore &PreparedModifierRestore::operator=(
    PreparedModifierRestore &&other) noexcept
{
  if (this != &other) {
    Rollback();
    m_impl = std::move(other.m_impl);
  }
  return *this;
}

void PreparedModifierRestore::Rollback()
{
  if (m_impl == nullptr || m_impl->committed) {
    return;
  }
  for (Impl::ObjectPlan &plan : m_impl->objects) {
    for (blender::ModifierData *modifier : plan.prepared_geometry_nodes) {
      if (modifier != nullptr) {
        BKE_modifier_free(modifier);
      }
    }
    plan.prepared_geometry_nodes.clear();
    plan.final_order.clear();
  }
  m_impl->objects.clear();
  m_impl->bmain = nullptr;
}

void PreparedModifierRestore::ReserveCommitUpdates(KX_Scene &scene) const
{
  if (m_impl == nullptr || m_impl->committed) {
    return;
  }
  size_t all_render_passes = 0;
  size_t overlay = 0;
  for (const Impl::ObjectPlan &plan : m_impl->objects) {
    if ((plan.object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0) {
      overlay++;
    }
    else {
      all_render_passes++;
    }
  }
  scene.ReserveIdsToUpdate(all_render_passes, overlay);
}

void PreparedModifierRestore::Commit(KX_Scene &scene)
{
  BLI_assert(m_impl != nullptr && !m_impl->committed && m_impl->bmain != nullptr);
  for (Impl::ObjectPlan &plan : m_impl->objects) {
    blender::BLI_listbase_clear(&plan.object->modifiers);
    for (blender::ModifierData *modifier : plan.final_order) {
      modifier->next = nullptr;
      modifier->prev = nullptr;
      blender::BLI_addtail(&plan.object->modifiers, modifier);
    }
    for (blender::ModifierData *modifier : plan.old_geometry_nodes) {
      modifier->next = nullptr;
      modifier->prev = nullptr;
      BKE_modifier_free(modifier);
    }
    plan.prepared_geometry_nodes.clear();
    const bool overlay_only =
        (plan.object->gameflag & blender::OB_OVERLAY_COLLECTION) != 0;
    scene.AppendToIdsToUpdate(
        &plan.object->id, blender::ID_RECALC_GEOMETRY, overlay_only);
  }
  /* Through the scene, so the render sync knows Blender's relations were rebuilt. */
  if (!m_impl->objects.empty()) {
    scene.TagForRelationsUpdate();
  }
  m_impl->committed = true;
}

LN_GameStateResult prepare_modifier_restore(
    blender::Main &bmain,
    const GeometryNodesAssetCatalog &catalog,
    const std::vector<ModifierRestoreTarget> &targets,
    PreparedModifierRestore &r_prepared,
    std::string &r_error)
{
  r_error.clear();
  r_prepared.Rollback();
  r_prepared.m_impl = std::make_unique<PreparedModifierRestore::Impl>();
  r_prepared.m_impl->bmain = &bmain;
  std::unordered_set<KX_GameObject *> seen_objects;
  try {
    seen_objects.reserve(targets.size());
    r_prepared.m_impl->objects.reserve(targets.size());
    for (const ModifierRestoreTarget &target : targets) {
      if (target.object == nullptr || target.saved == nullptr ||
          target.baseline == nullptr ||
          !target.saved->has_runtime_structure_state ||
          !seen_objects.insert(target.object).second)
      {
        r_error = "Load Game modifier plan contains an invalid or duplicate object";
        r_prepared.Rollback();
        return LN_GameStateResult::InternalError;
      }
      blender::Object *object = target.object->GetBlenderObject();
      if (object == nullptr) {
        r_error = "Load Game modifier target has no Blender object";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      const ObjectState &saved = *target.saved;
      if (saved.non_geometry_modifiers.size() !=
          target.baseline->non_geometry_modifiers.size())
      {
        r_error = "saved unsupported modifier stack differs from the current authored build";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      for (size_t index = 0; index < saved.non_geometry_modifiers.size(); ++index) {
        if (!non_geometry_modifier_state_equal(
                saved.non_geometry_modifiers[index],
                target.baseline->non_geometry_modifiers[index]))
        {
          r_error = "saved unsupported modifier state differs from the current authored build";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
      }
      std::vector<blender::ModifierData *> current_non_geometry;
      std::vector<NonGeometryModifierState> current_non_geometry_state;
      std::unordered_set<std::string> names;
      std::unordered_set<int32_t> uids;
      for (blender::ModifierData &modifier : object->modifiers) {
        if (!names.insert(modifier.name).second || modifier.persistent_uid <= 0 ||
            !uids.insert(modifier.persistent_uid).second)
        {
          r_error = "current modifier stack has an invalid or duplicate identity";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        if (modifier.type == blender::eModifierType_Nodes) {
          continue;
        }
        else {
          current_non_geometry.push_back(&modifier);
          current_non_geometry_state.push_back({modifier.name,
                                                int32_t(modifier.type),
                                                modifier.persistent_uid,
                                                uint32_t(modifier.mode) &
                                                    kPersistentModifierModeMask});
        }
      }
      if (current_non_geometry_state.size() != saved.non_geometry_modifiers.size()) {
        r_error = "authored modifier stack changed for object '" + target.object->GetName() +
                  "'";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      for (size_t index = 0; index < current_non_geometry_state.size(); ++index) {
        if (!non_geometry_modifier_state_equal(
                current_non_geometry_state[index],
                saved.non_geometry_modifiers[index]))
        {
          r_error = "authored modifier state changed for object '" +
                    target.object->GetName() + "'";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
      }

      /* Only Geometry Nodes modifiers get a saved state (the others must already match it): an
       * object without any, now or saved, keeps its stack as it is. Rebuilding it re-evaluated
       * every saved object at each load, and took grouped (Auto) objects out of their groups. */
      const bool has_geometry_nodes = std::any_of(
          object->modifiers.begin(), object->modifiers.end(), [](const blender::ModifierData &md) {
            return md.type == blender::eModifierType_Nodes;
          });
      if (!has_geometry_nodes && saved.geometry_nodes_modifiers.empty()) {
        continue;
      }
      /* Its modifiers get the saved state: drawn by its own object, and not reused when it goes
       * (KX_Scene::ParkCopy). */
      target.object->NoteBlenderObjectChanged();

      r_prepared.m_impl->objects.emplace_back();
      PreparedModifierRestore::Impl::ObjectPlan &plan =
          r_prepared.m_impl->objects.back();
      plan.game_object = target.object;
      plan.object = object;
      plan.old_geometry_nodes.reserve(saved.geometry_nodes_modifiers.size());
      for (blender::ModifierData &modifier : object->modifiers) {
        if (modifier.type == blender::eModifierType_Nodes) {
          plan.old_geometry_nodes.push_back(&modifier);
        }
      }
      plan.prepared_geometry_nodes.reserve(saved.geometry_nodes_modifiers.size());
      plan.final_order.assign(
          saved.non_geometry_modifiers.size() +
              saved.geometry_nodes_modifiers.size(),
          nullptr);

      for (const GeometryNodesModifierState &modifier_state :
           saved.geometry_nodes_modifiers)
      {
        blender::bNodeTree *node_group =
            catalog.ResolveNodeGroup(bmain, modifier_state);
        if (node_group == nullptr) {
          r_error = "saved Geometry Nodes asset '" +
                    modifier_state.node_group_name +
                    "' is missing or changed in the current game build";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        blender::ModifierData *prepared_modifier =
            BKE_modifier_new(blender::eModifierType_Nodes);
        if (prepared_modifier == nullptr) {
          r_error = "could not allocate a Geometry Nodes modifier restore";
          r_prepared.Rollback();
          return LN_GameStateResult::InternalError;
        }
        plan.prepared_geometry_nodes.push_back(prepared_modifier);
        blender::NodesModifierData &prepared_nodes =
            *reinterpret_cast<blender::NodesModifierData *>(prepared_modifier);
        blender::STRNCPY(prepared_modifier->name, modifier_state.name.c_str());
        prepared_modifier->persistent_uid = modifier_state.persistent_uid;
        prepared_modifier->mode = blender::ModifierMode(modifier_state.mode);
        if (prepared_nodes.node_group != node_group) {
          blender::id_us_plus(&node_group->id);
          if (prepared_nodes.node_group != nullptr) {
            blender::id_us_min(&prepared_nodes.node_group->id);
          }
          prepared_nodes.node_group = node_group;
        }
        blender::MOD_nodes_update_interface(object, &prepared_nodes);
        if (!apply_geometry_nodes_inputs(
                bmain, *object, prepared_nodes, modifier_state, r_error))
        {
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        if (!apply_geometry_nodes_outputs(
                *object, prepared_nodes, modifier_state, r_error))
        {
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        if (modifier_state.stack_index >= plan.final_order.size() ||
            plan.final_order[modifier_state.stack_index] != nullptr)
        {
          r_error = "saved Geometry Nodes stack position is invalid";
          r_prepared.Rollback();
          return LN_GameStateResult::CorruptData;
        }
        plan.final_order[modifier_state.stack_index] = prepared_modifier;
      }

      size_t non_geometry_index = 0;
      for (blender::ModifierData *&slot : plan.final_order) {
        if (slot != nullptr) {
          continue;
        }
        if (non_geometry_index >= current_non_geometry.size()) {
          r_error = "saved modifier stack has inconsistent positions";
          r_prepared.Rollback();
          return LN_GameStateResult::CorruptData;
        }
        slot = current_non_geometry[non_geometry_index++];
      }
      if (non_geometry_index != current_non_geometry.size()) {
        r_error = "saved modifier stack omitted an authored modifier";
        r_prepared.Rollback();
        return LN_GameStateResult::CorruptData;
      }
    }
  }
  catch (...) {
    r_error = "could not prepare Geometry Nodes modifier restoration";
    r_prepared.Rollback();
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

}  // namespace upbge::logic_nodes::game_state
