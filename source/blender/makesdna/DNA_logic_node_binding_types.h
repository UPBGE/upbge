/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file DNA_logic_node_binding_types.h
 *  \ingroup DNA
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {

/** Native Logic Nodes tree binding on a game object (see #Object.logic_node_bindings). */
typedef struct LogicNodeBinding {
  struct LogicNodeBinding *next, *prev;
  /** LogicNodeTree ID name without the two-char prefix. */
  char tree_name[64];
  short enabled;
  short _pad;
  int _pad2;
} LogicNodeBinding;

}  // namespace blender
