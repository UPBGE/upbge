/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_geometry_set.hh"
#include "BKE_mesh.h"

#include "GEO_mesh_primitive_cuboid.hh"

namespace blender::geometry {

struct CuboidConfig {
  float3 size;
  int verts_x;
  int verts_y;
  int verts_z;
  int edges_x;
  int edges_y;
  int edges_z;
  int vertex_count;
  int poly_count;
  int loop_count;

  CuboidConfig(float3 size, int verts_x, int verts_y, int verts_z)
      : size(size),
        verts_x(verts_x),
        verts_y(verts_y),
        verts_z(verts_z),
        edges_x(verts_x - 1),
        edges_y(verts_y - 1),
        edges_z(verts_z - 1)
  {
    BLI_assert(edges_x > 0 && edges_y > 0 && edges_z > 0);
    this->vertex_count = this->get_vertex_count();
    this->poly_count = this->get_poly_count();
    this->loop_count = this->poly_count * 4;
  }

 private:
  int get_vertex_count()
  {
    const int inner_position_count = (verts_x - 2) * (verts_y - 2) * (verts_z - 2);
    return verts_x * verts_y * verts_z - inner_position_count;
  }

  int get_poly_count()
  {
    return 2 * (edges_x * edges_y + edges_y * edges_z + edges_z * edges_x);
  }
};

static void calculate_vertices(const CuboidConfig &config, MutableSpan<MVert> verts)
{
  const float z_bottom = -config.size.z / 2.0f;
  const float z_delta = config.size.z / config.edges_z;

  const float x_left = -config.size.x / 2.0f;
  const float x_delta = config.size.x / config.edges_x;

  const float y_front = -config.size.y / 2.0f;
  const float y_delta = config.size.y / config.edges_y;

  int vert_index = 0;

  for (const int z : IndexRange(config.verts_z)) {
    if (ELEM(z, 0, config.edges_z)) {
      /* Fill bottom and top. */
      const float z_pos = z_bottom + z_delta * z;
      for (const int y : IndexRange(config.verts_y)) {
        const float y_pos = y_front + y_delta * y;
        for (const int x : IndexRange(config.verts_x)) {
          const float x_pos = x_left + x_delta * x;
          copy_v3_v3(verts[vert_index++].co, float3(x_pos, y_pos, z_pos));
        }
      }
    }
    else {
      for (const int y : IndexRange(config.verts_y)) {
        if (ELEM(y, 0, config.edges_y)) {
          /* Fill y-sides. */
          const float y_pos = y_front + y_delta * y;
          const float z_pos = z_bottom + z_delta * z;
          for (const int x : IndexRange(config.verts_x)) {
            const float x_pos = x_left + x_delta * x;
            copy_v3_v3(verts[vert_index++].co, float3(x_pos, y_pos, z_pos));
          }
        }
        else {
          /* Fill x-sides. */
          const float x_pos = x_left;
          const float y_pos = y_front + y_delta * y;
          const float z_pos = z_bottom + z_delta * z;
          copy_v3_v3(verts[vert_index++].co, float3(x_pos, y_pos, z_pos));
          const float x_pos2 = x_left + x_delta * config.edges_x;
          copy_v3_v3(verts[vert_index++].co, float3(x_pos2, y_pos, z_pos));
        }
      }
    }
  }
}

/* vert_1 = bottom left, vert_2 = bottom right, vert_3 = top right, vert_4 = top left.
 * Hence they are passed as 1,4,3,2 when calculating polys clockwise, and 1,2,3,4 for
 * anti-clockwise.
 */
static void define_quad(MutableSpan<MPoly> polys,
                        MutableSpan<MLoop> loops,
                        const int poly_index,
                        const int loop_index,
                        const int vert_1,
                        const int vert_2,
                        const int vert_3,
                        const int vert_4)
{
  MPoly &poly = polys[poly_index];
  poly.loopstart = loop_index;
  poly.totloop = 4;

  MLoop &loop_1 = loops[loop_index];
  loop_1.v = vert_1;
  MLoop &loop_2 = loops[loop_index + 1];
  loop_2.v = vert_2;
  MLoop &loop_3 = loops[loop_index + 2];
  loop_3.v = vert_3;
  MLoop &loop_4 = loops[loop_index + 3];
  loop_4.v = vert_4;
}

static void calculate_polys(const CuboidConfig &config,
                            MutableSpan<MPoly> polys,
                            MutableSpan<MLoop> loops)
{
  int loop_index = 0;
  int poly_index = 0;

  /* Number of vertices in an XY cross-section of the cube (barring top and bottom faces). */
  const int xy_cross_section_vert_count = config.verts_x * config.verts_y -
                                          (config.verts_x - 2) * (config.verts_y - 2);

  /* Calculate polys for Bottom faces. */
  int vert_1_start = 0;

  for ([[maybe_unused]] const int y : IndexRange(config.edges_y)) {
    for (const int x : IndexRange(config.edges_x)) {
      const int vert_1 = vert_1_start + x;
      const int vert_2 = vert_1_start + config.verts_x + x;
      const int vert_3 = vert_2 + 1;
      const int vert_4 = vert_1 + 1;

      define_quad(polys, loops, poly_index, loop_index, vert_1, vert_2, vert_3, vert_4);
      loop_index += 4;
      poly_index++;
    }
    vert_1_start += config.verts_x;
  }

  /* Calculate polys for Front faces. */
  vert_1_start = 0;
  int vert_2_start = config.verts_x * config.verts_y;

  for ([[maybe_unused]] const int z : IndexRange(config.edges_z)) {
    for (const int x : IndexRange(config.edges_x)) {
      define_quad(polys,
                  loops,
                  poly_index,
                  loop_index,
                  vert_1_start + x,
                  vert_1_start + x + 1,
                  vert_2_start + x + 1,
                  vert_2_start + x);
      loop_index += 4;
      poly_index++;
    }
    vert_1_start = vert_2_start;
    vert_2_start += config.verts_x * config.verts_y - (config.verts_x - 2) * (config.verts_y - 2);
  }

  /* Calculate polys for Top faces. */
  vert_1_start = config.verts_x * config.verts_y +
                 (config.verts_z - 2) * (config.verts_x * config.verts_y -
                                         (config.verts_x - 2) * (config.verts_y - 2));
  vert_2_start = vert_1_start + config.verts_x;

  for ([[maybe_unused]] const int y : IndexRange(config.edges_y)) {
    for (const int x : IndexRange(config.edges_x)) {
      define_quad(polys,
                  loops,
                  poly_index,
                  loop_index,
                  vert_1_start + x,
                  vert_1_start + x + 1,
                  vert_2_start + x + 1,
                  vert_2_start + x);
      loop_index += 4;
      poly_index++;
    }
    vert_2_start += config.verts_x;
    vert_1_start += config.verts_x;
  }

  /* Calculate polys for Back faces. */
  vert_1_start = config.verts_x * config.edges_y;
  vert_2_start = vert_1_start + xy_cross_section_vert_count;

  for (const int z : IndexRange(config.edges_z)) {
    if (z == (config.edges_z - 1)) {
      vert_2_start += (config.verts_x - 2) * (config.verts_y - 2);
    }
    for (const int x : IndexRange(config.edges_x)) {
      define_quad(polys,
                  loops,
                  poly_index,
                  loop_index,
                  vert_1_start + x,
                  vert_2_start + x,
                  vert_2_start + x + 1,
                  vert_1_start + x + 1);
      loop_index += 4;
      poly_index++;
    }
    vert_2_start += xy_cross_section_vert_count;
    vert_1_start += xy_cross_section_vert_count;
  }

  /* Calculate polys for Left faces. */
  vert_1_start = 0;
  vert_2_start = config.verts_x * config.verts_y;

  for (const int z : IndexRange(config.edges_z)) {
    for (const int y : IndexRange(config.edges_y)) {
      int vert_1;
      int vert_2;
      int vert_3;
      int vert_4;

      if (z == 0 || y == 0) {
        vert_1 = vert_1_start + config.verts_x * y;
        vert_4 = vert_1 + config.verts_x;
      }
      else {
        vert_1 = vert_1_start + 2 * y;
        vert_1 += config.verts_x - 2;
        vert_4 = vert_1 + 2;
      }

      if (y == 0 || z == (config.edges_z - 1)) {
        vert_2 = vert_2_start + config.verts_x * y;
        vert_3 = vert_2 + config.verts_x;
      }
      else {
        vert_2 = vert_2_start + 2 * y;
        vert_2 += config.verts_x - 2;
        vert_3 = vert_2 + 2;
      }

      define_quad(polys, loops, poly_index, loop_index, vert_1, vert_2, vert_3, vert_4);
      loop_index += 4;
      poly_index++;
    }
    if (z == 0) {
      vert_1_start += config.verts_x * config.verts_y;
    }
    else {
      vert_1_start += xy_cross_section_vert_count;
    }
    vert_2_start += xy_cross_section_vert_count;
  }

  /* Calculate polys for Right faces. */
  vert_1_start = config.edges_x;
  vert_2_start = vert_1_start + config.verts_x * config.verts_y;

  for (const int z : IndexRange(config.edges_z)) {
    for (const int y : IndexRange(config.edges_y)) {
      int vert_1 = vert_1_start;
      int vert_2 = vert_2_start;
      int vert_3 = vert_2_start + 2;
      int vert_4 = vert_1 + config.verts_x;

      if (z == 0) {
        vert_1 = vert_1_start + config.verts_x * y;
        vert_4 = vert_1 + config.verts_x;
      }
      else {
        vert_1 = vert_1_start + 2 * y;
        vert_4 = vert_1 + 2;
      }

      if (z == (config.edges_z - 1)) {
        vert_2 = vert_2_start + config.verts_x * y;
        vert_3 = vert_2 + config.verts_x;
      }
      else {
        vert_2 = vert_2_start + 2 * y;
        vert_3 = vert_2 + 2;
      }

      if (y == (config.edges_y - 1)) {
        vert_3 = vert_2 + config.verts_x;
        vert_4 = vert_1 + config.verts_x;
      }

      define_quad(polys, loops, poly_index, loop_index, vert_1, vert_4, vert_3, vert_2);
      loop_index += 4;
      poly_index++;
    }
    if (z == 0) {
      vert_1_start += config.verts_x * config.verts_y;
    }
    else {
      vert_1_start += xy_cross_section_vert_count;
    }
    vert_2_start += xy_cross_section_vert_count;
  }
}

static void calculate_uvs(const CuboidConfig &config, Mesh *mesh, const bke::AttributeIDRef &uv_id)
{
  bke::MutableAttributeAccessor attributes = bke::mesh_attributes_for_write(*mesh);
  bke::SpanAttributeWriter<float2> uv_attribute =
      attributes.lookup_or_add_for_write_only_span<float2>(uv_id, ATTR_DOMAIN_CORNER);
  MutableSpan<float2> uvs = uv_attribute.span;

  int loop_index = 0;

  const float x_delta = 0.25f / static_cast<float>(config.edges_x);
  const float y_delta = 0.25f / static_cast<float>(config.edges_y);
  const float z_delta = 0.25f / static_cast<float>(config.edges_z);

  /* Calculate bottom face UVs. */
  for (const int y : IndexRange(config.edges_y)) {
    for (const int x : IndexRange(config.edges_x)) {
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.375f - y * y_delta);
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.375f - (y + 1) * y_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.375f - (y + 1) * y_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.375f - y * y_delta);
    }
  }

  /* Calculate front face UVs. */
  for (const int z : IndexRange(config.edges_z)) {
    for (const int x : IndexRange(config.edges_x)) {
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.375f + (z + 1) * z_delta);
    }
  }

  /* Calculate top face UVs. */
  for (const int y : IndexRange(config.edges_y)) {
    for (const int x : IndexRange(config.edges_x)) {
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.625f + y * y_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.625f + y * y_delta);
      uvs[loop_index++] = float2(0.25f + (x + 1) * x_delta, 0.625f + (y + 1) * y_delta);
      uvs[loop_index++] = float2(0.25f + x * x_delta, 0.625f + (y + 1) * y_delta);
    }
  }

  /* Calculate back face UVs. */
  for (const int z : IndexRange(config.edges_z)) {
    for (const int x : IndexRange(config.edges_x)) {
      uvs[loop_index++] = float2(1.0f - x * x_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(1.0f - x * x_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(1.0f - (x + 1) * x_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(1.0f - (x + 1) * x_delta, 0.375f + z * z_delta);
    }
  }

  /* Calculate left face UVs. */
  for (const int z : IndexRange(config.edges_z)) {
    for (const int y : IndexRange(config.edges_y)) {
      uvs[loop_index++] = float2(0.25f - y * y_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(0.25f - y * y_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(0.25f - (y + 1) * y_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(0.25f - (y + 1) * y_delta, 0.375f + z * z_delta);
    }
  }

  /* Calculate right face UVs. */
  for (const int z : IndexRange(config.edges_z)) {
    for (const int y : IndexRange(config.edges_y)) {
      uvs[loop_index++] = float2(0.50f + y * y_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(0.50f + (y + 1) * y_delta, 0.375f + z * z_delta);
      uvs[loop_index++] = float2(0.50f + (y + 1) * y_delta, 0.375f + (z + 1) * z_delta);
      uvs[loop_index++] = float2(0.50f + y * y_delta, 0.375f + (z + 1) * z_delta);
    }
  }

  uv_attribute.finish();
}

Mesh *create_cuboid_mesh(const float3 &size,
                         const int verts_x,
                         const int verts_y,
                         const int verts_z,
                         const bke::AttributeIDRef &uv_id)
{
  const CuboidConfig config(size, verts_x, verts_y, verts_z);

  Mesh *mesh = BKE_mesh_new_nomain(
      config.vertex_count, 0, 0, config.loop_count, config.poly_count);

  calculate_vertices(config, {mesh->mvert, mesh->totvert});

  calculate_polys(config, {mesh->mpoly, mesh->totpoly}, {mesh->mloop, mesh->totloop});
  BKE_mesh_calc_edges(mesh, false, false);

  if (uv_id) {
    calculate_uvs(config, mesh, uv_id);
  }

  return mesh;
}

Mesh *create_cuboid_mesh(const float3 &size,
                         const int verts_x,
                         const int verts_y,
                         const int verts_z)
{
  return create_cuboid_mesh(size, verts_x, verts_y, verts_z, {});
}

}  // namespace blender::geometry
