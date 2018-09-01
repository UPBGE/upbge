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
#include "BLI_fileops.h"
#include "MEM_guardedalloc.h"

#include "BKE_python_component.h"
#include "BKE_report.h"
#include "BKE_context.h"
#include "BKE_main.h"
#include "BKE_text.h"
#include "BKE_appdir.h"

#include "RNA_types.h"

#ifdef WITH_PYTHON
#include "Python.h"
#include "generic/py_capi_utils.h"
#endif

#include <string.h>

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

static struct PyModuleDef bge_module_def = {
	PyModuleDef_HEAD_INIT, /* m_base */
	"bge",  /* m_name */
	module_documentation,  /* m_doc */
	0,  /* m_size */
	NULL,  /* m_methods */
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
	NULL,  /* m_methods */
	NULL,  /* m_reload */
	NULL,  /* m_traverse */
	NULL,  /* m_clear */
	NULL,  /* m_free */
};

static int verify_class(PyObject *cls)
{
	return PyType_IsSubtype((PyTypeObject *)cls, &PythonComponentType);
}

static PythonComponentProperty *create_property(char *name)
{
	PythonComponentProperty *cprop;

	cprop = MEM_callocN(sizeof(PythonComponentProperty), "PythonComponentProperty");
	BLI_strncpy(cprop->name, name, sizeof(cprop->name));

	return cprop;
}

#endif

static PythonComponentProperty *copy_property(PythonComponentProperty *cprop)
{
	PythonComponentProperty *cpropn;

	cpropn = MEM_dupallocN(cprop);

	BLI_duplicatelist(&cpropn->enumval, &cprop->enumval);
	for (LinkData *link = cpropn->enumval.first; link; link = link->next) {
		link->data = MEM_dupallocN(link->data);
	}

	return cpropn;
}

static void free_property(PythonComponentProperty *cprop)
{
	for (LinkData *link = cprop->enumval.first; link; link = link->next) {
		MEM_freeN(link->data);
	}
	BLI_freelistN(&cprop->enumval);
	MEM_freeN(cprop);
}

static void free_properties(ListBase *lb)
{
	PythonComponentProperty *cprop;

	while ((cprop = lb->first)) {
		BLI_remlink(lb, cprop);
		free_property(cprop);
	}
}

#ifdef WITH_PYTHON
static void create_properties(PythonComponent *pycomp, PyObject *cls)
{
	PyObject *args_dict, *pyitems;
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
		PythonComponentProperty *cprop;
		char name[64];
		bool free = false;
		PyObject *pyitem = PyList_GetItem(pyitems, i);
		PyObject *pykey = PyTuple_GetItem(pyitem, 0);
		PyObject *pyvalue = PyTuple_GetItem(pyitem, 1);

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
			unsigned int j = 0;
			cprop->type = CPROP_TYPE_SET;

			memset(&cprop->enumval, 0, sizeof(ListBase));
			// Iterate to convert every enums to char.
			while ((v = PyIter_Next(iterator))) {
				if (!PyUnicode_Check(v)) {
					printf("Enum property \"%s\" contains a non-string item (%u)\n", name, j);
					continue;
				}

				LinkData *link = MEM_callocN(sizeof(LinkData), "PythonComponentProperty set link data");
				char *str = MEM_callocN(MAX_PROPSTRING, "PythonComponentProperty set string");
				BLI_strncpy(str, _PyUnicode_AsString(v), MAX_PROPSTRING);

				link->data = str;
				BLI_addtail(&cprop->enumval, link);

				Py_DECREF(v);
				++j;
			}
			Py_DECREF(iterator);
			cprop->itemval = 0;
		}
		else if (PySequence_Check(pyvalue)) {
			int len = PySequence_Size(pyvalue);
			switch (len) {
				case 2:
					cprop->type = CPROP_TYPE_VEC2;
					break;
				case 3:
					cprop->type = CPROP_TYPE_VEC3;
					break;
				case 4:
					cprop->type = CPROP_TYPE_VEC4;
					break;
				default:
					printf("Sequence property \"%s\" length %i out of range [2, 4]\n", name, len);
					free = true;
					break;
			}

			if (!free) {
				for (unsigned int j = 0; j < len; ++j) {
					PyObject *item = PySequence_GetItem(pyvalue, j);
					if (PyFloat_Check(item)) {
						cprop->vec[j] = PyFloat_AsDouble(item);
					}
					else {
						printf("Sequence property \"%s\" contains a non-float item (%u)\n", name, j);
					}
					Py_DECREF(item);
				}
			}
		}
		else {
			// Unsupported type
			printf("Unsupported type %s found for property \"%s\", skipping\n", Py_TYPE(pyvalue)->tp_name, name);
			free = true;
		}

		if (free) {
			free_property(cprop);
			continue;
		}

		bool found = false;
		for (PythonComponentProperty *propit = pycomp->properties.first; propit; propit = propit->next) {
			if ((strcmp(propit->name, cprop->name) == 0) && propit->type == cprop->type) {
				/* We never reuse a enum property because we don't know if one of the
				 * enum value was modified and it easier to just copy the current item
				 * index than the list.
				 */
				if (cprop->type == CPROP_TYPE_SET) {
					/* Unfortunatly the python type set doesn't repect an order even with same
					 * content. To solve that we iterate on all new enums and find the coresponding
					 * index for the old enum name.
					 */
					char *str = ((LinkData *)BLI_findlink(&propit->enumval, propit->itemval))->data;
					int j = 0;
					for (LinkData *link = cprop->enumval.first; link; link = link->next) {
						if (strcmp(link->data, str) == 0) {
							cprop->itemval = j;
						}
						++j;
					}
					break;
				}
				/* We found a coresponding property in the old component, so the new one
				 * is released, the old property is removed from the original list and
				 * added to the new list.
				 */
				free_property(cprop);
				/* The exisiting property is removed to allow at the end free properties
				 * that are no longuer used.
				 */
				BLI_remlink(&pycomp->properties, propit);
				BLI_addtail(&properties, propit);
				found = true;
				break;
			}
		}
		// If no exisiting property was found we add it simply.
		if (!found) {
			BLI_addtail(&properties, cprop);
		}
	}

	// Free properties no used in the new component.
	for (PythonComponentProperty *propit = pycomp->properties.first; propit;) {
		PythonComponentProperty *prop = propit;
		propit = propit->next;
		free_property(prop);
	}
	// Set the new property list.
	pycomp->properties = properties;

}
#endif /* WITH_PYTHON */

static bool load_component(PythonComponent *pc, ReportList *reports, Main *maggie)
{
#ifdef WITH_PYTHON

	/* Macro used to release all python variable if the convertion fail or succeed.
	 * The "value" argument is false on failure and true on succes.
	 */
	#define FINISH(value) \
		PyErr_Print(); \
		if (mod) { \
			/* Take the module out of the module list so it's not cached \
			   by Python (this allows for simpler reloading of components)*/ \
			PyDict_DelItemString(sys_modules, pc->module); \
		} \
		Py_XDECREF(mod); \
		Py_XDECREF(item); \
		PyDict_DelItemString(sys_modules, "bge"); \
		PyDict_DelItemString(sys_modules, "bge.types"); \
		BLI_split_dir_part(maggie->name, path, sizeof(path)); \
		pypath = PyC_UnicodeFromByte(path); \
		index = PySequence_Index(sys_path, pypath); \
		/* Safely remove the value by finding their index. */ \
		if (index != -1) { \
			PySequence_DelItem(sys_path, index); \
		} \
		Py_DECREF(pypath); \
		for (Library *lib = (Library *)maggie->library.first; lib; lib = (Library *)lib->id.next) { \
			BLI_split_dir_part(lib->filepath, path, sizeof(path)); \
			pypath = PyC_UnicodeFromByte(path); \
			index = PySequence_Index(sys_path, pypath); \
			/* Safely remove the value by finding their index. */ \
			if (index != -1) { \
				PySequence_DelItem(sys_path, index); \
			} \
			Py_DECREF(pypath); \
		} \
		PyGILState_Release(state); \
		return value;

	PyObject *mod, *item = NULL, *sys_path, *pypath, *sys_modules, *bgemod, *bgesubmod;
	PyGILState_STATE state;
	char path[FILE_MAX];
	int index;

	state = PyGILState_Ensure();

	// Set the current file directory do import path to allow extern modules.
	sys_path = PySys_GetObject("path");
	/* Add to sys.path the path to all the used library to follow game engine sys.path management.
	 * These path are remove later in FINISH. */
	for (Library *lib = (Library *)maggie->library.first; lib; lib = (Library *)lib->id.next) {
		BLI_split_dir_part(lib->filepath, path, sizeof(path));
		pypath = PyC_UnicodeFromByte(path);
		PyList_Insert(sys_path, 0, pypath);
		Py_DECREF(pypath);
	}
	/* Add default path */
	BLI_split_dir_part(maggie->name, path, sizeof(path));
	pypath = PyC_UnicodeFromByte(path);
	PyList_Insert(sys_path, 0, pypath);
	Py_DECREF(pypath);

	// Setup BGE fake module and submodule.
	sys_modules = PyThreadState_GET()->interp->modules;
	bgemod = PyModule_Create(&bge_module_def);
	bgesubmod = PyModule_Create(&bge_types_module_def);

	PyModule_AddObject(bgemod, "types", bgesubmod);
	PyType_Ready(&PythonComponentType);
	PyModule_AddObject(bgesubmod, "KX_PythonComponent", (PyObject *)&PythonComponentType);

	PyDict_SetItemString(sys_modules, "bge", bgemod);
	PyDict_SetItemString(sys_modules, "bge.types", bgesubmod);
	PyDict_SetItemString(PyModule_GetDict(bgemod), "__component__", Py_True);

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

	(void)pc;
	(void)reports;
	(void)maggie;

	return true;

#endif /* WITH_PYTHON */
}

PythonComponent *BKE_python_component_new(char *import, ReportList *reports, bContext *context)
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
	if (!load_component(pc, reports, CTX_data_main(context))) {
		BKE_python_component_free(pc);
		return NULL;
	}

	return pc;
}

PythonComponent *BKE_python_component_create_file(char *import, ReportList *reports, bContext *context)
{
	char *classname;
	char *modulename;
	char filename[FILE_MAX];
	char respath[FILE_MAX];
	size_t filesize = 0;
	unsigned char *orgfilecontent;
	char *filecontent;
	Main *maggie = CTX_data_main(context);
	struct Text *text;
	PythonComponent *pc;

	// Don't bother with an empty string
	if (strcmp(import, "") == 0) {
		BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No component was specified.");
		return NULL;
	}

	// Extract the module name and the class name.
	modulename = strtok(import, ".");
	classname = strtok(NULL, ".");

	if (!classname) {
		BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No component class name was specified.");
		return NULL;
	}

	strcpy(filename, modulename);
	BLI_path_extension_ensure(filename, FILE_MAX, ".py");

	if (BLI_findstring(&maggie->text, filename, offsetof(ID, name) + 2)) {
		BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "File %s already exists.", filename);
		return NULL;
	}

	text = BKE_text_add(maggie, filename);

	BLI_strncpy(respath, BKE_appdir_folder_id(BLENDER_SYSTEM_SCRIPTS, "templates_py"), sizeof(respath));
	BLI_path_append(respath, sizeof(respath), "python_component.py");

	orgfilecontent = BLI_file_read_text_as_mem(respath, 0, &filesize);
	orgfilecontent[filesize] = '\0';

	filecontent = BLI_str_replaceN((char *)orgfilecontent, "%Name%", classname);

	BKE_text_write(text, NULL, (char *)filecontent);

	MEM_freeN(filecontent);

	pc = MEM_callocN(sizeof(PythonComponent), "PythonComponent");

	// Copy module and class names.
	strcpy(pc->module, modulename);
	if (classname) {
		strcpy(pc->name, classname);
	}

	// Try load the component.
	if (!load_component(pc, reports, CTX_data_main(context))) {
		BKE_python_component_free(pc);
		return NULL;
	}

	BKE_reportf(reports, RPT_INFO, "File %s created.", filename);

	return pc;
}

void BKE_python_component_reload(PythonComponent *pc, ReportList *reports, bContext *context)
{
	load_component(pc, reports, CTX_data_main(context));
}

static PythonComponent *copy_component(PythonComponent *comp)
{
	PythonComponent *compn;
	PythonComponentProperty *cprop, *cpropn;

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

void BKE_python_component_copy_list(ListBase *lbn, const ListBase *lbo)
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

void BKE_python_component_free(PythonComponent *pc)
{
	free_properties(&pc->properties);

	MEM_freeN(pc);
}

void BKE_python_component_free_list(ListBase *lb)
{
	PythonComponent *pc;

	while ((pc = lb->first)) {
		BLI_remlink(lb, pc);
		BKE_python_component_free(pc);
	}
}

void *BKE_python_component_argument_dict_new(PythonComponent *pc)
{
#ifdef WITH_PYTHON
	PythonComponentProperty *cprop = (PythonComponentProperty *)pc->properties.first;
	PyObject *args = PyDict_New();

	while (cprop) {
		PyObject *value;
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
		else if (cprop->type == CPROP_TYPE_SET) {
			LinkData *link = BLI_findlink(&cprop->enumval, cprop->itemval);
			value = PyUnicode_FromString(link->data);
		}
		else if (cprop->type == CPROP_TYPE_VEC2 ||
				 cprop->type == CPROP_TYPE_VEC3 ||
				 cprop->type == CPROP_TYPE_VEC4)
		{
			int size;
			switch (cprop->type) {
				case CPROP_TYPE_VEC2:
					size = 2;
					break;
				case CPROP_TYPE_VEC3:
					size = 3;
					break;
				case CPROP_TYPE_VEC4:
					size = 4;
					break;
			}
			value = PyList_New(size);
			// Fill the vector list.
			for (unsigned int i = 0; i < size; ++i) {
				PyList_SetItem(value, i, PyFloat_FromDouble(cprop->vec[i]));
			}
		}
		else {
			cprop = cprop->next;
			continue;
		}

		PyDict_SetItemString(args, cprop->name, value);

		cprop = cprop->next;
	}

	return args;

#else

	(void)pc;

	return NULL;

#endif /* WITH_PYTHON */
}
