/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file rna_logic_node_binding.cc
 *  \ingroup RNA
 */

#include "BLI_string.h"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "BKE_main.hh"

#include "DNA_ID.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "WM_types.hh"

#include "rna_internal.hh"

using namespace blender;

#ifdef RNA_RUNTIME

#include "BLI_listbase.h"
#include "BKE_global.hh"
#include "BKE_logic_node_binding.hh"
#include "BKE_report.hh"

#include "WM_api.hh"

namespace blender {

static bNodeTree *logic_node_binding_find_tree(Main *bmain, const char *tree_name)
{
  if (bmain == nullptr || tree_name == nullptr || tree_name[0] == '\0') {
    return nullptr;
  }

  bNodeTree *node_tree = reinterpret_cast<bNodeTree *>(
      BLI_findstring(&bmain->nodetrees, tree_name, offsetof(ID, name) + 2));
  if (node_tree != nullptr && node_tree->type != NTREE_LOGIC) {
    return nullptr;
  }
  return node_tree;
}

static PointerRNA rna_LogicNodeBinding_tree_get(PointerRNA *ptr)
{
  LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(ptr->data);
  if (G_MAIN == nullptr) {
    return PointerRNA_NULL;
  }

  bNodeTree *node_tree = logic_node_binding_find_tree(G_MAIN, binding->tree_name);
  if (node_tree == nullptr) {
    return PointerRNA_NULL;
  }

  return RNA_id_pointer_create(&node_tree->id);
}

static void rna_LogicNodeBinding_tree_set(PointerRNA *ptr, PointerRNA value, ReportList *reports)
{
  LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(ptr->data);
  if (value.data == nullptr) {
    binding->tree_name[0] = '\0';
    return;
  }

  bNodeTree *node_tree = static_cast<bNodeTree *>(value.data);
  if (node_tree->type != NTREE_LOGIC) {
    BKE_report(reports, RPT_ERROR, "Logic node bindings require a LogicNodeTree");
    return;
  }

  BLI_strncpy(binding->tree_name, node_tree->id.name + 2, sizeof(binding->tree_name));
}

static void rna_LogicNodeBinding_tree_name_set(PointerRNA *ptr, const char *value)
{
  LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(ptr->data);
  BLI_strncpy(binding->tree_name, value, sizeof(binding->tree_name));
}

static std::optional<std::string> rna_LogicNodeBinding_path(const PointerRNA *ptr)
{
  const LogicNodeBinding *binding = static_cast<const LogicNodeBinding *>(ptr->data);
  if (binding->tree_name[0] == '\0') {
    return "Logic Node Binding";
  }
  return std::string("Logic Node Binding: ") + binding->tree_name;
}

static void rna_LogicNodeBinding_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  WM_main_add_notifier(NC_LOGIC, nullptr);
}

LogicNodeBinding *rna_GameObjectSettings_logic_node_binding_new(Object *object)
{
  if (object == nullptr) {
    return nullptr;
  }
  LogicNodeBinding *binding = BKE_logic_node_binding_add(object);
  WM_main_add_notifier(NC_LOGIC, object);
  return binding;
}

void rna_GameObjectSettings_logic_node_binding_remove(Object *object, int index)
{
  if (object == nullptr || index < 0) {
    return;
  }
  LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(
      BLI_findlink(&object->logic_node_bindings, index));
  if (binding != nullptr) {
    BKE_logic_node_binding_remove(object, binding);
    WM_main_add_notifier(NC_LOGIC, object);
  }
}

void rna_GameObjectSettings_logic_node_binding_clear(Object *object)
{
  if (object == nullptr) {
    return;
  }
  BKE_logic_node_binding_free_list(&object->logic_node_bindings);
  WM_main_add_notifier(NC_LOGIC, object);
}

}  /* namespace blender */

#else

namespace blender {

void RNA_def_logic_node_binding(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "LogicNodeBinding", nullptr);
  RNA_def_struct_sdna(srna, "LogicNodeBinding");
  RNA_def_struct_ui_text(srna, "Logic Node Binding", "Native logic node tree applied to an object");
  RNA_def_struct_ui_icon(srna, ICON_NODETREE);
  RNA_def_struct_path_func(srna, "rna_LogicNodeBinding_path");

  prop = RNA_def_property(srna, "tree_name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "tree_name");
  RNA_def_property_string_maxlength(prop, sizeof(LogicNodeBinding::tree_name));
  RNA_def_property_ui_text(prop, "Tree Name", "Name of the applied LogicNodeTree");
  RNA_def_property_string_funcs(prop, nullptr, nullptr, "rna_LogicNodeBinding_tree_name_set");
  RNA_def_property_update(prop, NC_LOGIC, "rna_LogicNodeBinding_update");

  prop = RNA_def_property(srna, "tree", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "LogicNodeTree");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_pointer_funcs(
      prop, "rna_LogicNodeBinding_tree_get", "rna_LogicNodeBinding_tree_set", nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Tree", "Logic node tree datablock applied to this object");
  RNA_def_property_update(prop, NC_LOGIC, "rna_LogicNodeBinding_update");

  prop = RNA_def_property(srna, "enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "enabled", 0);
  RNA_def_property_ui_text(prop, "Enabled", "Whether this logic tree starts enabled at runtime");
  RNA_def_property_update(prop, NC_LOGIC, "rna_LogicNodeBinding_update");
}

void RNA_def_logic_node_binding_collection(BlenderRNA * /*brna*/, StructRNA *game_settings_srna)
{
  PropertyRNA *prop;

  prop = RNA_def_property(game_settings_srna, "logic_node_bindings", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "logic_node_bindings", nullptr);
  RNA_def_property_struct_type(prop, "LogicNodeBinding");
  RNA_def_property_ui_text(
      prop, "Logic Node Trees", "Native logic node trees applied to this game object");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(game_settings_srna,
                          "logic_node_binding_new",
                          "rna_GameObjectSettings_logic_node_binding_new");
  RNA_def_function_ui_description(func, "Add a native logic node tree binding");
  parm = RNA_def_pointer(
      func, "binding", "LogicNodeBinding", "", "Newly created logic node binding");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(game_settings_srna,
                          "logic_node_binding_remove",
                          "rna_GameObjectSettings_logic_node_binding_remove");
  RNA_def_function_ui_description(func, "Remove a native logic node tree binding");
  parm = RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Binding index", 0, 10000);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(game_settings_srna,
                          "logic_node_binding_clear",
                          "rna_GameObjectSettings_logic_node_binding_clear");
  RNA_def_function_ui_description(func, "Remove all native logic node tree bindings");
}

}  // namespace blender

#endif
