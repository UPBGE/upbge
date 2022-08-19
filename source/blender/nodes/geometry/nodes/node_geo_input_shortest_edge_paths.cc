/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>

#include "BKE_curves.hh"

#include "BLI_map.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_set.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_shortest_edge_paths_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>(N_("End Vertex")).default_value(false).hide_value().supports_field();
  b.add_input<decl::Float>(N_("Edge Cost")).default_value(1.0f).hide_value().supports_field();
  b.add_output<decl::Int>(N_("Next Vertex Index")).field_source();
  b.add_output<decl::Float>(N_("Total Cost")).field_source();
}

typedef std::pair<float, int> VertPriority;

struct EdgeVertMap {
  Array<Vector<int>> edges_by_vertex_map;

  EdgeVertMap(const Mesh *mesh)
  {
    const Span<MEdge> edges{mesh->medge, mesh->totedge};
    edges_by_vertex_map.reinitialize(mesh->totvert);
    for (const int edge_i : edges.index_range()) {
      const MEdge &edge = edges[edge_i];
      edges_by_vertex_map[edge.v1].append(edge_i);
      edges_by_vertex_map[edge.v2].append(edge_i);
    }
  }
};

static void shortest_paths(const Mesh *mesh,
                           EdgeVertMap &maps,
                           const IndexMask end_selection,
                           const VArray<float> &input_cost,
                           MutableSpan<int> r_next_index,
                           MutableSpan<float> r_cost)
{
  const Span<MVert> verts{mesh->mvert, mesh->totvert};
  const Span<MEdge> edges{mesh->medge, mesh->totedge};
  Array<bool> visited(mesh->totvert, false);

  std::priority_queue<VertPriority, std::vector<VertPriority>, std::greater<VertPriority>> queue;

  for (const int start_vert_i : end_selection) {
    r_cost[start_vert_i] = 0.0f;
    queue.emplace(0.0f, start_vert_i);
  }

  while (!queue.empty()) {
    const float cost_i = queue.top().first;
    const int vert_i = queue.top().second;
    queue.pop();
    if (visited[vert_i]) {
      continue;
    }
    visited[vert_i] = true;
    const Span<int> incident_edge_indices = maps.edges_by_vertex_map[vert_i];
    for (const int edge_i : incident_edge_indices) {
      const MEdge &edge = edges[edge_i];
      const int neighbor_vert_i = edge.v1 + edge.v2 - vert_i;
      if (visited[neighbor_vert_i]) {
        continue;
      }
      const float edge_cost = std::max(0.0f, input_cost[edge_i]);
      const float new_neighbour_cost = cost_i + edge_cost;
      if (new_neighbour_cost < r_cost[neighbor_vert_i]) {
        r_cost[neighbor_vert_i] = new_neighbour_cost;
        r_next_index[neighbor_vert_i] = vert_i;
        queue.emplace(new_neighbour_cost, neighbor_vert_i);
      }
    }
  }
}

class ShortestEdgePathsNextVertFieldInput final : public GeometryFieldInput {
 private:
  Field<bool> end_selection_;
  Field<float> cost_;

 public:
  ShortestEdgePathsNextVertFieldInput(Field<bool> end_selection, Field<float> cost)
      : GeometryFieldInput(CPPType::get<int>(), "Shortest Edge Paths Next Vertex Field"),
        end_selection_(end_selection),
        cost_(cost)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 [[maybe_unused]] IndexMask mask) const final
  {
    if (component.type() != GEO_COMPONENT_TYPE_MESH) {
      return {};
    }
    const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
    const Mesh *mesh = mesh_component.get_for_read();
    if (mesh == nullptr) {
      return {};
    }
    GeometryComponentFieldContext edge_context{component, ATTR_DOMAIN_EDGE};
    fn::FieldEvaluator edge_evaluator{edge_context, mesh->totedge};
    edge_evaluator.add(cost_);
    edge_evaluator.evaluate();
    const VArray<float> input_cost = edge_evaluator.get_evaluated<float>(0);

    GeometryComponentFieldContext point_context{component, ATTR_DOMAIN_POINT};
    fn::FieldEvaluator point_evaluator{point_context, mesh->totvert};
    point_evaluator.add(end_selection_);
    point_evaluator.evaluate();
    const IndexMask end_selection = point_evaluator.get_evaluated_as_mask(0);

    Array<int> next_index(mesh->totvert, -1);
    Array<float> cost(mesh->totvert, FLT_MAX);

    if (!end_selection.is_empty()) {
      EdgeVertMap maps(mesh);
      shortest_paths(mesh, maps, end_selection, input_cost, next_index, cost);
    }
    threading::parallel_for(next_index.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        if (next_index[i] == -1) {
          next_index[i] = i;
        }
      }
    });
    return component.attributes()->adapt_domain<int>(
        VArray<int>::ForContainer(std::move(next_index)), ATTR_DOMAIN_POINT, domain);
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 8466507837;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const ShortestEdgePathsNextVertFieldInput *other_field =
            dynamic_cast<const ShortestEdgePathsNextVertFieldInput *>(&other)) {
      return other_field->end_selection_ == end_selection_ && other_field->cost_ == cost_;
    }
    return false;
  }
};

class ShortestEdgePathsCostFieldInput final : public GeometryFieldInput {
 private:
  Field<bool> end_selection_;
  Field<float> cost_;

 public:
  ShortestEdgePathsCostFieldInput(Field<bool> end_selection, Field<float> cost)
      : GeometryFieldInput(CPPType::get<float>(), "Shortest Edge Paths Cost Field"),
        end_selection_(end_selection),
        cost_(cost)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 [[maybe_unused]] IndexMask mask) const final
  {
    if (component.type() != GEO_COMPONENT_TYPE_MESH) {
      return {};
    }
    const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
    const Mesh *mesh = mesh_component.get_for_read();
    if (mesh == nullptr) {
      return {};
    }
    GeometryComponentFieldContext edge_context{component, ATTR_DOMAIN_EDGE};
    fn::FieldEvaluator edge_evaluator{edge_context, mesh->totedge};
    edge_evaluator.add(cost_);
    edge_evaluator.evaluate();
    const VArray<float> input_cost = edge_evaluator.get_evaluated<float>(0);

    GeometryComponentFieldContext point_context{component, ATTR_DOMAIN_POINT};
    fn::FieldEvaluator point_evaluator{point_context, mesh->totvert};
    point_evaluator.add(end_selection_);
    point_evaluator.evaluate();
    const IndexMask end_selection = point_evaluator.get_evaluated_as_mask(0);

    Array<int> next_index(mesh->totvert, -1);
    Array<float> cost(mesh->totvert, FLT_MAX);

    if (!end_selection.is_empty()) {
      EdgeVertMap maps(mesh);
      shortest_paths(mesh, maps, end_selection, input_cost, next_index, cost);
    }
    threading::parallel_for(cost.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        if (cost[i] == FLT_MAX) {
          cost[i] = 0;
        }
      }
    });
    return component.attributes()->adapt_domain<float>(
        VArray<float>::ForContainer(std::move(cost)), ATTR_DOMAIN_POINT, domain);
  }

  uint64_t hash() const override
  {
    return get_default_hash_2(end_selection_, cost_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const ShortestEdgePathsCostFieldInput *other_field =
            dynamic_cast<const ShortestEdgePathsCostFieldInput *>(&other)) {
      return other_field->end_selection_ == end_selection_ && other_field->cost_ == cost_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<bool> end_selection = params.extract_input<Field<bool>>("End Vertex");
  Field<float> cost = params.extract_input<Field<float>>("Edge Cost");

  Field<int> next_vert_field{
      std::make_shared<ShortestEdgePathsNextVertFieldInput>(end_selection, cost)};
  Field<float> cost_field{std::make_shared<ShortestEdgePathsCostFieldInput>(end_selection, cost)};
  params.set_output("Next Vertex Index", std::move(next_vert_field));
  params.set_output("Total Cost", std::move(cost_field));
}

}  // namespace blender::nodes::node_geo_input_shortest_edge_paths_cc

void register_node_type_geo_input_shortest_edge_paths()
{
  namespace file_ns = blender::nodes::node_geo_input_shortest_edge_paths_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_INPUT_SHORTEST_EDGE_PATHS, "Shortest Edge Paths", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
