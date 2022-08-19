/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node.h"

#include "NOD_geometry.h"

#include "NOD_common.h"
#include "node_common.h"
#include "node_geometry_util.hh"

void register_node_type_geo_group()
{
  static bNodeType ntype;

  node_type_base_custom(&ntype, "GeometryNodeGroup", "Group", NODE_CLASS_GROUP);
  ntype.type = NODE_GROUP;
  ntype.poll = geo_node_poll_default;
  ntype.poll_instance = node_group_poll_instance;
  ntype.insert_link = node_insert_link_default;
  ntype.rna_ext.srna = RNA_struct_find("GeometryNodeGroup");
  BLI_assert(ntype.rna_ext.srna != nullptr);
  RNA_struct_blender_type_set(ntype.rna_ext.srna, &ntype);

  node_type_size(&ntype, 140, 60, 400);
  ntype.labelfunc = node_group_label;
  node_type_group_update(&ntype, node_group_update);

  nodeRegisterType(&ntype);
}

void register_node_type_geo_custom_group(bNodeType *ntype)
{
  /* These methods can be overridden but need a default implementation otherwise. */
  if (ntype->poll == nullptr) {
    ntype->poll = geo_node_poll_default;
  }
  if (ntype->insert_link == nullptr) {
    ntype->insert_link = node_insert_link_default;
  }
}
