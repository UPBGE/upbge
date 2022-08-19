/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_delaunay_2d.h"
#include "BLI_math_vec_types.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_curves.hh"
#include "BKE_mesh.h"

#include "BLI_task.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_fill_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveFill)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curve")).supported_type(GEO_COMPONENT_TYPE_CURVE);
  b.add_output<decl::Geometry>(N_("Mesh"));
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeGeometryCurveFill *data = MEM_cnew<NodeGeometryCurveFill>(__func__);

  data->mode = GEO_NODE_CURVE_FILL_MODE_TRIANGULATED;
  node->storage = data;
}

static meshintersect::CDT_result<double> do_cdt(const bke::CurvesGeometry &curves,
                                                const CDT_output_type output_type)
{
  meshintersect::CDT_input<double> input;
  input.need_ids = false;
  input.vert.reinitialize(curves.evaluated_points_num());
  input.face.reinitialize(curves.curves_num());

  Span<float3> positions = curves.evaluated_positions();

  for (const int i_curve : curves.curves_range()) {
    const IndexRange points = curves.evaluated_points_for_curve(i_curve);

    for (const int i : points) {
      input.vert[i] = double2(positions[i].x, positions[i].y);
    }

    input.face[i_curve].resize(points.size());
    MutableSpan<int> face_verts = input.face[i_curve];
    for (const int i : face_verts.index_range()) {
      face_verts[i] = points[i];
    }
  }
  meshintersect::CDT_result<double> result = delaunay_2d_calc(input, output_type);
  return result;
}

/* Converts the CDT result into a Mesh. */
static Mesh *cdt_to_mesh(const meshintersect::CDT_result<double> &result)
{
  const int vert_len = result.vert.size();
  const int edge_len = result.edge.size();
  const int poly_len = result.face.size();
  int loop_len = 0;
  for (const Vector<int> &face : result.face) {
    loop_len += face.size();
  }

  Mesh *mesh = BKE_mesh_new_nomain(vert_len, edge_len, 0, loop_len, poly_len);
  MutableSpan<MVert> verts{mesh->mvert, mesh->totvert};
  MutableSpan<MEdge> edges{mesh->medge, mesh->totedge};
  MutableSpan<MLoop> loops{mesh->mloop, mesh->totloop};
  MutableSpan<MPoly> polys{mesh->mpoly, mesh->totpoly};

  for (const int i : IndexRange(result.vert.size())) {
    copy_v3_v3(verts[i].co, float3((float)result.vert[i].x, (float)result.vert[i].y, 0.0f));
  }
  for (const int i : IndexRange(result.edge.size())) {
    edges[i].v1 = result.edge[i].first;
    edges[i].v2 = result.edge[i].second;
    edges[i].flag = ME_EDGEDRAW | ME_EDGERENDER;
  }
  int i_loop = 0;
  for (const int i : IndexRange(result.face.size())) {
    polys[i].loopstart = i_loop;
    polys[i].totloop = result.face[i].size();
    for (const int j : result.face[i].index_range()) {
      loops[i_loop].v = result.face[i][j];
      i_loop++;
    }
  }

  /* The delaunay triangulation doesn't seem to return all of the necessary edges, even in
   * triangulation mode. */
  BKE_mesh_calc_edges(mesh, true, false);
  return mesh;
}

static void curve_fill_calculate(GeometrySet &geometry_set, const GeometryNodeCurveFillMode mode)
{
  if (!geometry_set.has_curves()) {
    return;
  }

  const Curves &curves_id = *geometry_set.get_curves_for_read();
  const bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id.geometry);
  if (curves.curves_num() == 0) {
    geometry_set.replace_curves(nullptr);
    return;
  }

  const CDT_output_type output_type = (mode == GEO_NODE_CURVE_FILL_MODE_NGONS) ?
                                          CDT_CONSTRAINTS_VALID_BMESH_WITH_HOLES :
                                          CDT_INSIDE_WITH_HOLES;

  const meshintersect::CDT_result<double> results = do_cdt(curves, output_type);
  Mesh *mesh = cdt_to_mesh(results);

  geometry_set.replace_mesh(mesh);
  geometry_set.replace_curves(nullptr);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");

  const NodeGeometryCurveFill &storage = node_storage(params.node());
  const GeometryNodeCurveFillMode mode = (GeometryNodeCurveFillMode)storage.mode;

  geometry_set.modify_geometry_sets(
      [&](GeometrySet &geometry_set) { curve_fill_calculate(geometry_set, mode); });

  params.set_output("Mesh", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_curve_fill_cc

void register_node_type_geo_curve_fill()
{
  namespace file_ns = blender::nodes::node_geo_curve_fill_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_FILL_CURVE, "Fill Curve", NODE_CLASS_GEOMETRY);

  node_type_init(&ntype, file_ns::node_init);
  node_type_storage(
      &ntype, "NodeGeometryCurveFill", node_free_standard_storage, node_copy_standard_storage);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  nodeRegisterType(&ntype);
}
