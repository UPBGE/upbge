/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bmesh
 */

#include "BLI_compiler_attrs.h"

#include "bmesh.h"

#ifndef NDEBUG
char *BM_mesh_debug_info(BMesh *bm) ATTR_NONNULL(1) ATTR_MALLOC ATTR_WARN_UNUSED_RESULT;
void BM_mesh_debug_print(BMesh *bm) ATTR_NONNULL(1);
#endif /* NDEBUG */
