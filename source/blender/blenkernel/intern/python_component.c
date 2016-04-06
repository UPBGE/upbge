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

PyDoc_STRVAR(class_documentation,
"This is the fake BGE class KX_PythonComponent from fake BGE module bge.types"
);

static PyTypeObject PythonComponentType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_PythonComponent",           /* tp_name */
	sizeof(PyObject),               /* tp_basicsize */
	0,                              /* tp_itemsize */
	(destructor)NULL,               /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	(reprfunc)NULL,                 /* tp_repr */
	NULL,                           /* tp_as_number */
	NULL,                           /* tp_as_sequence */
	NULL,                           /* tp_as_mapping */
	(hashfunc)NULL,                 /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	class_documentation,            /* tp_doc */
	(traverseproc)NULL,             /* tp_traverse */
	(inquiry)NULL,                  /* tp_clear */
	(richcmpfunc)NULL,              /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	NULL,                           /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                           /* tp_getset */
	NULL,                           /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	PyType_GenericAlloc,            /* tp_alloc */
	PyType_GenericNew,              /* tp_new */
	NULL,                           /* tp_free */
	NULL,                           /* tp_is_gc */
	NULL,                           /* tp_bases */
	NULL,                           /* tp_mro */
	NULL,                           /* tp_cache */
	NULL,                           /* tp_subclasses */
	NULL,                           /* tp_weaklist */
	NULL                            /* tp_del */
};

PyDoc_STRVAR(module_documentation,
"This is the fake BGE API module used only to import the KX_PythonComponent class from bge.types.KX_PythonComponent"
);

static struct PyMethodDef module_methods[] = {
	{NULL, NULL, 0, NULL}
};

static struct PyModuleDef bge_module_def = {
	PyModuleDef_HEAD_INIT, /* m_base */
	"bge",  /* m_name */
	module_documentation,  /* m_doc */
	0,  /* m_size */
	module_methods,  /* m_methods */
	NULL,  /* m_reload */
	NULL,  /* m_traverse */
	NULL,  /* m_clear */
	NULL,  /* m_free */
};

static struct PyModuleDef bge_types_module_def = {
	PyModuleDef_HEAD_INIT, /* m_base */
	"types",  /* m_name */
	module_documentation,  /* m_doc */
	0,  /* m_size */
	module_methods,  /* m_methods */
	NULL,  /* m_reload */
	NULL,  /* m_traverse */
	NULL,  /* m_clear */
	NULL,  /* m_free */
};

#endif

static int verify_class(PyObject *cls)
{
#ifdef WITH_PYTHON
	return PyType_IsSubtype((PyTypeObject *)cls, &PythonComponentType);
#endif
	return 0;
}

static ComponentProperty *create_property(char *name)
{
	ComponentProperty *cprop;

	cprop = MEM_callocN(sizeof(ComponentProperty), "ComponentProperty");
	BLI_strncpy(cprop->name, name, sizeof(cprop->name));

	return cprop;
}

static ComponentProperty *copy_property(ComponentProperty *cprop)
{
	ComponentProperty *cpropn;

	cpropn = MEM_dupallocN(cprop);

	BLI_duplicatelist(&cpropn->enumval, &cprop->enumval);
	for (LinkData *link = cpropn->enumval.first; link; link = link->next) {
		link->data = MEM_dupallocN(link->data);
	}

	return cpropn;
}

static void free_component_property(ComponentProperty *cprop)
{
	for (LinkData *link = cprop->enumval.first; link; link = link->next) {
		MEM_freeN(link->data);
	}
	BLI_freelistN(&cprop->enumval);
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

		cprop = create_property(name);

		// Determine the type and default value
		if (PyBool_Check(pyvalue)) {
			cprop->type = CPROP_TYPE_BOOLEAN;
			cprop->boolval = PyLong_AsLong(pyvalue) != 0;
		}
		else if (PyLong_Check(pyvalue)) {
			cprop->type = CPROP_TYPE_INT;
			cprop->intval = PyLong_AsLong(pyvalue);
		}
		else if (PyFloat_Check(pyvalue)) {
			cprop->type = CPROP_TYPE_FLOAT;
			cprop->floatval = (float)PyFloat_AsDouble(pyvalue);
		}
		else if (PyUnicode_Check(pyvalue)) {
			cprop->type = CPROP_TYPE_STRING;
			BLI_strncpy((char*)cprop->strval, _PyUnicode_AsString(pyvalue), MAX_PROPSTRING);
		}
		else if (PySet_Check(pyvalue)) {
			PyObject *iterator = PyObject_GetIter(pyvalue), *v = NULL;
			char *str;
			cprop->type = CPROP_TYPE_SET;
			int j = 0;

			memset(&cprop->enumval, 0, sizeof(ListBase));
			while ((v = PyIter_Next(iterator))) {
				LinkData *link = MEM_callocN(sizeof(LinkData), "ComponentProperty set link data");
				str = MEM_callocN(MAX_PROPSTRING, "ComponentProperty set string");
				BLI_strncpy(str, _PyUnicode_AsString(v), MAX_PROPSTRING);

				link->data = str;
				BLI_addtail(&cprop->enumval, link);

				++j;
			}
			cprop->itemval = 0;
		}
		else {
			// Unsupported type
			printf("Unsupported type found for args[%s], skipping\n", name);
			free_component_property(cprop);
			continue;
		}

		bool found = false;
		for (ComponentProperty *propit = pycomp->properties.first; propit; propit = propit->next) {
			if ((strcmp(propit->name, cprop->name) == 0) && propit->type == cprop->type) {
				/* We never reuse a enum property because we don't know if one of the
				 * enum value was modified and it easier to just copy the current item
				 * index than the list.
				 */
				if (cprop->type == CPROP_TYPE_SET) {
					cprop->itemval = propit->itemval;
					break;
				}
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

	#define FINISH(value) \
		if (mod) { \
			/* Take the module out of the module list so it's not cached \
			   by Python (this allows for simpler reloading of components)*/ \
			PyDict_DelItemString(sys_modules, pc->module); \
		} \
		Py_XDECREF(mod); \
		Py_XDECREF(item); \
		PyDict_DelItemString(sys_modules, "bge"); \
		PyDict_DelItemString(sys_modules, "bge.types"); \
		PySequence_DelItem(sys_path, 0); \
		PyGILState_Release(state); \
		return value;

	PyObject *mod, *mod_list, *mod_item, *modspec, *item, *sys_path, *pypath, *sys_modules, *bgemod, *bgesubmod;
	PyGILState_STATE state;
	char path[FILE_MAX];

	state = PyGILState_Ensure();

	sys_path = PySys_GetObject("path");
	BLI_split_dir_part(filename, path, sizeof(path));
	pypath = PyC_UnicodeFromByte(path);
	PyList_Insert(sys_path, 0, pypath);

	// Setup BGE fake module and submodule.
	sys_modules = PyThreadState_GET()->interp->modules;
	bgemod = PyModule_Create(&bge_module_def);
	bgesubmod = PyModule_Create(&bge_types_module_def);

	PyModule_AddObject(bgemod, "types", bgesubmod);
	PyType_Ready(&PythonComponentType);
	PyModule_AddObject(bgesubmod, "KX_PythonComponent", (PyObject *)&PythonComponentType);

	PyDict_SetItemString(sys_modules, "bge", bgemod);
	PyDict_SetItemString(sys_modules, "bge.types", bgesubmod);

	// Try to load up the module
	mod = PyImport_ImportModule(pc->module);

	if (!mod) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "No module named \"%s\" or script error at loading.", pc->module);
		FINISH(false);
	}
	else if (strlen(pc->module) > 0 && strlen(pc->name) == 0) {
		BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No component class was specified, only the module was.");
		FINISH(false);
	}

	mod_list = PyDict_Values(PyModule_GetDict(mod));

	// Reload all modules imported in the component module script.
	for (unsigned int i = 0, size = PySequence_Size(mod_list); i < size; ++i) {
		item = PySequence_GetItem(mod_list, i);
		if (PyModule_Check(item)) {
			// If there's no spec, then the module can't be reloaded.
			modspec = PyObject_GetAttrString(item, "__spec__");
			if (modspec != Py_None) {
				mod_item = PyImport_ReloadModule(item);
				Py_XDECREF(mod_item);
			}
			Py_DECREF(modspec);
		}
		Py_DECREF(item);
	}
	Py_DECREF(mod_list);

	item = PyObject_GetAttrString(mod, pc->name);
	if (!item) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "No class named %s was found.", pc->name);
		FINISH(false);
	}

	// Check the subclass with our own function since we don't have access to the KX_PythonComponent type object
	if (!verify_class(item)) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "A %s type was found, but it was not a valid subclass of KX_PythonComponent.", pc->name);
		FINISH(false);
	}
	else {
		// Setup the properties
		create_properties(pc, item);
	}

	FINISH(true);

	#undef ERROR

#else

	return true;

#endif /* WITH_PYTHON */
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

void reload_script_module_recursive_component(void *module)
{
#ifdef WITH_PYTHON
	PyObject *mod_list, *mod_item, *modspec, *item;

	mod_list = PyDict_Values(PyModule_GetDict(module));

	// Reload all modules imported in the component module script.
	for (unsigned int i = 0, size = PySequence_Size(mod_list); i < size; ++i) {
		item = PySequence_GetItem(mod_list, i);
		if (PyModule_Check(item)) {
			// If there's no spec, then the module can't be reloaded.
			modspec = PyObject_GetAttrString(item, "__spec__");
			if (modspec != Py_None) {
				reload_script_module_recursive_component(item);
				mod_item = PyImport_ReloadModule(item);
				Py_XDECREF(mod_item);
			}
			Py_DECREF(modspec);
		}
		Py_DECREF(item);
	}
	Py_DECREF(mod_list);
#endif /* WITH_PYTHON */
}

void *argument_dict_from_component(PythonComponent *pc)
{
#ifdef WITH_PYTHON
	ComponentProperty *cprop;
	PyObject *args, *value;

	args = PyDict_New();

	cprop = (ComponentProperty *)pc->properties.first;

	while (cprop) {
		if (cprop->type == CPROP_TYPE_INT) {
			value = PyLong_FromLong(cprop->intval);
		}
		else if (cprop->type == CPROP_TYPE_FLOAT) {
			value = PyFloat_FromDouble(cprop->floatval);
		}
		else if (cprop->type == CPROP_TYPE_BOOLEAN) {
			value = PyBool_FromLong(cprop->boolval);
		}
		else if (cprop->type == CPROP_TYPE_STRING) {
			value = PyUnicode_FromString(cprop->strval);
		}
		/*else if (cprop->type == CPROP_TYPE_SET) {
			value = PyUnicode_FromString((char *)cprop->ptr2);
		}*/
		else {
			cprop = cprop->next;
			continue;
		}

		PyDict_SetItemString(args, cprop->name, value);

		cprop = cprop->next;
	}

	return args;

#else

	return NULL;

#endif /* WITH_PYTHON */
}
