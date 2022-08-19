/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_geo_accumulate_field_cc {

NODE_STORAGE_FUNCS(NodeAccumulateField)

static void node_declare(NodeDeclarationBuilder &b)
{
  std::string value_in_description = "The values to be accumulated";
  std::string leading_out_description =
      "The running total of values in the corresponding group, starting at the first value";
  std::string trailing_out_description =
      "The running total of values in the corresponding group, starting at zero";
  std::string total_out_description = "The total of all of the values in the corresponding group";

  b.add_input<decl::Vector>(N_("Value"), "Value Vector")
      .default_value({1.0f, 1.0f, 1.0f})
      .supports_field()
      .description(N_(value_in_description));
  b.add_input<decl::Float>(N_("Value"), "Value Float")
      .default_value(1.0f)
      .supports_field()
      .description(N_(value_in_description));
  b.add_input<decl::Int>(N_("Value"), "Value Int")
      .default_value(1)
      .supports_field()
      .description(N_(value_in_description));
  b.add_input<decl::Int>(N_("Group Index"))
      .supports_field()
      .description(
          N_("An index used to group values together for multiple separate accumulations"));

  b.add_output<decl::Vector>(N_("Leading"), "Leading Vector")
      .field_source()
      .description(N_(leading_out_description));
  b.add_output<decl::Float>(N_("Leading"), "Leading Float")
      .field_source()
      .description(N_(leading_out_description));
  b.add_output<decl::Int>(N_("Leading"), "Leading Int")
      .field_source()
      .description(N_(leading_out_description));

  b.add_output<decl::Vector>(N_("Trailing"), "Trailing Vector")
      .field_source()
      .description(N_(trailing_out_description));
  b.add_output<decl::Float>(N_("Trailing"), "Trailing Float")
      .field_source()
      .description(N_(trailing_out_description));
  b.add_output<decl::Int>(N_("Trailing"), "Trailing Int")
      .field_source()
      .description(N_(trailing_out_description));

  b.add_output<decl::Vector>(N_("Total"), "Total Vector")
      .field_source()
      .description(N_(total_out_description));
  b.add_output<decl::Float>(N_("Total"), "Total Float")
      .field_source()
      .description(N_(total_out_description));
  b.add_output<decl::Int>(N_("Total"), "Total Int")
      .field_source()
      .description(N_(total_out_description));
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", 0, "", ICON_NONE);
  uiItemR(layout, ptr, "domain", 0, "", ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeAccumulateField *data = MEM_cnew<NodeAccumulateField>(__func__);
  data->data_type = CD_PROP_FLOAT;
  data->domain = ATTR_DOMAIN_POINT;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeAccumulateField &storage = node_storage(*node);
  const eCustomDataType data_type = static_cast<eCustomDataType>(storage.data_type);

  bNodeSocket *sock_in_vector = (bNodeSocket *)node->inputs.first;
  bNodeSocket *sock_in_float = sock_in_vector->next;
  bNodeSocket *sock_in_int = sock_in_float->next;

  bNodeSocket *sock_out_vector = (bNodeSocket *)node->outputs.first;
  bNodeSocket *sock_out_float = sock_out_vector->next;
  bNodeSocket *sock_out_int = sock_out_float->next;

  bNodeSocket *sock_out_first_vector = sock_out_int->next;
  bNodeSocket *sock_out_first_float = sock_out_first_vector->next;
  bNodeSocket *sock_out_first_int = sock_out_first_float->next;
  bNodeSocket *sock_out_total_vector = sock_out_first_int->next;
  bNodeSocket *sock_out_total_float = sock_out_total_vector->next;
  bNodeSocket *sock_out_total_int = sock_out_total_float->next;

  nodeSetSocketAvailability(ntree, sock_in_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, sock_in_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, sock_in_int, data_type == CD_PROP_INT32);

  nodeSetSocketAvailability(ntree, sock_out_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, sock_out_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, sock_out_int, data_type == CD_PROP_INT32);

  nodeSetSocketAvailability(ntree, sock_out_first_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, sock_out_first_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, sock_out_first_int, data_type == CD_PROP_INT32);

  nodeSetSocketAvailability(ntree, sock_out_total_vector, data_type == CD_PROP_FLOAT3);
  nodeSetSocketAvailability(ntree, sock_out_total_float, data_type == CD_PROP_FLOAT);
  nodeSetSocketAvailability(ntree, sock_out_total_int, data_type == CD_PROP_INT32);
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
      return CD_PROP_FLOAT3;
    default:
      return {};
  }
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
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

    params.add_item(
        IFACE_("Group Index"),
        [type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeAccumulateField");
          node_storage(node).data_type = *type;
          params.update_and_connect_available_socket(node, "Group Index");
        },
        -1);
  }
}

template<typename T> class AccumulateFieldInput final : public GeometryFieldInput {
 private:
  Field<T> input_;
  Field<int> group_index_;
  eAttrDomain source_domain_;
  AccumulationMode accumulation_mode_;

 public:
  AccumulateFieldInput(const eAttrDomain source_domain,
                       Field<T> input,
                       Field<int> group_index,
                       AccumulationMode accumulation_mode)
      : GeometryFieldInput(CPPType::get<T>(), "Accumulation"),
        input_(input),
        group_index_(group_index),
        source_domain_(source_domain),
        accumulation_mode_(accumulation_mode)
  {
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 IndexMask UNUSED(mask)) const final
  {
    const GeometryComponentFieldContext field_context{component, source_domain_};
    const int domain_size = component.attribute_domain_size(field_context.domain());
    if (domain_size == 0) {
      return {};
    }
    const AttributeAccessor attributes = *component.attributes();

    fn::FieldEvaluator evaluator{field_context, domain_size};
    evaluator.add(input_);
    evaluator.add(group_index_);
    evaluator.evaluate();
    const VArray<T> values = evaluator.get_evaluated<T>(0);
    const VArray<int> group_indices = evaluator.get_evaluated<int>(1);

    Array<T> accumulations_out(domain_size);

    if (group_indices.is_single()) {
      T accumulation = T();
      if (accumulation_mode_ == AccumulationMode::Leading) {
        for (const int i : values.index_range()) {
          accumulation = values[i] + accumulation;
          accumulations_out[i] = accumulation;
        }
      }
      else {
        for (const int i : values.index_range()) {
          accumulations_out[i] = accumulation;
          accumulation = values[i] + accumulation;
        }
      }
    }
    else {
      Map<int, T> accumulations;
      if (accumulation_mode_ == AccumulationMode::Leading) {
        for (const int i : values.index_range()) {
          T &accumulation_value = accumulations.lookup_or_add_default(group_indices[i]);
          accumulation_value += values[i];
          accumulations_out[i] = accumulation_value;
        }
      }
      else {
        for (const int i : values.index_range()) {
          T &accumulation_value = accumulations.lookup_or_add_default(group_indices[i]);
          accumulations_out[i] = accumulation_value;
          accumulation_value += values[i];
        }
      }
    }

    return attributes.adapt_domain<T>(
        VArray<T>::ForContainer(std::move(accumulations_out)), source_domain_, domain);
  }

  uint64_t hash() const override
  {
    return get_default_hash_4(input_, group_index_, source_domain_, accumulation_mode_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const AccumulateFieldInput *other_accumulate = dynamic_cast<const AccumulateFieldInput *>(
            &other)) {
      return input_ == other_accumulate->input_ &&
             group_index_ == other_accumulate->group_index_ &&
             source_domain_ == other_accumulate->source_domain_ &&
             accumulation_mode_ == other_accumulate->accumulation_mode_;
    }
    return false;
  }
};

template<typename T> class TotalFieldInput final : public GeometryFieldInput {
 private:
  Field<T> input_;
  Field<int> group_index_;
  eAttrDomain source_domain_;

 public:
  TotalFieldInput(const eAttrDomain source_domain, Field<T> input, Field<int> group_index)
      : GeometryFieldInput(CPPType::get<T>(), "Total Value"),
        input_(input),
        group_index_(group_index),
        source_domain_(source_domain)
  {
  }

  GVArray get_varray_for_context(const GeometryComponent &component,
                                 const eAttrDomain domain,
                                 IndexMask UNUSED(mask)) const final
  {
    const GeometryComponentFieldContext field_context{component, source_domain_};
    const int domain_size = component.attribute_domain_size(field_context.domain());
    if (domain_size == 0) {
      return {};
    }
    const AttributeAccessor attributes = *component.attributes();

    fn::FieldEvaluator evaluator{field_context, domain_size};
    evaluator.add(input_);
    evaluator.add(group_index_);
    evaluator.evaluate();
    const VArray<T> values = evaluator.get_evaluated<T>(0);
    const VArray<int> group_indices = evaluator.get_evaluated<int>(1);

    if (group_indices.is_single()) {
      T accumulation = T();
      for (const int i : values.index_range()) {
        accumulation = values[i] + accumulation;
      }
      return VArray<T>::ForSingle(accumulation, domain_size);
    }

    Array<T> accumulations_out(domain_size);
    Map<int, T> accumulations;
    for (const int i : values.index_range()) {
      T &value = accumulations.lookup_or_add_default(group_indices[i]);
      value = value + values[i];
    }
    for (const int i : values.index_range()) {
      accumulations_out[i] = accumulations.lookup(group_indices[i]);
    }

    return attributes.adapt_domain<T>(
        VArray<T>::ForContainer(std::move(accumulations_out)), source_domain_, domain);
  }

  uint64_t hash() const override
  {
    return get_default_hash_3(input_, group_index_, source_domain_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const TotalFieldInput *other_field = dynamic_cast<const TotalFieldInput *>(&other)) {
      return input_ == other_field->input_ && group_index_ == other_field->group_index_ &&
             source_domain_ == other_field->source_domain_;
    }
    return false;
  }
};

template<typename T> std::string identifier_suffix()
{
  if constexpr (std::is_same_v<T, int>) {
    return "Int";
  }
  if constexpr (std::is_same_v<T, float>) {
    return "Float";
  }
  if constexpr (std::is_same_v<T, float3>) {
    return "Vector";
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeAccumulateField &storage = node_storage(params.node());
  const eCustomDataType data_type = static_cast<eCustomDataType>(storage.data_type);
  const eAttrDomain source_domain = static_cast<eAttrDomain>(storage.domain);

  Field<int> group_index_field = params.extract_input<Field<int>>("Group Index");
  attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
    using T = decltype(dummy);
    if constexpr (std::is_same_v<T, int> || std::is_same_v<T, float> ||
                  std::is_same_v<T, float3>) {
      const std::string suffix = " " + identifier_suffix<T>();
      Field<T> input_field = params.extract_input<Field<T>>("Value" + suffix);
      if (params.output_is_required("Leading" + suffix)) {
        params.set_output(
            "Leading" + suffix,
            Field<T>{std::make_shared<AccumulateFieldInput<T>>(
                source_domain, input_field, group_index_field, AccumulationMode::Leading)});
      }
      if (params.output_is_required("Trailing" + suffix)) {
        params.set_output(
            "Trailing" + suffix,
            Field<T>{std::make_shared<AccumulateFieldInput<T>>(
                source_domain, input_field, group_index_field, AccumulationMode::Trailing)});
      }
      if (params.output_is_required("Total" + suffix)) {
        params.set_output("Total" + suffix,
                          Field<T>{std::make_shared<TotalFieldInput<T>>(
                              source_domain, input_field, group_index_field)});
      }
    }
  });
}
}  // namespace blender::nodes::node_geo_accumulate_field_cc

void register_node_type_geo_accumulate_field()
{
  namespace file_ns = blender::nodes::node_geo_accumulate_field_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_ACCUMULATE_FIELD, "Accumulate Field", NODE_CLASS_CONVERTER);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  node_type_init(&ntype, file_ns::node_init);
  node_type_update(&ntype, file_ns::node_update);
  ntype.draw_buttons = file_ns::node_layout;
  ntype.declare = file_ns::node_declare;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  node_type_storage(
      &ntype, "NodeAccumulateField", node_free_standard_storage, node_copy_standard_storage);
  nodeRegisterType(&ntype);
}
