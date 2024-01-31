/**
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
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include <stdlib.h>

#include "RNA_define.hh"

#include "DNA_property_types.h"
#include "DNA_python_proxy_types.h"
#include "rna_internal.hh"

#include "WM_types.hh"

#ifdef RNA_RUNTIME

static StructRNA *rna_PythonProxyProperty_refine(struct PointerRNA *ptr)
{
  PythonProxyProperty *pprop = (PythonProxyProperty *)ptr->data;

  switch (pprop->type) {
    case PPROP_TYPE_BOOLEAN:
      return &RNA_ProxyBooleanProperty;
    case PPROP_TYPE_INT:
      return &RNA_ProxyIntProperty;
    case PPROP_TYPE_FLOAT:
      return &RNA_ProxyFloatProperty;
    case PPROP_TYPE_STRING:
      return &RNA_ProxyStringProperty;
    case PPROP_TYPE_SET:
      return &RNA_ProxySetProperty;
    case PPROP_TYPE_VEC2:
      return &RNA_ProxyVector2DProperty;
    case PPROP_TYPE_VEC3:
      return &RNA_ProxyVector3DProperty;
    case PPROP_TYPE_VEC4:
      return &RNA_ProxyVector4DProperty;
    case PPROP_TYPE_COL3:
      return &RNA_ProxyColor3Property;
    case PPROP_TYPE_COL4:
      return &RNA_ProxyColor4Property;
#  define PT_DEF(name, lower, upper) \
    case PPROP_TYPE_##upper: \
      return &RNA_Proxy##name##Property;
      POINTER_TYPES
#  undef PT_DEF
    default:
      return &RNA_PythonProxyProperty;
  }
}

static int rna_ProxySetProperty_get(struct PointerRNA *ptr)
{
  PythonProxyProperty *pprop = (PythonProxyProperty *)(ptr->data);
  return pprop->itemval;
}

static void rna_ProxySetProperty_set(struct PointerRNA *ptr, int value)
{
  PythonProxyProperty *pprop = (PythonProxyProperty *)(ptr->data);
  pprop->itemval = value;
}

static const EnumPropertyItem *rna_ProxySetProperty_itemf(bContext * /*C*/,
                                                          PointerRNA *ptr,
                                                          PropertyRNA * /*prop*/,
                                                          bool *r_free)
{
  PythonProxyProperty *pprop = (PythonProxyProperty *)(ptr->data);
  EnumPropertyItem *items = nullptr;
  int totitem = 0;
  int j = 0;

  for (LinkData *link = (LinkData *)pprop->enumval.first; link; link = link->next, ++j) {
    EnumPropertyItem item = {0, "", 0, "", ""};
    item.value = j;
    item.identifier = (const char *)link->data;
    item.icon = 0;
    item.name = (const char *)link->data;
    item.description = "";
    RNA_enum_item_add(&items, &totitem, &item);
  }

  RNA_enum_item_end(&items, &totitem);
  *r_free = true;

  return items;
}
#else

static void rna_def_py_proxy(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* Python Proxy */
  srna = RNA_def_struct(brna, "PythonProxy", nullptr);
  RNA_def_struct_sdna(srna, "PythonProxy");
  RNA_def_struct_ui_text(srna, "Python Proxy", "");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "module", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "module");
  RNA_def_property_ui_text(prop, "Module", "");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "show_expanded", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", COMPONENT_SHOW);
  RNA_def_property_ui_text(prop, "Expanded", "Set sensor expanded in the user interface");
  RNA_def_property_ui_icon(prop, ICON_TRIA_RIGHT, 1);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  prop = RNA_def_property(srna, "properties", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "properties", nullptr);
  RNA_def_property_struct_type(prop, "PythonProxyProperty");
  RNA_def_property_ui_text(prop, "Properties", "Proxy properties");

  RNA_define_lib_overridable(false);
}

static void rna_def_py_proxy_property(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static EnumPropertyItem empty_items[] = {{0, "EMPTY", 0, "Empty", ""}, {0, nullptr, 0, nullptr, nullptr}};

  RNA_define_lib_overridable(true);

  /* Base Python Proxy Property */
  srna = RNA_def_struct(brna, "PythonProxyProperty", nullptr);
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(srna, "Python Proxy Property", "A property of a Python Proxy");
  RNA_def_struct_refine_func(srna, "rna_PythonProxyProperty_refine");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Boolean */
  srna = RNA_def_struct(brna, "ProxyBooleanProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Boolean Property", "A boolean property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "boolval", 1);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Int */
  srna = RNA_def_struct(brna, "ProxyIntProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Integer Property", "An integer property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "intval");
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Float */
  srna = RNA_def_struct(brna, "ProxyFloatProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Float Property", "A float property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "floatval");
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* String */
  srna = RNA_def_struct(brna, "ProxyStringProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy String Property", "A string property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "strval");
  RNA_def_property_string_maxlength(prop, MAX_PROPSTRING);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Set */
  srna = RNA_def_struct(brna, "ProxySetProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(srna, "Python Proxy Set Property", "A set property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, empty_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_ProxySetProperty_get",
                              "rna_ProxySetProperty_set",
                              "rna_ProxySetProperty_itemf");
  RNA_def_property_enum_default(prop, 0);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Vector 2D */
  srna = RNA_def_struct(brna, "ProxyVector2DProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Vector 2D Property", "A 2D vector property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COORDS);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Vector 3D */
  srna = RNA_def_struct(brna, "ProxyVector3DProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Vector 3D Property", "A 3D vector property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COORDS);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Vector 4D */
  srna = RNA_def_struct(brna, "ProxyVector4DProperty", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Vector 4D Property", "A 4D vector property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COORDS);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Color 3 */
  srna = RNA_def_struct(brna, "ProxyColor3Property", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Color 3 Property", "A 3 channels color property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

  /* Color 4 */
  srna = RNA_def_struct(brna, "ProxyColor4Property", "PythonProxyProperty");
  RNA_def_struct_sdna(srna, "PythonProxyProperty");
  RNA_def_struct_ui_text(
      srna, "Python Proxy Color 4 Property", "A 4 channels color property of a Python Proxy");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "vec");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Value", "Property value");
  RNA_def_property_update(prop, NC_LOGIC, nullptr);

#  define PT_DEF(name, lower, upper) \
    srna = RNA_def_struct(brna, STRINGIFY(Proxy##name##Property), "PythonProxyProperty"); \
    RNA_def_struct_sdna(srna, "PythonProxyProperty"); \
    RNA_def_struct_ui_text(srna, \
                           STRINGIFY(Python Proxy##name##Property), \
                           STRINGIFY(name##property of a Python Proxy)); \
    prop = RNA_def_property(srna, "value", PROP_POINTER, PROP_NONE); \
    RNA_def_property_pointer_sdna(prop, nullptr, STRINGIFY(lower)); \
    RNA_def_property_struct_type(prop, STRINGIFY(name)); \
    RNA_def_property_ui_text(prop, "Value", "Property value"); \
    RNA_def_property_flag(prop, PROP_EDITABLE); \
    RNA_def_property_update(prop, NC_LOGIC, nullptr);

  POINTER_TYPES
#  undef PT_DEF

  RNA_define_lib_overridable(false);
}

void RNA_def_py_proxy(BlenderRNA *brna)
{
  rna_def_py_proxy(brna);
  rna_def_py_proxy_property(brna);
}

#endif /* RNA_RUNTIME */
