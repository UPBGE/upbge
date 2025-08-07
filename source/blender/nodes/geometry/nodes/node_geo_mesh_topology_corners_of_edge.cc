/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "BKE_mesh_mapping.hh"

#include "BLI_array_utils.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_topology_corners_of_edge_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Edge Index")
      .implicit_field(NODE_DEFAULT_INPUT_INDEX_FIELD)
      .description("The edge to retrieve data from. Defaults to the edge from the context")
      .structure_type(StructureType::Field);
  b.add_input<decl::Float>("Weights").supports_field().hide_value().description(
      "Values that sort the corners attached to the edge");
  b.add_input<decl::Int>("Sort Index")
      .min(0)
      .supports_field()
      .description("Which of the sorted corners to output");
  b.add_output<decl::Int>("Corner Index")
      .field_source_reference_all()
      .description(
          "A corner of the input edge in its face's winding order, chosen by the sort index");
  b.add_output<decl::Int>("Total").field_source().reference_pass({0}).description(
      "The number of faces or corners connected to each edge");
}

class CornersOfEdgeInput final : public bke::MeshFieldInput {
  const Field<int> edge_index_;
  const Field<int> sort_index_;
  const Field<float> sort_weight_;

 public:
  CornersOfEdgeInput(Field<int> edge_index, Field<int> sort_index, Field<float> sort_weight)
      : bke::MeshFieldInput(CPPType::get<int>(), "Corner of Edge"),
        edge_index_(std::move(edge_index)),
        sort_index_(std::move(sort_index)),
        sort_weight_(std::move(sort_weight))
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    const IndexRange edge_range(mesh.edges_num);
    Array<int> map_offsets;
    Array<int> map_indices;
    const Span<int> corner_edges = mesh.corner_edges();
    const GroupedSpan<int> edge_to_corner_map = bke::mesh::build_edge_to_corner_map(
        mesh.corner_edges(), mesh.edges_num, map_offsets, map_indices);

    const bke::MeshFieldContext context{mesh, domain};
    fn::FieldEvaluator evaluator{context, &mask};
    evaluator.add(edge_index_);
    evaluator.add(sort_index_);
    evaluator.evaluate();
    const VArray<int> edge_indices = evaluator.get_evaluated<int>(0);
    const VArray<int> indices_in_sort = evaluator.get_evaluated<int>(1);

    const bke::MeshFieldContext corner_context{mesh, AttrDomain::Corner};
    fn::FieldEvaluator corner_evaluator{corner_context, corner_edges.size()};
    corner_evaluator.add(sort_weight_);
    corner_evaluator.evaluate();
    const VArray<float> all_sort_weights = corner_evaluator.get_evaluated<float>(0);
    const bool use_sorting = !all_sort_weights.is_single();

    Array<int> corner_of_edge(mask.min_array_size());
    mask.foreach_segment(GrainSize(1024), [&](const IndexMaskSegment segment) {
      /* Reuse arrays to avoid allocation. */
      Array<int64_t> corner_indices;
      Array<float> sort_weights;
      Array<int> sort_indices;

      for (const int selection_i : segment) {
        const int edge_i = edge_indices[selection_i];
        const int index_in_sort = indices_in_sort[selection_i];
        if (!edge_range.contains(edge_i)) {
          corner_of_edge[selection_i] = 0;
          continue;
        }

        const Span<int> corners = edge_to_corner_map[edge_i];
        if (corners.is_empty()) {
          corner_of_edge[selection_i] = 0;
          continue;
        }

        const int index_in_sort_wrapped = mod_i(index_in_sort, corners.size());
        if (use_sorting) {
          /* Retrieve a compressed array of weights for each edge. */
          sort_weights.reinitialize(corners.size());
          IndexMaskMemory memory;
          all_sort_weights.materialize_compressed(IndexMask::from_indices<int>(corners, memory),
                                                  sort_weights.as_mutable_span());

          /* Sort a separate array of compressed indices corresponding to the compressed weights.
           * This allows using `materialize_compressed` to avoid virtual function call overhead
           * when accessing values in the sort weights. However, it means a separate array of
           * indices within the compressed array is necessary for sorting. */
          sort_indices.reinitialize(corners.size());
          array_utils::fill_index_range<int>(sort_indices);
          std::stable_sort(sort_indices.begin(), sort_indices.end(), [&](int a, int b) {
            return sort_weights[a] < sort_weights[b];
          });
          corner_of_edge[selection_i] = corners[sort_indices[index_in_sort_wrapped]];
        }
        else {
          corner_of_edge[selection_i] = corners[index_in_sort_wrapped];
        }
      }
    });

    return VArray<int>::from_container(std::move(corner_of_edge));
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const override
  {
    edge_index_.node().for_each_field_input_recursive(fn);
    sort_index_.node().for_each_field_input_recursive(fn);
    sort_weight_.node().for_each_field_input_recursive(fn);
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Edge;
  }
};

class CornersOfEdgeCountInput final : public bke::MeshFieldInput {
 public:
  CornersOfEdgeCountInput() : bke::MeshFieldInput(CPPType::get<int>(), "Edge Corner Count")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    if (domain != AttrDomain::Edge) {
      return {};
    }
    Array<int> counts(mesh.edges_num, 0);
    array_utils::count_indices(mesh.corner_edges(), counts);
    return VArray<int>::from_container(std::move(counts));
  }

  uint64_t hash() const final
  {
    return 2345897985577;
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    return dynamic_cast<const CornersOfEdgeCountInput *>(&other) != nullptr;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Edge;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const Field<int> edge_index = params.extract_input<Field<int>>("Edge Index");
  if (params.output_is_required("Total")) {
    params.set_output("Total",
                      Field<int>(std::make_shared<bke::EvaluateAtIndexInput>(
                          edge_index,
                          Field<int>(std::make_shared<CornersOfEdgeCountInput>()),
                          AttrDomain::Edge)));
  }
  if (params.output_is_required("Corner Index")) {
    params.set_output("Corner Index",
                      Field<int>(std::make_shared<CornersOfEdgeInput>(
                          edge_index,
                          params.extract_input<Field<int>>("Sort Index"),
                          params.extract_input<Field<float>>("Weights"))));
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCornersOfEdge", GEO_NODE_MESH_TOPOLOGY_CORNERS_OF_EDGE);
  ntype.ui_name = "Corners of Edge";
  ntype.ui_description = "Retrieve face corners connected to edges";
  ntype.enum_name_legacy = "CORNERS_OF_EDGE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_topology_corners_of_edge_cc
