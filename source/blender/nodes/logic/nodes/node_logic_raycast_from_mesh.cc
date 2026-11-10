/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BLI_vector.hh"
#include "DNA_logic_node_binding_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "ED_undo.hh"
#include "RNA_access.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::node_logic_raycast_from_mesh_cc {
namespace decl = blender::nodes::logic::decl;

enum Domain { Vertices = 0, Faces };
enum Direction { Normals = 0, InvertedNormals };

/* -------------------------------------------------------------------- */
/** \name Vertex Group field
 *
 * Drawn as Blender's modifier field: a text box whose dropdown lists the vertex groups of the
 * object the rays come from, any typed name (an attribute, a group of another object) still
 * accepted. Red only when no object that can run this node has the name: a typo for sure.
 * \{ */

static const bNodeSocket *find_input(const bNode &node, const char *identifier)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (STREQ(socket->identifier, identifier)) {
      return socket;
    }
  }
  return nullptr;
}

static bool tree_runs_on(const bNodeTree &tree, const Object &object)
{
  for (const LogicNodeBinding *binding =
           static_cast<const LogicNodeBinding *>(object.logic_node_bindings.first);
       binding != nullptr;
       binding = binding->next)
  {
    if (binding->tree == &tree) {
      return true;
    }
  }
  return false;
}

/**
 * The objects whose mesh the rays may come from: the Source Object, else every object this tree is
 * assigned to. `r_listed` is the one whose groups the dropdown shows: the Source Object, else the
 * active object when it uses the tree, else the first such mesh object. Nothing when Source
 * Object is linked (known only while playing).
 */
static void vertex_group_sources(const bNodeTree &tree,
                                 const bNode &node,
                                 const bContext &C,
                                 Object *&r_listed,
                                 Vector<Object *> &r_users)
{
  r_listed = nullptr;
  r_users.clear();
  const bNodeSocket *source = find_input(node, "Source Object");
  if (source == nullptr || source->is_directly_linked()) {
    return;
  }
  if (source->type == SOCK_OBJECT && source->default_value != nullptr) {
    if (Object *object = static_cast<const bNodeSocketValueObject *>(source->default_value)->value)
    {
      r_users.append(object);
      r_listed = object->type == OB_MESH ? object : nullptr;
      return;
    }
  }
  if (Main *bmain = CTX_data_main(&C)) {
    for (Object *object = static_cast<Object *>(bmain->objects.first); object != nullptr;
         object = static_cast<Object *>(object->id.next))
    {
      if (tree_runs_on(tree, *object)) {
        r_users.append(object);
      }
    }
  }
  Object *active = CTX_data_active_object(&C);
  if (active != nullptr && active->type == OB_MESH && r_users.contains(active)) {
    r_listed = active;
    return;
  }
  for (Object *object : r_users) {
    if (object->type == OB_MESH) {
      r_listed = object;
      return;
    }
  }
}

/** Whether the object's mesh has the vertex group or attribute, or may get it while evaluated
 * (a Geometry Nodes modifier can add attributes the original mesh lacks). */
static bool object_may_have_group(const Object &object, const char *name)
{
  if (object.type != OB_MESH || object.data == nullptr) {
    return false;
  }
  for (const ModifierData *modifier = static_cast<const ModifierData *>(object.modifiers.first);
       modifier != nullptr;
       modifier = modifier->next)
  {
    if (modifier->type == eModifierType_Nodes) {
      return true;
    }
  }
  const Mesh &mesh = *reinterpret_cast<const Mesh *>(object.data);
  return mesh.attributes().contains(name);
}

/** What the field's list needs to find its node again (it is filled while the list is open). */
struct VertexGroupSearchData {
  const bNodeTree *tree = nullptr;
  int32_t node_id = 0;
};

static char vertex_group_item;

/** The vertex groups, then the other attributes (Boolean or number, not Blender's own nor hidden
 * ones) of `object`'s mesh: what the node can cast from. */
struct VertexGroupItem {
  std::string name;
  int icon = ICON_NONE;
};
static Vector<VertexGroupItem> vertex_group_items(const Object &object)
{
  Vector<VertexGroupItem> items;
  if (object.type != OB_MESH || object.data == nullptr) {
    return items;
  }
  const Mesh &mesh = *reinterpret_cast<const Mesh *>(object.data);
  for (const bDeformGroup *group = static_cast<const bDeformGroup *>(mesh.vertex_group_names.first);
       group != nullptr;
       group = group->next)
  {
    items.append({group->name, ICON_GROUP_VERTEX});
  }
  const int groups = items.size();
  mesh.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.is_builtin || iter.name.startswith(".") ||
        !ELEM(iter.data_type,
              bke::AttrType::Bool,
              bke::AttrType::Int8,
              bke::AttrType::Int32,
              bke::AttrType::Float))
    {
      return;
    }
    for (int index = 0; index < groups; index++) {
      if (items[index].name == iter.name) {
        return;
      }
    }
    const int icon = iter.domain == bke::AttrDomain::Face  ? ICON_FACESEL :
                     iter.domain == bke::AttrDomain::Edge  ? ICON_EDGESEL :
                     iter.domain == bke::AttrDomain::Point ? ICON_VERTEXSEL :
                                                             ICON_NONE;
    items.append({iter.name, icon});
  });
  return items;
}

static void vertex_group_search_update(const bContext *C,
                                       void *data_v,
                                       const char *search_string,
                                       ui::SearchItems *items,
                                       const bool is_first)
{
  const VertexGroupSearchData &data = *static_cast<const VertexGroupSearchData *>(data_v);
  SpaceNode *space = C != nullptr ? CTX_wm_space_node(C) : nullptr;
  bNodeTree *tree = space != nullptr ? space->edittree : nullptr;
  if (tree == nullptr || tree != data.tree) {
    return;
  }
  tree->ensure_topology_cache();
  const bNode *node = tree->node_by_id(data.node_id);
  if (node == nullptr) {
    return;
  }
  Object *listed = nullptr;
  Vector<Object *> users;
  vertex_group_sources(*tree, *node, *C, listed, users);
  if (listed == nullptr) {
    return;
  }
  const Vector<VertexGroupItem> entries = vertex_group_items(*listed);
  ui::string_search::StringSearch<const VertexGroupItem> search;
  for (const VertexGroupItem &entry : entries) {
    search.add(entry.name, &entry);
  }
  const StringRef query = is_first ? StringRef() : StringRef(search_string);
  for (const VertexGroupItem *entry : search.query(query)) {
    if (!ui::search_item_add(items, entry->name, &vertex_group_item, entry->icon, 0, 0)) {
      break;
    }
  }
}

static void vertex_group_changed(bContext *C, void * /*arg1*/, void * /*arg2*/)
{
  ED_undo_push(C, "Set Vertex Group");
}

static void draw_vertex_group(CustomSocketDrawParams &params)
{
  if (params.socket.is_directly_linked()) {
    params.draw_standard(params.layout);
    return;
  }
  Object *listed = nullptr;
  Vector<Object *> users;
  vertex_group_sources(params.tree, params.node, params.C, listed, users);
  if (listed == nullptr) {
    params.draw_standard(params.layout);
    return;
  }

  const char *name = static_cast<const bNodeSocketValueString *>(params.socket.default_value)
                         ->value;
  bool found = name[0] == '\0';
  for (const Object *object : users) {
    found = found || object_may_have_group(*object, name);
  }

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label(params.label, ICON_NONE);
  /* The socket's own text, edited as text (any name is kept) with a list of what the object has:
   * Blender's search field (as a modifier's Vertex Group), red when no user has the name. */
  ui::Button *button = uiDefAutoButR(row.block(),
                                     &params.socket_ptr,
                                     RNA_struct_find_property(&params.socket_ptr, "default_value"),
                                     0,
                                     "",
                                     ICON_GROUP_VERTEX,
                                     0,
                                     0,
                                     10 * UI_UNIT_X,
                                     UI_UNIT_Y,
                                     ui::ButtonType::SearchMenu);
  VertexGroupSearchData *data = MEM_new<VertexGroupSearchData>(__func__);
  data->tree = &params.tree;
  data->node_id = params.node.identifier;
  /* Before the search is set: setting it checks the text against the listed object's items and
   * turns the field red unless any text is a valid result (the red is decided below instead). */
  button_func_search_set_results_are_suggestions(button, true);
  button_func_search_set(button,
                         nullptr,
                         vertex_group_search_update,
                         data,
                         true,
                         [](void *arg) { MEM_delete(static_cast<VertexGroupSearchData *>(arg)); },
                         nullptr,
                         nullptr);
  button_func_set(button, vertex_group_changed, nullptr, nullptr);
  if (!found) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
}

/** \} */

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; an unconnected query evaluates when an output is read");
  b.add_input<decl::Object>("Source Object"_ustr)
      .description(
          "Object whose mesh gives the rays, in the shape it had when the game started (an "
          "armature or shape keys do not move the rays); the logic tree's owner when empty");
  b.add_input<decl::String>("Vertex Group"_ustr)
      .description(
          "Cast only from this vertex group (or a Boolean or Float attribute of that name): "
          "its vertices with a weight above 0, or the faces whose corners are all in it. Max "
          "Rays spreads over the group only. Empty: the whole mesh. The list shows the groups of "
          "the Source Object (or of the selected object using this tree); any name can be typed. "
          "Red: no object using this tree has that name. A name the mesh does not have casts "
          "nothing (warned once)")
      .custom_draw(draw_vertex_group);
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("Additional object to exclude; the source object is always excluded");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Additional objects to exclude; invalid entries and duplicates are ignored");
  b.add_input<decl::Float>("Start Offset"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .description("Move each origin along its ray direction before casting to avoid self-intersection");
  b.add_input<decl::Float>("Max Distance"_ustr)
      .default_value(100.0f)
      .min(0.001f)
      .description("Maximum distance travelled by every generated ray in world units");
  b.add_input<decl::Int>("Max Rays"_ustr)
      .default_value(256)
      .min(1)
      .max(256)
      .description(
          "The most rays cast. A mesh with more faces (or vertices) than this casts from an even "
          "spread of them in Blender's order, the same ones every time: 3 rays on 4 faces use "
          "faces 0, 1 and 2");
  b.add_input<decl::String>("Property"_ustr)
      .description("Only report hits on objects with this game property; empty accepts any object");
  b.add_input<decl::Bool>("X-Ray"_ustr)
      .description("Skip objects that do not match the property filter instead of letting them block a ray");
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter shared by all generated rays; zero accepts all layers");
  b.add_input<decl::Bool>("Include Sensors"_ustr)
      .description("Allow Jolt sensor bodies to be returned");
  b.add_input<decl::Bool>("Hit Backfaces"_ustr)
      .description("Report back sides of one-sided triangle surfaces");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description("Draw every generated ray as a transient game-engine debug line");

  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the complete synchronous batch has finished");
  b.add_output<decl::Condition>("Any Hit"_ustr)
      .description("True when at least one generated ray has a matching hit");
  b.add_output<decl::Condition>("All Hit"_ustr)
      .description("True when at least one ray was generated and every ray has a matching hit");
  b.add_output<decl::Int>("Hit Count"_ustr)
      .description("Number of generated rays that have a matching hit");
  b.add_output<decl::Int>("Ray Count"_ustr)
      .description("Number of valid rays generated after filtering and Max Rays sampling");
  b.add_output<decl::List>("Hits"_ustr)
      .description("Per-ray matching-hit flags, one Boolean for every generated ray");
  b.add_output<decl::List>("Blocked"_ustr)
      .description(
          "Per-ray flags for nonmatching blockers when X-Ray is disabled, one Boolean for every generated ray");
  b.add_output<decl::List>("Hit Objects"_ustr)
      .description("Objects from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Points"_ustr)
      .description("World positions from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Normals"_ustr)
      .description("World normals from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Distances"_ustr)
      .description("Travel distances from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Fractions"_ustr)
      .description(
          "Normalized distances from matching hits only, in generated-ray order; length equals Hit Count");
  b.add_output<decl::List>("Face Indices"_ustr)
      .description(
          "Polygon indices from matching hits only, in generated-ray order; unavailable values are -1");
  b.add_output<decl::List>("Has UVs"_ustr)
      .description("UV-validity flags for matching hits only, in generated-ray order");
  b.add_output<decl::List>("UVs"_ustr)
      .description(
          "UV values for matching hits only as X/Y vectors with Z set to 0; invalid UVs are zero vectors");
  b.add_output<decl::List>("Ray Origins"_ustr)
      .description("Per-ray world origins after Start Offset is applied; length equals Ray Count");
  b.add_output<decl::List>("Ray Directions"_ustr)
      .description("Per-ray normalized world directions; length equals Ray Count");
  b.add_output<decl::List>("End Points"_ustr)
      .description("Per-ray furthest world points tested; length equals Ray Count");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mesh_domain", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "ray_direction", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Vertices;
  node->custom2 = Normals;
}

static void node_update(bNodeTree * /*tree*/, bNode *node)
{
  if (node->custom1 < Vertices || node->custom1 > Faces) {
    node->custom1 = Vertices;
  }
  if (node->custom2 < Normals || node->custom2 > InvertedNormals) {
    node->custom2 = Normals;
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRaycastFromMesh"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_raycast_from_mesh_cc
