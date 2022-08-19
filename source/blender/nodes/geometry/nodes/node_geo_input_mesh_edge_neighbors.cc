/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_mesh.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_edge_neighbors_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>(N_("Face Count"))
      .field_source()
      .description(N_("The number of faces that use each edge as one of their sides"));
}

class EdgeNeighborCountFieldInput final : public GeometryFieldInput {
 public:
  EdgeNeighborCountFieldInput()
      : GeometryFieldInput(CPPType::get<int>(), "Edge Neighbor Count Field")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 IndexMask UNUSED(mask)) const final
  {
    if (component.type() == GEO_COMPONENT_TYPE_MESH) {
      const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
      const Mesh *mesh = mesh_component.get_for_read();
      if (mesh == nullptr) {
        return {};
      }

      Array<int> face_count(mesh->totedge, 0);
      for (const int i : IndexRange(mesh->totloop)) {
        face_count[mesh->mloop[i].e]++;
      }

      return mesh_component.attributes()->adapt_domain<int>(
          VArray<int>::ForContainer(std::move(face_count)), ATTR_DOMAIN_EDGE, domain);
    }
    return {};
  }

  uint64_t hash() const override
  {
    /* Some random constant hash. */
    return 985671075;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const EdgeNeighborCountFieldInput *>(&other) != nullptr;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> neighbor_count_field{std::make_shared<EdgeNeighborCountFieldInput>()};
  params.set_output("Face Count", std::move(neighbor_count_field));
}

}  // namespace blender::nodes::node_geo_input_mesh_edge_neighbors_cc

void register_node_type_geo_input_mesh_edge_neighbors()
{
  namespace file_ns = blender::nodes::node_geo_input_mesh_edge_neighbors_cc;

  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_INPUT_MESH_EDGE_NEIGHBORS, "Edge Neighbors", NODE_CLASS_INPUT);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
