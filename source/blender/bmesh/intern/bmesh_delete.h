/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bmesh
 */

void BMO_mesh_delete_oflag_tagged(BMesh *bm, short oflag, char htype);
void BM_mesh_delete_hflag_tagged(BMesh *bm, char hflag, char htype);

/**
 * \warning oflag applies to different types in some contexts,
 * not just the type being removed.
 */
void BMO_mesh_delete_oflag_context(BMesh *bm, short oflag, int type);
/**
 * \warning oflag applies to different types in some contexts,
 * not just the type being removed.
 */
void BM_mesh_delete_hflag_context(BMesh *bm, char hflag, int type);
