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

#include "DNA_python_component_types.h"
#include "DNA_property_types.h" /* For MAX_PROPSTRING */
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "MEM_guardedalloc.h"
#include "BKE_python_component.h"

#include "RNA_types.h"

#ifdef WITH_PYTHON
#include "Python.h"
#endif

static int verify_class(PyObject *cls)
{
#ifdef WITH_PYTHON
	PyObject *list, *item;
	char *name;
	int comp;

	list = PyObject_GetAttrString(cls, "__bases__");

	for (unsigned int i = 0, size = PyTuple_Size(list); i < size; ++i) {
		item = PyObject_GetAttrString(PyTuple_GetItem(list, i), "__name__");
		name = _PyUnicode_AsString(item);

		// We don't want to decref until after the comprison
		comp = strcmp("KX_PythonComponent", name);
		Py_DECREF(item);

		if (comp == 0) {
			Py_DECREF(list);
			return 1;
		}
	}

	Py_DECREF(list);
#endif
	return 0;
}

static ComponentProperty *create_property(char *name, short type, int data, void *ptr)
{
	ComponentProperty *cprop;

	cprop = MEM_mallocN(sizeof(ComponentProperty), "ComponentProperty");

	if (cprop) {
		BLI_strncpy(cprop->name, name, sizeof(cprop->name));
		cprop->type = type;

		cprop->data = 0;
		cprop->ptr = NULL;
		cprop->ptr2 = NULL;

		if (type == CPROP_TYPE_INT) {
			cprop->data = data;
		}
		else if (type == CPROP_TYPE_FLOAT) {
			*((float *)&cprop->data) = *(float*)(&data);
		}
		else if (type == CPROP_TYPE_BOOLEAN) {
			cprop->data = data;
		}
		else if (type == CPROP_TYPE_STRING) {
			cprop->ptr = ptr;
		}
		else if (type == CPROP_TYPE_SET) {
			cprop->ptr = ptr;
			cprop->ptr2 = (void *)((EnumPropertyItem*)ptr)->identifier;
			cprop->data = 0;
		}
	}

	return cprop;
}

static ComponentProperty *copy_property(ComponentProperty *cprop)
{
	ComponentProperty *cpropn;

	cpropn = MEM_dupallocN(cprop);
	cpropn->ptr = MEM_dupallocN(cpropn->ptr);
	cpropn->ptr2 = MEM_dupallocN(cpropn->ptr2);

	return cpropn;
}

static void free_component_property(ComponentProperty *cprop)
{
	if (cprop->ptr) {
		MEM_freeN(cprop->ptr);
	}
	if (cprop->ptr2) {
		MEM_freeN(cprop->ptr2);
	}
	MEM_freeN(cprop);
}

static void free_component_properties(ListBase *lb)
{
	ComponentProperty *cprop;

	while ((cprop = lb->first)) {
		BLI_remlink(lb, cprop);
		free_component_property(cprop);
	}
}

static void create_properties(PythonComponent *pycomp, PyObject *cls)
{
#ifdef WITH_PYTHON
	PyObject *args_dict, *pykey, *pyvalue, *pyitems, *pyitem;
	ComponentProperty *cprop;
	char name[64];
	int data;
	short type;
	void *ptr = NULL;

	args_dict = PyObject_GetAttrString(cls, "args");

	// If there is no args dict, then we are already done
	if (!args_dict || !PyDict_Check(args_dict)) {
		Py_XDECREF(args_dict);
		return;
	}

	// Otherwise, parse the dict:
	// key => value
	// key = property name
	// value = default value
	// type(value) = property type
	pyitems = PyMapping_Items(args_dict);

	for (unsigned int i = 0, size = PyList_Size(pyitems); i < size; ++i) {
		pyitem = PyList_GetItem(pyitems, i);
		pykey = PyTuple_GetItem(pyitem, 0);
		pyvalue = PyTuple_GetItem(pyitem, 1);

		// Make sure type(key) == string
		if (!PyUnicode_Check(pykey)) {
			printf("Non-string key found in the args dictionary, skipping\n");
			continue;
		}

		BLI_strncpy(name, _PyUnicode_AsString(pykey), sizeof(name));

		// Determine the type and default value
		if (PyBool_Check(pyvalue)) {
			type = CPROP_TYPE_BOOLEAN;
			data = PyLong_AsLong(pyvalue) != 0;
		}
		else if (PyLong_Check(pyvalue)) {
			type = CPROP_TYPE_INT;
			data = PyLong_AsLong(pyvalue);
		}
		else if (PyFloat_Check(pyvalue)) {
			type = CPROP_TYPE_FLOAT;
			*((float*)&data) = (float)PyFloat_AsDouble(pyvalue);
		}
		else if (PyUnicode_Check(pyvalue)) {
			type = CPROP_TYPE_STRING;
			ptr = MEM_callocN(MAX_PROPSTRING, "ComponentProperty string");
			BLI_strncpy((char*)ptr, _PyUnicode_AsString(pyvalue), MAX_PROPSTRING);
		}
		else if (PySet_Check(pyvalue)) {
			int len = PySet_Size(pyvalue), j = 0;
			EnumPropertyItem *items;
			PyObject *iterator = PyObject_GetIter(pyvalue), *v = NULL;
			char *str;
			type = CPROP_TYPE_SET;

			// Create an EnumPropertyItem array
			ptr = MEM_callocN(sizeof(EnumPropertyItem) * (len + 1), "ComponentProperty set");
			items = (EnumPropertyItem*)ptr;
			while ((v = PyIter_Next(iterator))) {
				str = MEM_callocN(MAX_PROPSTRING, "ComponentProperty set string");
				BLI_strncpy(str, _PyUnicode_AsString(v), MAX_PROPSTRING);

				items[j].value = j;
				items[j].identifier = str;
				items[j].icon = 0;
				items[j].name = str;
				items[j].description = "";

				++j;
			}
			data = 0;
		}
		else {
			// Unsupported type
			printf("Unsupported type found for args[%s], skipping\n", name);
			continue;
		}

		cprop = create_property(name, type, data, ptr);

		if (cprop) {
			BLI_addtail(&pycomp->properties, cprop);
		}
		else {
			// Cleanup ptr if it's set
			if (ptr) MEM_freeN(ptr);
		}
	}
#endif /* WITH_PYTHON */
}

PythonComponent *new_component_from_module_name(char *import)
{
	PythonComponent *pc = NULL;

#ifdef WITH_PYTHON
	PyObject *mod, *mod_list, *item, *py_name;
	PyGILState_STATE state;
	char *name, *cls, path[64];
	char *module_name;

	// Don't bother with an empty string
	if (strcmp(import, "") == 0) {
		printf("No component was specified.\n");
		return NULL;
	}

	module_name = strtok(import, ".");
	cls = strtok(NULL, ".");

	if (cls && module_name) {
		strcpy(path, module_name);
	}
	else if (!cls && module_name) {
		printf("No component class was specified, only the module was.\n");
		return NULL;
	}

	state = PyGILState_Ensure();

	// Try to load up the module
	mod = PyImport_ImportModule(path);

	if (mod) {
		// Get the list of objects in the module
		mod_list = PyDict_Values(PyModule_GetDict(mod));

		// Now iterate the list
		for (unsigned int i = 0, size = PyList_Size(mod_list); i < size; ++i) {
			item = PyList_GetItem(mod_list, i);

			// We only want to bother checking type objects
			if (!PyType_Check(item)) {
				continue;
			}

			// Make sure the name matches
			py_name = PyObject_GetAttrString(item, "__name__");
			name = _PyUnicode_AsString(py_name);
			Py_DECREF(py_name);

			if (strcmp(name, cls) != 0) {
				continue;
			}

			// Check the subclass with our own function since we don't have access to the KX_PythonComponent type object
			if (!verify_class(item)) {
				printf("A %s type was found, but it was not a valid subclass of KX_PythonComponent\n", cls);
			}
			else {
				// We have a valid class, make a component
				pc = MEM_callocN(sizeof(PythonComponent), "PythonComponent");

				strcpy(pc->module, path);
				strcpy(pc->name, cls);

				// Setup the properties
				create_properties(pc, item);

				break;
			}
		}

		// If we still have a NULL component, then we didn't find a suitable class
		if (pc == NULL) {
			printf("No suitable class was found for a component at %s\n", import);
		}

		// Take the module out of the module list so it's not cached by Python (this allows for simpler reloading of components)
		PyDict_DelItemString(PyImport_GetModuleDict(), path);

		// Cleanup our Python objects
		Py_DECREF(mod);
		Py_DECREF(mod_list);
	}
	else {
		PyErr_Print();
		printf("Unable to load component from %s\n", import);
	}

	PyGILState_Release(state);
#endif /* WITH_PYTHON */

	return pc;
}

static PythonComponent *copy_component(PythonComponent *comp)
{
	PythonComponent *compn;
	ComponentProperty *cprop, *cpropn;

	compn = MEM_dupallocN(comp);

	BLI_listbase_clear(&compn->properties);
	cprop = comp->properties.first;
	while (cprop) {
		cpropn = copy_property(cprop);
		BLI_addtail(&compn->properties, cpropn);
		cprop = cprop->next;
	}

	return compn;
}

void copy_components(ListBase *lbn, ListBase *lbo)
{
	PythonComponent *comp, *compn;

	lbn->first = lbn->last = NULL;
	comp = lbo->first;
	while (comp) {
		compn = copy_component(comp);
		BLI_addtail(lbn, compn);
		comp = comp->next;
	}
}

void free_component(PythonComponent *pc)
{
	free_component_properties(&pc->properties);

	MEM_freeN(pc);
}

void free_components(ListBase *lb)
{
	PythonComponent *pc;

	while ((pc = lb->first)) {
		BLI_remlink(lb, pc);
		free_component(pc);
	}
}
