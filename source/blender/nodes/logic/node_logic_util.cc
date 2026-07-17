/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BLT_translation.hh"

#include "NOD_socket_search_link.hh"

#include "node_util.hh"

namespace blender {

namespace nodes::logic {

Span<NodeMetadata> node_metadata()
{
  return logic_node_metadata();
}

const NodeMetadata *node_metadata(StringRef idname)
{
  return find_logic_node_metadata(idname);
}

void add_input_button_status_outputs(NodeDeclarationBuilder &b)
{
  namespace decl = blender::nodes::logic::decl;
  b.add_output<decl::Execution>("If Pressed"_ustr, "Out"_ustr);
  b.add_output<decl::Condition>("Active"_ustr);
}

}  // namespace nodes::logic

bool logic_node_poll_default(const bke::bNodeType * /*ntype*/,
                             const bNodeTree *ntree,
                             const char **r_disabled_hint)
{
  if (ntree == nullptr || ntree->type != NTREE_LOGIC) {
    *r_disabled_hint = RPT_("Not a logic node tree");
    return false;
  }
  return true;
}

void logic_node_type_base(bke::bNodeType *ntype, UString idname)
{
  bke::node_type_base(*ntype, idname);

  if (const nodes::logic::NodeMetadata *metadata = nodes::logic::node_metadata(idname.ref())) {
    ntype->ui_name = metadata->ui_name;
    ntype->ui_description = metadata->ui_description;
  }

  ntype->poll = logic_node_poll_default;
  ntype->insert_link = node_insert_link_default;
  ntype->gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

}  // namespace blender
