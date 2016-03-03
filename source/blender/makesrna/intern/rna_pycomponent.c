/**
 * $Id: rna_controller.c 32883 2010-11-05 07:35:21Z campbellbarton $
 *
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

#include <stdlib.h>

#include "RNA_define.h"

#include "rna_internal.h"
#include "DNA_component_types.h"
#include "DNA_property_types.h"

#include "WM_types.h"

#ifdef RNA_RUNTIME

static StructRNA* rna_ComponentProperty_refine(struct PointerRNA *ptr)
{
	ComponentProperty *cprop = (ComponentProperty*)ptr->data;

	switch(cprop->type) {
	case CPROP_TYPE_BOOLEAN:
		return &RNA_ComponentBooleanProperty;
	case CPROP_TYPE_INT:
		return &RNA_ComponentIntProperty;
	case CPROP_TYPE_FLOAT:
		return &RNA_ComponentFloatProperty;
	case CPROP_TYPE_STRING:
		return &RNA_ComponentStringProperty;
	case CPROP_TYPE_SET:
		return &RNA_ComponentSetProperty;
	default:
		return &RNA_ComponentProperty;
	}
}

static float rna_ComponentFloatProperty_value_get(PointerRNA *ptr)
{
	ComponentProperty *cprop = (ComponentProperty*)(ptr->data);
	return *(float*)(&cprop->data);
}

static void rna_ComponentFloatProperty_value_set(PointerRNA *ptr, float value)
{
	ComponentProperty *cprop = (ComponentProperty*)(ptr->data);
	*(float*)(&cprop->data) = value;
}
static int rna_ComponentSetProperty_get(struct PointerRNA *ptr)
{
	ComponentProperty *cprop = (ComponentProperty*)(ptr->data);
	return cprop->data;
}

static void rna_ComponentSetProperty_set(struct PointerRNA *ptr, int value)
{
	ComponentProperty *cprop = (ComponentProperty*)(ptr->data);
	cprop->data = value;
	cprop->poin2 = (((EnumPropertyItem*)cprop->poin)+value)->identifier;
}

EnumPropertyItem *rna_ComponentSetProperty_itemf(bContext *C, PointerRNA *ptr, int *free)
{

	ComponentProperty *cprop = (ComponentProperty*)(ptr->data);
	*free = 0;
	return (EnumPropertyItem*)cprop->poin;
}
#else

void rna_def_py_component(BlenderRNA *brna)
{
	StructRNA *srna;
	PropertyRNA *prop;

	/* Python Component */
	srna = RNA_def_struct(brna, "PythonComponent", NULL);
	RNA_def_struct_sdna(srna, "PythonComponent");
	RNA_def_struct_ui_text(srna, "Python Component", "");

	prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
	RNA_def_property_string_sdna(prop, NULL, "name");
	RNA_def_property_ui_text(prop, "Name", "");
	RNA_def_struct_name_property(srna, prop);
	RNA_def_property_clear_flag(prop, PROP_EDITABLE);
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	prop = RNA_def_property(srna, "properties", PROP_COLLECTION, PROP_NONE);
	RNA_def_property_collection_sdna(prop, NULL, "properties", NULL);
	RNA_def_property_struct_type(prop, "ComponentProperty");
	RNA_def_property_ui_text(prop, "Properties", "Component properties");
}

void rna_def_py_component_property(BlenderRNA *brna)
{
	StructRNA *srna;
	PropertyRNA *prop;

	static EnumPropertyItem empty_items[] = {
		{0, "EMPTY", 0, "Empty", ""},
		{0, NULL, 0, NULL, NULL}
	};

	/* Base Python Component Property */
	srna = RNA_def_struct(brna, "ComponentProperty", NULL);
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component Property", "A property of a Python Component");
	RNA_def_struct_refine_func(srna, "rna_ComponentProperty_refine");

	prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
	RNA_def_property_string_sdna(prop, NULL, "name");
	RNA_def_property_ui_text(prop, "Name", "");
	RNA_def_struct_name_property(srna, prop);
	RNA_def_property_clear_flag(prop, PROP_EDITABLE);
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	/* Boolean */
	srna = RNA_def_struct(brna, "ComponentBooleanProperty", "ComponentProperty");
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component Boolean Property", "A boolean property of a Python Component");

	prop = RNA_def_property(srna, "value", PROP_BOOLEAN, PROP_NONE);
	RNA_def_property_boolean_sdna(prop, NULL, "data", 1);
	RNA_def_property_ui_text(prop, "Value", "Property value");
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	/* Int */
	srna = RNA_def_struct(brna, "ComponentIntProperty", "ComponentProperty");
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component Integer Property", "An integer property of a Python Component");

	prop = RNA_def_property(srna, "value", PROP_INT, PROP_NONE);
	RNA_def_property_int_sdna(prop, NULL, "data");
	RNA_def_property_ui_text(prop, "Value", "Property value");
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	/* Float */
	srna = RNA_def_struct(brna, "ComponentFloatProperty", "ComponentProperty");
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component Float Property", "A float property of a Python Component");

	prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_NONE);
	RNA_def_property_ui_text(prop, "Value", "Property value");
	RNA_def_property_float_funcs(prop, "rna_ComponentFloatProperty_value_get", "rna_ComponentFloatProperty_value_set", NULL);
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	/* String */
	srna = RNA_def_struct(brna, "ComponentStringProperty", "ComponentProperty");
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component String Property", "A string property of a Python Component");

	prop = RNA_def_property(srna, "value", PROP_STRING, PROP_NONE);
	RNA_def_property_string_sdna(prop, NULL, "poin");
	RNA_def_property_string_maxlength(prop, MAX_PROPSTRING);
	RNA_def_property_ui_text(prop, "Value", "Property value");
	RNA_def_property_update(prop, NC_LOGIC, NULL);

	/* Set */
	//#if 0
	srna = RNA_def_struct(brna, "ComponentSetProperty", "ComponentProperty");
	RNA_def_struct_sdna(srna, "ComponentProperty");
	RNA_def_struct_ui_text(srna, "Python Component Set Property", "A set property of a Python Component");

	prop = RNA_def_property(srna, "value", PROP_ENUM, PROP_NONE);
	RNA_def_property_enum_items(prop, empty_items);
	RNA_def_property_enum_funcs(prop, "rna_ComponentSetProperty_get", "rna_ComponentSetProperty_set", "rna_ComponentSetProperty_itemf");
	RNA_def_property_enum_default(prop, 0);
	//RNA_def_property_enum_funcs(prop, NULL, NULL, "rna_ComponentSetProperty_itemf");
	RNA_def_property_ui_text(prop, "Value", "Property value");
	RNA_def_property_update(prop, NC_LOGIC, NULL);
	//#endif

}

void RNA_def_py_component(BlenderRNA *brna)
{
	rna_def_py_component(brna);
	rna_def_py_component_property(brna);
}

#endif /* RNA_RUNTIME */
