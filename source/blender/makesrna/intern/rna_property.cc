/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Foundation (2008).
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/makesrna/intern/rna_property.c
 *  \ingroup RNA
 */

#include <stdlib.h>

#include "DNA_object_types.h"
#include "DNA_property_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

using namespace blender;

namespace blender {
const EnumPropertyItem rna_enum_gameproperty_type_items[] = {
    {GPROP_BOOL, "BOOL", 0, "Boolean", "Boolean Property"},
    {GPROP_INT, "INT", 0, "Integer", "Integer Property"},
    {GPROP_FLOAT, "FLOAT", 0, "Float", "Floating-Point Property"},
    {GPROP_STRING, "STRING", 0, "String", "String Property"},
    {GPROP_TIME, "TIMER", 0, "Timer", "Timer Property"},
    {GPROP_VECTOR, "VECTOR", 0, "Vector", "X, Y and Z values"},
    {GPROP_COLOR, "COLOR", 0, "Color", "Red, green, blue and alpha values"},
    {GPROP_OBJECT, "OBJECT", 0, "Object", "Another object"},
    {GPROP_CHOICE, "CHOICE", 0, "Choice", "One of a list of options"},
    {GPROP_LIST, "LIST", 0, "List", "Values of one type"},
    {GPROP_TABLE, "TABLE", 0, "Table", "Rows with the same named columns"},
    {0, nullptr, 0, nullptr, nullptr}};
}  // namespace blender

/** The types a List item or a Table column can have. */
static const EnumPropertyItem gameproperty_item_type_items[] = {
    {GPROP_BOOL, "BOOL", 0, "Boolean", ""},
    {GPROP_INT, "INT", 0, "Integer", ""},
    {GPROP_FLOAT, "FLOAT", 0, "Float", ""},
    {GPROP_STRING, "STRING", 0, "String", ""},
    {GPROP_VECTOR, "VECTOR", 0, "Vector", ""},
    {GPROP_COLOR, "COLOR", 0, "Color", ""},
    {GPROP_OBJECT, "OBJECT", 0, "Object", ""},
    {0, nullptr, 0, nullptr, nullptr}};

#ifdef RNA_RUNTIME

#  include "BKE_game_persistence.hh"
#  include "BKE_property.hh"

static void rna_GameProperty_persistence_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  bProperty *property = static_cast<bProperty *>(ptr->data);
  Object *object = reinterpret_cast<Object *>(ptr->owner_id);
  if (bmain != nullptr && property != nullptr && object != nullptr &&
      (property->flag & PROP_SAVE_GAME) != 0)
  {
    BKE_game_persistence_ensure(*bmain, scene, *object);
  }
}

static StructRNA *rna_GameProperty_refine(struct PointerRNA *ptr)
{
  bProperty *property = (bProperty *)ptr->data;

  switch (property->type) {
    case GPROP_BOOL:
      return RNA_GameBooleanProperty;
    case GPROP_INT:
      return RNA_GameIntProperty;
    case GPROP_FLOAT:
      return RNA_GameFloatProperty;
    case GPROP_STRING:
      return RNA_GameStringProperty;
    case GPROP_TIME:
      return RNA_GameTimerProperty;
    case GPROP_VECTOR:
      return RNA_GameVectorProperty;
    case GPROP_COLOR:
      return RNA_GameColorProperty;
    case GPROP_OBJECT:
      return RNA_GameObjectProperty;
    case GPROP_CHOICE:
      return RNA_GameChoiceProperty;
    case GPROP_LIST:
      return RNA_GameListProperty;
    case GPROP_TABLE:
      return RNA_GameTableProperty;
    default:
      return RNA_GameProperty;
  }
}

/* for both float and timer */
static float rna_GameFloatProperty_value_get(PointerRNA *ptr)
{
  bProperty *prop = (bProperty *)(ptr->data);
  return *(float *)(&prop->data);
}

static void rna_GameFloatProperty_value_set(PointerRNA *ptr, float value)
{
  bProperty *prop = (bProperty *)(ptr->data);
  CLAMP(value, -FLT_MAX, FLT_MAX);
  *(float *)(&prop->data) = value;
}

static void rna_GameProperty_type_set(PointerRNA *ptr, int value)
{
  bProperty *prop = (bProperty *)(ptr->data);
  if (prop->type == value) {
    return;
  }
  bProperty *owner = nullptr;
  BKE_bproperty_owner_list((Object *)ptr->owner_id, prop, &owner);
  if (owner != nullptr && BLI_findindex(&owner->columns, prop) == -1) {
    /* List items and Table cells have the type of their List or column. */
    return;
  }
  prop->type = value;
  BKE_bproperty_init(prop);
  if (owner != nullptr) {
    BKE_bproperty_table_sync(owner);
  }
}

static const EnumPropertyItem *rna_GameProperty_type_itemf(bContext * /*C*/,
                                                           PointerRNA *ptr,
                                                           PropertyRNA * /*prop*/,
                                                           bool *r_free)
{
  *r_free = false;
  const Object *ob = (const Object *)ptr->owner_id;
  if (ob != nullptr && ptr->data != nullptr && BLI_findindex(&ob->prop, ptr->data) == -1) {
    return gameproperty_item_type_items;
  }
  return rna_enum_gameproperty_type_items;
}

static void rna_GameProperty_name_set(PointerRNA *ptr, const char *value)
{
  Object *ob = (Object *)ptr->owner_id;
  bProperty *prop = (bProperty *)ptr->data;
  BLI_strncpy_utf8(prop->name, value, sizeof(prop->name));

  bProperty *owner = nullptr;
  ListBase *list = BKE_bproperty_owner_list(ob, prop, &owner);
  if (list == nullptr || (owner != nullptr && owner->type == GPROP_ROW)) {
    return;
  }
  const char *default_name = owner == nullptr                ? DATA_("Property") :
                             owner->type == GPROP_CHOICE     ? DATA_("Option") :
                             list == &owner->columns         ? DATA_("Column") :
                                                               DATA_("Item");
  BLI_uniquename(list, prop, default_name, '.', offsetof(bProperty, name), sizeof(prop->name));
}

static void rna_GameVectorProperty_value_get(PointerRNA *ptr, float *values)
{
  copy_v3_v3(values, ((bProperty *)ptr->data)->vec);
}

static void rna_GameVectorProperty_value_set(PointerRNA *ptr, const float *values)
{
  copy_v3_v3(((bProperty *)ptr->data)->vec, values);
}

static void rna_GameListProperty_item_type_set(PointerRNA *ptr, int value)
{
  BKE_bproperty_list_set_item_type((bProperty *)ptr->data, value);
}

static int rna_GameChoiceProperty_value_get(PointerRNA *ptr)
{
  return ((bProperty *)ptr->data)->data;
}

static void rna_GameChoiceProperty_value_set(PointerRNA *ptr, int value)
{
  ((bProperty *)ptr->data)->data = value;
}

static const EnumPropertyItem *rna_GameChoiceProperty_value_itemf(bContext * /*C*/,
                                                                  PointerRNA *ptr,
                                                                  PropertyRNA * /*prop*/,
                                                                  bool *r_free)
{
  const bProperty *prop = (const bProperty *)ptr->data;
  EnumPropertyItem *items = nullptr;
  int totitem = 0;
  int index = 0;
  for (const bProperty *option = (const bProperty *)prop->items.first; option;
       option = option->next, index++)
  {
    EnumPropertyItem item = {index, option->name, 0, option->name, ""};
    RNA_enum_item_add(&items, &totitem, &item);
  }
  if (totitem == 0) {
    *r_free = false;
    return rna_enum_dummy_NULL_items;
  }
  RNA_enum_item_end(&items, &totitem);
  *r_free = true;
  return items;
}

static void rna_GameStringProperty_value_get(PointerRNA *ptr, char *value)
{
  bProperty *prop = (bProperty *)ptr->data;
  if (prop->poin) {
    strcpy(value, (const char *)prop->poin);
  }
  else {
    value[0] = '\0';
  }
}

static int rna_GameStringProperty_value_length(PointerRNA *ptr)
{
  bProperty *prop = (bProperty *)ptr->data;

  if (prop->poin) {
    return strlen((const char *)prop->poin);
  }
  else {
    return 0;
  }
}

static void rna_GameStringProperty_value_set(PointerRNA *ptr, const char *value)
{
  bProperty *prop = (bProperty *)ptr->data;

  /* An empty text has no buffer of its own: a loaded or copied one points at the property's own
   * data, which is not to be freed. */
  if (prop->poin && prop->poin != &prop->data) {
    MEM_delete_void(prop->poin);
  }

  if (value[0]) {
    prop->poin = BLI_strdup(value);
  }
  else {
    prop->poin = nullptr;
  }
}

#else

namespace blender {

void RNA_def_gameproperty(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* Base Struct for GameProperty */
  srna = RNA_def_struct(brna, "GameProperty", nullptr);
  RNA_def_struct_ui_text(srna, "Game Property", "Game engine user defined object property");
  RNA_def_struct_sdna(srna, "bProperty");
  RNA_def_struct_refine_func(srna, "rna_GameProperty_refine");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(
      prop, "Name", "Available as GameObject attributes in the game engine's python API");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_string_funcs(prop, nullptr, nullptr, "rna_GameProperty_name_set");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_gameproperty_type_items);
  RNA_def_property_ui_text(prop, "Type", "");
  RNA_def_property_enum_funcs(
      prop, nullptr, "rna_GameProperty_type_set", "rna_GameProperty_type_itemf");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "show_debug", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PROP_DEBUG);
  RNA_def_property_ui_text(prop, "Debug", "Print debug information for this property");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "use_save_game", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PROP_SAVE_GAME);
  RNA_def_property_ui_text(
      prop, "Save Game", "Include this property in Save Game and Load Game operations");
  RNA_def_property_update(prop, NC_LOGIC, "rna_GameProperty_persistence_update");

  /* GameBooleanProperty */
  srna = RNA_def_struct(brna, "GameBooleanProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game Boolean Property", "Game engine user defined Boolean property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "data", 1);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameIntProperty */
  srna = RNA_def_struct(brna, "GameIntProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game Integer Property", "Game engine user defined integer number property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "data");
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_range(prop, -INT_MAX, INT_MAX);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameFloatProperty */
  srna = RNA_def_struct(brna, "GameFloatProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game Float Property", "Game engine user defined floating point number property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_NONE);
  /* RNA_def_property_float_sdna(prop, nullptr, "data"); */
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_float_funcs(
      prop, "rna_GameFloatProperty_value_get", "rna_GameFloatProperty_value_set", nullptr);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameTimerProperty */
  srna = RNA_def_struct(brna, "GameTimerProperty", "GameProperty");
  RNA_def_struct_ui_text(srna, "Game Timer Property", "Game engine user defined timer property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_NONE);
  /* RNA_def_property_float_sdna(prop, nullptr, "data"); */
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_float_funcs(
      prop, "rna_GameFloatProperty_value_get", "rna_GameFloatProperty_value_set", nullptr);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameStringProperty */
  srna = RNA_def_struct(brna, "GameStringProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game String Property", "Game engine user defined text string property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "poin");
  RNA_def_property_string_maxlength(prop, MAX_PROPSTRING);
  RNA_def_property_string_funcs(prop,
                                "rna_GameStringProperty_value_get",
                                "rna_GameStringProperty_value_length",
                                "rna_GameStringProperty_value_set");
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameVectorProperty */
  srna = RNA_def_struct(brna, "GameVectorProperty", "GameProperty");
  RNA_def_struct_ui_text(srna, "Game Vector Property", "Game engine user defined vector property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(
      prop, "rna_GameVectorProperty_value_get", "rna_GameVectorProperty_value_set", nullptr);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameColorProperty */
  srna = RNA_def_struct(brna, "GameColorProperty", "GameProperty");
  RNA_def_struct_ui_text(srna, "Game Color Property", "Game engine user defined color property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 4);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 1, 3);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameObjectProperty */
  srna = RNA_def_struct(brna, "GameObjectProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game Object Property", "Game engine user defined object reference property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "object");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_clear_flag(prop, PROP_ID_REFCOUNT);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameChoiceProperty */
  srna = RNA_def_struct(brna, "GameChoiceProperty", "GameProperty");
  RNA_def_struct_ui_text(
      srna, "Game Choice Property", "Game engine user defined property: one of its options");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "value", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_dummy_NULL_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_GameChoiceProperty_value_get",
                              "rna_GameChoiceProperty_value_set",
                              "rna_GameChoiceProperty_value_itemf");
  RNA_def_property_ui_text(prop, "Value", "The selected option, a string in the game");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "options", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "items", nullptr);
  RNA_def_property_struct_type(prop, "GameProperty");
  RNA_def_property_ui_text(prop, "Options", "The options to choose from (their names)");

  prop = RNA_def_property(srna, "active_option_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "active_item");
  RNA_def_property_ui_text(prop, "Active Option Index", "");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameListProperty */
  srna = RNA_def_struct(brna, "GameListProperty", "GameProperty");
  RNA_def_struct_ui_text(srna, "Game List Property", "Game engine user defined list of values");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "item_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "item_type");
  RNA_def_property_enum_items(prop, gameproperty_item_type_items);
  RNA_def_property_enum_funcs(prop, nullptr, "rna_GameListProperty_item_type_set", nullptr);
  RNA_def_property_ui_text(prop, "Item Type", "Type of the values in the list");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "elements", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "items", nullptr);
  RNA_def_property_struct_type(prop, "GameProperty");
  RNA_def_property_ui_text(prop, "Elements", "Values in the list");

  prop = RNA_def_property(srna, "active_element_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "active_item");
  RNA_def_property_ui_text(prop, "Active Element Index", "");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "inventory_rules", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "inventory_rules");
  RNA_def_property_ui_text(prop,
                           "Inventory Rules",
                           "The scene's Inventory Rules this list follows as an inventory "
                           "(Scene > Items and UI > Inventory Rules); empty: the defaults");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* GameTableRow */
  srna = RNA_def_struct(brna, "GameTableRow", nullptr);
  RNA_def_struct_ui_text(srna, "Game Table Row", "Row of a game engine table property");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "cells", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "items", nullptr);
  RNA_def_property_struct_type(prop, "GameProperty");
  RNA_def_property_ui_text(prop, "Cells", "One value per column, in column order");

  /* GameTableProperty */
  srna = RNA_def_struct(brna, "GameTableProperty", "GameProperty");
  RNA_def_struct_ui_text(srna,
                         "Game Table Property",
                         "Game engine user defined table: rows with the same named columns");
  RNA_def_struct_sdna(srna, "bProperty");

  prop = RNA_def_property(srna, "columns", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "columns", nullptr);
  RNA_def_property_struct_type(prop, "GameProperty");
  RNA_def_property_ui_text(
      prop, "Columns", "Name and type of each column, and the value new rows start with");

  prop = RNA_def_property(srna, "rows", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "items", nullptr);
  RNA_def_property_struct_type(prop, "GameTableRow");
  RNA_def_property_ui_text(prop, "Rows", "");

  prop = RNA_def_property(srna, "active_column_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "active_column");
  RNA_def_property_ui_text(prop, "Active Column Index", "");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "active_row_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "active_item");
  RNA_def_property_ui_text(prop, "Active Row Index", "");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "inventory_rules", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "inventory_rules");
  RNA_def_property_ui_text(prop,
                           "Inventory Rules",
                           "The scene's Inventory Rules this table follows as an inventory "
                           "(Scene > Items and UI > Inventory Rules); empty: the defaults");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);
}

}  // namespace blender

#endif
