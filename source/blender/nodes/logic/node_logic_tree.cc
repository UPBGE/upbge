/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "NOD_logic.hh"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"

#include "CLG_log.h"

#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "node_common.h"

namespace blender {

static CLG_LogRef LOG = {"node.logic"};

bke::bNodeTreeType *ntreeType_Logic;

static void logic_node_tree_get_from_context(const bContext *C,
                                             bke::bNodeTreeType * /*treetype*/,
                                             bNodeTree **r_ntree,
                                             ID ** /*r_id*/,
                                             ID ** /*r_from*/)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return;
  }

  if (snode->selected_node_group != nullptr && snode->selected_node_group->type == NTREE_LOGIC) {
    *r_ntree = snode->selected_node_group;
    return;
  }

  if (snode->nodetree != nullptr && snode->nodetree->type == NTREE_LOGIC) {
    *r_ntree = snode->nodetree;
  }
}

/**
 * Python addon nodes use `LogicNode*` idnames on `BGELogicTree`. Native trees only allow
 * `LogicNative*` (plus layout nodes). Strip addon nodes if they appear in a native tree.
 */
static bool is_python_addon_logic_node_id(const char *idname)
{
  return idname && idname[0] && STRPREFIX(idname, "LogicNode") &&
         !STRPREFIX(idname, "LogicNative");
}

static void logic_node_tree_remove_python_addon_nodes(bNodeTree *ntree)
{
  Vector<bNode *> nodes_to_remove;
  for (bNode *node = static_cast<bNode *>(ntree->nodes.first); node != nullptr;
       node = node->next)
  {
    if (is_python_addon_logic_node_id(node->idname)) {
      nodes_to_remove.append(node);
    }
  }
  if (!nodes_to_remove.is_empty()) {
    CLOG_WARN(&LOG,
              "Removed %d Python addon logic node(s) from native LogicNodeTree '%s' "
              "(only LogicNative* nodes are allowed)",
              int(nodes_to_remove.size()),
              ntree->id.name + 2);
  }
  for (bNode *node : nodes_to_remove) {
    bke::node_remove_node(nullptr, *ntree, *node, false);
  }
}

static void logic_node_tree_update(bNodeTree *ntree)
{
  ntree_update_reroute_nodes(ntree);
  logic_node_tree_remove_python_addon_nodes(ntree);
}

static void foreach_nodeclass(void *calldata, bke::bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Events and Values"));
  func(calldata, NODE_CLASS_CONVERTER, N_("Flow and Math"));
  func(calldata, NODE_CLASS_OP_VECTOR, N_("Transform"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static bool logic_node_tree_validate_link(eNodeSocketDatatype type_a, eNodeSocketDatatype type_b)
{
  if (type_a == SOCK_CUSTOM || type_b == SOCK_CUSTOM) {
    return true;
  }
  return type_a == type_b;
}

static bool logic_node_tree_socket_type_valid(bke::bNodeTreeType * /*treetype*/,
                                              bke::bNodeSocketType *socket_type)
{
  return bke::node_is_static_socket_type(*socket_type) &&
         ELEM(socket_type->type,
              SOCK_CUSTOM,
              SOCK_BOOLEAN,
              SOCK_INT,
              SOCK_FLOAT,
              SOCK_VECTOR,
              SOCK_STRING,
              SOCK_ROTATION,
              SOCK_RGBA,
              SOCK_OBJECT,
              SOCK_IMAGE,
              SOCK_COLLECTION,
              SOCK_MATERIAL,
              SOCK_FONT,
              SOCK_SCENE,
              SOCK_TEXT_ID,
              SOCK_SOUND);
}

void register_node_tree_type_logic()
{
  bke::bNodeTreeType *tt = ntreeType_Logic = MEM_new<bke::bNodeTreeType>(__func__);
  tt->type = NTREE_LOGIC;
  tt->idname = "LogicNodeTree"_ustr;
  tt->ui_name = N_("Logic Node Editor");
  tt->ui_icon = ICON_NODETREE;
  tt->ui_description = N_("Edit UPBGE game logic using native C++ nodes (LogicNative only)");
  tt->rna_ext.srna = RNA_LogicNodeTree;
  tt->update = logic_node_tree_update;
  tt->get_from_context = logic_node_tree_get_from_context;
  tt->foreach_nodeclass = foreach_nodeclass;
  tt->valid_socket_type = logic_node_tree_socket_type_valid;
  tt->validate_link = logic_node_tree_validate_link;
  tt->no_group_interface = true;

  bke::node_tree_type_add(*tt);
}

}  // namespace blender
