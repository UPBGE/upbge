/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"

#include "BLI_task.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_topology_edges_of_vertex_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>(N_("Vertex Index"))
      .implicit_field(implicit_field_inputs::index)
      .description(
          N_("The vertex to retrieve data from. Defaults to the vertex from the context"));
  b.add_input<decl::Float>(N_("Weights"))
      .supports_field()
      .hide_value()
      .description(
          N_("Values used to sort the edges connected to the vertex. Uses indices by default"));
  b.add_input<decl::Int>(N_("Sort Index"))
      .min(0)
      .supports_field()
      .description(N_("Which of the sorted edges to output"));
  b.add_output<decl::Int>(N_("Edge Index"))
      .field_source_reference_all()
      .description(N_("An edge connected to the face, chosen by the sort index"));
  b.add_output<decl::Int>(N_("Total"))
      .field_source()
      .reference_pass({0})
      .description(N_("The number of edges connected to each vertex"));
}

static void convert_span(const Span<int> src, MutableSpan<int64_t> dst)
{
  for (const int i : src.index_range()) {
    dst[i] = src[i];
  }
}

class EdgesOfVertInput final : public bke::MeshFieldInput {
  const Field<int> vert_index_;
  const Field<int> sort_index_;
  const Field<float> sort_weight_;

 public:
  EdgesOfVertInput(Field<int> vert_index, Field<int> sort_index, Field<float> sort_weight)
      : bke::MeshFieldInput(CPPType::get<int>(), "Edge of Vertex"),
        vert_index_(std::move(vert_index)),
        sort_index_(std::move(sort_index)),
        sort_weight_(std::move(sort_weight))
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const eAttrDomain domain,
                                 const IndexMask mask) const final
  {
    const IndexRange vert_range(mesh.totvert);
    const Span<MEdge> edges = mesh.edges();
    Array<Vector<int>> vert_to_edge_map = bke::mesh_topology::build_vert_to_edge_map(edges,
                                                                                     mesh.totvert);

    const bke::MeshFieldContext context{mesh, domain};
    fn::FieldEvaluator evaluator{context, &mask};
    evaluator.add(vert_index_);
    evaluator.add(sort_index_);
    evaluator.evaluate();
    const VArray<int> vert_indices = evaluator.get_evaluated<int>(0);
    const VArray<int> indices_in_sort = evaluator.get_evaluated<int>(1);

    const bke::MeshFieldContext edge_context{mesh, ATTR_DOMAIN_EDGE};
    fn::FieldEvaluator edge_evaluator{edge_context, mesh.totedge};
    edge_evaluator.add(sort_weight_);
    edge_evaluator.evaluate();
    const VArray<float> all_sort_weights = edge_evaluator.get_evaluated<float>(0);

    Array<int> edge_of_vertex(mask.min_array_size());
    threading::parallel_for(mask.index_range(), 1024, [&](const IndexRange range) {
      /* Reuse arrays to avoid allocation. */
      Array<int64_t> edge_indices;
      Array<float> sort_weights;
      Array<int> sort_indices;

      for (const int selection_i : mask.slice(range)) {
        const int vert_i = vert_indices[selection_i];
        const int index_in_sort = indices_in_sort[selection_i];
        if (!vert_range.contains(vert_i)) {
          edge_of_vertex[selection_i] = 0;
          continue;
        }

        const Span<int> edges = vert_to_edge_map[vert_i];
        if (edges.is_empty()) {
          edge_of_vertex[selection_i] = 0;
          continue;
        }

        /* Retrieve the connected edge indices as 64 bit integers for #materialize_compressed. */
        edge_indices.reinitialize(edges.size());
        convert_span(edges, edge_indices);

        /* Retrieve a compressed array of weights for each edge. */
        sort_weights.reinitialize(edges.size());
        all_sort_weights.materialize_compressed(IndexMask(edge_indices),
                                                sort_weights.as_mutable_span());

        /* Sort a separate array of compressed indices corresponding to the compressed weights.
         * This allows using `materialize_compressed` to avoid virtual function call overhead
         * when accessing values in the sort weights. However, it means a separate array of
         * indices within the compressed array is necessary for sorting. */
        sort_indices.reinitialize(edges.size());
        std::iota(sort_indices.begin(), sort_indices.end(), 0);
        std::stable_sort(sort_indices.begin(), sort_indices.end(), [&](int a, int b) {
          return sort_weights[a] < sort_weights[b];
        });

        const int index_in_sort_wrapped = mod_i(index_in_sort, edges.size());
        edge_of_vertex[selection_i] = edge_indices[sort_indices[index_in_sort_wrapped]];
      }
    });

    return VArray<int>::ForContainer(std::move(edge_of_vertex));
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const override
  {
    vert_index_.node().for_each_field_input_recursive(fn);
    sort_index_.node().for_each_field_input_recursive(fn);
    sort_weight_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const final
  {
    return 98762349875636;
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const auto *typed = dynamic_cast<const EdgesOfVertInput *>(&other)) {
      return typed->vert_index_ == vert_index_ && typed->sort_index_ == sort_index_ &&
             typed->sort_weight_ == sort_weight_;
    }
    return false;
  }

  std::optional<eAttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return ATTR_DOMAIN_POINT;
  }
};

class EdgesOfVertCountInput final : public bke::MeshFieldInput {
 public:
  EdgesOfVertCountInput() : bke::MeshFieldInput(CPPType::get<int>(), "Corner Face Index")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const eAttrDomain domain,
                                 const IndexMask /*mask*/) const final
  {
    if (domain != ATTR_DOMAIN_POINT) {
      return {};
    }
    const Span<MEdge> edges = mesh.edges();
    Array<int> counts(mesh.totvert, 0);
    for (const int i : edges.index_range()) {
      counts[edges[i].v1]++;
      counts[edges[i].v2]++;
    }
    return VArray<int>::ForContainer(std::move(counts));
  }

  uint64_t hash() const final
  {
    return 436758278618374;
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (dynamic_cast<const EdgesOfVertCountInput *>(&other)) {
      return true;
    }
    return false;
  }

  std::optional<eAttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return ATTR_DOMAIN_POINT;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const Field<int> vert_index = params.extract_input<Field<int>>("Vertex Index");
  if (params.output_is_required("Total")) {
    params.set_output("Total",
                      Field<int>(std::make_shared<FieldAtIndexInput>(
                          vert_index,
                          Field<int>(std::make_shared<EdgesOfVertCountInput>()),
                          ATTR_DOMAIN_POINT)));
  }
  if (params.output_is_required("Edge Index")) {
    params.set_output("Edge Index",
                      Field<int>(std::make_shared<EdgesOfVertInput>(
                          vert_index,
                          params.extract_input<Field<int>>("Sort Index"),
                          params.extract_input<Field<float>>("Weights"))));
  }
}

}  // namespace blender::nodes::node_geo_mesh_topology_edges_of_vertex_cc

void register_node_type_geo_mesh_topology_edges_of_vertex()
{
  namespace file_ns = blender::nodes::node_geo_mesh_topology_edges_of_vertex_cc;

  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_MESH_TOPOLOGY_EDGES_OF_VERTEX, "Edges of Vertex", NODE_CLASS_INPUT);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
