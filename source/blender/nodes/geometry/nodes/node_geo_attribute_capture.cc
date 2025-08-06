/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_geo_capture_attribute.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "BLO_read_write.hh"

#include "BKE_library.hh"
#include "BKE_screen.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_attribute_capture_cc {

NODE_STORAGE_FUNCS(NodeGeometryAttributeCapture)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_default_layout();

  b.add_input<decl::Geometry>("Geometry")
      .description(
          "Geometry to evaluate the given fields and store the resulting attributes on. All "
          "geometry types except volumes are supported");
  b.add_output<decl::Geometry>("Geometry").propagate_all().align_with_previous();
  if (node != nullptr) {
    const NodeGeometryAttributeCapture &storage = node_storage(*node);
    for (const NodeGeometryAttributeCaptureItem &item :
         Span(storage.capture_items, storage.capture_items_num))
    {
      const eCustomDataType data_type = eCustomDataType(item.data_type);
      const std::string input_identifier =
          CaptureAttributeItemsAccessor::input_socket_identifier_for_item(item);
      const std::string output_identifier =
          CaptureAttributeItemsAccessor::output_socket_identifier_for_item(item);
      b.add_input(data_type, item.name, input_identifier)
          .field_on_all()
          .socket_name_ptr(&tree->id, CaptureAttributeItemsAccessor::item_srna, &item, "name");
      b.add_output(data_type, item.name, output_identifier).field_on_all().align_with_previous();
    }
  }
  b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Field);
  b.add_output<decl::Extend>("", "__extend__")
      .structure_type(StructureType::Field)
      .align_with_previous();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryAttributeCapture *data = MEM_callocN<NodeGeometryAttributeCapture>(__func__);
  data->domain = int8_t(AttrDomain::Point);
  node->storage = data;
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);

  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);

  if (uiLayout *panel = layout->panel(
          C, "capture_attribute_items", false, IFACE_("Capture Items")))
  {
    socket_items::ui::draw_items_list_with_operators<CaptureAttributeItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<CaptureAttributeItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "data_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<CaptureAttributeItemsAccessor>();
}

static void clean_unused_attributes(const AttributeFilter &attribute_filter,
                                    const Set<StringRef> &keep,
                                    GeometryComponent &component)
{
  std::optional<MutableAttributeAccessor> attributes = component.attributes_for_write();
  if (!attributes.has_value()) {
    return;
  }

  Vector<std::string> unused_ids;
  attributes->foreach_attribute([&](const bke::AttributeIter &iter) {
    if (!bke::attribute_name_is_anonymous(iter.name)) {
      return;
    }
    if (keep.contains(iter.name)) {
      return;
    }
    if (!attribute_filter.allow_skip(iter.name)) {
      return;
    }
    unused_ids.append(iter.name);
  });

  for (const std::string &unused_id : unused_ids) {
    attributes->remove(unused_id);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");

  if (!params.output_is_required("Geometry")) {
    params.error_message_add(
        NodeWarningType::Info,
        TIP_("The attribute output cannot be used without the geometry output"));
    params.set_default_remaining_outputs();
    return;
  }

  const NodeGeometryAttributeCapture &storage = node_storage(params.node());
  const AttrDomain domain = AttrDomain(storage.domain);

  Vector<const NodeGeometryAttributeCaptureItem *> used_items;
  Vector<GField> fields;
  Vector<std::string> attribute_id_ptrs;
  Set<StringRef> used_attribute_ids_set;
  for (const NodeGeometryAttributeCaptureItem &item :
       Span{storage.capture_items, storage.capture_items_num})
  {
    const std::string input_identifier =
        CaptureAttributeItemsAccessor::input_socket_identifier_for_item(item);
    const std::string output_identifier =
        CaptureAttributeItemsAccessor::output_socket_identifier_for_item(item);
    std::optional<std::string> attribute_id = params.get_output_anonymous_attribute_id_if_needed(
        output_identifier);
    if (!attribute_id) {
      continue;
    }
    used_attribute_ids_set.add(*attribute_id);
    fields.append(params.extract_input<GField>(input_identifier));
    attribute_id_ptrs.append(std::move(*attribute_id));
    used_items.append(&item);
  }

  if (fields.is_empty()) {
    params.set_output("Geometry", geometry_set);
    params.set_default_remaining_outputs();
    return;
  }

  Array<StringRef> attribute_ids(attribute_id_ptrs.size());
  for (const int i : attribute_id_ptrs.index_range()) {
    attribute_ids[i] = attribute_id_ptrs[i];
  }

  const auto capture_on = [&](GeometryComponent &component) {
    bke::try_capture_fields_on_geometry(component, attribute_ids, domain, fields);
    /* Changing of the anonymous attributes may require removing attributes that are no longer
     * needed. */
    clean_unused_attributes(
        params.get_attribute_filter("Geometry"), used_attribute_ids_set, component);
  };

  /* Run on the instances component separately to only affect the top level of instances. */
  if (domain == AttrDomain::Instance) {
    if (geometry_set.has_instances()) {
      capture_on(geometry_set.get_component_for_write(GeometryComponent::Type::Instance));
    }
  }
  else {
    static const Array<GeometryComponent::Type> types = {GeometryComponent::Type::Mesh,
                                                         GeometryComponent::Type::PointCloud,
                                                         GeometryComponent::Type::Curve,
                                                         GeometryComponent::Type::GreasePencil};

    geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
      for (const GeometryComponent::Type type : types) {
        if (geometry_set.has(type)) {
          capture_on(geometry_set.get_component_for_write(type));
        }
      }
    });
  }

  params.set_output("Geometry", geometry_set);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<CaptureAttributeItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<CaptureAttributeItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryAttributeCapture &src_storage = node_storage(*src_node);
  NodeGeometryAttributeCapture *dst_storage = MEM_dupallocN<NodeGeometryAttributeCapture>(
      __func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<CaptureAttributeItemsAccessor>(*src_node, *dst_node);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype type = eNodeSocketDatatype(params.other_socket().type);
  if (type == SOCK_GEOMETRY) {
    params.add_item(IFACE_("Geometry"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeCaptureAttribute");
      params.connect_available_socket(node, "Geometry");
    });
  }
  if (!CaptureAttributeItemsAccessor::supports_socket_type(type, params.node_tree().type)) {
    return;
  }

  params.add_item(IFACE_("Value"), [type](LinkSearchOpParams &params) {
    bNode &node = params.add_node("GeometryNodeCaptureAttribute");
    socket_items::add_item_with_socket_type_and_name<CaptureAttributeItemsAccessor>(
        params.node_tree, node, type, params.socket.name);
    params.update_and_connect_available_socket(node, params.socket.name);
  });
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  return &node.input_socket(output_socket.index());
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<CaptureAttributeItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<CaptureAttributeItemsAccessor>(&reader, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCaptureAttribute", GEO_NODE_CAPTURE_ATTRIBUTE);
  ntype.ui_name = "Capture Attribute";
  ntype.ui_description =
      "Store the result of a field on a geometry and output the data as a node socket. Allows "
      "remembering or interpolating data as the geometry changes, such as positions before "
      "deformation";
  ntype.enum_name_legacy = "CAPTURE_ATTRIBUTE";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryAttributeCapture", node_free_storage, node_copy_storage);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.internally_linked_input = node_internally_linked_input;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_attribute_capture_cc

namespace blender::nodes {

StructRNA *CaptureAttributeItemsAccessor::item_srna = &RNA_NodeGeometryCaptureAttributeItem;

void CaptureAttributeItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void CaptureAttributeItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
