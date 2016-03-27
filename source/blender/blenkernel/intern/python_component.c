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
#include "DNA_windowmanager_types.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_path_util.h"
#include "MEM_guardedalloc.h"

#include "BKE_python_component.h"
#include "BKE_report.h"
#include "BKE_context.h"
#include "BKE_main.h"

#include "RNA_types.h"

#ifdef WITH_PYTHON
#include "Python.h"
#include "generic/py_capi_utils.h"
#endif

#ifdef WITH_PYTHON

PyTypeObject PythonComponentType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_PythonComponent",           /* tp_name */
	sizeof(PyObject),               /* tp_basicsize */
	0,                              /* tp_itemsize */
	(destructor)NULL,  /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	(reprfunc)NULL,          /* tp_repr */
	NULL,              /* tp_as_number */
	NULL,              /* tp_as_sequence */
	NULL,               /* tp_as_mapping */
	(hashfunc)NULL,           /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	NULL, /* tp_doc */
	(traverseproc)NULL,  /* tp_traverse */
	(inquiry)NULL,  /* tp_clear */
	(richcmpfunc)NULL,    /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	NULL,                  /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                /* tp_getset */
	NULL,                           /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	PyType_GenericAlloc,                           /* tp_alloc */
	PyType_GenericNew,                      /* tp_new */
	NULL,                           /* tp_free */
	NULL,                           /* tp_is_gc */
	NULL,                           /* tp_bases */
	NULL,                           /* tp_mro */
	NULL,                           /* tp_cache */
	NULL,                           /* tp_subclasses */
	NULL,                           /* tp_weaklist */
	NULL                            /* tp_del */
};

#endif

static int verify_class(PyObject *cls)
{
#ifdef WITH_PYTHON
	return PyType_IsSubtype((PyTypeObject *)cls, &PythonComponentType);
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
	ListBase properties;
	memset(&properties, 0, sizeof(ListBase));

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
			bool found = false;
			for (ComponentProperty *propit = pycomp->properties.first; propit; propit = propit->next) {
				if ((strcmp(propit->name, cprop->name) == 0) && propit->type == cprop->type) {
					/* We found a coresponding property in the old component, so the new one
					 * is released, the old property is removed from the original list and
					 * added to the new list.
					 */
					free_component_property(cprop);
					BLI_remlink(&pycomp->properties, propit);
					BLI_addtail(&properties, propit);
					found = true;
					break;
				}
			}
			if (!found) {
				BLI_addtail(&properties, cprop);
			}
		}
		else {
			// Cleanup ptr if it's set
			if (ptr) {
				MEM_freeN(ptr);
			}
		}
	}

	// Free properties no used in the new component.
	for (ComponentProperty *propit = pycomp->properties.first; propit;) {
		ComponentProperty *prop = propit;
		propit = propit->next;
		free_component_property(prop);
	}
	// Set the new property list.
	pycomp->properties = properties;

#endif /* WITH_PYTHON */
}

static bool load_component(PythonComponent *pc, ReportList *reports, char *filename)
{
#ifdef WITH_PYTHON

	#define ERROR \
		PyDict_DelItemString(sys_modules, "bge"); \
		Py_DECREF(bgemod); \
		Py_DECREF(bgesubmod); \
		PySequence_DelItem(sys_path, 0); \
		PyGILState_Release(state); \
		return false;

	PyObject *mod, *mod_list, *item, *py_name, *sys_path, *pypath;
	PyObject *bgemod;
	PyObject *bgesubmod;
	PyObject *sys_modules;
	PyGILState_STATE state;
	char *name;
	char path[FILE_MAX];

	state = PyGILState_Ensure();

	sys_path = PySys_GetObject("path");
	BLI_split_dir_part(filename, path, sizeof(path));
	pypath = PyC_UnicodeFromByte(path);
	PyList_Insert(sys_path, 0, pypath);

	sys_modules = PyThreadState_GET()->interp->modules;
	bgemod = PyModule_New("bge");
	bgesubmod = PyModule_New("bge.types");

	PyModule_AddObject(bgemod, "types", bgesubmod);
	PyType_Ready(&PythonComponentType);
	PyModule_AddObject(bgesubmod, "KX_PythonComponent", (PyObject *)&PythonComponentType);

	PyDict_SetItemString(sys_modules, "bge", bgemod);
	PyDict_SetItemString(sys_modules, "bge.types", bgesubmod);

	// Try to load up the module
	mod = PyImport_ImportModule(pc->module);
	if (!mod) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "No module named \"%s\" or script error at loading.", pc->module);
		ERROR;
	}
	else {
		if (strlen(pc->module) > 0 && strlen(pc->name) == 0) {
			BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No component class was specified, only the module was.");
			ERROR;
		}
	}

	// Get the list of objects in the module
	mod_list = PyDict_Values(PyModule_GetDict(mod));

	// Now iterate the list
	bool found = false;
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

		if (strcmp(name, pc->name) != 0) {
			continue;
		}

		// Check the subclass with our own function since we don't have access to the KX_PythonComponent type object
		if (!verify_class(item)) {
			BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "A %s type was found, but it was not a valid subclass of KX_PythonComponent.", pc->name);
			ERROR;
		}
		else {
			// Setup the properties
			create_properties(pc, item);
			found = true;
			break;
		}
	}

	if (!found) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "No class named %s was found.", pc->name);
		ERROR;
	}
	// Take the module out of the module list so it's not cached by Python (this allows for simpler reloading of components)
	PyDict_DelItemString(PyImport_GetModuleDict(), pc->module);

	// Cleanup our Python objects
	Py_DECREF(mod);
	Py_DECREF(mod_list);
	PyDict_DelItemString(sys_modules, "bge");
	Py_DECREF(bgemod);
	Py_DECREF(bgesubmod);

	PySequence_DelItem(sys_path, 0);
	PyGILState_Release(state);

	#undef ERROR

#endif /* WITH_PYTHON */

	return true;
}

PythonComponent *new_component_from_module_name(char *import, ReportList *reports, bContext *context)
{
	char *classname;
	char *modulename;
	PythonComponent *pc;

	// Don't bother with an empty string
	if (strcmp(import, "") == 0) {
		BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No component was specified.");
		return NULL;
	}

	// Extract the module name and the class name.
	modulename = strtok(import, ".");
	classname = strtok(NULL, ".");

	pc = MEM_callocN(sizeof(PythonComponent), "PythonComponent");

	// Copy module and class names.
	strcpy(pc->module, modulename);
	if (classname) {
		strcpy(pc->name, classname);
	}

	// Try load the component.
	if (!load_component(pc, reports, CTX_data_main(context)->name)) {
		free_component(pc);
		return NULL;
	}

	return pc;
}

void reload_component(PythonComponent *pc, ReportList *reports, bContext *context)
{
	load_component(pc, reports, CTX_data_main(context)->name);
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
