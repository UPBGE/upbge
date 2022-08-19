/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BKE_context.h"
#include "BKE_lib_id.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

/* **************** SWITCH VIEW ******************** */

namespace blender::nodes::node_composite_switchview_cc {

static bNodeSocketTemplate cmp_node_switch_view_out[] = {
    {SOCK_RGBA, N_("Image"), 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    {-1, ""},
};

static bNodeSocket *ntreeCompositSwitchViewAddSocket(bNodeTree *ntree,
                                                     bNode *node,
                                                     const char *name)
{
  bNodeSocket *sock = nodeAddStaticSocket(
      ntree, node, SOCK_IN, SOCK_RGBA, PROP_NONE, nullptr, name);
  return sock;
}

static void cmp_node_switch_view_sanitycheck(bNodeTree *ntree, bNode *node)
{
  if (!BLI_listbase_is_empty(&node->inputs)) {
    return;
  }

  bNodeSocket *sock = ntreeCompositSwitchViewAddSocket(ntree, node, "No View");
  sock->flag |= SOCK_HIDDEN;
}

static void cmp_node_switch_view_update(bNodeTree *ntree, bNode *node)
{
  Scene *scene = (Scene *)node->id;

  /* only update when called from the operator button */
  if (node->update != NODE_UPDATE_OPERATOR) {
    return;
  }

  if (scene == nullptr) {
    nodeRemoveAllSockets(ntree, node);
    /* make sure there is always one socket */
    cmp_node_switch_view_sanitycheck(ntree, node);
    return;
  }

  /* remove the views that were removed */
  bNodeSocket *sock = (bNodeSocket *)node->inputs.last;
  while (sock) {
    SceneRenderView *srv = (SceneRenderView *)BLI_findstring(
        &scene->r.views, sock->name, offsetof(SceneRenderView, name));

    if (srv == nullptr) {
      bNodeSocket *sock_del = sock;
      sock = sock->prev;
      nodeRemoveSocket(ntree, node, sock_del);
    }
    else {
      if (srv->viewflag & SCE_VIEW_DISABLE) {
        sock->flag |= SOCK_HIDDEN;
      }
      else {
        sock->flag &= ~SOCK_HIDDEN;
      }

      sock = sock->prev;
    }
  }

  /* add the new views */
  LISTBASE_FOREACH (SceneRenderView *, srv, &scene->r.views) {
    sock = (bNodeSocket *)BLI_findstring(&node->inputs, srv->name, offsetof(bNodeSocket, name));

    if (sock == nullptr) {
      sock = ntreeCompositSwitchViewAddSocket(ntree, node, srv->name);
    }

    if (srv->viewflag & SCE_VIEW_DISABLE) {
      sock->flag |= SOCK_HIDDEN;
    }
    else {
      sock->flag &= ~SOCK_HIDDEN;
    }
  }

  /* make sure there is always one socket */
  cmp_node_switch_view_sanitycheck(ntree, node);
}

static void init_switch_view(const bContext *C, PointerRNA *ptr)
{
  Scene *scene = CTX_data_scene(C);
  bNodeTree *ntree = (bNodeTree *)ptr->owner_id;
  bNode *node = (bNode *)ptr->data;

  /* store scene for updates */
  node->id = (ID *)scene;
  id_us_plus(node->id);

  if (scene) {
    RenderData *rd = &scene->r;

    LISTBASE_FOREACH (SceneRenderView *, srv, &rd->views) {
      bNodeSocket *sock = ntreeCompositSwitchViewAddSocket(ntree, node, srv->name);

      if (srv->viewflag & SCE_VIEW_DISABLE) {
        sock->flag |= SOCK_HIDDEN;
      }
    }
  }

  /* make sure there is always one socket */
  cmp_node_switch_view_sanitycheck(ntree, node);
}

static void node_composit_buts_switch_view_ex(uiLayout *layout,
                                              bContext *UNUSED(C),
                                              PointerRNA *UNUSED(ptr))
{
  uiItemFullO(layout,
              "NODE_OT_switch_view_update",
              "Update Views",
              ICON_FILE_REFRESH,
              nullptr,
              WM_OP_INVOKE_DEFAULT,
              0,
              nullptr);
}

using namespace blender::realtime_compositor;

class SwitchViewOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &input = get_input(context().get_view_name());
    Result &result = get_result("Image");
    input.pass_through(result);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new SwitchViewOperation(context, node);
}

}  // namespace blender::nodes::node_composite_switchview_cc

void register_node_type_cmp_switch_view()
{
  namespace file_ns = blender::nodes::node_composite_switchview_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_SWITCH_VIEW, "Switch View", NODE_CLASS_CONVERTER);
  node_type_socket_templates(&ntype, nullptr, file_ns::cmp_node_switch_view_out);
  ntype.draw_buttons_ex = file_ns::node_composit_buts_switch_view_ex;
  ntype.initfunc_api = file_ns::init_switch_view;
  node_type_update(&ntype, file_ns::cmp_node_switch_view_update);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
