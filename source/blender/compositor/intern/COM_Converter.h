/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#ifdef WITH_CXX_GUARDEDALLOC
#  include "MEM_guardedalloc.h"
#endif

struct bNode;

namespace blender::compositor {

class Node;
class NodeOperation;
class NodeOperationInput;
class NodeOperationOutput;
class NodeOperationBuilder;

/**
 * \brief Wraps a bNode in its Node instance.
 *
 * For all nodetypes a wrapper class is created.
 *
 * \note When adding a new node to blender, this method needs to be changed to return the correct
 * Node instance.
 *
 * \see Node
 */
Node *COM_convert_bnode(bNode *b_node);

/**
 * \brief True if the node is considered 'fast'.
 *
 * Slow nodes will be skipped if fast execution is required.
 */
bool COM_bnode_is_fast_node(const bNode &b_node);

/**
 * \brief This function will add a date-type conversion rule when the to-socket does not support
 * the from-socket actual data type.
 */
NodeOperation *COM_convert_data_type(const NodeOperationOutput &from,
                                     const NodeOperationInput &to);

/**
 * \brief This function will add a resolution rule based on the settings of the NodeInput.
 *
 * \note Conversion logic is implemented in this function.
 * \see InputSocketResizeMode for the possible conversions.
 */
void COM_convert_canvas(NodeOperationBuilder &builder,
                        NodeOperationOutput *from_socket,
                        NodeOperationInput *to_socket);

}  // namespace blender::compositor
