/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. */

#include "COM_CornerPinNode.h"

#include "COM_PlaneCornerPinOperation.h"

namespace blender::compositor {

CornerPinNode::CornerPinNode(bNode *editor_node) : Node(editor_node)
{
}

void CornerPinNode::convert_to_operations(NodeConverter &converter,
                                          const CompositorContext & /*context*/) const
{
  NodeInput *input_image = this->get_input_socket(0);
  /* NOTE: socket order differs between UI node and operations:
   * bNode uses intuitive order following top-down layout:
   *   upper-left, upper-right, lower-left, lower-right
   * Operations use same order as the tracking blenkernel functions expect:
   *   lower-left, lower-right, upper-right, upper-left
   */
  const int node_corner_index[4] = {3, 4, 2, 1};

  NodeOutput *output_warped_image = this->get_output_socket(0);
  NodeOutput *output_plane = this->get_output_socket(1);

  PlaneCornerPinWarpImageOperation *warp_image_operation = new PlaneCornerPinWarpImageOperation();
  converter.add_operation(warp_image_operation);
  PlaneCornerPinMaskOperation *plane_mask_operation = new PlaneCornerPinMaskOperation();
  converter.add_operation(plane_mask_operation);

  converter.map_input_socket(input_image, warp_image_operation->get_input_socket(0));
  for (int i = 0; i < 4; i++) {
    NodeInput *corner_input = get_input_socket(node_corner_index[i]);
    converter.map_input_socket(corner_input, warp_image_operation->get_input_socket(i + 1));
    converter.map_input_socket(corner_input, plane_mask_operation->get_input_socket(i));
  }
  converter.map_output_socket(output_warped_image, warp_image_operation->get_output_socket());
  converter.map_output_socket(output_plane, plane_mask_operation->get_output_socket());
}

}  // namespace blender::compositor
