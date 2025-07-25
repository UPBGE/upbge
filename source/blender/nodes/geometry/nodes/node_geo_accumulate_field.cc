/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "BLI_array.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_virtual_array.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_geo_accumulate_field_cc {

NODE_STORAGE_FUNCS(NodeAccumulateField)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node_storage(*node).data_type);
    BaseSocketDeclarationBuilder *value_declaration = nullptr;
    switch (data_type) {
      case CD_PROP_FLOAT3:
        value_declaration = &b.add_input<decl::Vector>("Value").default_value({1.0f, 1.0f, 1.0f});
        break;
      case CD_PROP_FLOAT:
        value_declaration = &b.add_input<decl::Float>("Value").default_value(1.0f);
        break;
      case CD_PROP_INT32:
        value_declaration = &b.add_input<decl::Int>("Value").default_value(1);
        break;
      case CD_PROP_FLOAT4X4:
        value_declaration = &b.add_input<decl::Matrix>("Value");
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
    value_declaration->supports_field().description("The values to be accumulated");
  }

  b.add_input<decl::Int>("Group ID", "Group Index")
      .supports_field()
      .hide_value()
      .description("An index used to group values together for multiple separate accumulations");

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node_storage(*node).data_type);
    b.add_output(data_type, "Leading")
        .field_source_reference_all()
        .description(
            "The running total of values in the corresponding group, starting at the first value");
    b.add_output(data_type, "Trailing")
        .field_source_reference_all()
        .description("The running total of values in the corresponding group, starting at zero");
    b.add_output(data_type, "Total")
        .field_source_reference_all()
        .description("The total of all of the values in the corresponding group");
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeAccumulateField *data = MEM_callocN<NodeAccumulateField>(__func__);
  data->data_type = CD_PROP_FLOAT;
  data->domain = int16_t(AttrDomain::Point);
  node->storage = data;
}

enum class AccumulationMode { Leading = 0, Trailing = 1 };

static std::optional<eCustomDataType> node_type_from_other_socket(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return CD_PROP_FLOAT;
    case SOCK_BOOLEAN:
    case SOCK_INT:
      return CD_PROP_INT32;
    case SOCK_VECTOR:
    case SOCK_RGBA:
    case SOCK_ROTATION:
      return CD_PROP_FLOAT3;
    case SOCK_MATRIX:
      return CD_PROP_FLOAT4X4;
    default:
      return std::nullopt;
  }
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  search_link_ops_for_declarations(params, declaration.inputs);

  const std::optional<eCustomDataType> type = node_type_from_other_socket(params.other_socket());
  if (!type) {
    return;
  }
  if (params.in_out() == SOCK_OUT) {
    params.add_item(
        IFACE_("Leading"),
        [type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeAccumulateField");
          node_storage(node).data_type = *type;
          params.update_and_connect_available_socket(node, "Leading");
        },
        0);
    params.add_item(
        IFACE_("Trailing"),
        [type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeAccumulateField");
          node_storage(node).data_type = *type;
          params.update_and_connect_available_socket(node, "Trailing");
        },
        -1);
    params.add_item(
        IFACE_("Total"),
        [type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeAccumulateField");
          node_storage(node).data_type = *type;
          params.update_and_connect_available_socket(node, "Total");
        },
        -2);
  }
  else {
    params.add_item(
        IFACE_("Value"),
        [type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeAccumulateField");
          node_storage(node).data_type = *type;
          params.update_and_connect_available_socket(node, "Value");
        },
        0);
  }
}

template<typename T> struct AccumulationInfo {
  static inline const T initial_value = []() {
    if constexpr (std::is_same_v<T, float4x4>) {
      return float4x4::identity();
    }
    else {
      return T();
    }
  }();

  static T accumulate(const T &a, const T &b)
  {
    if constexpr (std::is_same_v<T, float4x4>) {
      return a * b;
    }
    else {
      return a + b;
    }
  }
};

class AccumulateFieldInput final : public bke::GeometryFieldInput {
 private:
  GField input_;
  Field<int> group_index_;
  AttrDomain source_domain_;
  AccumulationMode accumulation_mode_;

 public:
  AccumulateFieldInput(const AttrDomain source_domain,
                       GField input,
                       Field<int> group_index,
                       AccumulationMode accumulation_mode)
      : bke::GeometryFieldInput(input.cpp_type(), "Accumulation"),
        input_(std::move(input)),
        group_index_(std::move(group_index)),
        source_domain_(source_domain),
        accumulation_mode_(accumulation_mode)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    const AttributeAccessor attributes = *context.attributes();
    const int64_t domain_size = attributes.domain_size(source_domain_);
    if (domain_size == 0) {
      return {};
    }

    const bke::GeometryFieldContext source_context{context, source_domain_};
    fn::FieldEvaluator evaluator{source_context, domain_size};
    evaluator.add(input_);
    evaluator.add(group_index_);
    evaluator.evaluate();
    const GVArray g_values = evaluator.get_evaluated(0);
    const VArray<int> group_indices = evaluator.get_evaluated<int>(1);

    GVArray g_output;

    bke::attribute_math::convert_to_static_type(g_values.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (is_same_any_v<T, int, float, float3, float4x4>) {
        Array<T> outputs(domain_size);
        const VArray<T> values = g_values.typed<T>();

        if (group_indices.is_single()) {
          T accumulation = AccumulationInfo<T>::initial_value;
          if (accumulation_mode_ == AccumulationMode::Leading) {
            for (const int i : values.index_range()) {
              accumulation = AccumulationInfo<T>::accumulate(accumulation, values[i]);
              outputs[i] = accumulation;
            }
          }
          else {
            for (const int i : values.index_range()) {
              outputs[i] = accumulation;
              accumulation = AccumulationInfo<T>::accumulate(accumulation, values[i]);
            }
          }
        }
        else {
          Map<int, T> accumulations;
          if (accumulation_mode_ == AccumulationMode::Leading) {
            for (const int i : values.index_range()) {
              T &accumulation_value = accumulations.lookup_or_add(
                  group_indices[i], AccumulationInfo<T>::initial_value);
              accumulation_value = AccumulationInfo<T>::accumulate(accumulation_value, values[i]);
              outputs[i] = accumulation_value;
            }
          }
          else {
            for (const int i : values.index_range()) {
              T &accumulation_value = accumulations.lookup_or_add(
                  group_indices[i], AccumulationInfo<T>::initial_value);
              outputs[i] = accumulation_value;
              accumulation_value = AccumulationInfo<T>::accumulate(accumulation_value, values[i]);
            }
          }
        }

        g_output = VArray<T>::from_container(std::move(outputs));
      }
    });

    return attributes.adapt_domain(std::move(g_output), source_domain_, context.domain());
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const final
  {
    input_.node().for_each_field_input_recursive(fn);
    group_index_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const override
  {
    return get_default_hash(input_, group_index_, source_domain_, accumulation_mode_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const AccumulateFieldInput *other_accumulate = dynamic_cast<const AccumulateFieldInput *>(
            &other))
    {
      return input_ == other_accumulate->input_ &&
             group_index_ == other_accumulate->group_index_ &&
             source_domain_ == other_accumulate->source_domain_ &&
             accumulation_mode_ == other_accumulate->accumulation_mode_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(
      const GeometryComponent & /*component*/) const override
  {
    return source_domain_;
  }
};

class TotalFieldInput final : public bke::GeometryFieldInput {
 private:
  GField input_;
  Field<int> group_index_;
  AttrDomain source_domain_;

 public:
  TotalFieldInput(const AttrDomain source_domain, GField input, Field<int> group_index)
      : bke::GeometryFieldInput(input.cpp_type(), "Total Value"),
        input_(std::move(input)),
        group_index_(std::move(group_index)),
        source_domain_(source_domain)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    const AttributeAccessor attributes = *context.attributes();
    const int64_t domain_size = attributes.domain_size(source_domain_);
    if (domain_size == 0) {
      return {};
    }

    const bke::GeometryFieldContext source_context{context, source_domain_};
    fn::FieldEvaluator evaluator{source_context, domain_size};
    evaluator.add(input_);
    evaluator.add(group_index_);
    evaluator.evaluate();
    const GVArray g_values = evaluator.get_evaluated(0);
    const VArray<int> group_indices = evaluator.get_evaluated<int>(1);

    GVArray g_outputs;

    bke::attribute_math::convert_to_static_type(g_values.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (is_same_any_v<T, int, float, float3, float4x4>) {
        const VArray<T> values = g_values.typed<T>();
        if (group_indices.is_single()) {
          T accumulation = AccumulationInfo<T>::initial_value;
          for (const int i : values.index_range()) {
            accumulation = AccumulationInfo<T>::accumulate(accumulation, values[i]);
          }
          g_outputs = VArray<T>::from_single(accumulation, domain_size);
        }
        else {
          Map<int, T> accumulations;
          for (const int i : values.index_range()) {
            T &value = accumulations.lookup_or_add(group_indices[i],
                                                   AccumulationInfo<T>::initial_value);
            value = AccumulationInfo<T>::accumulate(value, values[i]);
          }
          Array<T> outputs(domain_size);
          for (const int i : values.index_range()) {
            outputs[i] = accumulations.lookup(group_indices[i]);
          }
          g_outputs = VArray<T>::from_container(std::move(outputs));
        }
      }
    });

    return attributes.adapt_domain(std::move(g_outputs), source_domain_, context.domain());
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const final
  {
    input_.node().for_each_field_input_recursive(fn);
    group_index_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const override
  {
    return get_default_hash(input_, group_index_, source_domain_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const TotalFieldInput *other_field = dynamic_cast<const TotalFieldInput *>(&other)) {
      return input_ == other_field->input_ && group_index_ == other_field->group_index_ &&
             source_domain_ == other_field->source_domain_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(
      const GeometryComponent & /*component*/) const override
  {
    return source_domain_;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeAccumulateField &storage = node_storage(params.node());
  const AttrDomain source_domain = AttrDomain(storage.domain);

  const Field<int> group_index_field = params.extract_input<Field<int>>("Group Index");
  const GField input_field = params.extract_input<GField>("Value");
  if (params.output_is_required("Leading")) {
    params.set_output<GField>(
        "Leading",
        GField{std::make_shared<AccumulateFieldInput>(
            source_domain, input_field, group_index_field, AccumulationMode::Leading)});
  }
  if (params.output_is_required("Trailing")) {
    params.set_output<GField>(
        "Trailing",
        GField{std::make_shared<AccumulateFieldInput>(
            source_domain, input_field, group_index_field, AccumulationMode::Trailing)});
  }
  if (params.output_is_required("Total")) {
    params.set_output<GField>(
        "Total",
        GField{std::make_shared<TotalFieldInput>(source_domain, input_field, group_index_field)});
  }
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem items[] = {
      {CD_PROP_FLOAT, "FLOAT", 0, "Float", "Add floating point values"},
      {CD_PROP_INT32, "INT", 0, "Integer", "Add integer values"},
      {CD_PROP_FLOAT3, "FLOAT_VECTOR", 0, "Vector", "Add 3D vector values"},
      {CD_PROP_FLOAT4X4, "TRANSFORM", 0, "Transform", "Multiply transformation matrices"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Type of data that is accumulated",
                    items,
                    NOD_storage_enum_accessors(data_type),
                    CD_PROP_FLOAT);

  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "",
                    rna_enum_attribute_domain_items,
                    NOD_storage_enum_accessors(domain),
                    int(AttrDomain::Point),
                    nullptr,
                    true);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeAccumulateField", GEO_NODE_ACCUMULATE_FIELD);
  ntype.ui_name = "Accumulate Field";
  ntype.ui_description =
      "Add the values of an evaluated field together and output the running total for each "
      "element";
  ntype.enum_name_legacy = "ACCUMULATE_FIELD";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_type_storage(
      ntype, "NodeAccumulateField", node_free_standard_storage, node_copy_standard_storage);
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_accumulate_field_cc
