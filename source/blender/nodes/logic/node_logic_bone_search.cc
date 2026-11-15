/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The bone nodes' name fields (2026-11-13): a Bone Name input drawn as Blender's own bone fields
 * (a constraint's Bone, a bone's Parent), a text box whose dropdown lists the armature's bones,
 * and a Constraint Name input listing that bone's constraints. Any typed name is still kept: the
 * list only helps. The field is red only when the name surely cannot be found: the armature the
 * node acts on is known while editing and has no such bone (or the bone no such constraint).
 *
 * Which armature: the node's object input when set; empty, the object running the tree (the
 * owner) for every bone node but Set Parent, so the armatures this tree is applied to; linked
 * (or Set Parent's empty Parent Object, or a tree applied to nothing yet), only the game knows:
 * the scene's armatures are listed and nothing is red. The engine needs an armature object itself
 * (LN's ArmatureFacade): a set or owning object that is no armature is red.
 */

#include "node_logic_util.hh"

#include <string>

#include "MEM_guardedalloc.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_vector.hh"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "ED_undo.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::nodes::logic {

namespace {

enum class NameField { Bone, Constraint };

/** What a field's dropdown needs to find its node again (it is filled while the list is open). */
struct NameFieldData {
  const bNodeTree *tree = nullptr;
  int32_t node_id = 0;
  NameField field = NameField::Bone;
  std::string armature_socket;
  /* Constraint: the input naming the bone. */
  std::string bone_socket;
  bool empty_is_owner = true;
};

/** A one-byte address the dropdown's items point at (the text is what is applied). */
char name_item;

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

/** The armatures a node may act on, and whether the field may be red (only when they surely are
 * the ones: the object input set, or the objects this tree is applied to). */
struct Armatures {
  Vector<Object *> objects;
  bool exact = false;
};

void add_scene_armatures(const bContext &C, Vector<Object *> &r_objects)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr) {
    return;
  }
  FOREACH_SCENE_OBJECT_BEGIN (scene, object) {
    if (object->type == OB_ARMATURE && object->data != nullptr) {
      r_objects.append_non_duplicates(object);
    }
  }
  FOREACH_SCENE_OBJECT_END;
}

Armatures node_armatures(const bNodeTree &tree,
                         const bNode &node,
                         const bContext &C,
                         const StringRef armature_socket,
                         const bool empty_is_owner)
{
  Armatures result;
  const bNodeSocket *socket = find_input(node, armature_socket);
  if (socket != nullptr && !socket->is_directly_linked() && socket->type == SOCK_OBJECT &&
      socket->default_value != nullptr)
  {
    if (Object *object = static_cast<const bNodeSocketValueObject *>(socket->default_value)->value)
    {
      /* That object, an armature or not: a bone of another object is never found. */
      if (object->type == OB_ARMATURE && object->data != nullptr) {
        result.objects.append(object);
      }
      result.exact = true;
      return result;
    }
    if (empty_is_owner) {
      Vector<Object *> users;
      if (Main *bmain = CTX_data_main(&C)) {
        for (Object *object = static_cast<Object *>(bmain->objects.first); object != nullptr;
             object = static_cast<Object *>(object->id.next))
        {
          if (tree_runs_on(tree, *object)) {
            users.append(object);
          }
        }
      }
      if (!users.is_empty()) {
        for (Object *object : users) {
          if (object->type == OB_ARMATURE && object->data != nullptr) {
            result.objects.append(object);
          }
        }
        result.exact = true;
      }
    }
  }
  if (!result.exact) {
    /* Known only in the game (a linked input, Set Parent's empty parent, a tree applied to
     * nothing yet): the scene's armatures, nothing red. */
    add_scene_armatures(C, result.objects);
  }
  /* The active object's first. */
  if (Object *active = CTX_data_active_object(&C)) {
    const int64_t index = result.objects.first_index_of_try(active);
    if (index > 0) {
      result.objects.remove(index);
      result.objects.prepend(active);
    }
  }
  return result;
}

void add_bone_names(const Bone &bone, Vector<std::string> &r_names)
{
  r_names.append_non_duplicates(bone.name);
  for (const Bone &child : bone.childbase) {
    add_bone_names(child, r_names);
  }
}

/** The armature's bone names in its hierarchy's order (edit bones while it is being edited). */
void add_bone_names(const Object &armature, Vector<std::string> &r_names)
{
  const bArmature &data = *reinterpret_cast<const bArmature *>(armature.data);
  if (data.edbo != nullptr) {
    for (const EditBone &bone : *data.edbo) {
      r_names.append_non_duplicates(bone.name);
    }
    return;
  }
  for (const Bone &bone : data.bonebase) {
    add_bone_names(bone, r_names);
  }
}

bool has_bone(const Object &armature, const char *name)
{
  bArmature &data = *reinterpret_cast<bArmature *>(armature.data);
  if (data.edbo != nullptr) {
    for (const EditBone &bone : *data.edbo) {
      if (STREQ(bone.name, name)) {
        return true;
      }
    }
    return false;
  }
  return BKE_armature_find_bone_name(&data, name) != nullptr;
}

/** The constraints of the pose bone `bone` (every pose bone's when `bone` is null). */
void add_constraint_names(const Object &armature, const char *bone, Vector<std::string> &r_names)
{
  if (armature.pose == nullptr) {
    return;
  }
  for (const bPoseChannel &channel : armature.pose->chanbase) {
    if (bone != nullptr && !STREQ(channel.name, bone)) {
      continue;
    }
    for (const bConstraint &constraint : channel.constraints) {
      r_names.append_non_duplicates(constraint.name);
    }
  }
}

const char *string_value(const bNodeSocket &socket)
{
  return static_cast<const bNodeSocketValueString *>(socket.default_value)->value;
}

/** The bone a Constraint field's bone input names; null when only the game knows it. */
const char *named_bone(const bNode &node, const StringRef bone_socket)
{
  const bNodeSocket *socket = find_input(node, bone_socket);
  if (socket == nullptr || socket->is_directly_linked() || socket->type != SOCK_STRING ||
      socket->default_value == nullptr || string_value(*socket)[0] == '\0')
  {
    return nullptr;
  }
  return string_value(*socket);
}

/** The names the field's dropdown lists. */
Vector<std::string> field_names(const NameFieldData &data,
                                const bNode &node,
                                const Armatures &armatures)
{
  Vector<std::string> names;
  for (const Object *armature : armatures.objects) {
    if (data.field == NameField::Bone) {
      add_bone_names(*armature, names);
    }
    else {
      add_constraint_names(*armature, named_bone(node, data.bone_socket), names);
    }
  }
  return names;
}

/** Whether `name` surely cannot be found: red. */
bool field_name_missing(const NameFieldData &data,
                        const bNode &node,
                        const Armatures &armatures,
                        const char *name)
{
  if (!armatures.exact || name[0] == '\0') {
    return false;
  }
  if (data.field == NameField::Bone) {
    for (const Object *armature : armatures.objects) {
      if (has_bone(*armature, name)) {
        return false;
      }
    }
    return true;
  }
  /* A constraint: red when its bone is there and holds no such constraint (a missing bone is
   * the Bone field's red), or, the bone known only in the game, when no bone holds one. */
  const char *bone = named_bone(node, data.bone_socket);
  bool bone_found = bone == nullptr;
  for (const Object *armature : armatures.objects) {
    if (bone != nullptr && !has_bone(*armature, bone)) {
      continue;
    }
    bone_found = true;
    Vector<std::string> constraints;
    add_constraint_names(*armature, bone, constraints);
    if (constraints.contains(name)) {
      return false;
    }
  }
  return bone_found;
}

void field_search_update(const bContext *C,
                         void *data_v,
                         const char *search_string,
                         ui::SearchItems *items,
                         const bool is_first)
{
  const NameFieldData &data = *static_cast<const NameFieldData *>(data_v);
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
  const Armatures armatures = node_armatures(
      *tree, *node, *C, data.armature_socket, data.empty_is_owner);
  const Vector<std::string> names = field_names(data, *node, armatures);
  ui::string_search::StringSearch<const std::string> search;
  for (const std::string &name : names) {
    search.add(name, &name);
  }
  const int icon = data.field == NameField::Bone ? ICON_BONE_DATA : ICON_CONSTRAINT_BONE;
  const StringRef query = is_first ? StringRef() : StringRef(search_string);
  for (const std::string *name : search.query(query)) {
    if (!ui::search_item_add(items, *name, &name_item, icon, 0, 0)) {
      break;
    }
  }
}

void draw_name_field(CustomSocketDrawParams &params, const NameFieldData &spec)
{
  if (params.socket.is_directly_linked() || params.socket.type != SOCK_STRING ||
      params.socket.default_value == nullptr)
  {
    params.draw_standard(params.layout);
    return;
  }
  const Armatures armatures = node_armatures(
      params.tree, params.node, params.C, spec.armature_socket, spec.empty_is_owner);
  const bool missing = field_name_missing(
      spec, params.node, armatures, string_value(params.socket));

  params.layout.alignment_set(ui::LayoutAlign::Expand);
  ui::Layout &row = params.layout.row(true);
  row.label(params.label, ICON_NONE);
  /* The socket's own text, edited as text (any name is kept), with a list of names: Blender's
   * search field, as a constraint's Bone. */
  ui::Button *button = uiDefAutoButR(row.block(),
                                     &params.socket_ptr,
                                     RNA_struct_find_property(&params.socket_ptr, "default_value"),
                                     0,
                                     "",
                                     spec.field == NameField::Bone ? ICON_BONE_DATA :
                                                                     ICON_CONSTRAINT_BONE,
                                     0,
                                     0,
                                     10 * UI_UNIT_X,
                                     UI_UNIT_Y,
                                     ui::ButtonType::SearchMenu);
  NameFieldData *data = MEM_new<NameFieldData>(__func__, spec);
  data->tree = &params.tree;
  data->node_id = params.node.identifier;
  /* Before the search is set: setting it checks the text against the list at once and turns the
   * field red unless any text is a valid result (the red is decided above instead). */
  button_func_search_set_results_are_suggestions(button, true);
  button_func_search_set(button,
                         nullptr,
                         field_search_update,
                         data,
                         true,
                         [](void *arg) { MEM_delete(static_cast<NameFieldData *>(arg)); },
                         nullptr,
                         nullptr);
  button_func_set(
      button,
      [](bContext *C, void *arg1, void * /*arg2*/) {
        ED_undo_push(C, static_cast<const char *>(arg1));
      },
      const_cast<char *>(spec.field == NameField::Bone ? "Set Bone Name" :
                                                         "Set Constraint Name"),
      nullptr);
  if (missing) {
    button_flag_enable(button, ui::BUT_REDALERT);
  }
}

}  // namespace

CustomSocketDrawFn bone_name_field(const StringRefNull armature_socket, const bool empty_is_owner)
{
  NameFieldData spec;
  spec.field = NameField::Bone;
  spec.armature_socket = armature_socket;
  spec.empty_is_owner = empty_is_owner;
  return [spec](CustomSocketDrawParams &params) { draw_name_field(params, spec); };
}

CustomSocketDrawFn bone_constraint_field(const StringRefNull armature_socket,
                                         const StringRefNull bone_socket)
{
  NameFieldData spec;
  spec.field = NameField::Constraint;
  spec.armature_socket = armature_socket;
  spec.bone_socket = bone_socket;
  return [spec](CustomSocketDrawParams &params) { draw_name_field(params, spec); };
}

}  // namespace blender::nodes::logic
