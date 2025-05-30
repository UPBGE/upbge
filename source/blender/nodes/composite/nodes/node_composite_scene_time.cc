/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup cmpnodes
 */

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes {

static void cmp_node_scene_time_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Seconds");
  b.add_output<decl::Float>("Frame");
}

using namespace blender::compositor;

class SceneTimeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    execute_seconds();
    execute_frame();
  }

  void execute_seconds()
  {
    Result &result = get_result("Seconds");
    if (!result.should_compute()) {
      return;
    }

    result.allocate_single_value();
    result.set_single_value(context().get_time());
  }

  void execute_frame()
  {
    Result &result = get_result("Frame");
    if (!result.should_compute()) {
      return;
    }

    result.allocate_single_value();
    result.set_single_value(float(context().get_frame_number()));
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new SceneTimeOperation(context, node);
}

}  // namespace blender::nodes

static void register_node_type_cmp_scene_time()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeSceneTime", CMP_NODE_SCENE_TIME);
  ntype.ui_name = "Scene Time";
  ntype.ui_description = "Input the current scene time in seconds or frames";
  ntype.enum_name_legacy = "SCENE_TIME";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = blender::nodes::cmp_node_scene_time_declare;
  ntype.get_compositor_operation = blender::nodes::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_scene_time)
