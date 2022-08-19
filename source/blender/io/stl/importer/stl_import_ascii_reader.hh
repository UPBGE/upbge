/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup stl
 */

#pragma once

#include <cstdio>

#include "BKE_mesh.h"

#include "stl_import.hh"

/**
 * ASCII STL spec:
 * <pre>
 * solid name
 *   facet normal ni nj nk
 *     outer loop
 *       vertex v1x v1y v1z
 *       vertex v2x v2y v2z
 *       vertex v3x v3y v3z
 *     endloop
 *   endfacet
 *   ...
 * endsolid name
 * </pre>
 */

namespace blender::io::stl {

Mesh *read_stl_ascii(const char *filepath, Main *bmain, char *mesh_name, bool use_custom_normals);

}  // namespace blender::io::stl
