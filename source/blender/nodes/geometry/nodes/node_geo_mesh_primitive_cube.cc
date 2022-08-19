/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_material.h"
#include "BKE_mesh.h"

#include "GEO_mesh_primitive_cuboid.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_primitive_cube_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(N_("Size"))
      .default_value(float3(1))
      .min(0.0f)
      .subtype(PROP_TRANSLATION)
      .description(N_("Side length along each axis"));
  b.add_input<decl::Int>(N_("Vertices X"))
      .default_value(2)
      .min(2)
      .max(1000)
      .description(N_("Number of vertices for the X side of the shape"));
  b.add_input<decl::Int>(N_("Vertices Y"))
      .default_value(2)
      .min(2)
      .max(1000)
      .description(N_("Number of vertices for the Y side of the shape"));
  b.add_input<decl::Int>(N_("Vertices Z"))
      .default_value(2)
      .min(2)
      .max(1000)
      .description(N_("Number of vertices for the Z side of the shape"));
  b.add_output<decl::Geometry>(N_("Mesh"));
}

static Mesh *create_cuboid_mesh(const float3 &size,
                                const int verts_x,
                                const int verts_y,
                                const int verts_z)
{
  Mesh *mesh = geometry::create_cuboid_mesh(size, verts_x, verts_y, verts_z, "uv_map");
  BKE_id_material_eval_ensure_default_slot(&mesh->id);
  return mesh;
}

static Mesh *create_cube_mesh(const float3 size,
                              const int verts_x,
                              const int verts_y,
                              const int verts_z)
{
  const int dimensions = (verts_x - 1 > 0) + (verts_y - 1 > 0) + (verts_z - 1 > 0);
  if (dimensions == 0) {
    return create_line_mesh(float3(0), float3(0), 1);
  }
  if (dimensions == 1) {
    float3 start;
    float3 delta;
    if (verts_x > 1) {
      start = {-size.x / 2.0f, 0, 0};
      delta = {size.x / (verts_x - 1), 0, 0};
    }
    else if (verts_y > 1) {
      start = {0, -size.y / 2.0f, 0};
      delta = {0, size.y / (verts_y - 1), 0};
    }
    else {
      start = {0, 0, -size.z / 2.0f};
      delta = {0, 0, size.z / (verts_z - 1)};
    }

    return create_line_mesh(start, delta, verts_x * verts_y * verts_z);
  }
  if (dimensions == 2) {
    if (verts_z == 1) { /* XY plane. */
      return create_grid_mesh(verts_x, verts_y, size.x, size.y);
    }
    if (verts_y == 1) { /* XZ plane. */
      Mesh *mesh = create_grid_mesh(verts_x, verts_z, size.x, size.z);
      transform_mesh(*mesh, float3(0), float3(M_PI_2, 0.0f, 0.0f), float3(1));
      return mesh;
    }
    /* YZ plane. */
    Mesh *mesh = create_grid_mesh(verts_z, verts_y, size.z, size.y);
    transform_mesh(*mesh, float3(0), float3(0.0f, M_PI_2, 0.0f), float3(1));
    return mesh;
  }

  return create_cuboid_mesh(size, verts_x, verts_y, verts_z);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const float3 size = params.extract_input<float3>("Size");
  const int verts_x = params.extract_input<int>("Vertices X");
  const int verts_y = params.extract_input<int>("Vertices Y");
  const int verts_z = params.extract_input<int>("Vertices Z");
  if (verts_x < 1 || verts_y < 1 || verts_z < 1) {
    params.error_message_add(NodeWarningType::Info, TIP_("Vertices must be at least 1"));
    params.set_default_remaining_outputs();
    return;
  }

  Mesh *mesh = create_cube_mesh(size, verts_x, verts_y, verts_z);

  params.set_output("Mesh", GeometrySet::create_with_mesh(mesh));
}

}  // namespace blender::nodes::node_geo_mesh_primitive_cube_cc

void register_node_type_geo_mesh_primitive_cube()
{
  namespace file_ns = blender::nodes::node_geo_mesh_primitive_cube_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_MESH_PRIMITIVE_CUBE, "Cube", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
