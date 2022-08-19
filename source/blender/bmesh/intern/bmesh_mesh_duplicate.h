/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bmesh
 */

/**
 * Geometry must be completely isolated.
 */
void BM_mesh_copy_arrays(BMesh *bm_src,
                         BMesh *bm_dst,
                         BMVert **verts_src,
                         uint verts_src_len,
                         BMEdge **edges_src,
                         uint edges_src_len,
                         BMFace **faces_src,
                         uint faces_src_len);
