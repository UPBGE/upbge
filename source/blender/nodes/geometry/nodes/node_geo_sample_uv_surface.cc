/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_mesh.h"
#include "BKE_type_conversions.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GEO_reverse_uv_sampler.hh"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_uv_surface_cc {

using geometry::ReverseUVSampler;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Mesh")).supported_type(GEO_COMPONENT_TYPE_MESH);

  b.add_input<decl::Float>(N_("Value"), "Value_Float").hide_value().field_on_all();
  b.add_input<decl::Int>(N_("Value"), "Value_Int").hide_value().field_on_all();
  b.add_input<decl::Vector>(N_("Value"), "Value_Vector").hide_value().field_on_all();
  b.add_input<decl::Color>(N_("Value"), "Value_Color").hide_value().field_on_all();
  b.add_input<decl::Bool>(N_("Value"), "Value_Bool").hide_value().field_on_all();

  b.add_input<decl::Vector>(N_("Source UV Map"))
      .hide_value()
      .field_on_all()
      .description(N_("The mesh UV map to sample. Should not have overlapping faces"));
  b.add_input<decl::Vector>(N_("Sample UV"))
      .supports_field()
      .description(N_("The coordinates to sample within the UV map"));

  b.add_output<decl::Float>(N_("Value"), "Value_Float").dependent_field({7});
  b.add_output<decl::Int>(N_("Value"), "Value_Int").dependent_field({7});
  b.add_output<decl::Vector>(N_("Value"), "Value_Vector").dependent_field({7});
  b.add_output<decl::Color>(N_("Value"), "Value_Color").dependent_field({7});
  b.add_output<decl::Bool>(N_("Value"), "Value_Bool").dependent_field({7});

  b.add_output<decl::Bool>(N_("Is Valid"))
      .dependent_field({7})
      .description(N_("Whether the node could find a single face to sample at the UV coordinate"));
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", 0, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const eCustomDataType data_type = eCustomDataType(node->custom1);

  bNodeSocket *in_socket_mesh = static_cast<bNodeSocket *>(node->inputs.first);
  bNodeSocket *in_socket_float = in_socket_mesh->next;
  bNodeSocket *in_socket_int32 = in_socket_float->next;
  bNodeSocket *in_socket_vector = in_socket_int32->next;
  bNodeSocket *in_socket_color4f = in_socket_vector->next;
  bNodeSocket *in_socket_bool = in_socket_color4f->next;

  nodeSetSocketAvailability(ntree, in_socket_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, in_socket_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, in_socket_color4f, data_type == CD_PROP_COLOR);
  nodeSetSocketAvailability(ntree, in_socket_bool, data_type == CD_PROP_BOOL);
  nodeSetSocketAvailability(ntree, in_socket_int32, data_type == CD_PROP_INT32);

  bNodeSocket *out_socket_float = static_cast<bNodeSocket *>(node->outputs.first);
  bNodeSocket *out_socket_int32 = out_socket_float->next;
  bNodeSocket *out_socket_vector = out_socket_int32->next;
  bNodeSocket *out_socket_color4f = out_socket_vector->next;
  bNodeSocket *out_socket_bool = out_socket_color4f->next;

  nodeSetSocketAvailability(ntree, out_socket_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, out_socket_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, out_socket_color4f, data_type == CD_PROP_COLOR);
  nodeSetSocketAvailability(ntree, out_socket_bool, data_type == CD_PROP_BOOL);
  nodeSetSocketAvailability(ntree, out_socket_int32, data_type == CD_PROP_INT32);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().fixed_declaration;
  search_link_ops_for_declarations(params, declaration.inputs.as_span().take_back(2));
  search_link_ops_for_declarations(params, declaration.inputs.as_span().take_front(1));
  search_link_ops_for_declarations(params, declaration.outputs.as_span().take_back(1));

  const std::optional<eCustomDataType> type = node_data_type_to_custom_data_type(
      eNodeSocketDatatype(params.other_socket().type));
  if (type && *type != CD_PROP_STRING) {
    /* The input and output sockets have the same name. */
    params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSampleUVSurface");
      node.custom1 = *type;
      params.update_and_connect_available_socket(node, "Value");
    });
  }
}

class SampleMeshBarycentricFunction : public fn::MultiFunction {
  GeometrySet source_;
  GField src_field_;

  /**
   * Use the most complex domain for now ensuring no information is lost. In the future, it should
   * be possible to use the most complex domain required by the field inputs, to simplify sampling
   * and avoid domain conversions.
   */
  eAttrDomain domain_ = ATTR_DOMAIN_CORNER;

  fn::MFSignature signature_;

  std::optional<bke::MeshFieldContext> source_context_;
  std::unique_ptr<FieldEvaluator> source_evaluator_;
  const GVArray *source_data_;

  Span<MLoopTri> looptris_;

 public:
  SampleMeshBarycentricFunction(GeometrySet geometry, GField src_field)
      : source_(std::move(geometry)), src_field_(std::move(src_field))
  {
    source_.ensure_owns_direct_data();
    this->set_signature(&signature_);
    signature_ = this->create_signature();
    this->evaluate_source();
  }

  fn::MFSignature create_signature()
  {
    fn::MFSignatureBuilder signature{"Sample Barycentric Triangles"};
    signature.single_input<int>("Triangle Index");
    signature.single_input<float3>("Barycentric Weight");
    signature.single_output("Value", src_field_.cpp_type());
    return signature.build();
  }

  void call(IndexMask mask, fn::MFParams params, fn::MFContext /*context*/) const override
  {
    const VArraySpan<int> triangle_indices = params.readonly_single_input<int>(0,
                                                                               "Triangle Index");
    const VArraySpan<float3> bary_weights = params.readonly_single_input<float3>(
        1, "Barycentric Weight");
    GMutableSpan dst = params.uninitialized_single_output(2, "Value");

    attribute_math::convert_to_static_type(src_field_.cpp_type(), [&](auto dummy) {
      using T = decltype(dummy);
      const VArray<T> src_typed = source_data_->typed<T>();
      MutableSpan<T> dst_typed = dst.typed<T>();
      for (const int i : mask) {
        const int triangle_index = triangle_indices[i];
        if (triangle_indices[i] != -1) {
          dst_typed[i] = attribute_math::mix3(bary_weights[i],
                                              src_typed[looptris_[triangle_index].tri[0]],
                                              src_typed[looptris_[triangle_index].tri[1]],
                                              src_typed[looptris_[triangle_index].tri[2]]);
        }
        else {
          dst_typed[i] = {};
        }
      }
    });
  }

 private:
  void evaluate_source()
  {
    const Mesh &mesh = *source_.get_mesh_for_read();
    looptris_ = mesh.looptris();
    source_context_.emplace(bke::MeshFieldContext{mesh, domain_});
    const int domain_size = mesh.attributes().domain_size(domain_);
    source_evaluator_ = std::make_unique<FieldEvaluator>(*source_context_, domain_size);
    source_evaluator_->add(src_field_);
    source_evaluator_->evaluate();
    source_data_ = &source_evaluator_->get_evaluated(0);
  }
};

class ReverseUVSampleFunction : public fn::MultiFunction {
  GeometrySet source_;
  Field<float2> src_uv_map_field_;

  std::optional<bke::MeshFieldContext> source_context_;
  std::unique_ptr<FieldEvaluator> source_evaluator_;
  VArraySpan<float2> source_uv_map_;

  std::optional<ReverseUVSampler> reverse_uv_sampler_;

 public:
  ReverseUVSampleFunction(GeometrySet geometry, Field<float2> src_uv_map_field)
      : source_(std::move(geometry)), src_uv_map_field_(std::move(src_uv_map_field))
  {
    source_.ensure_owns_direct_data();
    static fn::MFSignature signature = create_signature();
    this->set_signature(&signature);
    this->evaluate_source();
  }

  static fn::MFSignature create_signature()
  {
    fn::MFSignatureBuilder signature{"Sample UV Surface"};
    signature.single_input<float2>("Sample UV");
    signature.single_output<bool>("Is Valid");
    signature.single_output<int>("Triangle Index");
    signature.single_output<float3>("Barycentric Weights");
    return signature.build();
  }

  void call(IndexMask mask, fn::MFParams params, fn::MFContext /*context*/) const override
  {
    const VArraySpan<float2> sample_uvs = params.readonly_single_input<float2>(0, "Sample UV");
    MutableSpan<bool> is_valid = params.uninitialized_single_output_if_required<bool>(1,
                                                                                      "Is Valid");
    MutableSpan<int> tri_index = params.uninitialized_single_output_if_required<int>(
        2, "Triangle Index");
    MutableSpan<float3> bary_weights = params.uninitialized_single_output_if_required<float3>(
        3, "Barycentric Weights");

    Array<ReverseUVSampler::Result> results(mask.min_array_size());
    reverse_uv_sampler_->sample_many(sample_uvs, results);

    if (!is_valid.is_empty()) {
      std::transform(results.begin(),
                     results.end(),
                     is_valid.begin(),
                     [](const ReverseUVSampler::Result &result) {
                       return result.type == ReverseUVSampler::ResultType::Ok;
                     });
    }
    if (!tri_index.is_empty()) {
      std::transform(results.begin(),
                     results.end(),
                     tri_index.begin(),
                     [](const ReverseUVSampler::Result &result) { return result.looptri_index; });
    }

    if (!bary_weights.is_empty()) {
      std::transform(results.begin(),
                     results.end(),
                     bary_weights.begin(),
                     [](const ReverseUVSampler::Result &result) { return result.bary_weights; });
    }
  }

 private:
  void evaluate_source()
  {
    const Mesh &mesh = *source_.get_mesh_for_read();
    source_context_.emplace(bke::MeshFieldContext{mesh, ATTR_DOMAIN_CORNER});
    source_evaluator_ = std::make_unique<FieldEvaluator>(*source_context_, mesh.totloop);
    source_evaluator_->add(src_uv_map_field_);
    source_evaluator_->evaluate();
    source_uv_map_ = source_evaluator_->get_evaluated<float2>(0);

    reverse_uv_sampler_.emplace(source_uv_map_, mesh.looptris());
  }
};

static GField get_input_attribute_field(GeoNodeExecParams &params, const eCustomDataType data_type)
{
  switch (data_type) {
    case CD_PROP_FLOAT:
      return params.extract_input<Field<float>>("Value_Float");
    case CD_PROP_FLOAT3:
      return params.extract_input<Field<float3>>("Value_Vector");
    case CD_PROP_COLOR:
      return params.extract_input<Field<ColorGeometry4f>>("Value_Color");
    case CD_PROP_BOOL:
      return params.extract_input<Field<bool>>("Value_Bool");
    case CD_PROP_INT32:
      return params.extract_input<Field<int>>("Value_Int");
    default:
      BLI_assert_unreachable();
  }
  return {};
}

static void output_attribute_field(GeoNodeExecParams &params, GField field)
{
  switch (bke::cpp_type_to_custom_data_type(field.cpp_type())) {
    case CD_PROP_FLOAT: {
      params.set_output("Value_Float", Field<float>(field));
      break;
    }
    case CD_PROP_FLOAT3: {
      params.set_output("Value_Vector", Field<float3>(field));
      break;
    }
    case CD_PROP_COLOR: {
      params.set_output("Value_Color", Field<ColorGeometry4f>(field));
      break;
    }
    case CD_PROP_BOOL: {
      params.set_output("Value_Bool", Field<bool>(field));
      break;
    }
    case CD_PROP_INT32: {
      params.set_output("Value_Int", Field<int>(field));
      break;
    }
    default:
      break;
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Mesh");
  const eCustomDataType data_type = eCustomDataType(params.node().custom1);
  const Mesh *mesh = geometry.get_mesh_for_read();
  if (mesh == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }
  if (mesh->totpoly == 0 && mesh->totvert != 0) {
    params.error_message_add(NodeWarningType::Error, TIP_("The source mesh must have faces"));
    params.set_default_remaining_outputs();
    return;
  }

  /* Do reverse sampling of the UV map first. */
  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  const CPPType &float2_type = CPPType::get<float2>();
  Field<float2> source_uv_map = conversions.try_convert(
      params.extract_input<Field<float3>>("Source UV Map"), float2_type);
  Field<float2> sample_uvs = conversions.try_convert(
      params.extract_input<Field<float3>>("Sample UV"), float2_type);
  auto uv_op = FieldOperation::Create(
      std::make_shared<ReverseUVSampleFunction>(geometry, std::move(source_uv_map)),
      {std::move(sample_uvs)});
  params.set_output("Is Valid", Field<bool>(uv_op, 0));

  /* Use the output of the UV sampling to interpolate the mesh attribute. */
  GField field = get_input_attribute_field(params, data_type);
  auto sample_op = FieldOperation::Create(
      std::make_shared<SampleMeshBarycentricFunction>(std::move(geometry), std::move(field)),
      {Field<int>(uv_op, 1), Field<float3>(uv_op, 2)});
  output_attribute_field(params, GField(sample_op, 0));
}

}  // namespace blender::nodes::node_geo_sample_uv_surface_cc

void register_node_type_geo_sample_uv_surface()
{
  namespace file_ns = blender::nodes::node_geo_sample_uv_surface_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SAMPLE_UV_SURFACE, "Sample UV Surface", NODE_CLASS_GEOMETRY);
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  nodeRegisterType(&ntype);
}
