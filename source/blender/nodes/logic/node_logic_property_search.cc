/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The property name fields (2026-11-15): every input that names a game property (Get / Set Game
 * Property, Modify Property, the list and dictionary nodes, the Property filters of the rays and
 * On Collision...), a tree property or a global property is drawn as Blender's search field: a
 * text box whose dropdown lists the names there are, each with its type or use beside it. Any
 * typed name is kept (properties can be made while the game runs): the list only helps.
 *
 * Red only when the name is surely a mistake:
 * - a game property of an object: the objects the node acts on are known while editing (its
 *   Object input set, or empty: the objects this tree is applied to, when no Install Logic Tree
 *   node may give the tree to others) and none of them has it, or they are not known and no
 *   object of the file has it; and nothing else in the file names it (another node's property
 *   field, which may make it while the game runs, or live text such as "{Player.gold}");
 * - a filter: no object of the file has it and nothing else names it (the filter passes nothing);
 * - Get Tree Property: no Set or Toggle Tree Property of the same tree writes it (it then always
 *   gives the default; only the tree's own nodes can write its properties);
 * - a global property never: another .blend of the game (Start Other Game) or an earlier play
 *   (Persistent) may have set it.
 */

#include "node_logic_util.hh"

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_curve_types.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_property_types.h"
#include "DNA_space_types.h"

#include "ED_undo.hh"

#include "NOD_logic_merged.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::logic {

namespace {

enum class FieldKind : uint8_t {
  /** A game property of the object an Object input names. */
  ObjectProperty,
  /** A filter: only objects with this game property count. */
  Filter,
  /** A tree property (Get / Set / Toggle Tree Property). */
  TreeProperty,
  /** A global property's name, or its Category. */
  GlobalProperty,
  GlobalCategory,
};

/** What a field is (plain data: the pick gets a byte copy of it, see #FieldData). The socket
 * names are string literals of the node declarations. */
struct FieldSpec {
  FieldKind kind;
  /** ObjectProperty: the input naming the object. */
  const char *object_socket;
  /** ObjectProperty: where an empty object input leads: this input (Inventory's To Object leads
   * to Object), null: the object running the tree. */
  const char *fallback_socket;
  /** ObjectProperty: whether an empty object input surely means the objects this tree is applied
   * to (Object Grid View's means the player whose menu it is in first: not). */
  bool empty_is_owner;
  /** TreeProperty: the node reads the property (Get Tree Property): red when nothing writes it. */
  bool reads;
  /** GlobalProperty: the input naming the Category. */
  const char *category_socket;
};

/** What a field's dropdown and its pick need to find the field again. Trivial: Blender copies it
 * byte for byte for the pick (button_func_search_set with an exec function). */
struct FieldData {
  const bNodeTree *tree;
  int32_t node_id;
  char socket_identifier[64];
  FieldSpec spec;
};
static_assert(std::is_trivial_v<FieldData>);

/* -------------------------------------------------------------------- */
/** \name Sockets and objects
 * \{ */

const bNodeSocket *find_input(const bNode &node, const StringRef identifier)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
       socket != nullptr;
       socket = socket->next)
  {
    if (socket->identifier == identifier) {
      return socket;
    }
  }
  return nullptr;
}

/** The node's idname, or for a merged node the older node its option is (Install Logic Tree is
 * an option of the logic tree node). */
const char *node_kind(const bNode &node)
{
  if (const MergedNode *merged = find_merged_node(node.idname)) {
    return merged_option_idname(*merged, node);
  }
  return node.idname;
}

const char *string_value(const bNodeSocket &socket)
{
  return static_cast<const bNodeSocketValueString *>(socket.default_value)->value;
}

/** The text a string input holds while it is not linked (its name, typed in); null otherwise.
 * Only for the tree being drawn (its links are known). */
const char *typed_text(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_STRING || socket->default_value == nullptr ||
      socket->is_directly_linked() || !socket->is_available())
  {
    return nullptr;
  }
  return string_value(*socket);
}

bool tree_runs_on(const bNodeTree &tree, const Object &object)
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

const bProperty *find_property(const Object &object, const char *name)
{
  for (const bProperty *property = static_cast<const bProperty *>(object.prop.first);
       property != nullptr;
       property = property->next)
  {
    if (STREQ(property->name, name)) {
      return property;
    }
  }
  return nullptr;
}

bool any_object_has(const Main &bmain, const char *name)
{
  for (const Object &object : bmain.objects) {
    if (find_property(object, name) != nullptr) {
      return true;
    }
  }
  return false;
}

/** Whether an Install Logic Tree node may give `tree` to objects while the game runs (its Tree
 * Name is this tree's, or linked): the objects it is applied to are then not all its users. */
bool tree_may_be_installed(const Main &bmain, const bNodeTree &tree)
{
  for (const bNodeTree &other : bmain.nodetrees) {
    if (other.type != NTREE_LOGIC) {
      continue;
    }
    for (const bNode &node : other.nodes) {
      if (!STREQ(node_kind(node), "LogicNativeInstallLogicTree")) {
        continue;
      }
      const bNodeSocket *name = find_input(node, "Tree Name");
      if (name == nullptr || name->type != SOCK_STRING || name->default_value == nullptr) {
        continue;
      }
      /* (Another tree's links are not looked up: a linked name counts as any tree.) */
      if (name->link != nullptr || STREQ(string_value(*name), tree.id.name + 2)) {
        return true;
      }
    }
  }
  return false;
}

/** The objects a game property field acts on. `exact`: they surely are the ones (the object
 * input set, or the objects this tree is applied to: then `from_owner`, and sure only once
 * #settle_install found no Install Logic Tree node that may give the tree to others). */
struct Targets {
  Vector<const Object *> objects;
  bool exact = false;
  bool from_owner = false;
};

/** The owners are not all the objects running the tree when an Install Logic Tree node may give
 * it to others (a scan of every tree: done only where the answer is needed). */
void settle_install(Targets &targets, const Main &bmain, const bNodeTree &tree)
{
  if (targets.from_owner && targets.exact && tree_may_be_installed(bmain, tree)) {
    targets.exact = false;
  }
}

Targets field_targets(const bNodeTree &tree,
                      const bNode &node,
                      const FieldSpec &spec,
                      const bContext &C)
{
  Targets targets;
  const char *socket_name = spec.object_socket;
  for (int hop = 0; hop < 2 && socket_name != nullptr; hop++) {
    const bNodeSocket *socket = find_input(node, socket_name);
    if (socket == nullptr || socket->type != SOCK_OBJECT || socket->default_value == nullptr ||
        socket->is_directly_linked())
    {
      /* Linked: only the game knows the object. */
      return targets;
    }
    if (const Object *object =
            static_cast<const bNodeSocketValueObject *>(socket->default_value)->value)
    {
      targets.objects.append(object);
      targets.exact = true;
      return targets;
    }
    /* Empty: the input it leads to, else the object running the tree. */
    socket_name = hop == 0 ? spec.fallback_socket : nullptr;
  }
  const Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return targets;
  }
  for (const Object &object : bmain->objects) {
    if (tree_runs_on(tree, object)) {
      targets.objects.append(&object);
    }
  }
  /* The active object's first. */
  if (const Object *active = CTX_data_active_object(&C)) {
    const int64_t index = targets.objects.first_index_of_try(active);
    if (index > 0) {
      targets.objects.remove(index);
      targets.objects.prepend(active);
    }
  }
  targets.from_owner = true;
  targets.exact = spec.empty_is_owner && !targets.objects.is_empty();
  return targets;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Names used elsewhere
 * \{ */

/** Whether `socket` of `node` names a game property: the Property inputs of every node but the
 * tree and global property ones (and Evaluate Object Property reading an attribute). */
bool names_game_property(const bNode &node, const bNodeSocket &socket)
{
  if (!STREQ(socket.identifier, "Property") && !STREQ(socket.identifier, "To Property")) {
    return false;
  }
  const char *kind = node_kind(node);
  if (STREQ(kind, "LogicNativeEvaluateProperty")) {
    return node.custom2 == 0;
  }
  return !STR_ELEM(kind,
                   "LogicNativeGetTreeProperty",
                   "LogicNativeSetTreeProperty",
                   "LogicNativeToggleTreeProperty",
                   "LogicNativeGetGlobalProperty",
                   "LogicNativeSetGlobalProperty");
}

/** Whether live text names `name`: "{gold}", "{Player.gold}" or "{gold:1}" (the engine's rules,
 * LN_Manager::TextPropertyValue: ":N" decimals, the object before the last dot). */
bool live_text_names(const StringRef text, const StringRef name)
{
  int64_t start = text.find('{');
  while (start != StringRef::not_found) {
    const int64_t end = text.find('}', start + 1);
    if (end == StringRef::not_found) {
      return false;
    }
    StringRef token = text.substr(start + 1, end - start - 1);
    if (token.size() >= 2 && token[token.size() - 2] == ':' &&
        token[token.size() - 1] >= '0' && token[token.size() - 1] <= '9')
    {
      token = token.drop_suffix(2);
    }
    const int64_t dot = token.rfind('.');
    if (token == name ||
        (dot != StringRef::not_found && dot > 0 && token.substr(dot + 1) == name))
    {
      return true;
    }
    start = text.find('{', end + 1);
  }
  return false;
}

/** Whether anything in the file but the field `self` names the game property `name`: another
 * node's property field (it may make the property while the game runs) or live text (a text
 * object's, or a text a node writes). Links of other trees are not looked up: a linked field's
 * text counts too (it only keeps a field from turning red). */
bool name_used_elsewhere(const Main &bmain, const char *name, const bNodeSocket *self)
{
  for (const bNodeTree &tree : bmain.nodetrees) {
    if (tree.type != NTREE_LOGIC) {
      continue;
    }
    for (const bNode &node : tree.nodes) {
      for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first);
           socket != nullptr;
           socket = socket->next)
      {
        if (socket == self || socket->type != SOCK_STRING || socket->default_value == nullptr ||
            !socket->is_available())
        {
          continue;
        }
        const char *text = string_value(*socket);
        if (names_game_property(node, *socket) ? STREQ(text, name) :
                                                 live_text_names(text, name))
        {
          return true;
        }
      }
    }
  }
  for (const Object &object : bmain.objects) {
    if (object.type == OB_FONT && object.data != nullptr) {
      const Curve &text = *reinterpret_cast<const Curve *>(object.data);
      if (text.str != nullptr && live_text_names(text.str, name)) {
        return true;
      }
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The listed names
 * \{ */

StringRefNull property_type_name(const short type)
{
  const char *name = nullptr;
  if (!RNA_enum_name_from_value(rna_enum_gameproperty_type_items, type, &name)) {
    return "";
  }
  return IFACE_(name);
}

/** A name of the dropdown and what is shown beside it. */
struct ListedName {
  std::string name;
  std::string hint;
  /** A game property's type (GPROP_*) when every object having it agrees, else -1. */
  short type = -1;
};

/** The game properties of `objects`, by name, with their types and the objects having them. */
struct PropertyNames {
  struct Entry {
    std::string name;
    Vector<short> types;
    int objects = 0;
    const Object *first_object = nullptr;
  };
  Vector<Entry> entries;
  Map<std::string, int64_t> index;

  void add(const Object &object)
  {
    for (const bProperty *property = static_cast<const bProperty *>(object.prop.first);
         property != nullptr;
         property = property->next)
    {
      const int64_t at = index.lookup_or_add_cb(property->name, [&]() {
        entries.append({property->name, {}, 0, &object});
        return entries.size() - 1;
      });
      Entry &entry = entries[at];
      entry.types.append_non_duplicates(property->type);
      entry.objects++;
    }
  }

  /** The type ("Float"; "Float / Integer" when objects differ), with the one object having it
   * or how many when `say_objects`. */
  static std::string hint(const Entry &entry, const bool say_objects)
  {
    std::string text;
    for (const short type : entry.types) {
      text += (text.empty() ? "" : " / ") + std::string(property_type_name(type));
    }
    if (say_objects) {
      text += " · ";
      text += entry.objects == 1 ? std::string(entry.first_object->id.name + 2) :
                                   fmt::format(fmt::runtime(IFACE_("{} objects")), entry.objects);
    }
    return text;
  }
};

bool name_less(const std::string &a, const std::string &b)
{
  return BLI_strcasecmp_natural(a.c_str(), b.c_str()) < 0;
}

void sort_by_name(MutableSpan<PropertyNames::Entry> entries)
{
  std::stable_sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return name_less(a.name, b.name);
  });
}

/** A game property field's names: those of the objects it acts on (in their order, the active
 * object's first); not known exactly, after them every other object's of the file, by name. */
Vector<ListedName> object_property_names(const bNodeTree &tree,
                                         const bNode &node,
                                         const FieldSpec &spec,
                                         const bContext &C)
{
  Targets targets = spec.kind == FieldKind::ObjectProperty ? field_targets(tree, node, spec, C) :
                                                             Targets();
  if (const Main *bmain = CTX_data_main(&C)) {
    settle_install(targets, *bmain, tree);
  }
  PropertyNames names;
  for (const Object *object : targets.objects) {
    names.add(*object);
  }
  const int64_t own = names.entries.size();
  const bool whole_file = !targets.exact;
  if (whole_file) {
    if (const Main *bmain = CTX_data_main(&C)) {
      for (const Object &object : bmain->objects) {
        if (!targets.objects.contains(&object)) {
          names.add(object);
        }
      }
    }
    sort_by_name(names.entries.as_mutable_span().drop_front(own));
  }
  /* One known object: its types alone; else which objects have each. */
  const bool say_objects = whole_file || targets.objects.size() > 1;
  Vector<ListedName> listed;
  for (const PropertyNames::Entry &entry : names.entries) {
    listed.append({entry.name,
                   PropertyNames::hint(entry, say_objects),
                   entry.types.size() == 1 ? entry.types[0] : short(-1)});
  }
  return listed;
}

/** What the tree property nodes of `tree` do with each name. */
struct TreePropertyUse {
  std::string name;
  bool set = false;
  bool toggled = false;
  bool read = false;
};

Vector<TreePropertyUse> tree_property_uses(const bNodeTree &tree)
{
  Vector<TreePropertyUse> uses;
  for (const bNode *node : tree.all_nodes()) {
    const char *kind = node_kind(*node);
    const bool set = STREQ(kind, "LogicNativeSetTreeProperty");
    const bool toggled = STREQ(kind, "LogicNativeToggleTreeProperty");
    const bool read = STREQ(kind, "LogicNativeGetTreeProperty");
    if (!set && !toggled && !read) {
      continue;
    }
    const char *name = typed_text(*node, "Property");
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    TreePropertyUse *use = nullptr;
    for (TreePropertyUse &existing : uses) {
      if (existing.name == name) {
        use = &existing;
        break;
      }
    }
    if (use == nullptr) {
      uses.append({name});
      use = &uses.last();
    }
    use->set |= set;
    use->toggled |= toggled;
    use->read |= read;
  }
  std::stable_sort(uses.begin(), uses.end(), [](const auto &a, const auto &b) {
    return name_less(a.name, b.name);
  });
  return uses;
}

/** A tree property field's names: those this tree's nodes use; for a reading field only those
 * some node writes (a name only read is never set: listing it would offer a mistake). */
Vector<ListedName> tree_property_names(const bNodeTree &tree, const bool reads)
{
  Vector<ListedName> listed;
  for (const TreePropertyUse &use : tree_property_uses(tree)) {
    if (reads && !use.set && !use.toggled) {
      continue;
    }
    std::string hint;
    for (const auto &[used, word] : {std::pair{use.set, IFACE_("Set")},
                                     std::pair{use.toggled, IFACE_("Toggled")},
                                     std::pair{use.read, IFACE_("Read")}})
    {
      if (used) {
        hint += (hint.empty() ? "" : ", ") + std::string(word);
      }
    }
    listed.append({use.name, hint});
  }
  return listed;
}

/** The global properties the file's nodes but `self` (the field's own: its name is no choice)
 * name: (category, property, set by a node). Other trees' links are not looked up: a linked
 * name's text is listed too (rare; it only adds a name). */
struct GlobalName {
  std::string category;
  std::string property;
  bool set = false;
};

Vector<GlobalName> global_names(const Main &bmain, const bNode *self)
{
  Vector<GlobalName> names;
  for (const bNodeTree &tree : bmain.nodetrees) {
    if (tree.type != NTREE_LOGIC) {
      continue;
    }
    for (const bNode &node : tree.nodes) {
      if (&node == self) {
        continue;
      }
      const char *kind = node_kind(node);
      const bool set = STREQ(kind, "LogicNativeSetGlobalProperty");
      if (!set && !STR_ELEM(kind,
                            "LogicNativeGetGlobalProperty",
                            "LogicNativeListGlobalProperties"))
      {
        continue;
      }
      const bNodeSocket *category = find_input(node, "Category");
      const bNodeSocket *property = find_input(node, "Property");
      const auto text = [](const bNodeSocket *socket) {
        return socket != nullptr && socket->type == SOCK_STRING &&
                       socket->default_value != nullptr ?
                   std::string(string_value(*socket)) :
                   std::string();
      };
      GlobalName name{text(category), text(property), set};
      if (name.category.empty()) {
        continue;
      }
      bool merged = false;
      for (GlobalName &existing : names) {
        if (existing.category == name.category && existing.property == name.property) {
          existing.set |= name.set;
          merged = true;
          break;
        }
      }
      if (!merged) {
        names.append(std::move(name));
      }
    }
  }
  return names;
}

Vector<ListedName> global_property_names(const bNode &node, const FieldSpec &spec, const Main &bmain)
{
  /* The node's own Category, typed in: only its properties. */
  const char *category = spec.category_socket != nullptr ? typed_text(node, spec.category_socket) :
                                                           nullptr;
  const bool one_category = category != nullptr && category[0] != '\0';
  Vector<GlobalName> names = global_names(bmain, &node);
  std::stable_sort(names.begin(), names.end(), [](const GlobalName &a, const GlobalName &b) {
    return a.category != b.category ? name_less(a.category, b.category) :
                                      name_less(a.property, b.property);
  });
  Vector<ListedName> listed;
  for (const GlobalName &name : names) {
    if (name.property.empty() || (one_category && name.category != category)) {
      continue;
    }
    bool repeated = false;
    for (const ListedName &existing : listed) {
      repeated |= existing.name == name.property;
    }
    if (repeated && one_category) {
      continue;
    }
    std::string hint = name.set ? IFACE_("Set") : IFACE_("Read");
    if (!one_category) {
      hint = name.category + " · " + hint;
    }
    listed.append({name.property, hint});
  }
  return listed;
}

Vector<ListedName> global_category_names(const bNode &node, const Main &bmain)
{
  Vector<ListedName> listed;
  Vector<int> counts;
  for (const GlobalName &name : global_names(bmain, &node)) {
    int64_t at = -1;
    for (int64_t index = 0; index < listed.size(); index++) {
      if (listed[index].name == name.category) {
        at = index;
        break;
      }
    }
    if (at < 0) {
      listed.append({name.category, ""});
      counts.append(0);
      at = listed.size() - 1;
    }
    counts[at] += name.property.empty() ? 0 : 1;
  }
  for (int64_t index = 0; index < listed.size(); index++) {
    listed[index].hint = counts[index] == 1 ?
                             std::string(IFACE_("1 property")) :
                             fmt::format(fmt::runtime(IFACE_("{} properties")), counts[index]);
  }
  Vector<int64_t> order(listed.size());
  for (int64_t index = 0; index < listed.size(); index++) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(), [&](const int64_t a, const int64_t b) {
    return name_less(listed[a].name, listed[b].name);
  });
  Vector<ListedName> sorted;
  for (const int64_t index : order) {
    sorted.append(listed[index]);
  }
  return sorted;
}

Vector<ListedName> field_names(const bNodeTree &tree,
                               const bNode &node,
                               const FieldSpec &spec,
                               const bContext &C)
{
  const Main *bmain = CTX_data_main(&C);
  switch (spec.kind) {
    case FieldKind::ObjectProperty:
    case FieldKind::Filter:
      return object_property_names(tree, node, spec, C);
    case FieldKind::TreeProperty:
      return tree_property_names(tree, spec.reads);
    case FieldKind::GlobalProperty:
      return bmain != nullptr ? global_property_names(node, spec, *bmain) : Vector<ListedName>();
    case FieldKind::GlobalCategory:
      return bmain != nullptr ? global_category_names(node, *bmain) : Vector<ListedName>();
  }
  return {};
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Red
 * \{ */

/** Whether `name` in the field `socket` is surely a mistake (see the file's comment). */
bool field_name_wrong(const bNodeTree &tree,
                      const bNode &node,
                      const bNodeSocket &socket,
                      const FieldSpec &spec,
                      const bContext &C,
                      const char *name)
{
  if (name[0] == '\0') {
    return false;
  }
  const Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return false;
  }
  switch (spec.kind) {
    case FieldKind::ObjectProperty: {
      /* The cheap answer first: an object it acts on has it (drawn every redraw). */
      Targets targets = field_targets(tree, node, spec, C);
      for (const Object *object : targets.objects) {
        if (find_property(*object, name) != nullptr) {
          return false;
        }
      }
      settle_install(targets, *bmain, tree);
      if (!targets.exact && any_object_has(*bmain, name)) {
        return false;
      }
      return !name_used_elsewhere(*bmain, name, &socket);
    }
    case FieldKind::Filter:
      return !any_object_has(*bmain, name) && !name_used_elsewhere(*bmain, name, &socket);
    case FieldKind::TreeProperty: {
      if (!spec.reads) {
        return false;
      }
      for (const TreePropertyUse &use : tree_property_uses(tree)) {
        if (use.name == name) {
          return !use.set && !use.toggled;
        }
      }
      return true;
    }
    case FieldKind::GlobalProperty:
    case FieldKind::GlobalCategory:
      return false;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The field
 * \{ */

/** The names the open dropdown shows; its items carry their index + 1, so that the pick reads
 * the name back (the dropdown's text is the name, the separator and the hint). One dropdown is
 * open at a time. A std::vector: it lives until Blender exits, out of the guarded allocator. */
std::vector<ListedName> &shown_names()
{
  static std::vector<ListedName> names;
  return names;
}

int field_icon(const FieldKind kind)
{
  switch (kind) {
    case FieldKind::ObjectProperty:
      return ICON_PROPERTIES;
    case FieldKind::Filter:
      return ICON_FILTER;
    case FieldKind::TreeProperty:
      return ICON_NODETREE;
    case FieldKind::GlobalProperty:
      return ICON_WORLD;
    case FieldKind::GlobalCategory:
      return ICON_FILE_FOLDER;
  }
  return ICON_NONE;
}

/** The field's node in the edited tree; null when it is gone or another tree is edited. */
bNode *field_node(const bContext *C, const FieldData &data, bNodeTree *&r_tree)
{
  SpaceNode *space = C != nullptr ? CTX_wm_space_node(C) : nullptr;
  r_tree = space != nullptr ? space->edittree : nullptr;
  if (r_tree == nullptr || r_tree != data.tree) {
    return nullptr;
  }
  r_tree->ensure_topology_cache();
  return r_tree->node_by_id(data.node_id);
}

void field_search_update(const bContext *C,
                         void *data_v,
                         const char *search_string,
                         ui::SearchItems *items,
                         const bool is_first)
{
  const FieldData &data = *static_cast<const FieldData *>(data_v);
  bNodeTree *tree = nullptr;
  const bNode *node = field_node(C, data, tree);
  std::vector<ListedName> &shown = shown_names();
  shown.clear();
  if (node == nullptr) {
    return;
  }
  for (ListedName &listed : field_names(*tree, *node, data.spec, *C)) {
    shown.push_back(std::move(listed));
  }
  ui::string_search::StringSearch<const ListedName> search;
  for (const ListedName &listed : shown) {
    search.add(listed.name, &listed);
  }
  const int icon = field_icon(data.spec.kind);
  const StringRef query = is_first ? StringRef() : StringRef(search_string);
  for (const ListedName *listed : search.query(query)) {
    const int index = int(listed - shown.data());
    const bool hinted = !listed->hint.empty();
    const std::string text = hinted ? listed->name + UI_SEP_CHAR_S + listed->hint : listed->name;
    if (!ui::search_item_add(items,
                             text,
                             POINTER_FROM_INT(index + 1),
                             icon,
                             hinted ? ui::BUT_HAS_SEP_CHAR : 0,
                             0))
    {
      break;
    }
  }
}

/** Get / Set Game Property's Type for a game property type: Timer reads and writes its seconds
 * (Float), Choice its selected option (String), Table its rows (List); null for none. */
const char *game_property_option_of_type(const short type)
{
  switch (type) {
    case GPROP_BOOL:
      return "BOOLEAN";
    case GPROP_INT:
      return "INT";
    case GPROP_FLOAT:
    case GPROP_TIME:
      return "FLOAT";
    case GPROP_STRING:
    case GPROP_CHOICE:
      return "STRING";
    case GPROP_VECTOR:
      return "VECTOR";
    case GPROP_COLOR:
      return "COLOR";
    case GPROP_OBJECT:
      return "OBJECT";
    case GPROP_LIST:
    case GPROP_TABLE:
      return "LIST";
    default:
      return nullptr;
  }
}

/** A game property picked from Get / Set Game Property's list sets the node's Type to the
 * property's (2026-11-15): a Float read of a Vector property would give 0. Not while the Type is
 * Any (it takes every type as it is: the user's choice), nor for a name whose objects disagree.
 * Through RNA, as the dropdown sets it: the sockets follow, links kept by identifier. */
void follow_picked_type(bContext *C, bNodeTree &tree, bNode &node, const short type)
{
  if (!STR_ELEM(node.idname, "LogicNativeGetGamePropertyValue", "LogicNativeSetGamePropertyValue"))
  {
    return;
  }
  const MergedNode *merged = find_merged_node(node.idname);
  const char *wanted = game_property_option_of_type(type);
  if (merged == nullptr || wanted == nullptr) {
    return;
  }
  const int current = merged_option_index(*merged, node);
  if (STREQ(merged->options[current].identifier, "ANY")) {
    return;
  }
  for (const int option : merged->options.index_range()) {
    if (!STREQ(merged->options[option].identifier, wanted)) {
      continue;
    }
    if (option != current) {
      PointerRNA ptr = RNA_pointer_create_discrete(&tree.id, node.typeinfo->rna_ext.srna, &node);
      if (PropertyRNA *prop = RNA_struct_find_property(&ptr, "variant")) {
        RNA_property_enum_set(&ptr, prop, option);
        RNA_property_update(C, &ptr, prop);
      }
    }
    return;
  }
}

/** After a pick or a typed name: the search wrote the picked item's whole text (name, separator,
 * hint) into the socket, so the name alone is written back, and a game property's type becomes
 * Get / Set Game Property's Type (#follow_picked_type); one undo step. */
void field_search_exec(bContext *C, void *data_v, void *item_v)
{
  const FieldData &data = *static_cast<const FieldData *>(data_v);
  bNodeTree *tree = nullptr;
  bNode *node = field_node(C, data, tree);
  bNodeSocket *socket = node != nullptr ? const_cast<bNodeSocket *>(
                                              find_input(*node, data.socket_identifier)) :
                                          nullptr;
  const int index = POINTER_AS_INT(item_v) - 1;
  const std::vector<ListedName> &shown = shown_names();
  if (socket != nullptr && socket->type == SOCK_STRING && socket->default_value != nullptr &&
      index >= 0 && index < int(shown.size()))
  {
    const ListedName picked = shown[index];
    char *value = static_cast<bNodeSocketValueString *>(socket->default_value)->value;
    const std::string text = picked.hint.empty() ? picked.name :
                                                   picked.name + UI_SEP_CHAR_S + picked.hint;
    /* The socket holds the item's text: it was picked (a typed name is kept as typed). */
    if (value == text) {
      if (value != picked.name) {
        BLI_strncpy_utf8(value, picked.name.c_str(), sizeof(bNodeSocketValueString::value));
        BKE_ntree_update_tag_socket_property(tree, socket);
        BKE_main_ensure_invariants(*CTX_data_main(C), tree->id);
      }
      /* Last: the Type's change rebuilds the node's sockets. */
      if (data.spec.kind == FieldKind::ObjectProperty && picked.type >= 0) {
        follow_picked_type(C, *tree, *node, picked.type);
      }
    }
  }
  ED_undo_push(C, "Set Property Name");
}

void draw_field(CustomSocketDrawParams &params, const FieldSpec &spec)
{
  if (params.socket.is_directly_linked() || params.socket.type != SOCK_STRING ||
      params.socket.default_value == nullptr)
  {
    params.draw_standard(params.layout);
    return;
  }
  const bool wrong = field_name_wrong(
      params.tree, params.node, params.socket, spec, params.C, string_value(params.socket));

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label(params.label, ICON_NONE);
  /* The socket's own text, edited as text (any name is kept), with a list of names: Blender's
   * search field, as a modifier's Vertex Group. */
  ui::Button *button = uiDefAutoButR(row.block(),
                                     &params.socket_ptr,
                                     RNA_struct_find_property(&params.socket_ptr, "default_value"),
                                     0,
                                     "",
                                     field_icon(spec.kind),
                                     0,
                                     0,
                                     10 * UI_UNIT_X,
                                     UI_UNIT_Y,
                                     ui::ButtonType::SearchMenu);
  FieldData *data = MEM_new_zeroed<FieldData>(__func__);
  data->tree = &params.tree;
  data->node_id = params.node.identifier;
  STRNCPY(data->socket_identifier, params.socket.identifier);
  data->spec = spec;
  /* Before the search is set: setting it checks the text against the list at once and turns the
   * field red unless any text is a valid result (the red is decided above instead). */
  button_func_search_set_results_are_suggestions(button, true);
  /* The exec function owns the data (Blender frees it with the button, and gives the pick a
   * copy). */
  button_func_search_set(
      button, nullptr, field_search_update, data, true, nullptr, field_search_exec, nullptr);
  if (wrong) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
}

CustomSocketDrawFn make_field(const FieldSpec &spec)
{
  return [spec](CustomSocketDrawParams &params) { draw_field(params, spec); };
}

/** \} */

}  // namespace

CustomSocketDrawFn game_property_field(const char *object_socket,
                                       const char *fallback_socket,
                                       const bool empty_is_owner)
{
  return make_field(
      {FieldKind::ObjectProperty, object_socket, fallback_socket, empty_is_owner, false, nullptr});
}

CustomSocketDrawFn game_property_filter_field()
{
  return make_field({FieldKind::Filter, nullptr, nullptr, false, false, nullptr});
}

CustomSocketDrawFn tree_property_field(const bool reads)
{
  return make_field({FieldKind::TreeProperty, nullptr, nullptr, false, reads, nullptr});
}

CustomSocketDrawFn global_property_field(const char *category_socket)
{
  return make_field(
      {FieldKind::GlobalProperty, nullptr, nullptr, false, false, category_socket});
}

CustomSocketDrawFn global_category_field()
{
  return make_field({FieldKind::GlobalCategory, nullptr, nullptr, false, false, nullptr});
}

}  // namespace blender::nodes::logic
