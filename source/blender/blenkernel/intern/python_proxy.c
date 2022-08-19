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

#include "BLI_fileops.h"

#include "BLI_listbase.h"

#include "BLI_path_util.h"
#include "BLI_string.h"
#include "DNA_property_types.h" /* For MAX_PROPSTRING */
#include "DNA_python_proxy_types.h"
#include "DNA_windowmanager_types.h"
#include "MEM_guardedalloc.h"

#include "BKE_appdir.h"
#include "BKE_context.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_python_proxy.h"
#include "BKE_report.h"
#include "BKE_text.h"

#include "RNA_types.h"

#ifdef WITH_PYTHON
#  include "Python.h"

#  include "../../python/intern/bpy_rna.h"
#  include "generic/bpy_internal_import.h"
#  include "generic/py_capi_utils.h"
#endif

#include <string.h>

#ifdef WITH_PYTHON

#  define FAKE_TYPES \
    FT_DEF(KX_2DFilter, NULL) \
    FT_DEF(KX_2DFilterManager, NULL) \
    FT_DEF(KX_2DFilterOffScreen, NULL) \
    FT_DEF(KX_BlenderMaterial, NULL) \
    FT_DEF(KX_CharacterWrapper, NULL) \
    FT_DEF(KX_CollisionContactPoint, NULL) \
    FT_DEF(KX_ConstraintWrapper, NULL) \
    FT_DEF(KX_GameObject, NULL) \
    FT_DEF(KX_FontObject, &FT_KX_GameObject) \
    FT_DEF(KX_Camera, &FT_KX_GameObject) \
    FT_DEF(KX_LibLoadStatus, NULL) \
    FT_DEF(KX_LightObject, &FT_KX_GameObject) \
    FT_DEF(KX_LodLevel, NULL) \
    FT_DEF(KX_LodManager, NULL) \
    FT_DEF(KX_MeshProxy, NULL) \
    FT_DEF(KX_NavMeshObject, &FT_KX_GameObject) \
    FT_DEF(KX_PolyProxy, NULL) \
    FT_DEF(KX_PythonComponent, NULL) \
    FT_DEF(KX_Scene, NULL) \
    FT_DEF(KX_VehicleWrapper, NULL) \
    FT_DEF(KX_VertexProxy, NULL) \
    FT_DEF(BL_ArmatureBone, NULL) \
    FT_DEF(BL_ArmatureChannel, NULL) \
    FT_DEF(BL_ArmatureConstraint, NULL) \
    FT_DEF(BL_ArmatureObject, &FT_KX_GameObject) \
    FT_DEF(BL_Shader, NULL) \
    FT_DEF(BL_Texture, NULL) \
    FT_DEF(SCA_2DFilterActuator, NULL) \
    FT_DEF(SCA_ANDController, NULL) \
    FT_DEF(SCA_ActionActuator, NULL) \
    FT_DEF(SCA_ActuatorSensor, NULL) \
    FT_DEF(SCA_AddObjectActuator, NULL) \
    FT_DEF(SCA_AlwaysSensor, NULL) \
    FT_DEF(SCA_ArmatureActuator, NULL) \
    FT_DEF(SCA_ArmatureSensor, NULL) \
    FT_DEF(SCA_CameraActuator, NULL) \
    FT_DEF(SCA_CollisionSensor, NULL) \
    FT_DEF(SCA_ConstraintActuator, NULL) \
    FT_DEF(SCA_DelaySensor, NULL) \
    FT_DEF(SCA_DynamicActuator, NULL) \
    FT_DEF(SCA_EndObjectActuator, NULL) \
    FT_DEF(SCA_GameActuator, NULL) \
    FT_DEF(SCA_IActuator, NULL) \
    FT_DEF(SCA_IController, NULL) \
    FT_DEF(SCA_ILogicBrick, NULL) \
    FT_DEF(SCA_IObject, NULL) \
    FT_DEF(SCA_ISensor, NULL) \
    FT_DEF(SCA_InputEvent, NULL) \
    FT_DEF(SCA_JoystickSensor, NULL) \
    FT_DEF(SCA_KeyboardSensor, NULL) \
    FT_DEF(SCA_MouseActuator, NULL) \
    FT_DEF(SCA_MouseFocusSensor, NULL) \
    FT_DEF(SCA_MouseSensor, NULL) \
    FT_DEF(SCA_NANDController, NULL) \
    FT_DEF(SCA_NORController, NULL) \
    FT_DEF(SCA_NearSensor, NULL) \
    FT_DEF(SCA_NetworkMessageActuator, NULL) \
    FT_DEF(SCA_NetworkMessageSensor, NULL) \
    FT_DEF(SCA_ORController, NULL) \
    FT_DEF(SCA_ObjectActuator, NULL) \
    FT_DEF(SCA_ParentActuator, NULL) \
    FT_DEF(SCA_PropertyActuator, NULL) \
    FT_DEF(SCA_PropertySensor, NULL) \
    FT_DEF(SCA_PythonController, NULL) \
    FT_DEF(SCA_PythonJoystick, NULL) \
    FT_DEF(SCA_PythonKeyboard, NULL) \
    FT_DEF(SCA_PythonMouse, NULL) \
    FT_DEF(SCA_RadarSensor, NULL) \
    FT_DEF(SCA_RandomActuator, NULL) \
    FT_DEF(SCA_RandomSensor, NULL) \
    FT_DEF(SCA_RaySensor, NULL) \
    FT_DEF(SCA_ReplaceMeshActuator, NULL) \
    FT_DEF(SCA_SceneActuator, NULL) \
    FT_DEF(SCA_SoundActuator, NULL) \
    FT_DEF(SCA_StateActuator, NULL) \
    FT_DEF(SCA_SteeringActuator, NULL) \
    FT_DEF(SCA_TrackToActuator, NULL) \
    FT_DEF(SCA_VibrationActuator, NULL) \
    FT_DEF(SCA_VisibilityActuator, NULL) \
    FT_DEF(SCA_XNORController, NULL) \
    FT_DEF(SCA_XORController, NULL)

#  define FakeType(Type, Base) \
    static PyTypeObject FT_##Type = { \
        PyVarObject_HEAD_INIT(NULL, 0) STRINGIFY(Type), /* tp_name */ \
        sizeof(PyObject),                               /* tp_basicsize */ \
        0,                                              /* tp_itemsize */ \
        (destructor)NULL,                               /* tp_dealloc */ \
        0,                                              /* tp_vectorcall_offset */ \
        NULL,                                           /* tp_getattr */ \
        NULL,                                           /* tp_setattr */ \
        NULL,                                           /* tp_compare */ \
        (reprfunc)NULL,                                 /* tp_repr */ \
        NULL,                                           /* tp_as_number */ \
        NULL,                                           /* tp_as_sequence */ \
        NULL,                                           /* tp_as_mapping */ \
        (hashfunc)NULL,                                 /* tp_hash */ \
        NULL,                                           /* tp_call */ \
        NULL,                                           /* tp_str */ \
        NULL,                                           /* tp_getattro */ \
        NULL,                                           /* tp_setattro */ \
        NULL,                                           /* tp_as_buffer */ \
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,       /* tp_flags */ \
        NULL,                                           /* tp_doc */ \
        (traverseproc)NULL,                             /* tp_traverse */ \
        (inquiry)NULL,                                  /* tp_clear */ \
        (richcmpfunc)NULL,                              /* tp_richcompare */ \
        0,                                              /* tp_weaklistoffset */ \
        NULL,                                           /* tp_iter */ \
        NULL,                                           /* tp_iternext */ \
        NULL,                                           /* tp_methods */ \
        NULL,                                           /* tp_members */ \
        NULL,                                           /* tp_getset */ \
        Base,                                           /* tp_base */ \
        NULL,                                           /* tp_dict */ \
        NULL,                                           /* tp_descr_get */ \
        NULL,                                           /* tp_descr_set */ \
        0,                                              /* tp_dictoffset */ \
        NULL,                                           /* tp_init */ \
        PyType_GenericAlloc,                            /* tp_alloc */ \
        PyType_GenericNew,                              /* tp_new */ \
        NULL,                                           /* tp_free */ \
        NULL,                                           /* tp_is_gc */ \
        NULL,                                           /* tp_bases */ \
        NULL,                                           /* tp_mro */ \
        NULL,                                           /* tp_cache */ \
        NULL,                                           /* tp_subclasses */ \
        NULL,                                           /* tp_weaklist */ \
        NULL                                            /* tp_del */ \
    };

#  define FT_DEF(Type, Base) FakeType(Type, Base)
FAKE_TYPES
#  undef FT_DEF

PyDoc_STRVAR(module_documentation,
             "This is the fake BGE API module used only to import core classes from bge.types");

static struct PyModuleDef bge_module_def = {
    PyModuleDef_HEAD_INIT, /* m_base */
    "bge",                 /* m_name */
    module_documentation,  /* m_doc */
    0,                     /* m_size */
    NULL,                  /* m_methods */
    NULL,                  /* m_reload */
    NULL,                  /* m_traverse */
    NULL,                  /* m_clear */
    NULL,                  /* m_free */
};

static struct PyModuleDef bge_types_module_def = {
    PyModuleDef_HEAD_INIT, /* m_base */
    "types",               /* m_name */
    module_documentation,  /* m_doc */
    0,                     /* m_size */
    NULL,                  /* m_methods */
    NULL,                  /* m_reload */
    NULL,                  /* m_traverse */
    NULL,                  /* m_clear */
    NULL,                  /* m_free */
};

static int verify_custom_object_class(PyObject *cls)
{
  return PyType_IsSubtype((PyTypeObject *)cls, &FT_KX_GameObject);
}

static int verify_component_class(PyObject *cls)
{
  return PyType_IsSubtype((PyTypeObject *)cls, &FT_KX_PythonComponent);
}

static PythonProxyProperty *create_property(char *name)
{
  PythonProxyProperty *pprop;

  pprop = MEM_callocN(sizeof(PythonProxyProperty), "PythonProxyProperty");
  BLI_strncpy(pprop->name, name, sizeof(pprop->name));

  return pprop;
}

#endif

static PythonProxyProperty *copy_property(PythonProxyProperty *pprop)
{
  PythonProxyProperty *ppropn;

  ppropn = MEM_dupallocN(pprop);

  BLI_duplicatelist(&ppropn->enumval, &pprop->enumval);
  for (LinkData *link = ppropn->enumval.first; link; link = link->next) {
    link->data = MEM_dupallocN(link->data);
  }

  return ppropn;
}

static void free_property(PythonProxyProperty *pprop)
{
  for (LinkData *link = pprop->enumval.first; link; link = link->next) {
    MEM_freeN(link->data);
  }
  BLI_freelistN(&pprop->enumval);
  MEM_freeN(pprop);
}

static void free_properties(ListBase *lb)
{
  PythonProxyProperty *pprop;

  while ((pprop = lb->first)) {
    BLI_remlink(lb, pprop);
    free_property(pprop);
  }
}

#ifdef WITH_PYTHON
static void create_properties(PythonProxy *pp, PyObject *cls)
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
    PythonProxyProperty *pprop;
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

    pprop = create_property(name);

    // Determine the type and default value
    if (PyBool_Check(pyvalue)) {
      pprop->type = PPROP_TYPE_BOOLEAN;
      pprop->boolval = PyLong_AsLong(pyvalue) != 0;
    }
    else if (PyLong_Check(pyvalue)) {
      pprop->type = PPROP_TYPE_INT;
      pprop->intval = PyLong_AsLong(pyvalue);
    }
    else if (PyFloat_Check(pyvalue)) {
      pprop->type = PPROP_TYPE_FLOAT;
      pprop->floatval = (float)PyFloat_AsDouble(pyvalue);
    }
    else if (PyUnicode_Check(pyvalue)) {
      pprop->type = PPROP_TYPE_STRING;
      BLI_strncpy((char *)pprop->strval, _PyUnicode_AsString(pyvalue), MAX_PROPSTRING);
    }
    else if (PySet_Check(pyvalue)) {
      PyObject *iterator = PyObject_GetIter(pyvalue), *v = NULL;
      unsigned int j = 0;
      pprop->type = PPROP_TYPE_SET;

      memset(&pprop->enumval, 0, sizeof(ListBase));
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
        BLI_addtail(&pprop->enumval, link);

        Py_DECREF(v);
        ++j;
      }
      Py_DECREF(iterator);
      pprop->itemval = 0;
    }
    else if (PySequence_Check(pyvalue)) {
      int len = PySequence_Size(pyvalue);
      switch (len) {
        case 2:
          pprop->type = PPROP_TYPE_VEC2;
          break;
        case 3:
          pprop->type = PPROP_TYPE_VEC3;
          break;
        case 4:
          pprop->type = PPROP_TYPE_VEC4;
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
            pprop->vec[j] = PyFloat_AsDouble(item);
          }
          else {
            printf("Sequence property \"%s\" contains a non-float item (%u)\n", name, j);
          }
          Py_DECREF(item);
        }
      }
    }
    else if (PyType_Check(pyvalue)) {
      const char *tp_name = ((PyTypeObject *)pyvalue)->tp_name;

      free = true;

#  define PT_DEF(name, lower, upper) \
    if (!strcmp(tp_name, STRINGIFY(name))) { \
      pprop->type = PPROP_TYPE_##upper; \
      free = false; \
    }
      POINTER_TYPES
#  undef PT_DEF

      if (free) {
        printf("Unsupported pointer type %s found for property \"%s\", skipping\n",
               Py_TYPE(pyvalue)->tp_name,
               name);
      }
    }
    else {
      // Unsupported type
      printf("Unsupported type %s found for property \"%s\", skipping\n",
             Py_TYPE(pyvalue)->tp_name,
             name);
      free = true;
    }

    if (free) {
      free_property(pprop);
      continue;
    }

    bool found = false;
    for (PythonProxyProperty *propit = pp->properties.first; propit; propit = propit->next) {
      if ((strcmp(propit->name, pprop->name) == 0) && propit->type == pprop->type) {
        /* We never reuse a enum property because we don't know if one of the
         * enum value was modified and it easier to just copy the current item
         * index than the list.
         */
        if (pprop->type == PPROP_TYPE_SET) {
          /* Unfortunatly the python type set doesn't repect an order even with same
           * content. To solve that we iterate on all new enums and find the coresponding
           * index for the old enum name.
           */
          char *str = ((LinkData *)BLI_findlink(&propit->enumval, propit->itemval))->data;
          int j = 0;
          for (LinkData *link = pprop->enumval.first; link; link = link->next) {
            if (strcmp(link->data, str) == 0) {
              pprop->itemval = j;
            }
            ++j;
          }
          break;
        }
        /* We found a coresponding property in the old component, so the new one
         * is released, the old property is removed from the original list and
         * added to the new list.
         */
        free_property(pprop);
        /* The exisiting property is removed to allow at the end free properties
         * that are no longuer used.
         */
        BLI_remlink(&pp->properties, propit);
        BLI_addtail(&properties, propit);
        found = true;
        break;
      }
    }
    // If no exisiting property was found we add it simply.
    if (!found) {
      BLI_addtail(&properties, pprop);
    }
  }

  // Free properties no used in the new component.
  for (PythonProxyProperty *propit = pp->properties.first; propit;) {
    PythonProxyProperty *prop = propit;
    propit = propit->next;
    free_property(prop);
  }
  // Set the new property list.
  pp->properties = properties;
}


static bool load_class(PythonProxy *pp,
                       int (*verifier)(PyObject *),
                       ReportList *reports,
                       Main *maggie)
{

/* Macro used to release all python variable if the convertion fail or succeed.
 * The "value" argument is false on failure and true on succes.
 */
/* Macro used to release all python variable if the convertion fail or succeed.
 * The "value" argument is false on failure and true on succes.
 */
#  define FINISH(value) \
    PyErr_Print(); \
    if (mod) { \
      /* Take the module out of the module list so it's not cached \
         by Python (this allows for simpler reloading of components)*/ \
      PyDict_DelItemString(sys_modules, pp->module); \
    } \
    Py_XDECREF(mod); \
    Py_XDECREF(item); \
    PyDict_DelItemString(sys_modules, "bge"); \
    PyDict_DelItemString(sys_modules, "bge.types"); \
    BLI_split_dir_part(maggie->filepath, path, sizeof(path)); \
    pypath = PyC_UnicodeFromByte(path); \
    index = PySequence_Index(sys_path, pypath); \
    /* Safely remove the value by finding their index. */ \
    if (index != -1) { \
      PySequence_DelItem(sys_path, index); \
    } \
    Py_DECREF(pypath); \
    for (Library *lib = (Library *)maggie->libraries.first; lib; lib = (Library *)lib->id.next) { \
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

  bpy_import_init(PyEval_GetBuiltins());
  bpy_import_main_set(maggie);

  // Set the current file directory do import path to allow extern modules.
  sys_path = PySys_GetObject("path");
  /* Add to sys.path the path to all the used library to follow game engine sys.path management.
   * These path are remove later in FINISH. */
  for (Library *lib = (Library *)maggie->libraries.first; lib; lib = (Library *)lib->id.next) {
    BLI_split_dir_part(lib->filepath, path, sizeof(path));
    pypath = PyC_UnicodeFromByte(path);
    PyList_Insert(sys_path, 0, pypath);
    Py_DECREF(pypath);
  }
  /* Add default path */
  BLI_split_dir_part(maggie->filepath, path, sizeof(path));
  pypath = PyC_UnicodeFromByte(path);
  PyList_Insert(sys_path, 0, pypath);
  Py_DECREF(pypath);

  // Setup BGE fake module and submodule.
  sys_modules = PyImport_GetModuleDict();
  bgemod = PyModule_Create(&bge_module_def);
  bgesubmod = PyModule_Create(&bge_types_module_def);

  PyModule_AddObject(bgemod, "types", bgesubmod);
  PyType_Ready(&FT_KX_PythonComponent);

#  define FT_DEF(Type, NULL) \
    PyType_Ready(&FT_##Type); \
    PyModule_AddObject(bgesubmod, STRINGIFY(Type), (PyObject *)&FT_##Type);

  FAKE_TYPES
#  undef FT_DEF

  PyDict_SetItemString(sys_modules, "bge", bgemod);
  PyDict_SetItemString(sys_modules, "bge.types", bgesubmod);
  PyDict_SetItemString(PyModule_GetDict(bgemod), "__component__", Py_True);

  // Try to load up the module
  mod = PyImport_ImportModule(pp->module);

  if (!mod) {
    BKE_reportf(reports,
                RPT_ERROR_INVALID_INPUT,
                "No module named \"%s\" or script error at loading.",
                pp->module);
    FINISH(false);
  }
  else if (strlen(pp->module) > 0 && strlen(pp->name) == 0) {
    BKE_report(reports,
               RPT_ERROR_INVALID_INPUT,
               "No component class was specified, only the module was.");
    FINISH(false);
  }

  item = PyObject_GetAttrString(mod, pp->name);
  if (!item) {
    BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "No class named %s was found.", pp->name);
    FINISH(false);
  }

  // Check the subclass with our own function since we don't have access to the KX_PythonComponent
  // type object
  if (!verifier(item)) {
    BKE_reportf(reports,
                RPT_ERROR_INVALID_INPUT,
                "A %s class was found, but it was not of an expected subtype.",
                pp->name);
    FINISH(false);
  }
  else {
    // Setup the properties
    create_properties(pp, item);
  }

  FINISH(true);

#  undef ERROR

}

PythonProxy *BKE_python_class_new(char *import,
                                  int (*verifier)(PyObject *),
                                  ReportList *reports,
                                  bContext *context)
{
  char *classname;
  char *modulename;
  PythonProxy *pp;

  // Don't bother with an empty string
  if (strcmp(import, "") == 0) {
    BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No class was specified.");
    return NULL;
  }

  // Extract the module name and the class name.
  modulename = strdup(import);

  char *pos = strrchr(modulename, '.');

  if (pos) {
    *pos = '\0';
    classname = pos + 1;
  }
  else {
    BKE_report(reports, RPT_ERROR_INVALID_INPUT, "Invalid module name.");
    return NULL;
  }

  pp = MEM_callocN(sizeof(PythonProxy), "PythonProxy");

  // Copy module and class names.
  strcpy(pp->module, modulename);
  if (classname) {
    strcpy(pp->name, classname);
  }

  // Try load the component.
  if (!load_class(pp, verifier, reports, CTX_data_main(context))) {
    BKE_python_proxy_free(pp);
    return NULL;
  }

  return pp;
}
#endif /* WITH_PYTHON */

PythonProxy *BKE_custom_object_new(char *import, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  return BKE_python_class_new(import, verify_custom_object_class, reports, context);
#else
(void)import;
(void)reports;
(void)context;

return NULL;
#endif /* WITH_PYTHON */
}

PythonProxy *BKE_python_component_new(char *import, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  return BKE_python_class_new(import, verify_component_class, reports, context);
#else
(void)import;
(void)reports;
(void)context;

return NULL;
#endif /* WITH_PYTHON */
}

#ifdef WITH_PYTHON
PythonProxy *BKE_python_class_create_file(char *import,
                                          const char *template_dir,
                                          const char *template_name,
                                          int (*verifier)(PyObject *),
                                          ReportList *reports,
                                          bContext *context)
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
  PythonProxy *pp;

  // Don't bother with an empty string
  if (strcmp(import, "") == 0) {
    BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No class name was specified.");
    return NULL;
  }

  // Extract the module name and the class name.
  modulename = strtok(import, ".");
  classname = strtok(NULL, ".");

  if (!classname) {
    BKE_report(reports, RPT_ERROR_INVALID_INPUT, "No class name was specified.");
    return NULL;
  }

  strcpy(filename, modulename);
  BLI_path_extension_ensure(filename, FILE_MAX, ".py");

  if (BLI_findstring(&maggie->texts, filename, offsetof(ID, name) + 2)) {
    BKE_reportf(reports, RPT_ERROR_INVALID_INPUT, "File %s already exists.", filename);
    return NULL;
  }

  text = BKE_text_add(maggie, filename);

  BLI_strncpy(
      respath, BKE_appdir_folder_id(BLENDER_SYSTEM_SCRIPTS, template_dir), sizeof(respath));
  BLI_path_append(respath, sizeof(respath), template_name);

  orgfilecontent = BLI_file_read_text_as_mem(respath, 0, &filesize);
  orgfilecontent[filesize] = '\0';

  filecontent = BLI_str_replaceN((char *)orgfilecontent, "%Name%", classname);

  BKE_text_write(text, filecontent, strlen(filecontent));

  MEM_freeN(filecontent);

  pp = MEM_callocN(sizeof(PythonProxy), "PythonProxy");

  // Copy module and class names.
  strcpy(pp->module, modulename);
  if (classname) {
    strcpy(pp->name, classname);
  }

  // Try load the component.
  if (!load_class(pp, verifier, reports, CTX_data_main(context))) {
    BKE_python_proxy_free(pp);
    return NULL;
  }

  BKE_reportf(reports, RPT_INFO, "File %s created.", filename);

  return pp;
}
#endif /* WITH_PYTHON */

PythonProxy *BKE_custom_object_create_file(char *import, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  return BKE_python_class_create_file(import,
                                      "templates_custom_objects",
                                      "custom_object.py",
                                      verify_custom_object_class,
                                      reports,
                                      context);
#else
(void)import;
(void)reports;
(void)context;

return NULL;
#endif /* WITH_PYTHON */
}

PythonProxy *BKE_python_component_create_file(char *import, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  return BKE_python_class_create_file(import,
                                      "templates_py_components",
                                      "python_component.py",
                                      verify_component_class,
                                      reports,
                                      context);
#else
  (void)import;
  (void)reports;
  (void)context;

  return NULL;
#endif /* WITH_PYTHON */
}

void BKE_custom_object_reload(PythonProxy *pp, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  load_class(pp, verify_custom_object_class, reports, CTX_data_main(context));
#else
  (void)pp;
  (void)reports;
  (void)context;
#endif /* WITH_PYTHON */
}

void BKE_python_component_reload(PythonProxy *pp, ReportList *reports, bContext *context)
{
#ifdef WITH_PYTHON
  load_class(pp, verify_component_class, reports, CTX_data_main(context));
#else
  (void)pp;
  (void)reports;
  (void)context;
#endif /* WITH_PYTHON */
}

PythonProxy *BKE_python_proxy_copy(PythonProxy *pp)
{
#ifdef WITH_PYTHON
  PythonProxy *proxyn;
  PythonProxyProperty *pprop, *ppropn;

  proxyn = MEM_dupallocN(pp);

  BLI_listbase_clear(&proxyn->properties);
  pprop = pp->properties.first;
  while (pprop) {
    ppropn = copy_property(pprop);
    BLI_addtail(&proxyn->properties, ppropn);
    pprop = pprop->next;
  }

  return proxyn;
#else
  (void)pp;

  return NULL;
#endif /* WITH_PYTHON */
}

void BKE_python_proxy_copy_list(ListBase *lbn, const ListBase *lbo)
{
#ifdef WITH_PYTHON
  PythonProxy *proxy, *proxyn;

  lbn->first = lbn->last = NULL;
  proxy = lbo->first;
  while (proxy) {
    proxyn = BKE_python_proxy_copy(proxy);
    BLI_addtail(lbn, proxyn);
    proxy = proxy->next;
  }
#else
  (void)lbn;
  (void)lbo;
#endif /* WITH_PYTHON */
}

void BKE_python_proxy_free(PythonProxy *pp)
{
#ifdef WITH_PYTHON
  free_properties(&pp->properties);

  MEM_freeN(pp);
#else
  (void)pp;
#endif /* WITH_PYTHON */
}

void BKE_python_proxy_free_list(ListBase *lb)
{
#ifdef WITH_PYTHON
  PythonProxy *pp;

  while ((pp = lb->first)) {
    BLI_remlink(lb, pp);
    BKE_python_proxy_free(pp);
  }
#else
  (void)lb;
#endif /* WITH_PYTHON */
}

void *BKE_python_proxy_argument_dict_new(PythonProxy *pp)
{
#ifdef WITH_PYTHON
  PythonProxyProperty *pprop = (PythonProxyProperty *)pp->properties.first;
  PyObject *args = PyDict_New();

  while (pprop) {
    PyObject *value;
    if (pprop->type == PPROP_TYPE_INT) {
      value = PyLong_FromLong(pprop->intval);
    }
    else if (pprop->type == PPROP_TYPE_FLOAT) {
      value = PyFloat_FromDouble(pprop->floatval);
    }
    else if (pprop->type == PPROP_TYPE_BOOLEAN) {
      value = PyBool_FromLong(pprop->boolval);
    }
    else if (pprop->type == PPROP_TYPE_STRING) {
      value = PyUnicode_FromString(pprop->strval);
    }
    else if (pprop->type == PPROP_TYPE_SET) {
      LinkData *link = BLI_findlink(&pprop->enumval, pprop->itemval);
      value = PyUnicode_FromString(link->data);
    }
    else if (pprop->type == PPROP_TYPE_VEC2 || pprop->type == PPROP_TYPE_VEC3 ||
             pprop->type == PPROP_TYPE_VEC4) {
      int size;
      switch (pprop->type) {
        case PPROP_TYPE_VEC2:
          size = 2;
          break;
        case PPROP_TYPE_VEC3:
          size = 3;
          break;
        case PPROP_TYPE_VEC4:
          size = 4;
          break;
      }
      value = PyList_New(size);
      // Fill the vector list.
      for (unsigned int i = 0; i < size; ++i) {
        PyList_SetItem(value, i, PyFloat_FromDouble(pprop->vec[i]));
      }
    }
#  define PT_DEF(name, lower, upper) \
    else if (pprop->type == PPROP_TYPE_##upper && pprop->lower) \
    { \
      ID *id = &pprop->lower->id; \
      if (id) { \
        if (!id->py_instance) { \
          id->py_instance = pyrna_id_CreatePyObject(id); \
        } \
        value = (PyObject *)id->py_instance; \
      } \
      else { \
        pprop = pprop->next; \
        continue; \
      } \
    }
    POINTER_TYPES
#  undef PT_DEF
    else
    {
      pprop = pprop->next;
      continue;
    }

    PyDict_SetItemString(args, pprop->name, value);

    pprop = pprop->next;
  }

  return args;

#else
  (void)pp;

  return NULL;
#endif /* WITH_PYTHON */
}

void BKE_python_proxy_id_loop(PythonProxy *pp, BKEPyProxyIDFunc func, void *userdata)
{
#ifdef WITH_PYTHON
  ListBase *properties = &pp->properties;
  PythonProxyProperty *prop;

  for (prop = properties->first; prop; prop = prop->next) {
#define PT_DEF(name, lower, upper) func(pp, (ID **)&prop->lower, userdata, IDWALK_CB_USER);
    POINTER_TYPES
#undef PT_DEF
  }

#else
  (void)pp;
  (void)func;
  (void)userdata;
#endif /* WITH_PYTHON */
}

void BKE_python_proxies_id_loop(ListBase *list, BKEPyProxyIDFunc func, void *userdata)
{
#ifdef WITH_PYTHON
  PythonProxy *pp;

  for (pp = list->first; pp; pp = pp->next) {
    BKE_python_proxy_id_loop(pp, func, userdata);
  }
#else
  (void)list;
  (void)func;
  (void)userdata;
#endif /* WITH_PYTHON */
}
