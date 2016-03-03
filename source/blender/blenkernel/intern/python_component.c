#include <stdlib.h>

#include "DNA_component_types.h"
#include "DNA_property_types.h" /* For MAX_PROPSTRING */
#include "BLI_sys_types.h"
#include "DNA_listBase.h"
#include "BLI_fileops.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "MEM_guardedalloc.h"
#include "BKE_pycomponent.h"

#include "RNA_types.h"

#ifdef WITH_PYTHON
#include "Python.h"
#endif

static int verify_class(PyObject *cls)
{
#ifdef WITH_PYTHON
	PyObject *list, *item;
	char *name;
	int i;
	int comp;

	list = PyObject_GetAttrString(cls, "__bases__");

	for (i = 0; i < PyTuple_Size(list); ++i)
	{
		item = PyObject_GetAttrString(PyTuple_GetItem(list, i), "__name__");
		name = _PyUnicode_AsString(item);


		// We don't want to decref until after the comprison
		comp = strcmp("KX_PythonComponent", name);
		Py_DECREF(item);

		if (comp == 0)
		{
			Py_DECREF(list);
			return 1;
		}
	}

	Py_DECREF(list);
#endif
	return 0;
}

static ComponentProperty *create_property(char *name, short type, int data, void *poin)
{
	ComponentProperty *cprop;

	cprop = MEM_mallocN(sizeof(ComponentProperty), "ComponentProperty");

	if (cprop)
	{
		BLI_strncpy(cprop->name, name, sizeof(cprop->name));
		cprop->type = type;

		cprop->data = 0;
		cprop->poin = NULL;
		cprop->poin2 = NULL;

		if (type == CPROP_TYPE_INT)
			cprop->data = data;
		else if (type == CPROP_TYPE_FLOAT)
			*((float *)&cprop->data) = *(float*)(&data);
		else if (type == CPROP_TYPE_BOOLEAN)
			cprop->data = data;
		else if (type == CPROP_TYPE_STRING)
			cprop->poin = poin;
		else if (type == CPROP_TYPE_SET)
		{
			cprop->poin = poin;
			cprop->poin2 = ((EnumPropertyItem*)poin)->identifier;
			cprop->data = 0;
		}
	}

	return cprop;
}

static void free_component_property(ComponentProperty *cprop)
{
	if (cprop->poin) MEM_freeN(cprop->poin);
	if (cprop->poin2) MEM_freeN(cprop->poin2);
	MEM_freeN(cprop);
}

static void free_component_properties(ListBase *lb)
{
	ComponentProperty *cprop;

	while (cprop= lb->first) {
		BLI_remlink(lb, cprop);
		free_component_property(cprop);
	}
}

static void create_properties(PythonComponent *pycomp, PyObject *cls)
{
#ifdef WITH_PYTHON
	PyObject *args_dict, *key, *value, *items, *item;
	ComponentProperty *cprop;
	char name[64];
	int i=0, data;
	short type;
	void *poin=NULL;

	args_dict = PyObject_GetAttrString(cls, "args");

	// If there is no args dict, then we are already done
	if (args_dict == NULL || !PyDict_Check(args_dict))
	{
		Py_XDECREF(args_dict);
		return;
	}

	// Otherwise, parse the dict:
	// key => value
	// key = property name
	// value = default value
	// type(value) = property type
	items = PyMapping_Items(args_dict);

	for (i=0; i<PyList_Size(items); ++i)
	{
		item = PyList_GetItem(items, i);
		key = PyTuple_GetItem(item, 0);
		value = PyTuple_GetItem(item, 1);

		// Make sure type(key) == string
		if (!PyUnicode_Check(key))
		{
			printf("Non-string key found in the args dictionary, skipping\n");
			continue;
		}

		BLI_strncpy(name, _PyUnicode_AsString(key), sizeof(name));

		// Determine the type and default value
		if (PyBool_Check(value))
		{
			type = CPROP_TYPE_BOOLEAN;
			data = PyLong_AsLong(value) != 0;
		}
		else if (PyLong_Check(value))
		{
			type = CPROP_TYPE_INT;
			data = PyLong_AsLong(value);
		}
		else if (PyFloat_Check(value))
		{
			type = CPROP_TYPE_FLOAT;
			*((float*)&data) = (float)PyFloat_AsDouble(value);
		}
		else if (PyUnicode_Check(value))
		{
			type = CPROP_TYPE_STRING;
			poin = MEM_callocN(MAX_PROPSTRING, "ComponentProperty string");
			BLI_strncpy((char*)poin, _PyUnicode_AsString(value), MAX_PROPSTRING);
		}
		else if (PySet_Check(value))
		{
			int len = PySet_Size(value), i=0;
			EnumPropertyItem *items;
			PyObject *iterator = PyObject_GetIter(value), *v=NULL;
			char *str;
			type = CPROP_TYPE_SET;

			// Create an EnumPropertyItem array
			poin = MEM_callocN(sizeof(EnumPropertyItem)*(len+1), "ComponentProperty set");
			items = (EnumPropertyItem*)poin;
			while (v = PyIter_Next(iterator))
			{

				str = MEM_callocN(MAX_PROPSTRING, "ComponentProperty set string");
				BLI_strncpy(str, _PyUnicode_AsString(v), MAX_PROPSTRING);
				printf("SET: %s\n", str);
				items[i].value = i;
				items[i].identifier = str;
				items[i].icon = 0;
				items[i].name = str;
				items[i].description = "";

				i++;
			}

			data = 0;
		}
		else
		{
			// Unsupported type
			printf("Unsupported type found for args[%s], skipping\n", name);
			continue;
		}

		cprop = create_property(name, type, data, poin);

		if (cprop)
			BLI_addtail(&pycomp->properties, cprop);
		else
			// Cleanup poin if it's set
			if (poin) MEM_freeN(poin);
	}

#endif /* WITH_PYTHON */
}

static PyObject *arg_dict_from_component(PythonComponent *pc)
{
	ComponentProperty *cprop;
	PyObject *args= NULL, *value=NULL;

#ifdef WITH_PYTHON
	args = PyDict_New();

	cprop = pc->properties.first;

	while (cprop)
	{
		if (cprop->type == CPROP_TYPE_INT)
			value = PyLong_FromLong(cprop->data);
		else if (cprop->type == CPROP_TYPE_FLOAT)
			value = PyFloat_FromDouble(*(float*)(&cprop->data));
		else if (cprop->type == CPROP_TYPE_BOOLEAN)
			value = PyBool_FromLong(cprop->data);
		else if (cprop->type == CPROP_TYPE_STRING)
			value = PyUnicode_FromString((char*)cprop->poin);
		else
			continue;

		PyDict_SetItemString(args, cprop->name, value);

		cprop= cprop->next;
	}

#endif /* WITH_PYTHON */
	return args;
}

PythonComponent *new_component_from_import(char *import)
{
	PythonComponent *pc = NULL;

#ifdef WITH_PYTHON
	PyObject *mod, *mod_list, *item, *py_name;
	PyGILState_STATE state;
	char *name, *cls, path[64];
	int i;

	// Don't bother with an empty string
	if (strcmp(import, "") == 0)
		return NULL;

	char *module_name;
	module_name = strtok(import, ".");
	cls = strtok(NULL, ".");

	if (cls) {
		strcpy(path, module_name);
	}
	else {
		printf("No component class was specified, only the module was.\n");
		return NULL;
	}

	state = PyGILState_Ensure();

	// Try to load up the module
	mod = PyImport_ImportModule(path);

	if (mod)
	{
		// Get the list of objects in the module
		mod_list = PyDict_Values(PyModule_GetDict(mod));

		// Now iterate the list
		for (i = 0; i < PyList_Size(mod_list); ++i)
		{
			item = PyList_GetItem(mod_list, i);

			// We only want to bother checking type objects
			if (!PyType_Check(item))
				continue;

			// Make sure the name matches
			py_name = PyObject_GetAttrString(item, "__name__");
			name = _PyUnicode_AsString(py_name);
			Py_DECREF(py_name);

			if (strcmp(name, cls) != 0)
				continue;

			// Check the subclass with our own function since we don't have access to the KX_PythonComponent type object
			if (!verify_class(item))
			{
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
		if (pc == NULL)
			printf("No suitable class was found for a component at %s\n", import);

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

void free_component(PythonComponent *pc)
{
	free_component_properties(&pc->properties);

	MEM_freeN(pc);
}

void free_components(ListBase *lb)
{
	PythonComponent *pc;

	while (pc = lb->first) {
		BLI_remlink(lb, pc);
		free_component(pc);
	}
}
