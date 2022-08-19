/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bmesh
 */

/**
 * \param use_snap_center: Snap verts onto the plane.
 * \param use_tag: Only bisect tagged edges and faces.
 * \param oflag_center: Operator flag, enabled for geometry on the axis (existing and created)
 */
void BM_mesh_bisect_plane(BMesh *bm,
                          const float plane[4],
                          bool use_snap_center,
                          bool use_tag,
                          short oflag_center,
                          short oflag_new,
                          float eps);
