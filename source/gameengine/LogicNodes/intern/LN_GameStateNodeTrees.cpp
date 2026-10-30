/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateNodeTrees.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameStateNodeTrees.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "DNA_ID.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "KX_Scene.h"
#include "LN_GameStateGeometryNodes.h"

namespace upbge::logic_nodes::game_state {

namespace {

std::string node_tree_identity_key(const RuntimeNodeTreeType type,
                                   const std::string &name,
                                   const std::string &library_path)
{
  std::string key;
  key.reserve(name.size() + library_path.size() + 2u);
  key.push_back(char(type));
  key.append(name);
  key.push_back('\0');
  key.append(library_path);
  return key;
}

int16_t datablock_type_for_socket_type(const MaterialSocketValueType type)
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

bool socket_value_type(const blender::bNodeSocket &socket, MaterialSocketValueType &r_type)
{
  switch (socket.type) {
    case blender::SOCK_BOOLEAN:
      r_type = MaterialSocketValueType::Bool;
      return true;
    case blender::SOCK_INT:
      r_type = MaterialSocketValueType::Int;
      return true;
    case blender::SOCK_MENU:
      r_type = MaterialSocketValueType::Menu;
      return true;
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

blender::ID *socket_datablock(const blender::bNodeSocket &socket)
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
                                 MaterialSocketValueState &r_state,
                                 std::string &r_error)
{
  blender::ID *id = socket_datablock(socket);
  if (id == nullptr) {
    return true;
  }
  if (*reinterpret_cast<const int16_t *>(id->name) !=
          datablock_type_for_socket_type(r_state.type) ||
      id->session_uid == 0 ||
      (id->tag & (blender::ID_TAG_MISSING | blender::ID_TAG_RUNTIME |
                  blender::ID_TAG_NO_MAIN | blender::ID_TAG_TEMP_MAIN |
                  blender::ID_TAG_COPIED_ON_EVAL |
                  blender::ID_TAG_COPIED_ON_EVAL_FINAL_RESULT)) != 0)
  {
    r_error = "runtime node input references a runtime-only or invalid datablock";
    return false;
  }
  r_state.datablock_value.has_value = true;
  r_state.datablock_value.name = id->name + 2;
  r_state.datablock_value.library_path = id->lib ? id->lib->filepath : std::string();
  if (!is_valid_checkpoint_text(r_state.datablock_value.name, kMaxNameBytes, false) ||
      !is_valid_checkpoint_text(r_state.datablock_value.library_path, kMaxIdentityBytes))
  {
    r_error = "runtime node input has an invalid datablock identity";
    return false;
  }
  return true;
}

bool capture_socket_value(const blender::bNodeSocket &socket,
                          MaterialSocketValueState &r_state,
                          bool &r_supported,
                          std::string &r_error)
{
  r_supported = false;
  if (socket.default_value == nullptr || !socket_value_type(socket, r_state.type)) {
    return true;
  }
  r_supported = true;
  r_state.socket_identifier = socket.identifier;
  if (!is_valid_checkpoint_text(r_state.socket_identifier, kMaxNameBytes, false)) {
    r_error = "runtime node input has no stable socket identifier";
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
    case MaterialSocketValueType::Menu: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueMenu *>(socket.default_value);
      if (value.enum_items == nullptr || value.has_conflict() ||
          value.enum_items->find_item_by_identifier(value.value) == nullptr)
      {
        r_error = "runtime node menu input has no deterministic value";
        return false;
      }
      r_state.int_value = value.value;
      return true;
    }
    case MaterialSocketValueType::IntVector: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueIntVector *>(socket.default_value);
      if (value.dimensions < 2 || value.dimensions > 3) {
        r_error = "runtime node integer-vector input has invalid dimensions";
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
        r_error = "runtime node float input is not finite";
        return false;
      }
      r_state.float_values[0] = value;
      return true;
    }
    case MaterialSocketValueType::Vector: {
      const auto &value =
          *static_cast<const blender::bNodeSocketValueVector *>(socket.default_value);
      if (value.dimensions < 2 || value.dimensions > 4) {
        r_error = "runtime node vector input has invalid dimensions";
        return false;
      }
      r_state.dimensions = uint8_t(value.dimensions);
      std::copy_n(value.value, value.dimensions, r_state.float_values.begin());
      break;
    }
    case MaterialSocketValueType::Rotation:
      std::copy_n(
          static_cast<const blender::bNodeSocketValueRotation *>(socket.default_value)
              ->value_euler,
          3,
          r_state.float_values.begin());
      break;
    case MaterialSocketValueType::Color:
      std::copy_n(
          static_cast<const blender::bNodeSocketValueRGBA *>(socket.default_value)->value,
          4,
          r_state.float_values.begin());
      break;
    case MaterialSocketValueType::String:
      r_state.string_value =
          static_cast<const blender::bNodeSocketValueString *>(socket.default_value)->value;
      if (!is_valid_checkpoint_text(r_state.string_value, kMaxMaterialStringBytes)) {
        r_error = "runtime node string input is invalid or oversized";
        return false;
      }
      return true;
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
      return capture_datablock_reference(socket, r_state, r_error);
  }
  const size_t count = r_state.type == MaterialSocketValueType::Color ?
                           4u :
                       r_state.type == MaterialSocketValueType::Rotation ?
                           3u :
                           r_state.dimensions;
  if (!std::all_of(r_state.float_values.begin(),
                   r_state.float_values.begin() + count,
                   [](const float value) { return std::isfinite(value); }))
  {
    r_error = "runtime node input contains a non-finite value";
    return false;
  }
  return true;
}

blender::bNode *find_node(blender::bNodeTree &tree,
                          const int32_t identifier,
                          const std::string &type)
{
  blender::bNode *found = nullptr;
  for (blender::bNode &node : tree.nodes) {
    if (node.identifier != identifier) {
      continue;
    }
    if (found != nullptr || type != node.idname) {
      return nullptr;
    }
    found = &node;
  }
  return found;
}

blender::bNodeSocket *find_input(blender::bNode &node, const std::string &identifier)
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

bool socket_matches_state(const blender::bNodeSocket &socket,
                          const MaterialSocketValueState &state)
{
  MaterialSocketValueType type;
  if (socket.default_value == nullptr || !socket_value_type(socket, type) || type != state.type) {
    return false;
  }
  if (type == MaterialSocketValueType::IntVector) {
    return static_cast<const blender::bNodeSocketValueIntVector *>(socket.default_value)
               ->dimensions == state.dimensions;
  }
  if (type == MaterialSocketValueType::Vector) {
    return static_cast<const blender::bNodeSocketValueVector *>(socket.default_value)->dimensions ==
           state.dimensions;
  }
  if (type == MaterialSocketValueType::Menu) {
    const auto &value =
        *static_cast<const blender::bNodeSocketValueMenu *>(socket.default_value);
    return value.enum_items != nullptr && !value.has_conflict() &&
           value.enum_items->find_item_by_identifier(state.int_value) != nullptr;
  }
  return true;
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
  const int16_t type = datablock_type_for_socket_type(state.type);
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

bool set_socket_datablock(blender::bNodeSocket &socket, blender::ID *desired)
{
  blender::ID *current = socket_datablock(socket);
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

bool apply_socket_value(blender::bNodeSocket &socket,
                        const MaterialSocketValueState &state,
                        blender::ID *datablock)
{
  switch (state.type) {
    case MaterialSocketValueType::Bool: {
      auto &value = *static_cast<blender::bNodeSocketValueBoolean *>(socket.default_value);
      const bool current = value.value;
      value.value = state.bool_value;
      return current != state.bool_value;
    }
    case MaterialSocketValueType::Int: {
      auto &value = *static_cast<blender::bNodeSocketValueInt *>(socket.default_value);
      const bool changed = value.value != state.int_value;
      value.value = state.int_value;
      return changed;
    }
    case MaterialSocketValueType::Menu: {
      auto &value = *static_cast<blender::bNodeSocketValueMenu *>(socket.default_value);
      const bool changed = value.value != state.int_value;
      value.value = state.int_value;
      return changed;
    }
    case MaterialSocketValueType::IntVector: {
      auto &value =
          *static_cast<blender::bNodeSocketValueIntVector *>(socket.default_value);
      const bool changed = !std::equal(state.int_values.begin(),
                                       state.int_values.begin() + state.dimensions,
                                       value.value);
      std::copy_n(state.int_values.begin(), state.dimensions, value.value);
      return changed;
    }
    case MaterialSocketValueType::Float: {
      auto &value = *static_cast<blender::bNodeSocketValueFloat *>(socket.default_value);
      const bool changed = value.value != state.float_values[0];
      value.value = state.float_values[0];
      return changed;
    }
    case MaterialSocketValueType::Vector: {
      auto &value = *static_cast<blender::bNodeSocketValueVector *>(socket.default_value);
      const bool changed = !std::equal(state.float_values.begin(),
                                       state.float_values.begin() + state.dimensions,
                                       value.value);
      std::copy_n(state.float_values.begin(), state.dimensions, value.value);
      return changed;
    }
    case MaterialSocketValueType::Rotation: {
      auto &value =
          *static_cast<blender::bNodeSocketValueRotation *>(socket.default_value);
      const bool changed =
          !std::equal(state.float_values.begin(), state.float_values.begin() + 3, value.value_euler);
      std::copy_n(state.float_values.begin(), 3, value.value_euler);
      return changed;
    }
    case MaterialSocketValueType::Color: {
      auto &value = *static_cast<blender::bNodeSocketValueRGBA *>(socket.default_value);
      const bool changed =
          !std::equal(state.float_values.begin(), state.float_values.end(), value.value);
      std::copy(state.float_values.begin(), state.float_values.end(), value.value);
      return changed;
    }
    case MaterialSocketValueType::String: {
      auto &value = *static_cast<blender::bNodeSocketValueString *>(socket.default_value);
      const bool changed = !STREQ(value.value, state.string_value.c_str());
      blender::BLI_strncpy(value.value, state.string_value.c_str(), sizeof(value.value));
      return changed;
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
      return set_socket_datablock(socket, datablock);
  }
  BLI_assert_unreachable();
  return false;
}

}  // namespace

struct RuntimeNodeTreeCapture::Impl {
  const GeometryNodesAssetCatalog *catalog = nullptr;
  std::unordered_map<std::string, RuntimeNodeTreeState> states;

  LN_GameStateResult Capture(blender::bNodeTree &tree,
                             const RuntimeNodeTreeType type,
                             size_t &r_budget,
                             std::string &r_error)
  {
    std::string name;
    std::string library;
    std::string fingerprint;
    if (catalog == nullptr ||
        !catalog->DescribeNodeTree(tree, type, name, library, fingerprint))
    {
      r_error = "runtime node tree is missing or has unsupported structural edits";
      return LN_GameStateResult::UnsupportedContent;
    }
    const std::string key = node_tree_identity_key(type, name, library);
    if (states.find(key) != states.end()) {
      return LN_GameStateResult::Success;
    }
    if (states.size() >= kMaxRuntimeNodeTrees) {
      r_error = "runtime node-tree state exceeds the Save Game limit";
      return LN_GameStateResult::UnsupportedContent;
    }

    RuntimeNodeTreeState state;
    state.type = type;
    state.name = std::move(name);
    state.library_path = std::move(library);
    state.structural_fingerprint = std::move(fingerprint);
    if (!consume_budget(r_budget,
                        sizeof(state) + state.name.size() + state.library_path.size() +
                            state.structural_fingerprint.size() + 3u))
    {
      r_error = "runtime node-tree state exceeds the Save Game memory budget";
      return LN_GameStateResult::UnsupportedContent;
    }
    if (blender::BLI_listbase_count(&tree.nodes) > int(kMaxRuntimeNodesPerTree)) {
      r_error = "runtime node tree exceeds the node limit";
      return LN_GameStateResult::UnsupportedContent;
    }
    state.nodes.reserve(blender::BLI_listbase_count(&tree.nodes));
    for (blender::bNode &node : tree.nodes) {
      RuntimeNodeState node_state;
      node_state.node_identifier = node.identifier;
      node_state.node_type = node.idname;
      node_state.muted = (node.flag & blender::NODE_MUTED) != 0;
      if (node_state.node_identifier < 0 ||
          !is_valid_checkpoint_text(node_state.node_type, kMaxNameBytes, false) ||
          blender::BLI_listbase_count(&node.inputs) > int(kMaxRuntimeNodeInputsPerNode) ||
          !consume_budget(r_budget,
                          sizeof(node_state) + node_state.node_type.size() + 1u))
      {
        r_error = "runtime node identity or input count exceeds the Save Game limit";
        return LN_GameStateResult::UnsupportedContent;
      }
      node_state.inputs.reserve(blender::BLI_listbase_count(&node.inputs));
      for (blender::bNodeSocket &socket : node.inputs) {
        MaterialSocketValueState input;
        bool supported = false;
        if (!capture_socket_value(socket, input, supported, r_error)) {
          r_error = "node tree '" + state.name + "', node '" + node_state.node_type +
                    "': " + r_error;
          return LN_GameStateResult::UnsupportedContent;
        }
        if (!supported) {
          continue;
        }
        if (!consume_budget(r_budget,
                            sizeof(input) + input.socket_identifier.size() +
                                input.string_value.size() +
                                input.datablock_value.name.size() +
                                input.datablock_value.library_path.size() + 4u))
        {
          r_error = "runtime node input state exceeds the Save Game memory budget";
          return LN_GameStateResult::UnsupportedContent;
        }
        node_state.inputs.push_back(std::move(input));
      }
      std::sort(node_state.inputs.begin(),
                node_state.inputs.end(),
                [](const MaterialSocketValueState &a, const MaterialSocketValueState &b) {
                  return a.socket_identifier < b.socket_identifier;
                });
      if (std::adjacent_find(
              node_state.inputs.begin(),
              node_state.inputs.end(),
              [](const MaterialSocketValueState &a, const MaterialSocketValueState &b) {
                return a.socket_identifier == b.socket_identifier;
              }) != node_state.inputs.end())
      {
        r_error = "runtime node contains duplicate socket identifiers";
        return LN_GameStateResult::UnsupportedContent;
      }
      state.nodes.push_back(std::move(node_state));
    }
    std::sort(state.nodes.begin(),
              state.nodes.end(),
              [](const RuntimeNodeState &a, const RuntimeNodeState &b) {
                return a.node_identifier < b.node_identifier;
              });
    if (std::adjacent_find(
            state.nodes.begin(),
            state.nodes.end(),
            [](const RuntimeNodeState &a, const RuntimeNodeState &b) {
              return a.node_identifier == b.node_identifier;
            }) != state.nodes.end())
    {
      r_error = "runtime node tree contains duplicate node identifiers";
      return LN_GameStateResult::UnsupportedContent;
    }
    states.emplace(key, std::move(state));

    for (blender::bNode &node : tree.nodes) {
      if (node.id == nullptr ||
          *reinterpret_cast<const int16_t *>(node.id->name) != blender::ID_NT)
      {
        continue;
      }
      auto &nested = *reinterpret_cast<blender::bNodeTree *>(node.id);
      if (nested.type != tree.type) {
        continue;
      }
      const LN_GameStateResult nested_result = Capture(nested, type, r_budget, r_error);
      if (!LN_GameStateSucceeded(nested_result)) {
        return nested_result;
      }
    }
    return LN_GameStateResult::Success;
  }
};

RuntimeNodeTreeCapture::RuntimeNodeTreeCapture(const GeometryNodesAssetCatalog &catalog)
    : m_impl(std::make_unique<Impl>())
{
  m_impl->catalog = &catalog;
}

RuntimeNodeTreeCapture::~RuntimeNodeTreeCapture() = default;
RuntimeNodeTreeCapture::RuntimeNodeTreeCapture(RuntimeNodeTreeCapture &&) noexcept = default;
RuntimeNodeTreeCapture &RuntimeNodeTreeCapture::operator=(RuntimeNodeTreeCapture &&) noexcept =
    default;

LN_GameStateResult RuntimeNodeTreeCapture::CaptureGeometryTree(blender::bNodeTree &tree,
                                                               size_t &r_budget,
                                                               std::string &r_error)
{
  if (tree.type != blender::NTREE_GEOMETRY) {
    r_error = "Geometry Nodes modifier references a non-geometry node tree";
    return LN_GameStateResult::UnsupportedContent;
  }
  return m_impl->Capture(tree, RuntimeNodeTreeType::Geometry, r_budget, r_error);
}

LN_GameStateResult RuntimeNodeTreeCapture::CaptureCompositor(blender::Scene &scene,
                                                             Checkpoint &r_checkpoint,
                                                             size_t &r_budget,
                                                             std::string &r_error)
{
  r_checkpoint.compositor = {};
  blender::bNodeTree *tree = scene.compositing_node_group;
  if (tree == nullptr) {
    return LN_GameStateResult::Success;
  }
  if (tree->type != blender::NTREE_COMPOSIT) {
    r_error = "scene compositor references an invalid node-tree type";
    return LN_GameStateResult::UnsupportedContent;
  }
  std::string name;
  std::string library;
  std::string fingerprint;
  if (!m_impl->catalog->DescribeNodeTree(
          *tree, RuntimeNodeTreeType::Compositor, name, library, fingerprint))
  {
    r_error = "scene compositor has unsupported runtime structural edits";
    return LN_GameStateResult::UnsupportedContent;
  }
  if (!consume_budget(r_budget,
                      sizeof(CompositorState) + name.size() + library.size() +
                          fingerprint.size() + 3u))
  {
    r_error = "compositor state exceeds the Save Game memory budget";
    return LN_GameStateResult::UnsupportedContent;
  }
  r_checkpoint.compositor.has_node_tree = true;
  r_checkpoint.compositor.node_tree_name = std::move(name);
  r_checkpoint.compositor.node_tree_library_path = std::move(library);
  r_checkpoint.compositor.node_tree_structural_fingerprint = std::move(fingerprint);
  return m_impl->Capture(
      *tree, RuntimeNodeTreeType::Compositor, r_budget, r_error);
}

LN_GameStateResult RuntimeNodeTreeCapture::Finalize(Checkpoint &r_checkpoint,
                                                    std::string &r_error)
{
  try {
    r_checkpoint.runtime_node_trees.clear();
    r_checkpoint.runtime_node_trees.reserve(m_impl->states.size());
    for (auto &item : m_impl->states) {
      r_checkpoint.runtime_node_trees.push_back(std::move(item.second));
    }
    std::sort(r_checkpoint.runtime_node_trees.begin(),
              r_checkpoint.runtime_node_trees.end(),
              [](const RuntimeNodeTreeState &a, const RuntimeNodeTreeState &b) {
                return std::tie(a.type, a.name, a.library_path) <
                       std::tie(b.type, b.name, b.library_path);
              });
  }
  catch (...) {
    r_error = "could not finalize runtime node-tree state";
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

struct PreparedRuntimeNodeTreeRestore::Impl {
  struct InputPlan {
    blender::bNodeSocket *socket = nullptr;
    MaterialSocketValueState state;
    blender::ID *datablock = nullptr;
  };
  struct NodePlan {
    blender::bNodeTree *tree = nullptr;
    blender::bNode *node = nullptr;
    bool muted = false;
    std::vector<InputPlan> inputs;
  };
  struct TreePlan {
    blender::bNodeTree *tree = nullptr;
    std::vector<NodePlan> nodes;
  };

  blender::Main *bmain = nullptr;
  blender::Scene *scene = nullptr;
  blender::bNodeTree *saved_compositor = nullptr;
  std::vector<TreePlan> trees;
  bool committed = false;
};

PreparedRuntimeNodeTreeRestore::PreparedRuntimeNodeTreeRestore()
    : m_impl(std::make_unique<Impl>())
{
}

PreparedRuntimeNodeTreeRestore::~PreparedRuntimeNodeTreeRestore()
{
  Rollback();
}

PreparedRuntimeNodeTreeRestore::PreparedRuntimeNodeTreeRestore(
    PreparedRuntimeNodeTreeRestore &&) noexcept = default;

PreparedRuntimeNodeTreeRestore &PreparedRuntimeNodeTreeRestore::operator=(
    PreparedRuntimeNodeTreeRestore &&) noexcept = default;

void PreparedRuntimeNodeTreeRestore::Rollback() noexcept
{
  if (m_impl == nullptr || m_impl->committed) {
    return;
  }
  m_impl->trees.clear();
  m_impl->bmain = nullptr;
  m_impl->scene = nullptr;
  m_impl->saved_compositor = nullptr;
}

void PreparedRuntimeNodeTreeRestore::ReserveCommitUpdates(KX_Scene &scene) const
{
  if (m_impl == nullptr || m_impl->committed) {
    return;
  }
  scene.ReserveIdsToUpdate(m_impl->trees.size() + 1u, 0u);
}

void PreparedRuntimeNodeTreeRestore::Commit(KX_Scene &scene)
{
  BLI_assert(m_impl != nullptr && !m_impl->committed && m_impl->bmain != nullptr &&
             m_impl->scene != nullptr);
  if (m_impl->scene->compositing_node_group != m_impl->saved_compositor) {
    if (m_impl->saved_compositor != nullptr) {
      blender::id_us_plus(&m_impl->saved_compositor->id);
    }
    if (m_impl->scene->compositing_node_group != nullptr) {
      blender::id_us_min(&m_impl->scene->compositing_node_group->id);
    }
    m_impl->scene->compositing_node_group = m_impl->saved_compositor;
    blender::DEG_relations_tag_update(m_impl->bmain);
  }

  for (Impl::TreePlan &tree_plan : m_impl->trees) {
    bool tree_changed = false;
    for (Impl::NodePlan &node_plan : tree_plan.nodes) {
      const bool currently_muted = (node_plan.node->flag & blender::NODE_MUTED) != 0;
      if (currently_muted != node_plan.muted) {
        SET_FLAG_FROM_TEST(node_plan.node->flag, node_plan.muted, blender::NODE_MUTED);
        BKE_ntree_update_tag_node_mute(tree_plan.tree, node_plan.node);
        tree_changed = true;
      }
      for (Impl::InputPlan &input : node_plan.inputs) {
        if (apply_socket_value(*input.socket, input.state, input.datablock)) {
          BKE_ntree_update_tag_socket_property(tree_plan.tree, input.socket);
          tree_changed = true;
        }
      }
    }
    if (tree_changed) {
      BKE_main_ensure_invariants(*m_impl->bmain, tree_plan.tree->id);
    }
    scene.AppendToIdsToUpdate(
        &tree_plan.tree->id, blender::ID_RECALC_NTREE_OUTPUT, false);
  }
  if (m_impl->saved_compositor != nullptr) {
    scene.AppendToIdsToUpdate(
        &m_impl->saved_compositor->id, blender::ID_RECALC_NTREE_OUTPUT, false);
  }
  m_impl->committed = true;
}

LN_GameStateResult prepare_runtime_node_tree_restore(
    blender::Main &bmain,
    blender::Scene &scene,
    const GeometryNodesAssetCatalog &catalog,
    const Checkpoint &checkpoint,
    PreparedRuntimeNodeTreeRestore &r_prepared,
    std::string &r_error)
{
  r_error.clear();
  r_prepared.Rollback();
  r_prepared.m_impl = std::make_unique<PreparedRuntimeNodeTreeRestore::Impl>();
  auto &prepared = *r_prepared.m_impl;
  prepared.bmain = &bmain;
  prepared.scene = &scene;
  try {
    std::unordered_map<std::string, const RuntimeNodeTreeState *> saved_trees;
    std::unordered_map<std::string, blender::bNodeTree *> resolved_trees;
    saved_trees.reserve(checkpoint.runtime_node_trees.size());
    resolved_trees.reserve(checkpoint.runtime_node_trees.size());
    prepared.trees.reserve(checkpoint.runtime_node_trees.size());
    for (const RuntimeNodeTreeState &saved : checkpoint.runtime_node_trees) {
      const std::string key = node_tree_identity_key(saved.type, saved.name, saved.library_path);
      if (!saved_trees.emplace(key, &saved).second) {
        r_error = "Load Game contains duplicate runtime node-tree state";
        r_prepared.Rollback();
        return LN_GameStateResult::CorruptData;
      }
      blender::bNodeTree *tree = catalog.ResolveNodeTree(bmain, saved);
      if (tree == nullptr) {
        r_error = "saved runtime node tree '" + saved.name +
                  "' is missing or structurally changed in the current game build";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      resolved_trees.emplace(key, tree);
      prepared.trees.emplace_back();
      PreparedRuntimeNodeTreeRestore::Impl::TreePlan &tree_plan = prepared.trees.back();
      tree_plan.tree = tree;
      tree_plan.nodes.reserve(saved.nodes.size());
      if (saved.nodes.size() != size_t(blender::BLI_listbase_count(&tree->nodes))) {
        r_error = "saved runtime node tree has a different node manifest";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      for (const RuntimeNodeState &saved_node : saved.nodes) {
        blender::bNode *node = find_node(
            *tree, saved_node.node_identifier, saved_node.node_type);
        if (node == nullptr) {
          r_error = "saved runtime node is missing from node tree '" + saved.name + "'";
          r_prepared.Rollback();
          return LN_GameStateResult::Incompatible;
        }
        tree_plan.nodes.emplace_back();
        PreparedRuntimeNodeTreeRestore::Impl::NodePlan &node_plan =
            tree_plan.nodes.back();
        node_plan.tree = tree;
        node_plan.node = node;
        node_plan.muted = saved_node.muted;
        node_plan.inputs.reserve(saved_node.inputs.size());
        const size_t supported_input_count = std::count_if(
            node->inputs.begin(), node->inputs.end(), [](const blender::bNodeSocket &socket) {
              MaterialSocketValueType type;
              return socket.default_value != nullptr && socket_value_type(socket, type);
            });
        if (saved_node.inputs.size() != supported_input_count) {
          r_error = "saved runtime node input manifest is incomplete";
          r_prepared.Rollback();
          return LN_GameStateResult::CorruptData;
        }
        for (const MaterialSocketValueState &saved_input : saved_node.inputs) {
          blender::bNodeSocket *socket = find_input(*node, saved_input.socket_identifier);
          if (socket == nullptr || !socket_matches_state(*socket, saved_input)) {
            r_error = "saved runtime node input is missing or has a different type";
            r_prepared.Rollback();
            return LN_GameStateResult::Incompatible;
          }
          blender::ID *datablock = resolve_datablock(bmain, saved_input);
          if (saved_input.datablock_value.has_value && datablock == nullptr) {
            r_error = "saved runtime node input references a missing datablock";
            r_prepared.Rollback();
            return LN_GameStateResult::Incompatible;
          }
          node_plan.inputs.push_back({socket, saved_input, datablock});
        }
      }
    }

    std::unordered_set<std::string> required_trees;
    required_trees.reserve(saved_trees.size());
    const auto require_tree = [&](auto &&self,
                                  blender::bNodeTree &tree,
                                  const RuntimeNodeTreeType type) -> bool {
      std::string name;
      std::string library;
      std::string fingerprint;
      if (!catalog.DescribeNodeTree(tree, type, name, library, fingerprint)) {
        return false;
      }
      RuntimeNodeTreeState identity;
      identity.type = type;
      identity.name = std::move(name);
      identity.library_path = std::move(library);
      const std::string key =
          node_tree_identity_key(type, identity.name, identity.library_path);
      const auto saved = saved_trees.find(key);
      if (saved == saved_trees.end() ||
          saved->second->structural_fingerprint != fingerprint)
      {
        return false;
      }
      if (!required_trees.insert(key).second) {
        return true;
      }
      for (blender::bNode &node : tree.nodes) {
        if (node.id == nullptr ||
            *reinterpret_cast<const int16_t *>(node.id->name) != blender::ID_NT)
        {
          continue;
        }
        auto &nested = *reinterpret_cast<blender::bNodeTree *>(node.id);
        if (nested.type == tree.type && !self(self, nested, type)) {
          return false;
        }
      }
      return true;
    };
    const auto require_object_geometry = [&](const std::vector<ObjectState> &objects) {
      for (const ObjectState &object : objects) {
        if (!object.has_runtime_structure_state) {
          continue;
        }
        for (const GeometryNodesModifierState &modifier :
             object.geometry_nodes_modifiers)
        {
          const std::string key = node_tree_identity_key(
              RuntimeNodeTreeType::Geometry,
              modifier.node_group_name,
              modifier.node_group_library_path);
          const auto resolved = resolved_trees.find(key);
          const auto saved = saved_trees.find(key);
          if (resolved == resolved_trees.end() || saved == saved_trees.end() ||
              saved->second->structural_fingerprint !=
                  modifier.node_group_fingerprint ||
              !require_tree(
                  require_tree, *resolved->second, RuntimeNodeTreeType::Geometry))
          {
            return false;
          }
        }
      }
      return true;
    };
    if (!require_object_geometry(checkpoint.authored_objects) ||
        !require_object_geometry(checkpoint.spawned_instances))
    {
      r_error = "saved Geometry Nodes modifier is missing its exact node-tree state";
      r_prepared.Rollback();
      return LN_GameStateResult::CorruptData;
    }

    if (checkpoint.compositor.has_node_tree) {
      const std::string key = node_tree_identity_key(
          RuntimeNodeTreeType::Compositor,
          checkpoint.compositor.node_tree_name,
          checkpoint.compositor.node_tree_library_path);
      const auto state = saved_trees.find(key);
      if (state == saved_trees.end() ||
          state->second->structural_fingerprint !=
              checkpoint.compositor.node_tree_structural_fingerprint)
      {
        r_error = "saved compositor root is missing its runtime node-tree state";
        r_prepared.Rollback();
        return LN_GameStateResult::CorruptData;
      }
      prepared.saved_compositor = catalog.ResolveNodeTree(bmain, *state->second);
      if (prepared.saved_compositor == nullptr ||
          prepared.saved_compositor->type != blender::NTREE_COMPOSIT)
      {
        r_error = "saved compositor root is missing from the current game build";
        r_prepared.Rollback();
        return LN_GameStateResult::Incompatible;
      }
      if (!require_tree(require_tree,
                        *prepared.saved_compositor,
                        RuntimeNodeTreeType::Compositor))
      {
        r_error = "saved compositor is missing its exact node-tree state";
        r_prepared.Rollback();
        return LN_GameStateResult::CorruptData;
      }
    }
    if (required_trees.size() != saved_trees.size()) {
      r_error = "Load Game contains unreferenced runtime node-tree state";
      r_prepared.Rollback();
      return LN_GameStateResult::CorruptData;
    }
  }
  catch (...) {
    r_error = "could not prepare runtime node-tree restoration";
    r_prepared.Rollback();
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

}  // namespace upbge::logic_nodes::game_state
