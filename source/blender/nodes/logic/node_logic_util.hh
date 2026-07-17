/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "NOD_logic.hh"
#include "NOD_register.hh"             // IWYU pragma: export
#include "NOD_socket_declarations.hh"  // IWYU pragma: export

#include "node_logic_socket_declarations.hh"  // IWYU pragma: export

#include "RNA_access.hh"  // IWYU pragma: export

namespace blender {

bool logic_node_poll_default(const bke::bNodeType *ntype,
                             const bNodeTree *ntree,
                             const char **r_disabled_hint);
void logic_node_type_base(bke::bNodeType *ntype,
                          UString idname);

namespace nodes::logic {

void add_input_button_status_outputs(NodeDeclarationBuilder &b);

}  // namespace nodes::logic

}  // namespace blender
