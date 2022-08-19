/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. */

#include "COM_SunBeamsNode.h"
#include "COM_SunBeamsOperation.h"

namespace blender::compositor {

SunBeamsNode::SunBeamsNode(bNode *editor_node) : Node(editor_node)
{
  /* pass */
}

void SunBeamsNode::convert_to_operations(NodeConverter &converter,
                                         const CompositorContext & /*context*/) const
{
  NodeInput *input_socket = this->get_input_socket(0);
  NodeOutput *output_socket = this->get_output_socket(0);
  NodeSunBeams *data = (NodeSunBeams *)get_bnode()->storage;

  SunBeamsOperation *operation = new SunBeamsOperation();
  operation->set_data(*data);
  converter.add_operation(operation);

  converter.map_input_socket(input_socket, operation->get_input_socket(0));
  converter.map_output_socket(output_socket, operation->get_output_socket(0));
}

}  // namespace blender::compositor
