/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"

#include "BKE_attribute_math.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_index_cc {

NODE_STORAGE_FUNCS(NodeGeometrySampleIndex);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry"))
      .supported_type({GEO_COMPONENT_TYPE_MESH,
                       GEO_COMPONENT_TYPE_POINT_CLOUD,
                       GEO_COMPONENT_TYPE_CURVE,
                       GEO_COMPONENT_TYPE_INSTANCES});

  b.add_input<decl::Float>(N_("Value"), "Value_Float").hide_value().field_on_all();
  b.add_input<decl::Int>(N_("Value"), "Value_Int").hide_value().field_on_all();
  b.add_input<decl::Vector>(N_("Value"), "Value_Vector").hide_value().field_on_all();
  b.add_input<decl::Color>(N_("Value"), "Value_Color").hide_value().field_on_all();
  b.add_input<decl::Bool>(N_("Value"), "Value_Bool").hide_value().field_on_all();
  b.add_input<decl::Int>(N_("Index"))
      .supports_field()
      .description(N_("Which element to retrieve a value from on the geometry"));

  b.add_output<decl::Float>(N_("Value"), "Value_Float").dependent_field({6});
  b.add_output<decl::Int>(N_("Value"), "Value_Int").dependent_field({6});
  b.add_output<decl::Vector>(N_("Value"), "Value_Vector").dependent_field({6});
  b.add_output<decl::Color>(N_("Value"), "Value_Color").dependent_field({6});
  b.add_output<decl::Bool>(N_("Value"), "Value_Bool").dependent_field({6});
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", 0, "", ICON_NONE);
  uiItemR(layout, ptr, "domain", 0, "", ICON_NONE);
  uiItemR(layout, ptr, "clamp", 0, nullptr, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometrySampleIndex *data = MEM_cnew<NodeGeometrySampleIndex>(__func__);
  data->data_type = CD_PROP_FLOAT;
  data->domain = ATTR_DOMAIN_POINT;
  data->clamp = 0;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const eCustomDataType data_type = eCustomDataType(node_storage(*node).data_type);

  bNodeSocket *in_socket_geometry = static_cast<bNodeSocket *>(node->inputs.first);
  bNodeSocket *in_socket_float = in_socket_geometry->next;
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
  search_link_ops_for_declarations(params, declaration.inputs.as_span().take_back(1));
  search_link_ops_for_declarations(params, declaration.inputs.as_span().take_front(1));

  const std::optional<eCustomDataType> type = node_data_type_to_custom_data_type(
      (eNodeSocketDatatype)params.other_socket().type);
  if (type && *type != CD_PROP_STRING) {
    /* The input and output sockets have the same name. */
    params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSampleIndex");
      node_storage(node).data_type = *type;
      params.update_and_connect_available_socket(node, "Value");
    });
  }
}

static bool component_is_available(const GeometrySet &geometry,
                                   const GeometryComponentType type,
                                   const eAttrDomain domain)
{
  if (!geometry.has(type)) {
    return false;
  }
  const GeometryComponent &component = *geometry.get_component_for_read(type);
  return component.attribute_domain_size(domain) != 0;
}

static const GeometryComponent *find_source_component(const GeometrySet &geometry,
                                                      const eAttrDomain domain)
{
  /* Choose the other component based on a consistent order, rather than some more complicated
   * heuristic. This is the same order visible in the spreadsheet and used in the ray-cast node. */
  static const Array<GeometryComponentType> supported_types = {GEO_COMPONENT_TYPE_MESH,
                                                               GEO_COMPONENT_TYPE_POINT_CLOUD,
                                                               GEO_COMPONENT_TYPE_CURVE,
                                                               GEO_COMPONENT_TYPE_INSTANCES};
  for (const GeometryComponentType src_type : supported_types) {
    if (component_is_available(geometry, src_type, domain)) {
      return geometry.get_component_for_read(src_type);
    }
  }

  return nullptr;
}

template<typename T>
void copy_with_indices(const VArray<T> &src,
                       const VArray<int> &indices,
                       const IndexMask mask,
                       MutableSpan<T> dst)
{
  const IndexRange src_range = src.index_range();
  devirtualize_varray2(src, indices, [&](const auto src, const auto indices) {
    threading::parallel_for(mask.index_range(), 4096, [&](IndexRange range) {
      for (const int i : mask.slice(range)) {
        const int index = indices[i];
        if (src_range.contains(index)) {
          dst[i] = src[index];
        }
        else {
          dst[i] = {};
        }
      }
    });
  });
}

template<typename T>
void copy_with_clamped_indices(const VArray<T> &src,
                               const VArray<int> &indices,
                               const IndexMask mask,
                               MutableSpan<T> dst)
{
  const int last_index = src.index_range().last();
  devirtualize_varray2(src, indices, [&](const auto src, const auto indices) {
    threading::parallel_for(mask.index_range(), 4096, [&](IndexRange range) {
      for (const int i : mask.slice(range)) {
        const int index = indices[i];
        dst[i] = src[std::clamp(index, 0, last_index)];
      }
    });
  });
}

/**
 * The index-based transfer theoretically does not need realized data when there is only one
 * instance geometry set in the source. A future optimization could be removing that limitation
 * internally.
 */
class SampleIndexFunction : public fn::MultiFunction {
  GeometrySet src_geometry_;
  GField src_field_;
  eAttrDomain domain_;
  bool clamp_;

  fn::MFSignature signature_;

  std::optional<bke::GeometryFieldContext> geometry_context_;
  std::unique_ptr<FieldEvaluator> evaluator_;
  const GVArray *src_data_ = nullptr;

 public:
  SampleIndexFunction(GeometrySet geometry,
                      GField src_field,
                      const eAttrDomain domain,
                      const bool clamp)
      : src_geometry_(std::move(geometry)),
        src_field_(std::move(src_field)),
        domain_(domain),
        clamp_(clamp)
  {
    src_geometry_.ensure_owns_direct_data();

    signature_ = this->create_signature();
    this->set_signature(&signature_);

    this->evaluate_field();
  }

  fn::MFSignature create_signature()
  {
    fn::MFSignatureBuilder signature{"Sample Index"};
    signature.single_input<int>("Index");
    signature.single_output("Value", src_field_.cpp_type());
    return signature.build();
  }

  void evaluate_field()
  {
    const GeometryComponent *component = find_source_component(src_geometry_, domain_);
    if (component == nullptr) {
      return;
    }
    const int domain_num = component->attribute_domain_size(domain_);
    geometry_context_.emplace(bke::GeometryFieldContext(*component, domain_));
    evaluator_ = std::make_unique<FieldEvaluator>(*geometry_context_, domain_num);
    evaluator_->add(src_field_);
    evaluator_->evaluate();
    src_data_ = &evaluator_->get_evaluated(0);
  }

  void call(IndexMask mask, fn::MFParams params, fn::MFContext /*context*/) const override
  {
    const VArray<int> &indices = params.readonly_single_input<int>(0, "Index");
    GMutableSpan dst = params.uninitialized_single_output(1, "Value");

    const CPPType &type = dst.type();
    if (src_data_ == nullptr) {
      type.value_initialize_indices(dst.data(), mask);
      return;
    }

    attribute_math::convert_to_static_type(type, [&](auto dummy) {
      using T = decltype(dummy);
      if (clamp_) {
        copy_with_clamped_indices(src_data_->typed<T>(), indices, mask, dst.typed<T>());
      }
      else {
        copy_with_indices(src_data_->typed<T>(), indices, mask, dst.typed<T>());
      }
    });
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
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry");
  const NodeGeometrySampleIndex &storage = node_storage(params.node());
  const eCustomDataType data_type = eCustomDataType(storage.data_type);
  const eAttrDomain domain = eAttrDomain(storage.domain);

  auto fn = std::make_shared<SampleIndexFunction>(std::move(geometry),
                                                  get_input_attribute_field(params, data_type),
                                                  domain,
                                                  bool(storage.clamp));
  auto op = FieldOperation::Create(std::move(fn), {params.extract_input<Field<int>>("Index")});
  output_attribute_field(params, GField(std::move(op)));
}

}  // namespace blender::nodes::node_geo_sample_index_cc

void register_node_type_geo_sample_index()
{
  namespace file_ns = blender::nodes::node_geo_sample_index_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SAMPLE_INDEX, "Sample Index", NODE_CLASS_GEOMETRY);
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.declare = file_ns::node_declare;
  node_type_storage(
      &ntype, "NodeGeometrySampleIndex", node_free_standard_storage, node_copy_standard_storage);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  nodeRegisterType(&ntype);
}
