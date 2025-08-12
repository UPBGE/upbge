/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_function_ref.hh"

#include "DNA_node_types.h"

#include "BKE_bake_items.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_node_socket_value.hh"

namespace blender::bke::bake {

/**
 * Describes how bake items should be mapped to sockets.
 */
struct BakeSocketConfig {
  /** The type of every socket. */
  Vector<eNodeSocketDatatype> types;
  /**
   * The domain on which an the attribute corresponding to the socket should be stored (only used
   * for some socket types).
   */
  Vector<AttrDomain> domains;
  /** User-defined name of every socket. */
  Vector<StringRef> names;
  /**
   * Determines which geometries a field socket should be evaluated on. This can be used to
   * implement rules like a field should only be evaluated on the preceding or on all geometries.
   */
  Vector<Vector<int, 1>> geometries_by_attribute;
};

/**
 * Create new bake items from the socket values. The socket values are not destructed, but they may
 * be in a moved-from state afterwards.
 */
Array<std::unique_ptr<BakeItem>> move_socket_values_to_bake_items(
    MutableSpan<SocketValueVariant> socket_values,
    const BakeSocketConfig &config,
    BakeDataBlockMap *data_block_map);

/**
 * Create socket values from bake items.
 * - The data stored in the bake items may be in a moved-from state afterwards. Therefore, this
 *   should only be used when the bake items are not needed afterwards anymore.
 * - If a socket does not have a corresponding bake item, it's initialized to its default value.
 *
 * \param make_attribute_field: A function that creates a field input for any anonymous attributes
 *   being created for the baked data.
 */
Vector<SocketValueVariant> move_bake_items_to_socket_values(
    Span<BakeItem *> bake_items,
    const BakeSocketConfig &config,
    BakeDataBlockMap *data_block_map,
    FunctionRef<std::shared_ptr<AttributeFieldInput>(int socket_index, const CPPType &)>
        make_attribute_field);

/**
 * Similar to #move_bake_items_to_socket_values, but does not change the bake items. Hence, this
 * should be used when the bake items are still used later on.
 */
Vector<SocketValueVariant> copy_bake_items_to_socket_values(
    Span<const BakeItem *> bake_items,
    const BakeSocketConfig &config,
    BakeDataBlockMap *data_block_map,
    FunctionRef<std::shared_ptr<AttributeFieldInput>(int, const CPPType &)> make_attribute_field);

}  // namespace blender::bke::bake
