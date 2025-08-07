/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup stl
 */

#pragma once

#include <cstdio>

struct Mesh;

/* Binary STL spec.:
 * - UINT8[80]    - Header                  - 80 bytes
 * - UINT32       - Number of triangles     - 4 bytes
 *   For each triangle                      - 50 bytes:
 *   - REAL32[3]   - Normal vector          - 12 bytes
 *   - REAL32[3]   - Vertex 1               - 12 bytes
 *   - REAL32[3]   - Vertex 2               - 12 bytes
 *   - REAL32[3]   - Vertex 3               - 12 bytes
 *   - UINT16      - Attribute byte count   -  2 bytes
 */

namespace blender::io::stl {

Mesh *read_stl_binary(FILE *file, bool use_custom_normals);

}  // namespace blender::io::stl
