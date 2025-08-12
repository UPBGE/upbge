/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>

#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_curves.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "NOD_geometry_exec.hh"

#include "node_geometry_util.hh"

namespace blender::nodes {

Main *GeoNodeExecParams::bmain() const
{
  return DEG_get_bmain(this->depsgraph());
}

void GeoNodeExecParams::error_message_add(const NodeWarningType type,
                                          const StringRef message) const
{
  if (geo_eval_log::GeoTreeLogger *tree_logger = this->get_local_tree_logger()) {
    tree_logger->node_warnings.append(
        *tree_logger->allocator,
        {node_.identifier, {type, tree_logger->allocator->copy_string(message)}});
  }
}

void GeoNodeExecParams::used_named_attribute(const StringRef attribute_name,
                                             const NamedAttributeUsage usage)
{
  if (geo_eval_log::GeoTreeLogger *tree_logger = this->get_local_tree_logger()) {
    tree_logger->used_named_attributes.append(
        *tree_logger->allocator,
        {node_.identifier, tree_logger->allocator->copy_string(attribute_name), usage});
  }
}

void GeoNodeExecParams::check_input_geometry_set(StringRef identifier,
                                                 const GeometrySet &geometry_set) const
{
  const SocketDeclaration &decl = *node_.input_by_identifier(identifier)->runtime->declaration;
  const decl::Geometry *geo_decl = dynamic_cast<const decl::Geometry *>(&decl);
  if (geo_decl == nullptr) {
    return;
  }

  const bool only_realized_data = geo_decl->only_realized_data();
  const bool only_instances = geo_decl->only_instances();
  const Span<GeometryComponent::Type> supported_types = geo_decl->supported_types();

  if (only_realized_data) {
    if (geometry_set.has_instances()) {
      this->error_message_add(NodeWarningType::Info,
                              TIP_("Instances in input geometry are ignored"));
    }
  }
  if (only_instances) {
    if (geometry_set.has_realized_data()) {
      this->error_message_add(NodeWarningType::Info,
                              TIP_("Realized data in input geometry is ignored"));
    }
  }
  if (supported_types.is_empty()) {
    /* Assume all types are supported. */
    return;
  }
  const Vector<GeometryComponent::Type> types_in_geometry = geometry_set.gather_component_types(
      true, true);
  for (const GeometryComponent::Type type : types_in_geometry) {
    if (type == GeometryComponent::Type::Instance) {
      continue;
    }
    if (supported_types.contains(type)) {
      continue;
    }
    std::string message = RPT_("Input geometry has unsupported type: ");
    switch (type) {
      case GeometryComponent::Type::Mesh: {
        if (const Mesh *mesh = geometry_set.get_mesh()) {
          if (mesh->verts_num == 0) {
            continue;
          }
        }
        message += RPT_("Mesh");
        break;
      }
      case GeometryComponent::Type::PointCloud: {
        if (const PointCloud *pointcloud = geometry_set.get_pointcloud()) {
          if (pointcloud->totpoint == 0) {
            continue;
          }
        }
        message += RPT_("Point Cloud");
        break;
      }
      case GeometryComponent::Type::Instance: {
        BLI_assert_unreachable();
        break;
      }
      case GeometryComponent::Type::Volume: {
        message += CTX_RPT_(BLT_I18NCONTEXT_ID_ID, "Volume");
        break;
      }
      case GeometryComponent::Type::Curve: {
        if (const Curves *curves = geometry_set.get_curves()) {
          if (curves->geometry.point_num == 0) {
            continue;
          }
        }
        message += RPT_("Curve");
        break;
      }
      case GeometryComponent::Type::Edit: {
        continue;
      }
      case GeometryComponent::Type::GreasePencil: {
        if (const GreasePencil *grease_pencil = geometry_set.get_grease_pencil()) {
          if (grease_pencil->drawing_array_num == 0) {
            continue;
          }
        }
        message += RPT_("Grease Pencil");
        break;
      }
    }
    this->error_message_add(NodeWarningType::Info, std::move(message));
  }
}

void GeoNodeExecParams::check_output_geometry_set(const GeometrySet &geometry_set) const
{
  UNUSED_VARS_NDEBUG(geometry_set);
#ifndef NDEBUG
  if (const bke::CurvesEditHints *curve_edit_hints = geometry_set.get_curve_edit_hints()) {
    /* If this is not valid, it's likely that the number of stored deformed points does not match
     * the number of points in the original data. */
    BLI_assert(curve_edit_hints->is_valid());
  }
#endif
}

const bNodeSocket *GeoNodeExecParams::find_available_socket(const StringRef name) const
{
  for (const bNodeSocket *socket : node_.input_sockets()) {
    if (socket->is_available() && socket->name == name) {
      return socket;
    }
  }

  return nullptr;
}

void GeoNodeExecParams::set_default_remaining_outputs()
{
  set_default_remaining_node_outputs(params_, node_);
}

void GeoNodeExecParams::check_input_access(StringRef identifier) const
{
  const bNodeSocket *found_socket = nullptr;
  for (const bNodeSocket *socket : node_.input_sockets()) {
    if (socket->identifier == identifier) {
      found_socket = socket;
      break;
    }
  }

  if (found_socket == nullptr) {
    std::cout << "Did not find an input socket with the identifier '" << identifier << "'.\n";
    std::cout << "Possible identifiers are: ";
    for (const bNodeSocket *socket : node_.input_sockets()) {
      if (socket->is_available()) {
        std::cout << "'" << socket->identifier << "', ";
      }
    }
    std::cout << "\n";
    BLI_assert_unreachable();
  }
  else if (found_socket->flag & SOCK_UNAVAIL) {
    std::cout << "The socket corresponding to the identifier '" << identifier
              << "' is disabled.\n";
    BLI_assert_unreachable();
  }
}

void GeoNodeExecParams::check_output_access(StringRef identifier) const
{
  const bNodeSocket *found_socket = nullptr;
  for (const bNodeSocket *socket : node_.output_sockets()) {
    if (socket->identifier == identifier) {
      found_socket = socket;
      break;
    }
  }

  if (found_socket == nullptr) {
    std::cout << "Did not find an output socket with the identifier '" << identifier << "'.\n";
    std::cout << "Possible identifiers are: ";
    for (const bNodeSocket *socket : node_.output_sockets()) {
      if (socket->is_available()) {
        std::cout << "'" << socket->identifier << "', ";
      }
    }
    std::cout << "\n";
    BLI_assert_unreachable();
  }
  else if (found_socket->flag & SOCK_UNAVAIL) {
    std::cout << "The socket corresponding to the identifier '" << identifier
              << "' is disabled.\n";
    BLI_assert_unreachable();
  }
  else if (params_.output_was_set(this->get_output_index(identifier))) {
    std::cout << "The identifier '" << identifier << "' has been set already.\n";
    BLI_assert_unreachable();
  }
}

AttributeFilter::Result NodeAttributeFilter::filter(const StringRef attribute_name) const
{
  if (!bke::attribute_name_is_anonymous(attribute_name)) {
    return AttributeFilter::Result::Process;
  }
  if (!set_.names) {
    return AttributeFilter::Result::AllowSkip;
  }
  if (set_.names->contains(attribute_name)) {
    return AttributeFilter::Result::Process;
  }
  return AttributeFilter::Result::AllowSkip;
}

std::optional<std::string> GeoNodeExecParams::ensure_absolute_path(const StringRefNull path) const
{
  if (path.is_empty()) {
    return std::nullopt;
  }
  if (!BLI_path_is_rel(path.c_str())) {
    return path;
  }
  const Main &bmain = *this->bmain();
  const bNodeTree &tree = node_.owner_tree();
  const char *base_path = ID_BLEND_PATH(&bmain, &tree.id);
  if (!base_path || base_path[0] == '\0') {
    return std::nullopt;
  }
  char absolute_path[FILE_MAX];
  STRNCPY(absolute_path, path.c_str());
  BLI_path_abs(absolute_path, base_path);
  return absolute_path;
}

}  // namespace blender::nodes
