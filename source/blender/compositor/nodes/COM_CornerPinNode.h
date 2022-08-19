/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. */

#pragma once

#include "COM_Node.h"

#include "DNA_node_types.h"

namespace blender::compositor {

/**
 * \brief CornerPinNode
 * \ingroup Node
 */
class CornerPinNode : public Node {
 public:
  CornerPinNode(bNode *editor_node);
  void convert_to_operations(NodeConverter &converter,
                             const CompositorContext &context) const override;
};

}  // namespace blender::compositor
