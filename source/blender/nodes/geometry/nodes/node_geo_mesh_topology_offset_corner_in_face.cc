/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_topology_offset_corner_in_face_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Corner Index")
      .implicit_field(NODE_DEFAULT_INPUT_INDEX_FIELD)
      .description("The corner to retrieve data from. Defaults to the corner from the context")
      .structure_type(StructureType::Field);
  b.add_input<decl::Int>("Offset").supports_field().description(
      "The number of corners to move around the face before finding the result, "
      "circling around the start of the face if necessary");
  b.add_output<decl::Int>("Corner Index")
      .field_source_reference_all()
      .description("The index of the offset corner");
}

class OffsetCornerInFaceFieldInput final : public bke::MeshFieldInput {
  const Field<int> corner_index_;
  const Field<int> offset_;

 public:
  OffsetCornerInFaceFieldInput(Field<int> corner_index, Field<int> offset)
      : bke::MeshFieldInput(CPPType::get<int>(), "Offset Corner in Face"),
        corner_index_(std::move(corner_index)),
        offset_(std::move(offset))
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    const IndexRange corner_range(mesh.corners_num);
    const OffsetIndices faces = mesh.faces();

    const bke::MeshFieldContext context{mesh, domain};
    fn::FieldEvaluator evaluator{context, &mask};
    evaluator.add(corner_index_);
    evaluator.add(offset_);
    evaluator.evaluate();
    const VArray<int> corner_indices = evaluator.get_evaluated<int>(0);
    const VArray<int> offsets = evaluator.get_evaluated<int>(1);

    const Span<int> corner_to_face = mesh.corner_to_face_map();

    Array<int> offset_corners(mask.min_array_size());
    mask.foreach_index_optimized<int>(GrainSize(2048), [&](const int selection_i) {
      const int corner = corner_indices[selection_i];
      const int offset = offsets[selection_i];
      if (!corner_to_face.index_range().contains(corner)) {
        offset_corners[selection_i] = 0;
        return;
      }
      const IndexRange face = faces[corner_to_face[corner]];
      const int corner_index_in_face = corner - face.start();
      offset_corners[selection_i] = face.start() + math::mod_periodic<int>(
                                                       corner_index_in_face + offset, face.size());
    });

    return VArray<int>::from_container(std::move(offset_corners));
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const override
  {
    corner_index_.node().for_each_field_input_recursive(fn);
    offset_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const final
  {
    return get_default_hash(offset_);
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const OffsetCornerInFaceFieldInput *other_field =
            dynamic_cast<const OffsetCornerInFaceFieldInput *>(&other))
    {
      return other_field->corner_index_ == corner_index_ && other_field->offset_ == offset_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Corner;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Corner Index",
                    Field<int>(std::make_shared<OffsetCornerInFaceFieldInput>(
                        params.extract_input<Field<int>>("Corner Index"),
                        params.extract_input<Field<int>>("Offset"))));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, "GeometryNodeOffsetCornerInFace", GEO_NODE_MESH_TOPOLOGY_OFFSET_CORNER_IN_FACE);
  ntype.ui_name = "Offset Corner in Face";
  ntype.ui_description = "Retrieve corners in the same face as another";
  ntype.enum_name_legacy = "OFFSET_CORNER_IN_FACE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_topology_offset_corner_in_face_cc
