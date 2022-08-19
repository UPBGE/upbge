/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Triangulate.
 */

#pragma once

void BM_mesh_triangulate(BMesh *bm,
                         int quad_method,
                         int ngon_method,
                         int min_vertices,
                         bool tag_only,
                         BMOperator *op,
                         BMOpSlot *slot_facemap_out,
                         BMOpSlot *slot_facemap_double_out);
