/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string.h>

#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"

#include "BKE_node.h"

#include "BLT_translation.h"

#include "NOD_function.h"
#include "NOD_multi_function.hh"
#include "NOD_socket_declarations.hh"

#include "node_util.h"

#include "FN_multi_function_builder.hh"

void fn_node_type_base(struct bNodeType *ntype, int type, const char *name, short nclass);
