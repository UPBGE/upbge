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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * Initialize Python thingies.
 */

/** \file gameengine/Ketsji/KX_PythonInit.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#ifdef WITH_PYTHON
#  ifdef _XOPEN_SOURCE
#    undef _XOPEN_SOURCE
#  endif

#  include "KX_PythonInit.h"

#  include <Python.h>

#  include "../blender/python/BPY_extern.h"
#  include "../blender/python/BPY_extern_python.h"
#  include "BKE_appdir.h"
#  include "BKE_blender_version.h"
#  include "BKE_context.h"
#  include "BKE_global.h"
#  include "BKE_idtype.h"
#  include "BKE_library.h"
#  include "BKE_main.h"
#  include "BLI_blenlib.h"
#  include "BLI_utildefines.h"
#  include "DNA_ID.h"
#  include "DNA_scene_types.h"
#  include "GPU_material.h"
#  include "MEM_guardedalloc.h"
#  include "bgl.h"
#  include "bl_math_py_api.h"
#  include "blf_py_api.h"
#  include "bmesh/bmesh_py_api.h"
#  include "bpy.h"  // for bpy_sys_module_backup
#  include "bpy_intern_string.h"
#  include "bpy_internal_import.h" /* from the blender python api, but we want to import text too! */
#  include "bpy_path.h"
#  include "bpy_rna.h"
#  include "gpu/gpu_py_api.h"
#  include "idprop_py_api.h"
#  include "imbuf_py_api.h"
#  include "marshal.h"    /* python header for loading/saving dicts */
#  include "mathutils.h"  // 'mathutils' module copied here so the blenderlayer can use.
#  include "py_capi_utils.h"
#  include "python_utildefines.h"

#  ifdef WITH_AUDASPACE
#    include "../../../../intern/audaspace/intern/AUD_PyInit.h"
#  endif  // WITH_AUDASPACE

#  ifdef WITH_CYCLES
#    include "../../intern/cycles/blender/CCL_api.h"
#  endif

#  ifdef WITH_FLUID
#    include "../../intern/mantaflow/extern/manta_python_API.h"
#  endif

#endif /* WITH_PYTHON */

// directory header for py function getBlendFileList
#ifndef WIN32
#  include <dirent.h>
#  include <stdlib.h>
#else
#  include "BLI_winstuff.h"
#  include <io.h>
#endif

// python physics binding
#include "BL_Action.h"
#include "BL_Converter.h"
#include "BL_Shader.h"
#include "CM_Message.h"
#include "KX_Globals.h"
#include "KX_LibLoadStatus.h"
#include "KX_MeshProxy.h" /* for creating a new library of mesh objects */
#include "KX_NavMeshObject.h"
#include "KX_NetworkMessageScene.h"  //Needed for sendMessage()
#include "KX_PyConstraintBinding.h"
#include "KX_PyMath.h"
#include "KX_PythonInitTypes.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_2DFilterManager.h"
#include "RAS_ICanvas.h"
#include "SCA_ActionActuator.h"
#include "SCA_ArmatureSensor.h"
#include "SCA_ConstraintActuator.h"
#include "SCA_DynamicActuator.h"
#include "SCA_GameActuator.h"
#include "SCA_IInputDevice.h"
#include "SCA_JoystickManager.h" /* JOYINDEX_MAX */
#include "SCA_MouseActuator.h"
#include "SCA_MovementSensor.h"
#include "SCA_ParentActuator.h"
#include "SCA_PropertySensor.h"
#include "SCA_PythonJoystick.h"
#include "SCA_PythonKeyboard.h"
#include "SCA_PythonMouse.h"
#include "SCA_RadarSensor.h"
#include "SCA_RandomActuator.h"
#include "SCA_RaySensor.h"
#include "SCA_SceneActuator.h"
#include "SCA_SoundActuator.h"
#include "SCA_StateActuator.h"
#include "SCA_SteeringActuator.h"
#include "SCA_TrackToActuator.h"

// 'local' copy of canvas ptr, for window height/width python scripts

#ifdef WITH_PYTHON

static SCA_PythonKeyboard *gp_PythonKeyboard = nullptr;
static SCA_PythonMouse *gp_PythonMouse = nullptr;
static SCA_PythonJoystick *gp_PythonJoysticks[JOYINDEX_MAX] = {nullptr};

static struct {
  PyObject *path;
  PyObject *meta_path;
  PyObject *modules;
} gp_sys_backup = {nullptr};

/* Macro for building the keyboard translation */
//#define KX_MACRO_addToDict(dict, name) PyDict_SetItemString(dict, #name,
// PyLong_FromLong(SCA_IInputDevice::##name)) #define KX_MACRO_addToDict(dict, name)
// PyDict_SetItemString(dict, #name, item=PyLong_FromLong(name)); Py_DECREF(item)
/* For the defines for types from logic bricks, we do stuff explicitly... */
#  define KX_MACRO_addTypesToDict(dict, name, value) KX_MACRO_addTypesToDict_fn(dict, #  name, value)
static void KX_MACRO_addTypesToDict_fn(PyObject *dict, const char *name, long value)
{
  PyObject *item;

  item = PyLong_FromLong(value);
  PyDict_SetItemString(dict, name, item);
  Py_DECREF(item);
}

// temporarily python stuff, will be put in another place later !
#  include "EXP_Python.h"
#  include "SCA_PythonController.h"
// List of methods defined in the module

static PyObject *ErrorObject;

PyDoc_STRVAR(gPyGetRandomFloat_doc,
             "getRandomFloat()\n"
             "returns a random floating point value in the range [0..1]");
static PyObject *gPyGetRandomFloat(PyObject *)
{
  return PyFloat_FromDouble(MT_random());
}

static PyObject *gPySetGravity(PyObject *, PyObject *value)
{
  MT_Vector3 vec;
  if (!PyVecTo(value, vec))
    return nullptr;

  KX_Scene *scene = KX_GetActiveScene();
  if (scene)
    scene->SetGravity(vec);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    gPyExpandPath_doc,
    "expandPath(path)\n"
    "Converts a blender internal path into a proper file system path.\n"
    " path - the string path to convert.\n"
    "Use / as directory separator in path\n"
    "You can use '//' at the start of the string to define a relative path."
    "Blender replaces that string by the directory of the current .blend or runtime file to make "
    "a full path name.\n"
    "The function also converts the directory separator to the local file system format.");
static PyObject *gPyExpandPath(PyObject *, PyObject *args)
{
  char expanded[FILE_MAX];
  char *filename;

  if (!PyArg_ParseTuple(args, "s:ExpandPath", &filename))
    return nullptr;

  BLI_strncpy(expanded, filename, FILE_MAX);
  BLI_path_abs(expanded, KX_GetMainPath().c_str());
  return PyC_UnicodeFromByte(expanded);
}

PyDoc_STRVAR(gPyStartGame_doc,
             "startGame(blend)\n"
             "Loads the blend file");
static PyObject *gPyStartGame(PyObject *, PyObject *args)
{
  char *blendfile = (char *)"";

  if (!PyArg_ParseTuple(args, "s:startGame", &blendfile))
    return nullptr;

  KX_GetActiveEngine()->RequestExit(KX_ExitRequest::START_OTHER_GAME);
  KX_GetActiveEngine()->SetNameNextGame(blendfile);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(gPyEndGame_doc,
             "endGame()\n"
             "Ends the current game");
static PyObject *gPyEndGame(PyObject *)
{
  KX_GetActiveEngine()->RequestExit(KX_ExitRequest::QUIT_GAME);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(gPyRestartGame_doc,
             "restartGame()\n"
             "Restarts the current game by reloading the .blend file");
static PyObject *gPyRestartGame(PyObject *)
{
  KX_GetActiveEngine()->RequestExit(KX_ExitRequest::RESTART_GAME);
  KX_GetActiveEngine()->SetNameNextGame(KX_GetMainPath());

  Py_RETURN_NONE;
}

PyDoc_STRVAR(gPySaveGlobalDict_doc,
             "saveGlobalDict()\n"
             "Saves bge.logic.globalDict to a file");
static PyObject *gPySaveGlobalDict(PyObject *)
{
  saveGamePythonConfig();

  Py_RETURN_NONE;
}

PyDoc_STRVAR(gPyLoadGlobalDict_doc,
             "LoadGlobalDict()\n"
             "Loads bge.logic.globalDict from a file");
static PyObject *gPyLoadGlobalDict(PyObject *)
{
  loadGamePythonConfig();

  Py_RETURN_NONE;
}

PyDoc_STRVAR(gPyGetProfileInfo_doc,
             "getProfileInfo()\n"
             "returns a dictionary with profiling information");
static PyObject *gPyGetProfileInfo(PyObject *)
{
  return KX_GetActiveEngine()->GetPyProfileDict();
}

PyDoc_STRVAR(gPySendMessage_doc,
             "sendMessage(subject, [body, to, from])\n"
             "sends a message in same manner as a message actuator"
             " subject = Subject of the message"
             " body = Message body"
             " to = Name of object to send the message to"
             " from = Name of object to send the string from");
static PyObject *gPySendMessage(PyObject *, PyObject *args)
{
  char *subject = (char *)"";
  char *body = (char *)"";
  char *to = (char *)"";
  PyObject *pyfrom = Py_None;
  KX_GameObject *from = nullptr;
  KX_Scene *scene = KX_GetActiveScene();

  if (!PyArg_ParseTuple(args, "s|ssO:sendMessage", &subject, &body, &to, &pyfrom))
    return nullptr;

  if (!ConvertPythonToGameObject(scene->GetLogicManager(),
                                 pyfrom,
                                 &from,
                                 true,
                                 "sendMessage(subject, [body, to, from]): \"from\" argument")) {
    return nullptr;
  }

  scene->GetNetworkMessageScene()->SendMessage(to, from, subject, body);

  Py_RETURN_NONE;
}

// this gets a pointer to an array filled with floats
static PyObject *gPyGetSpectrum(PyObject *)
{
  PyObject *resultlist = PyList_New(512);

  for (int index = 0; index < 512; index++) {
    PyList_SET_ITEM(resultlist, index, PyFloat_FromDouble(0.0));
  }

  return resultlist;
}

static PyObject *gPySetLogicTicRate(PyObject *, PyObject *args)
{
  float ticrate;
  if (!PyArg_ParseTuple(args, "f:setLogicTicRate", &ticrate))
    return nullptr;

  KX_GetActiveEngine()->SetTicRate(ticrate);
  Py_RETURN_NONE;
}

static PyObject *gPyGetLogicTicRate(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetTicRate());
}

static PyObject *gPySetExitKey(PyObject *, PyObject *args)
{
  short exitkey;
  if (!PyArg_ParseTuple(args, "h:setExitKey", &exitkey))
    return nullptr;
  KX_GetActiveEngine()->SetExitKey(exitkey);
  Py_RETURN_NONE;
}

static PyObject *gPyGetExitKey(PyObject *)
{
  return PyLong_FromLong(KX_GetActiveEngine()->GetExitKey());
}

static PyObject *gPySetRender(PyObject *, PyObject *args)
{
  int render;
  if (!PyArg_ParseTuple(args, "i:setRender", &render))
    return nullptr;
  KX_GetActiveEngine()->SetRender(render);
  Py_RETURN_NONE;
}

static PyObject *gPyGetRender(PyObject *)
{
  return PyBool_FromLong(KX_GetActiveEngine()->GetRender());
}

static PyObject *gPySetMaxLogicFrame(PyObject *, PyObject *args)
{
  int frame;
  if (!PyArg_ParseTuple(args, "i:setMaxLogicFrame", &frame))
    return nullptr;

  KX_GetActiveEngine()->SetMaxLogicFrame(frame);
  Py_RETURN_NONE;
}

static PyObject *gPyGetMaxLogicFrame(PyObject *)
{
  return PyLong_FromLong(KX_GetActiveEngine()->GetMaxLogicFrame());
}

static PyObject *gPySetMaxPhysicsFrame(PyObject *, PyObject *args)
{
  int frame;
  if (!PyArg_ParseTuple(args, "i:setMaxPhysicsFrame", &frame))
    return nullptr;

  KX_GetActiveEngine()->SetMaxPhysicsFrame(frame);
  Py_RETURN_NONE;
}

static PyObject *gPyGetMaxPhysicsFrame(PyObject *)
{
  return PyLong_FromLong(KX_GetActiveEngine()->GetMaxPhysicsFrame());
}

static PyObject *gPySetPhysicsTicRate(PyObject *, PyObject *args)
{
  float ticrate;
  if (!PyArg_ParseTuple(args, "f:setPhysicsTicRate", &ticrate))
    return nullptr;

  KX_GetPhysicsEnvironment()->SetFixedTimeStep(true, ticrate);
  Py_RETURN_NONE;
}
#  if 0  // unused
static PyObject *gPySetPhysicsDebug(PyObject *, PyObject *args)
{
	int debugMode;
	if (!PyArg_ParseTuple(args, "i:setPhysicsDebug", &debugMode))
		return nullptr;
	
  KX_GetPhysicsEnvironment()->setDebugMode(debugMode);
	Py_RETURN_NONE;
}
#  endif

static PyObject *gPyGetPhysicsTicRate(PyObject *)
{
  return PyFloat_FromDouble(KX_GetPhysicsEnvironment()->GetFixedTimeStep());
}

static PyObject *gPyGetAverageFrameRate(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetAverageFrameRate());
}

static PyObject *gPyGetUseExternalClock(PyObject *)
{
  return PyBool_FromLong(KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::USE_EXTERNAL_CLOCK));
}

static PyObject *gPySetUseExternalClock(PyObject *, PyObject *args)
{
  int bUseExternalClock;

  if (!PyArg_ParseTuple(args, "p:setUseExternalClock", &bUseExternalClock))
    return nullptr;

  KX_GetActiveEngine()->SetFlag(KX_KetsjiEngine::USE_EXTERNAL_CLOCK, (bool)bUseExternalClock);
  Py_RETURN_NONE;
}

static PyObject *gPyGetClockTime(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetClockTime());
}

static PyObject *gPySetClockTime(PyObject *, PyObject *args)
{
  double externalClockTime;

  if (!PyArg_ParseTuple(args, "d:setClockTime", &externalClockTime))
    return nullptr;

  KX_GetActiveEngine()->SetClockTime(externalClockTime);
  Py_RETURN_NONE;
}

static PyObject *gPyGetFrameTime(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetFrameTime());
}

static PyObject *gPyGetRealTime(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetRealTime());
}

static PyObject *gPyGetTimeScale(PyObject *)
{
  return PyFloat_FromDouble(KX_GetActiveEngine()->GetTimeScale());
}

static PyObject *gPySetTimeScale(PyObject *, PyObject *args)
{
  double time_scale;

  if (!PyArg_ParseTuple(args, "d:setTimeScale", &time_scale))
    return nullptr;

  KX_GetActiveEngine()->SetTimeScale(time_scale);
  Py_RETURN_NONE;
}

static PyObject *gPyGetBlendFileList(PyObject *, PyObject *args)
{
  char cpath[FILE_MAX];
  char *searchpath = nullptr;
  PyObject *list, *value;

  DIR *dp;
  struct dirent *dirp;

  if (!PyArg_ParseTuple(args, "|s:getBlendFileList", &searchpath))
    return nullptr;

  list = PyList_New(0);

  if (searchpath) {
    BLI_strncpy(cpath, searchpath, FILE_MAX);
    BLI_path_abs(cpath, KX_GetMainPath().c_str());
  }
  else {
    /* Get the dir only */
    BLI_split_dir_part(KX_GetMainPath().c_str(), cpath, sizeof(cpath));
  }

  if ((dp = opendir(cpath)) == nullptr) {
    /* todo, show the errno, this shouldnt happen anyway if the blendfile is readable */
    CM_Error("could not read directory (" << cpath << ") failed, code " << errno << " ("
                                          << strerror(errno) << ")");
    return list;
  }

  while ((dirp = readdir(dp)) != nullptr) {
    if (BLI_path_extension_check_n(dirp->d_name, ".blend")) {
      value = PyC_UnicodeFromByte(dirp->d_name);
      PyList_Append(list, value);
      Py_DECREF(value);
    }
  }

  closedir(dp);
  return list;
}

PyDoc_STRVAR(gPyGetCurrentScene_doc,
             "getCurrentScene()\n"
             "Gets a reference to the current scene.");
static PyObject *gPyGetCurrentScene(PyObject *self)
{
  return KX_GetActiveScene()->GetProxy();
}

PyDoc_STRVAR(gPyGetSceneList_doc,
             "getSceneList()\n"
             "Return a list of converted scenes.");
static PyObject *gPyGetSceneList(PyObject *self)
{
  return KX_GetActiveEngine()->CurrentScenes()->GetProxy();
}

PyDoc_STRVAR(gPyGetInactiveSceneNames_doc,
             "getInactiveSceneNames()\n"
             "Get all inactive scenes names");
static PyObject *gPyGetInactiveSceneNames(PyObject *self)
{
  EXP_ListValue<EXP_StringValue> *list =
      KX_GetActiveEngine()->GetConverter()->GetInactiveSceneNames();

  return list->NewProxy(true);
}

static PyObject *pyPrintStats(PyObject *, PyObject *, PyObject *)
{
  KX_GetActiveEngine()->GetConverter()->PrintStats();
  Py_RETURN_NONE;
}

static PyObject *gPyGetGraphicsCardVendor(PyObject *, PyObject *, PyObject *)
{
  RAS_Rasterizer *rasterizer = KX_GetActiveEngine()->GetRasterizer();
  if (rasterizer) {
    const unsigned char *vendor = rasterizer->GetGraphicsCardVendor();
    return PyUnicode_FromString(reinterpret_cast<const char *>(vendor));
  }
  else {
    CM_Error("no rasterizer detected for getGraphicsCardVendor!");
    Py_RETURN_NONE;
  }
}

static PyObject *pyPrintExt(PyObject *, PyObject *, PyObject *)
{
  RAS_Rasterizer *rasterizer = KX_GetActiveEngine()->GetRasterizer();
  if (rasterizer)
    rasterizer->PrintHardwareInfo();
  else
    CM_Error("no rasterizer detected for PrintGLInfo!");

  Py_RETURN_NONE;
}

static PyObject *gLibLoad(PyObject *, PyObject *args, PyObject *kwds)
{
  KX_Scene *kx_scene = nullptr;
  PyObject *pyscene = Py_None;
  char *path;
  char *group;
  Py_buffer py_buffer;
  py_buffer.buf = nullptr;
  char *err_str = nullptr;
  KX_LibLoadStatus *status = nullptr;

  short options = 0;
  int load_actions = 0, verbose = 0, load_scripts = 1, asynchronous = 0;

  static const char *kwlist[] = {"path",
                                 "group",
                                 "buffer",
                                 "load_actions",
                                 "verbose",
                                 "load_scripts",
                                 "asynchronous",
                                 "scene",
                                 nullptr};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "ss|y*iiIiO:LibLoad",
                                   const_cast<char **>(kwlist),
                                   &path,
                                   &group,
                                   &py_buffer,
                                   &load_actions,
                                   &verbose,
                                   &load_scripts,
                                   &asynchronous,
                                   &pyscene))
    return nullptr;

  if (!ConvertPythonToScene(pyscene, &kx_scene, true, "invalid scene")) {
    return nullptr;
  }
  if (!kx_scene) {
    kx_scene = KX_GetActiveScene();
  }

  /* setup options */
  if (load_actions != 0)
    options |= BL_Converter::LIB_LOAD_LOAD_ACTIONS;
  if (verbose != 0)
    options |= BL_Converter::LIB_LOAD_VERBOSE;
  if (load_scripts != 0)
    options |= BL_Converter::LIB_LOAD_LOAD_SCRIPTS;
  if (asynchronous != 0)
    options |= BL_Converter::LIB_LOAD_ASYNC;

  BL_Converter *converter = KX_GetActiveEngine()->GetConverter();

  if (!py_buffer.buf) {
    char abs_path[FILE_MAX];
    // Make the path absolute
    BLI_strncpy(abs_path, path, sizeof(abs_path));
    BLI_path_abs(abs_path, KX_GetMainPath().c_str());

    if ((status = converter->LinkBlendFilePath(abs_path, group, kx_scene, &err_str, options))) {
      return status->GetProxy();
    }
  }
  else {

    if ((status = converter->LinkBlendFileMemory(
             py_buffer.buf, py_buffer.len, path, group, kx_scene, &err_str, options))) {
      PyBuffer_Release(&py_buffer);
      return status->GetProxy();
    }

    PyBuffer_Release(&py_buffer);
  }

  if (err_str) {
    PyErr_SetString(PyExc_ValueError, err_str);
    return nullptr;
  }

  Py_RETURN_FALSE;
}

static PyObject *gLibNew(PyObject *, PyObject *args)
{
  KX_Scene *kx_scene = KX_GetActiveScene();
  char *path = (char *)"";
  char *group;
  const char *name;
  PyObject *names;
  int idcode;

  if (!PyArg_ParseTuple(args, "ssO!:LibNew", &path, &group, &PyList_Type, &names))
    return nullptr;

  BL_Converter *converter = KX_GetActiveEngine()->GetConverter();

  if (converter->GetMainDynamicPath(path)) {
    PyErr_SetString(PyExc_KeyError, "the name of the path given exists");
    return nullptr;
  }

  idcode = BKE_idtype_idcode_from_name(group);
  if (idcode == 0) {
    PyErr_Format(PyExc_ValueError, "invalid group given \"%s\"", group);
    return nullptr;
  }

  Main *maggie = converter->CreateMainDynamic(path);

  /* Copy the object into main */
  if (idcode == ID_ME) {
    PyObject *ret = PyList_New(0);
    PyObject *item;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(names); i++) {
      name = _PyUnicode_AsString(PyList_GET_ITEM(names, i));
      if (name) {
        RAS_MeshObject *meshobj = converter->ConvertMeshSpecial(kx_scene, maggie, name);
        if (meshobj) {
          KX_MeshProxy *meshproxy = new KX_MeshProxy(meshobj);
          item = meshproxy->NewProxy(true);
          PyList_Append(ret, item);
          Py_DECREF(item);
        }
      }
      else {
        PyErr_Clear(); /* wasnt a string, ignore for now */
      }
    }

    return ret;
  }
  else {
    PyErr_Format(PyExc_ValueError, "only \"Mesh\" group currently supported");
    return nullptr;
  }

  Py_RETURN_NONE;
}

static PyObject *gLibFree(PyObject *, PyObject *args)
{
  char *path = (char *)"";

  if (!PyArg_ParseTuple(args, "s:LibFree", &path))
    return nullptr;

  if (KX_GetActiveEngine()->GetConverter()->FreeBlendFile(path)) {
    Py_RETURN_TRUE;
  }
  else {
    Py_RETURN_FALSE;
  }
}

static PyObject *gLibList(PyObject *, PyObject *args)
{
  const std::vector<Main *> &dynMaggie = KX_GetActiveEngine()->GetConverter()->GetMainDynamic();
  PyObject *list = PyList_New(dynMaggie.size());

  for (unsigned short i = 0, size = dynMaggie.size(); i < size; ++i) {
    PyList_SET_ITEM(list, i, PyUnicode_FromString(dynMaggie[i]->filepath));
  }

  return list;
}

struct PyNextFrameState pynextframestate;
static PyObject *gPyNextFrame(PyObject *)
{
  if (pynextframestate.func == nullptr)
    Py_RETURN_NONE;
  if (pynextframestate.state == nullptr)
    Py_RETURN_NONE;  // should never happen; raise exception instead?

  if (pynextframestate.func(pynextframestate.state))  // nonzero = stop
  {
    Py_RETURN_TRUE;
  }
  else  // 0 = go on
  {
    Py_RETURN_FALSE;
  }
}

static struct PyMethodDef game_methods[] = {
    {"expandPath", (PyCFunction)gPyExpandPath, METH_VARARGS, (const char *)gPyExpandPath_doc},
    {"startGame", (PyCFunction)gPyStartGame, METH_VARARGS, (const char *)gPyStartGame_doc},
    {"endGame", (PyCFunction)gPyEndGame, METH_NOARGS, (const char *)gPyEndGame_doc},
    {"restartGame", (PyCFunction)gPyRestartGame, METH_NOARGS, (const char *)gPyRestartGame_doc},
    {"saveGlobalDict",
     (PyCFunction)gPySaveGlobalDict,
     METH_NOARGS,
     (const char *)gPySaveGlobalDict_doc},
    {"loadGlobalDict",
     (PyCFunction)gPyLoadGlobalDict,
     METH_NOARGS,
     (const char *)gPyLoadGlobalDict_doc},
    {"sendMessage", (PyCFunction)gPySendMessage, METH_VARARGS, (const char *)gPySendMessage_doc},
    {"getCurrentController",
     (PyCFunction)SCA_PythonController::sPyGetCurrentController,
     METH_NOARGS,
     SCA_PythonController::sPyGetCurrentController__doc__},
    {"getCurrentScene", (PyCFunction)gPyGetCurrentScene, METH_NOARGS, gPyGetCurrentScene_doc},
    {"getInactiveSceneNames",
     (PyCFunction)gPyGetInactiveSceneNames,
     METH_NOARGS,
     (const char *)gPyGetInactiveSceneNames_doc},
    {"getSceneList", (PyCFunction)gPyGetSceneList, METH_NOARGS, (const char *)gPyGetSceneList_doc},
    {"getRandomFloat",
     (PyCFunction)gPyGetRandomFloat,
     METH_NOARGS,
     (const char *)gPyGetRandomFloat_doc},
    {"setGravity", (PyCFunction)gPySetGravity, METH_O, (const char *)"set Gravitation"},
    {"getSpectrum", (PyCFunction)gPyGetSpectrum, METH_NOARGS, (const char *)"get audio spectrum"},
    {"getMaxLogicFrame",
     (PyCFunction)gPyGetMaxLogicFrame,
     METH_NOARGS,
     (const char *)"Gets the max number of logic frame per render frame"},
    {"setMaxLogicFrame",
     (PyCFunction)gPySetMaxLogicFrame,
     METH_VARARGS,
     (const char *)"Sets the max number of logic frame per render frame"},
    {"getMaxPhysicsFrame",
     (PyCFunction)gPyGetMaxPhysicsFrame,
     METH_NOARGS,
     (const char *)"Gets the max number of physics frame per render frame"},
    {"setMaxPhysicsFrame",
     (PyCFunction)gPySetMaxPhysicsFrame,
     METH_VARARGS,
     (const char *)"Sets the max number of physics farme per render frame"},
    {"getLogicTicRate",
     (PyCFunction)gPyGetLogicTicRate,
     METH_NOARGS,
     (const char *)"Gets the logic tic rate"},
    {"setLogicTicRate",
     (PyCFunction)gPySetLogicTicRate,
     METH_VARARGS,
     (const char *)"Sets the logic tic rate"},
    {"getPhysicsTicRate",
     (PyCFunction)gPyGetPhysicsTicRate,
     METH_NOARGS,
     (const char *)"Gets the physics tic rate"},
    {"setPhysicsTicRate",
     (PyCFunction)gPySetPhysicsTicRate,
     METH_VARARGS,
     (const char *)"Sets the physics tic rate"},
    {"getExitKey",
     (PyCFunction)gPyGetExitKey,
     METH_NOARGS,
     (const char *)"Gets the key used to exit the game engine"},
    {"setExitKey",
     (PyCFunction)gPySetExitKey,
     METH_VARARGS,
     (const char *)"Sets the key used to exit the game engine"},
    {"setRender",
     (PyCFunction)gPySetRender,
     METH_VARARGS,
     (const char *)"Set the global render flag"},
    {"getRender",
     (PyCFunction)gPyGetRender,
     METH_NOARGS,
     (const char *)"get the global render flag value"},
    {"getUseExternalClock",
     (PyCFunction)gPyGetUseExternalClock,
     METH_NOARGS,
     (const char *)"Get if we use the time provided by an external clock"},
    {"setUseExternalClock",
     (PyCFunction)gPySetUseExternalClock,
     METH_VARARGS,
     (const char *)"Set if we use the time provided by an external clock"},
    {"getClockTime",
     (PyCFunction)gPyGetClockTime,
     METH_NOARGS,
     (const char *)"Get the last BGE render time. "
                   "The BGE render time is the simulated time corresponding to the next scene "
                   "that will be renderered"},
    {"setClockTime",
     (PyCFunction)gPySetClockTime,
     METH_VARARGS,
     (const char *)"Set the BGE render time. "
                   "The BGE render time is the simulated time corresponding to the next scene "
                   "that will be rendered"},
    {"getFrameTime",
     (PyCFunction)gPyGetFrameTime,
     METH_NOARGS,
     (const char *)"Get the BGE last frametime. "
                   "The BGE frame time is the simulated time corresponding to the last call of "
                   "the logic system"},
    {"getRealTime",
     (PyCFunction)gPyGetRealTime,
     METH_NOARGS,
     (const char *)"Get the real system time. "
                   "The real-time corresponds to the system time"},
    {"getAverageFrameRate",
     (PyCFunction)gPyGetAverageFrameRate,
     METH_NOARGS,
     (const char *)"Gets the estimated average frame rate"},
    {"getTimeScale",
     (PyCFunction)gPyGetTimeScale,
     METH_NOARGS,
     (const char *)"Get the time multiplier"},
    {"setTimeScale",
     (PyCFunction)gPySetTimeScale,
     METH_VARARGS,
     (const char *)"Set the time multiplier"},
    {"getBlendFileList",
     (PyCFunction)gPyGetBlendFileList,
     METH_VARARGS,
     (const char *)"Gets a list of blend files in the same directory as the current blend file"},
    {"PrintGLInfo",
     (PyCFunction)pyPrintExt,
     METH_NOARGS,
     (const char *)"Prints GL Extension Info"},
    {"getGraphicsCardVendor",
     (PyCFunction)gPyGetGraphicsCardVendor,
     METH_NOARGS,
     (const char *)"Gets graphics card vendor name"},
    {"PrintMemInfo",
     (PyCFunction)pyPrintStats,
     METH_NOARGS,
     (const char *)"Print engine statistics"},
    {"NextFrame",
     (PyCFunction)gPyNextFrame,
     METH_NOARGS,
     (const char *)"Render next frame (if Python has control)"},
    {"getProfileInfo", (PyCFunction)gPyGetProfileInfo, METH_NOARGS, gPyGetProfileInfo_doc},
    /* library functions */
    {"LibLoad", (PyCFunction)gLibLoad, METH_VARARGS | METH_KEYWORDS, (const char *)""},
    {"LibNew", (PyCFunction)gLibNew, METH_VARARGS, (const char *)""},
    {"LibFree", (PyCFunction)gLibFree, METH_VARARGS, (const char *)""},
    {"LibList", (PyCFunction)gLibList, METH_VARARGS, (const char *)""},

    {nullptr, (PyCFunction) nullptr, 0, nullptr}};

static PyObject *gPyGetWindowHeight(PyObject *, PyObject *args)
{
  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
  return PyLong_FromLong((canvas ? canvas->GetHeight() : 0));
}

static PyObject *gPyGetWindowWidth(PyObject *, PyObject *args)
{
  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
  return PyLong_FromLong((canvas ? canvas->GetWidth() : 0));
}

static PyObject *gPyEnableVisibility(PyObject *, PyObject *args)
{
  int visible;
  if (!PyArg_ParseTuple(args, "i:enableVisibility", &visible))
    return nullptr;

  // TODO
  Py_RETURN_NONE;
}

static PyObject *gPyShowMouse(PyObject *, PyObject *args)
{
  int visible;
  if (!PyArg_ParseTuple(args, "i:showMouse", &visible))
    return nullptr;

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();

  if (visible) {
    if (canvas)
      canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
  }
  else {
    if (canvas)
      canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);
  }

  Py_RETURN_NONE;
}

static PyObject *gPySetMousePosition(PyObject *, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:setMousePosition", &x, &y))
    return nullptr;

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();

  if (canvas)
    canvas->SetMousePosition(x, y);

  Py_RETURN_NONE;
}

static PyObject *gPySetEyeSeparation(PyObject *, PyObject *args)
{
  float sep;
  if (!PyArg_ParseTuple(args, "f:setEyeSeparation", &sep))
    return nullptr;

  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Rasterizer.setEyeSeparation(float), Rasterizer not available");
    return nullptr;
  }

  KX_GetActiveEngine()->GetRasterizer()->SetEyeSeparation(sep);

  Py_RETURN_NONE;
}

static PyObject *gPyGetEyeSeparation(PyObject *)
{
  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError, "Rasterizer.getEyeSeparation(), Rasterizer not available");
    return nullptr;
  }

  return PyFloat_FromDouble(KX_GetActiveEngine()->GetRasterizer()->GetEyeSeparation());
}

static PyObject *gPySetFocalLength(PyObject *, PyObject *args)
{
  float focus;
  if (!PyArg_ParseTuple(args, "f:setFocalLength", &focus))
    return nullptr;

  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Rasterizer.setFocalLength(float), Rasterizer not available");
    return nullptr;
  }

  KX_GetActiveEngine()->GetRasterizer()->SetFocalLength(focus);

  Py_RETURN_NONE;
}

static PyObject *gPyGetFocalLength(PyObject *, PyObject *, PyObject *)
{
  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError, "Rasterizer.getFocalLength(), Rasterizer not available");
    return nullptr;
  }

  return PyFloat_FromDouble(KX_GetActiveEngine()->GetRasterizer()->GetFocalLength());

  Py_RETURN_NONE;
}

static PyObject *gPyGetStereoEye(PyObject *, PyObject *, PyObject *)
{
  int flag = RAS_Rasterizer::RAS_STEREO_LEFTEYE;

  RAS_Rasterizer *rasterizer = KX_GetActiveEngine()->GetRasterizer();

  if (!rasterizer) {
    PyErr_SetString(PyExc_RuntimeError, "Rasterizer.getStereoEye(), Rasterizer not available");
    return nullptr;
  }

  if (rasterizer->Stereo())
    flag = rasterizer->GetEye();

  return PyLong_FromLong(flag);
}

static PyObject *gPyMakeScreenshot(PyObject *, PyObject *args)
{
  char *filename = (char *)"";
  if (!PyArg_ParseTuple(args, "s:makeScreenshot", &filename))
    return nullptr;

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();

  if (canvas) {
    canvas->MakeScreenShot(filename);
  }

  Py_RETURN_NONE;
}

static PyObject *gPySetGLSLMaterialSetting(PyObject *, PyObject *args, PyObject *)
{
  EXP_ShowDeprecationWarning("setGLSLMaterialSetting(settings, enable)", "nothing");

  Py_RETURN_NONE;
}

static PyObject *gPyGetGLSLMaterialSetting(PyObject *, PyObject *args, PyObject *)
{
  EXP_ShowDeprecationWarning("getGLSLMaterialSetting()", "nothing");

  return PyLong_FromLong(0);
}

static PyObject *gPySetMaterialType(PyObject *, PyObject *args, PyObject *)
{
  EXP_ShowDeprecationWarning("setMaterialMode(mode)", "nothing");

  Py_RETURN_NONE;
}

static PyObject *gPyGetMaterialType(PyObject *)
{
  EXP_ShowDeprecationWarning("getMaterialMode()", "nothing");

  return PyLong_FromLong(0);
}

static PyObject *gPySetAnisotropicFiltering(PyObject *, PyObject *args)
{
  short level;

  if (!PyArg_ParseTuple(args, "h:setAnisotropicFiltering", &level))
    return nullptr;

  if (level != 1 && level != 2 && level != 4 && level != 8 && level != 16) {
    PyErr_SetString(PyExc_ValueError,
                    "Rasterizer.setAnisotropicFiltering(level): Expected value of 1, 2, 4, 8, or "
                    "16 for value");
    return nullptr;
  }

  KX_GetActiveEngine()->GetRasterizer()->SetAnisotropicFiltering(level);

  Py_RETURN_NONE;
}

static PyObject *gPyGetAnisotropicFiltering(PyObject *, PyObject *args)
{
  return PyLong_FromLong(KX_GetActiveEngine()->GetRasterizer()->GetAnisotropicFiltering());
}

static PyObject *gPyDrawLine(PyObject *, PyObject *args)
{
  PyObject *ob_from;
  PyObject *ob_to;
  PyObject *ob_color;

  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Rasterizer.drawLine(obFrom, obTo, color): Rasterizer not available");
    return nullptr;
  }

  if (!PyArg_ParseTuple(args, "OOO:drawLine", &ob_from, &ob_to, &ob_color))
    return nullptr;

  MT_Vector3 from;
  MT_Vector3 to;
  MT_Vector3 color3;
  MT_Vector4 color4;

  if (!PyVecTo(ob_from, from))
    return nullptr;
  if (!PyVecTo(ob_to, to))
    return nullptr;

  // Allow conversion from vector 3d.
  if (PyVecTo(ob_color, color3)) {
    KX_RasterizerDrawDebugLine(from, to, MT_Vector4(color3.x(), color3.y(), color3.z(), 1.0f));
    Py_RETURN_NONE;
  }
  else {
    // Clear error message of the conversion from vector3d.
    PyErr_Clear();
    if (PyVecTo(ob_color, color4)) {
      KX_RasterizerDrawDebugLine(from, to, color4);
    }
    Py_RETURN_NONE;
  }

  return nullptr;
}

static PyObject *gPySetWindowSize(PyObject *, PyObject *args)
{
  int width, height;
  if (!PyArg_ParseTuple(args, "ii:resize", &width, &height))
    return nullptr;

  KX_GetActiveEngine()->GetCanvas()->ResizeWindow(width, height);
  Py_RETURN_NONE;
}

static PyObject *gPySetFullScreen(PyObject *, PyObject *value)
{
  KX_GetActiveEngine()->GetCanvas()->SetFullScreen(PyObject_IsTrue(value));
  Py_RETURN_NONE;
}

static PyObject *gPyGetFullScreen(PyObject *)
{
  return PyBool_FromLong(KX_GetActiveEngine()->GetCanvas()->GetFullScreen());
}

static PyObject *gPySetMipmapping(PyObject *, PyObject *args)
{
  int val = 0;

  if (!PyArg_ParseTuple(args, "i:setMipmapping", &val))
    return nullptr;

  if (val < 0 || val > RAS_Rasterizer::RAS_MIPMAP_MAX) {
    PyErr_SetString(PyExc_ValueError, "Rasterizer.setMipmapping(val): invalid mipmaping option");
    return nullptr;
  }

  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError, "Rasterizer.setMipmapping(val): Rasterizer not available");
    return nullptr;
  }

  KX_GetActiveEngine()->GetRasterizer()->SetMipmapping((RAS_Rasterizer::MipmapOption)val);
  Py_RETURN_NONE;
}

static PyObject *gPyGetMipmapping(PyObject *)
{
  if (!KX_GetActiveEngine()->GetRasterizer()) {
    PyErr_SetString(PyExc_RuntimeError, "Rasterizer.getMipmapping(): Rasterizer not available");
    return nullptr;
  }
  return PyLong_FromLong(KX_GetActiveEngine()->GetRasterizer()->GetMipmapping());
}

static PyObject *gPySetVsync(PyObject *, PyObject *args)
{
  int interval;

  if (!PyArg_ParseTuple(args, "i:setVsync", &interval))
    return nullptr;

  if (interval < 0 || interval > VSYNC_ADAPTIVE) {
    PyErr_SetString(
        PyExc_ValueError,
        "Rasterizer.setVsync(value): value must be VSYNC_OFF, VSYNC_ON, or VSYNC_ADAPTIVE");
    return nullptr;
  }

  if (interval == VSYNC_ADAPTIVE)
    interval = -1;
  KX_GetActiveEngine()->GetCanvas()->SetSwapInterval((interval == VSYNC_ON) ? 1 : 0);
  Py_RETURN_NONE;
}

static PyObject *gPyGetVsync(PyObject *)
{
  int interval = 0;
  KX_GetActiveEngine()->GetCanvas()->GetSwapInterval(interval);
  return PyLong_FromLong(interval);
}

static PyObject *gPyShowFramerate(PyObject *, PyObject *args)
{
  int visible;
  if (!PyArg_ParseTuple(args, "i:showFramerate", &visible))
    return nullptr;

  KX_GetActiveEngine()->SetFlag(KX_KetsjiEngine::SHOW_FRAMERATE, visible);
  Py_RETURN_NONE;
}

static PyObject *gPyShowProfile(PyObject *, PyObject *args)
{
  int visible;
  if (!PyArg_ParseTuple(args, "i:showProfile", &visible))
    return nullptr;

  KX_GetActiveEngine()->SetFlag(KX_KetsjiEngine::SHOW_PROFILE, visible);
  Py_RETURN_NONE;
}

static PyObject *gPyShowProperties(PyObject *, PyObject *args)
{
  int visible;
  if (!PyArg_ParseTuple(args, "i:showProperties", &visible))
    return nullptr;

  KX_GetActiveEngine()->SetFlag(KX_KetsjiEngine::SHOW_DEBUG_PROPERTIES, visible);
  Py_RETURN_NONE;
}

static PyObject *gPyAutoDebugList(PyObject *, PyObject *args)
{
  int add;
  if (!PyArg_ParseTuple(args, "i:autoAddProperties", &add))
    return nullptr;

  KX_GetActiveEngine()->SetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES, add);
  Py_RETURN_NONE;
}

static PyObject *gPyClearDebugList(PyObject *)
{
  KX_Scene *scene = KX_GetActiveScene();
  if (scene)
    scene->RemoveAllDebugProperties();

  Py_RETURN_NONE;
}

static PyObject *gPyGetDisplayDimensions(PyObject *)
{
  PyObject *result;
  int width, height;

  KX_GetActiveEngine()->GetCanvas()->GetDisplayDimensions(width, height);

  result = PyTuple_New(2);
  PyTuple_SET_ITEMS(result, PyLong_FromLong(width), PyLong_FromLong(height));

  return result;
}

PyDoc_STRVAR(Rasterizer_module_documentation,
             "This is the Python API for the game engine of Rasterizer");

static struct PyMethodDef rasterizer_methods[] = {
    {"getWindowWidth", (PyCFunction)gPyGetWindowWidth, METH_VARARGS, "getWindowWidth doc"},
    {"getWindowHeight", (PyCFunction)gPyGetWindowHeight, METH_VARARGS, "getWindowHeight doc"},
    {"makeScreenshot", (PyCFunction)gPyMakeScreenshot, METH_VARARGS, "make Screenshot doc"},
    {"enableVisibility", (PyCFunction)gPyEnableVisibility, METH_VARARGS, "enableVisibility doc"},
    {"showMouse", (PyCFunction)gPyShowMouse, METH_VARARGS, "showMouse(bool visible)"},
    {"setMousePosition",
     (PyCFunction)gPySetMousePosition,
     METH_VARARGS,
     "setMousePosition(int x,int y)"},

    {"setEyeSeparation",
     (PyCFunction)gPySetEyeSeparation,
     METH_VARARGS,
     "set the eye separation for stereo mode"},
    {"getEyeSeparation",
     (PyCFunction)gPyGetEyeSeparation,
     METH_NOARGS,
     "get the eye separation for stereo mode"},
    {"setFocalLength",
     (PyCFunction)gPySetFocalLength,
     METH_VARARGS,
     "set the focal length for stereo mode"},
    {"getFocalLength",
     (PyCFunction)gPyGetFocalLength,
     METH_VARARGS,
     "get the focal length for stereo mode"},
    {"getStereoEye",
     (PyCFunction)gPyGetStereoEye,
     METH_VARARGS,
     "get the current stereoscopy eye being rendered"},
    {"setMaterialMode",
     (PyCFunction)gPySetMaterialType,
     METH_VARARGS,
     "set the material mode to use for OpenGL rendering"},
    {"getMaterialMode",
     (PyCFunction)gPyGetMaterialType,
     METH_NOARGS,
     "get the material mode being used for OpenGL rendering"},
    {"setGLSLMaterialSetting",
     (PyCFunction)gPySetGLSLMaterialSetting,
     METH_VARARGS,
     "set the state of a GLSL material setting"},
    {"getGLSLMaterialSetting",
     (PyCFunction)gPyGetGLSLMaterialSetting,
     METH_VARARGS,
     "get the state of a GLSL material setting"},
    {"setAnisotropicFiltering",
     (PyCFunction)gPySetAnisotropicFiltering,
     METH_VARARGS,
     "set the anisotropic filtering level (must be one of 1, 2, 4, 8, 16)"},
    {"getAnisotropicFiltering",
     (PyCFunction)gPyGetAnisotropicFiltering,
     METH_VARARGS,
     "get the anisotropic filtering level"},
    {"drawLine", (PyCFunction)gPyDrawLine, METH_VARARGS, "draw a line on the screen"},
    {"setWindowSize", (PyCFunction)gPySetWindowSize, METH_VARARGS, ""},
    {"setFullScreen", (PyCFunction)gPySetFullScreen, METH_O, ""},
    {"getFullScreen", (PyCFunction)gPyGetFullScreen, METH_NOARGS, ""},
    {"getDisplayDimensions",
     (PyCFunction)gPyGetDisplayDimensions,
     METH_NOARGS,
     "Get the actual dimensions, in pixels, of the physical display (e.g., the monitor)."},
    {"setMipmapping", (PyCFunction)gPySetMipmapping, METH_VARARGS, ""},
    {"getMipmapping", (PyCFunction)gPyGetMipmapping, METH_NOARGS, ""},
    {"setVsync", (PyCFunction)gPySetVsync, METH_VARARGS, ""},
    {"getVsync", (PyCFunction)gPyGetVsync, METH_NOARGS, ""},
    {"showFramerate", (PyCFunction)gPyShowFramerate, METH_VARARGS, "show or hide the framerate"},
    {"showProfile", (PyCFunction)gPyShowProfile, METH_VARARGS, "show or hide the profile"},
    {"showProperties",
     (PyCFunction)gPyShowProperties,
     METH_VARARGS,
     "show or hide the debug properties"},
    {"autoDebugList",
     (PyCFunction)gPyAutoDebugList,
     METH_VARARGS,
     "enable or disable auto adding debug properties to the debug  list"},
    {"clearDebugList",
     (PyCFunction)gPyClearDebugList,
     METH_NOARGS,
     "clears the debug property list"},
    {nullptr, (PyCFunction) nullptr, 0, nullptr}};

PyDoc_STRVAR(GameLogic_module_documentation,
             "This is the Python API for the game engine of bge.logic");

static struct PyModuleDef GameLogic_module_def = {
    {},                             /* m_base */
    "GameLogic",                    /* m_name */
    GameLogic_module_documentation, /* m_doc */
    0,                              /* m_size */
    game_methods,                   /* m_methods */
    0,                              /* m_reload */
    0,                              /* m_traverse */
    0,                              /* m_clear */
    0,                              /* m_free */
};

PyMODINIT_FUNC initGameLogicPythonBinding()
{
  PyObject *m;
  PyObject *d;
  PyObject *item; /* temp PyObject *storage */

  EXP_PyObjectPlus::ClearDeprecationWarning(); /* Not that nice to call here but makes sure
                                              warnings are reset between loading scenes */

  m = PyModule_Create(&GameLogic_module_def);
  PyDict_SetItemString(PySys_GetObject("modules"), GameLogic_module_def.m_name, m);

  // Add some symbolic constants to the module
  d = PyModule_GetDict(m);

  // can be overwritten later for gameEngine instances that can load new blend files and
  // re-initialize this module for now its safe to make sure it exists for other areas such as the
  // web plugin

  PyDict_SetItemString(d, "globalDict", item = PyDict_New());
  Py_DECREF(item);

  // Add keyboard, mouse and joysticks attributes to this module
  BLI_assert(!gp_PythonKeyboard);
  gp_PythonKeyboard = new SCA_PythonKeyboard(KX_GetActiveEngine()->GetInputDevice());
  PyDict_SetItemString(d, "keyboard", gp_PythonKeyboard->GetProxy());

  BLI_assert(!gp_PythonMouse);
  gp_PythonMouse = new SCA_PythonMouse(KX_GetActiveEngine()->GetInputDevice(),
                                       KX_GetActiveEngine()->GetCanvas());
  PyDict_SetItemString(d, "mouse", gp_PythonMouse->GetProxy());

  PyObject *joylist = PyList_New(JOYINDEX_MAX);
  for (unsigned short i = 0; i < JOYINDEX_MAX; ++i) {
    PyList_SET_ITEM(joylist, i, Py_None);
  }
  PyDict_SetItemString(d, "joysticks", joylist);
  Py_DECREF(joylist);

  ErrorObject = PyUnicode_FromString("GameLogic.error");
  PyDict_SetItemString(d, "error", ErrorObject);
  Py_DECREF(ErrorObject);

  // XXXX Add constants here
  /* To use logic bricks, we need some sort of constants. Here, we associate */
  /* constants and sumbolic names. Add them to dictionary d.                 */

  /* 1. true and false: needed for everyone                                  */
  KX_MACRO_addTypesToDict(d, KX_TRUE, SCA_ILogicBrick::KX_TRUE);
  KX_MACRO_addTypesToDict(d, KX_FALSE, SCA_ILogicBrick::KX_FALSE);

  /* 2. Property sensor                                                      */
  KX_MACRO_addTypesToDict(d, KX_PROPSENSOR_EQUAL, SCA_PropertySensor::KX_PROPSENSOR_EQUAL);
  KX_MACRO_addTypesToDict(d, KX_PROPSENSOR_NOTEQUAL, SCA_PropertySensor::KX_PROPSENSOR_NOTEQUAL);
  KX_MACRO_addTypesToDict(d, KX_PROPSENSOR_INTERVAL, SCA_PropertySensor::KX_PROPSENSOR_INTERVAL);
  KX_MACRO_addTypesToDict(d, KX_PROPSENSOR_CHANGED, SCA_PropertySensor::KX_PROPSENSOR_CHANGED);
  KX_MACRO_addTypesToDict(
      d, KX_PROPSENSOR_EXPRESSION, SCA_PropertySensor::KX_PROPSENSOR_EXPRESSION);
  KX_MACRO_addTypesToDict(d, KX_PROPSENSOR_LESSTHAN, SCA_PropertySensor::KX_PROPSENSOR_LESSTHAN);
  KX_MACRO_addTypesToDict(
      d, KX_PROPSENSOR_GREATERTHAN, SCA_PropertySensor::KX_PROPSENSOR_GREATERTHAN);

  /* 3. Constraint actuator                                                  */
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_LOCX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_LOCX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_LOCY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_LOCY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_LOCZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_LOCZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ROTX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ROTX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ROTY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ROTY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ROTZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ROTZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRPX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRPX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRPY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRPY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRPZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRPZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRNX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRNX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRNY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRNY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DIRNZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DIRNZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ORIX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ORIX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ORIY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ORIY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_ORIZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_ORIZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHPX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHPX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHPY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHPY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHPZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHPZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHNX, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHNX);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHNY, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHNY);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_FHNZ, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_FHNZ);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_NORMAL, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_NORMAL);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_MATERIAL, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_MATERIAL);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_PERMANENT, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_PERMANENT);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DISTANCE, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DISTANCE);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_LOCAL, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_LOCAL);
  KX_MACRO_addTypesToDict(
      d, KX_CONSTRAINTACT_DOROTFH, SCA_ConstraintActuator::KX_ACT_CONSTRAINT_DOROTFH);

  /* 4. Random distribution types                                            */
  KX_MACRO_addTypesToDict(d, KX_RANDOMACT_BOOL_CONST, SCA_RandomActuator::KX_RANDOMACT_BOOL_CONST);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_BOOL_UNIFORM, SCA_RandomActuator::KX_RANDOMACT_BOOL_UNIFORM);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_BOOL_BERNOUILLI, SCA_RandomActuator::KX_RANDOMACT_BOOL_BERNOUILLI);
  KX_MACRO_addTypesToDict(d, KX_RANDOMACT_INT_CONST, SCA_RandomActuator::KX_RANDOMACT_INT_CONST);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_INT_UNIFORM, SCA_RandomActuator::KX_RANDOMACT_INT_UNIFORM);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_INT_POISSON, SCA_RandomActuator::KX_RANDOMACT_INT_POISSON);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_FLOAT_CONST, SCA_RandomActuator::KX_RANDOMACT_FLOAT_CONST);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_FLOAT_UNIFORM, SCA_RandomActuator::KX_RANDOMACT_FLOAT_UNIFORM);
  KX_MACRO_addTypesToDict(
      d, KX_RANDOMACT_FLOAT_NORMAL, SCA_RandomActuator::KX_RANDOMACT_FLOAT_NORMAL);
  KX_MACRO_addTypesToDict(d,
                          KX_RANDOMACT_FLOAT_NEGATIVE_EXPONENTIAL,
                          SCA_RandomActuator::KX_RANDOMACT_FLOAT_NEGATIVE_EXPONENTIAL);

  /* 5. Sound actuator                                                      */
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_PLAYSTOP, SCA_SoundActuator::KX_SOUNDACT_PLAYSTOP);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_PLAYEND, SCA_SoundActuator::KX_SOUNDACT_PLAYEND);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_LOOPSTOP, SCA_SoundActuator::KX_SOUNDACT_LOOPSTOP);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_LOOPEND, SCA_SoundActuator::KX_SOUNDACT_LOOPEND);
  KX_MACRO_addTypesToDict(
      d, KX_SOUNDACT_LOOPBIDIRECTIONAL, SCA_SoundActuator::KX_SOUNDACT_LOOPBIDIRECTIONAL);
  KX_MACRO_addTypesToDict(d,
                          KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP,
                          SCA_SoundActuator::KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP);

  /* 6. Action actuator													   */
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_PLAY, ACT_ACTION_PLAY);
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_PINGPONG, ACT_ACTION_PINGPONG);
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_FLIPPER, ACT_ACTION_FLIPPER);
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_LOOPSTOP, ACT_ACTION_LOOP_STOP);
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_LOOPEND, ACT_ACTION_LOOP_END);
  KX_MACRO_addTypesToDict(d, KX_ACTIONACT_PROPERTY, ACT_ACTION_FROM_PROP);

  /* 7. GL_BlendFunc */
  KX_MACRO_addTypesToDict(d, BL_ZERO, RAS_Rasterizer::RAS_ZERO);
  KX_MACRO_addTypesToDict(d, BL_ONE, RAS_Rasterizer::RAS_ONE);
  KX_MACRO_addTypesToDict(d, BL_SRC_COLOR, RAS_Rasterizer::RAS_SRC_COLOR);
  KX_MACRO_addTypesToDict(d, BL_ONE_MINUS_SRC_COLOR, RAS_Rasterizer::RAS_ONE_MINUS_SRC_COLOR);
  KX_MACRO_addTypesToDict(d, BL_DST_COLOR, RAS_Rasterizer::RAS_DST_COLOR);
  KX_MACRO_addTypesToDict(d, BL_ONE_MINUS_DST_COLOR, RAS_Rasterizer::RAS_ONE_MINUS_DST_COLOR);
  KX_MACRO_addTypesToDict(d, BL_SRC_ALPHA, RAS_Rasterizer::RAS_SRC_ALPHA);
  KX_MACRO_addTypesToDict(d, BL_ONE_MINUS_SRC_ALPHA, RAS_Rasterizer::RAS_ONE_MINUS_SRC_ALPHA);
  KX_MACRO_addTypesToDict(d, BL_DST_ALPHA, RAS_Rasterizer::RAS_DST_ALPHA);
  KX_MACRO_addTypesToDict(d, BL_ONE_MINUS_DST_ALPHA, RAS_Rasterizer::RAS_ONE_MINUS_DST_ALPHA);
  KX_MACRO_addTypesToDict(d, BL_SRC_ALPHA_SATURATE, RAS_Rasterizer::RAS_SRC_ALPHA_SATURATE);

  /* 8. UniformTypes */
  KX_MACRO_addTypesToDict(d, SHD_TANGENT, BL_Shader::SHD_TANGENT);
  KX_MACRO_addTypesToDict(d, MODELVIEWMATRIX, RAS_Shader::MODELVIEWMATRIX);
  KX_MACRO_addTypesToDict(d, MODELVIEWMATRIX_TRANSPOSE, RAS_Shader::MODELVIEWMATRIX_TRANSPOSE);
  KX_MACRO_addTypesToDict(d, MODELVIEWMATRIX_INVERSE, RAS_Shader::MODELVIEWMATRIX_INVERSE);
  KX_MACRO_addTypesToDict(
      d, MODELVIEWMATRIX_INVERSETRANSPOSE, RAS_Shader::MODELVIEWMATRIX_INVERSETRANSPOSE);
  KX_MACRO_addTypesToDict(d, MODELMATRIX, RAS_Shader::MODELMATRIX);
  KX_MACRO_addTypesToDict(d, MODELMATRIX_TRANSPOSE, RAS_Shader::MODELMATRIX_TRANSPOSE);
  KX_MACRO_addTypesToDict(d, MODELMATRIX_INVERSE, RAS_Shader::MODELMATRIX_INVERSE);
  KX_MACRO_addTypesToDict(
      d, MODELMATRIX_INVERSETRANSPOSE, RAS_Shader::MODELMATRIX_INVERSETRANSPOSE);
  KX_MACRO_addTypesToDict(d, VIEWMATRIX, RAS_Shader::VIEWMATRIX);
  KX_MACRO_addTypesToDict(d, VIEWMATRIX_TRANSPOSE, RAS_Shader::VIEWMATRIX_TRANSPOSE);
  KX_MACRO_addTypesToDict(d, VIEWMATRIX_INVERSE, RAS_Shader::VIEWMATRIX_INVERSE);
  KX_MACRO_addTypesToDict(d, VIEWMATRIX_INVERSETRANSPOSE, RAS_Shader::VIEWMATRIX_INVERSETRANSPOSE);
  KX_MACRO_addTypesToDict(d, CAM_POS, RAS_Shader::CAM_POS);
  KX_MACRO_addTypesToDict(d, CONSTANT_TIMER, RAS_Shader::CONSTANT_TIMER);
  KX_MACRO_addTypesToDict(d, EYE, RAS_Shader::EYE);

  /* 9. state actuator */
  KX_MACRO_addTypesToDict(d, KX_STATE1, (1 << 0));
  KX_MACRO_addTypesToDict(d, KX_STATE2, (1 << 1));
  KX_MACRO_addTypesToDict(d, KX_STATE3, (1 << 2));
  KX_MACRO_addTypesToDict(d, KX_STATE4, (1 << 3));
  KX_MACRO_addTypesToDict(d, KX_STATE5, (1 << 4));
  KX_MACRO_addTypesToDict(d, KX_STATE6, (1 << 5));
  KX_MACRO_addTypesToDict(d, KX_STATE7, (1 << 6));
  KX_MACRO_addTypesToDict(d, KX_STATE8, (1 << 7));
  KX_MACRO_addTypesToDict(d, KX_STATE9, (1 << 8));
  KX_MACRO_addTypesToDict(d, KX_STATE10, (1 << 9));
  KX_MACRO_addTypesToDict(d, KX_STATE11, (1 << 10));
  KX_MACRO_addTypesToDict(d, KX_STATE12, (1 << 11));
  KX_MACRO_addTypesToDict(d, KX_STATE13, (1 << 12));
  KX_MACRO_addTypesToDict(d, KX_STATE14, (1 << 13));
  KX_MACRO_addTypesToDict(d, KX_STATE15, (1 << 14));
  KX_MACRO_addTypesToDict(d, KX_STATE16, (1 << 15));
  KX_MACRO_addTypesToDict(d, KX_STATE17, (1 << 16));
  KX_MACRO_addTypesToDict(d, KX_STATE18, (1 << 17));
  KX_MACRO_addTypesToDict(d, KX_STATE19, (1 << 18));
  KX_MACRO_addTypesToDict(d, KX_STATE20, (1 << 19));
  KX_MACRO_addTypesToDict(d, KX_STATE21, (1 << 20));
  KX_MACRO_addTypesToDict(d, KX_STATE22, (1 << 21));
  KX_MACRO_addTypesToDict(d, KX_STATE23, (1 << 22));
  KX_MACRO_addTypesToDict(d, KX_STATE24, (1 << 23));
  KX_MACRO_addTypesToDict(d, KX_STATE25, (1 << 24));
  KX_MACRO_addTypesToDict(d, KX_STATE26, (1 << 25));
  KX_MACRO_addTypesToDict(d, KX_STATE27, (1 << 26));
  KX_MACRO_addTypesToDict(d, KX_STATE28, (1 << 27));
  KX_MACRO_addTypesToDict(d, KX_STATE29, (1 << 28));
  KX_MACRO_addTypesToDict(d, KX_STATE30, (1 << 29));

  /* All Sensors */
  KX_MACRO_addTypesToDict(d, KX_SENSOR_JUST_ACTIVATED, SCA_ISensor::KX_SENSOR_JUST_ACTIVATED);
  KX_MACRO_addTypesToDict(d, KX_SENSOR_ACTIVE, SCA_ISensor::KX_SENSOR_ACTIVE);
  KX_MACRO_addTypesToDict(d, KX_SENSOR_JUST_DEACTIVATED, SCA_ISensor::KX_SENSOR_JUST_DEACTIVATED);
  KX_MACRO_addTypesToDict(d, KX_SENSOR_INACTIVE, SCA_ISensor::KX_SENSOR_INACTIVE);

  /* Radar Sensor */
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_POS_X, SCA_RadarSensor::KX_RADAR_AXIS_POS_X);
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_POS_Y, SCA_RadarSensor::KX_RADAR_AXIS_POS_Y);
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_POS_Z, SCA_RadarSensor::KX_RADAR_AXIS_POS_Z);
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_NEG_X, SCA_RadarSensor::KX_RADAR_AXIS_NEG_X);
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_NEG_Y, SCA_RadarSensor::KX_RADAR_AXIS_NEG_Y);
  KX_MACRO_addTypesToDict(d, KX_RADAR_AXIS_NEG_Z, SCA_RadarSensor::KX_RADAR_AXIS_NEG_Z);

  /* Ray Sensor */
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_POS_X, SCA_RaySensor::KX_RAY_AXIS_POS_X);
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_POS_Y, SCA_RaySensor::KX_RAY_AXIS_POS_Y);
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_POS_Z, SCA_RaySensor::KX_RAY_AXIS_POS_Z);
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_NEG_X, SCA_RaySensor::KX_RAY_AXIS_NEG_X);
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_NEG_Y, SCA_RaySensor::KX_RAY_AXIS_NEG_Y);
  KX_MACRO_addTypesToDict(d, KX_RAY_AXIS_NEG_Z, SCA_RaySensor::KX_RAY_AXIS_NEG_Z);

  /* Movement Sensor */
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_POS_X, SCA_MovementSensor::KX_MOVEMENT_AXIS_POS_X);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_POS_Y, SCA_MovementSensor::KX_MOVEMENT_AXIS_POS_Y);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_POS_Z, SCA_MovementSensor::KX_MOVEMENT_AXIS_POS_Z);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_NEG_X, SCA_MovementSensor::KX_MOVEMENT_AXIS_NEG_X);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_NEG_Y, SCA_MovementSensor::KX_MOVEMENT_AXIS_NEG_Y);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_AXIS_NEG_Z, SCA_MovementSensor::KX_MOVEMENT_AXIS_NEG_Z);
  KX_MACRO_addTypesToDict(d, KX_MOVEMENT_ALL_AXIS, SCA_MovementSensor::KX_MOVEMENT_ALL_AXIS);

  /* TrackTo Actuator */
  KX_MACRO_addTypesToDict(d, KX_TRACK_UPAXIS_POS_X, SCA_TrackToActuator::KX_TRACK_UPAXIS_POS_X);
  KX_MACRO_addTypesToDict(d, KX_TRACK_UPAXIS_POS_Y, SCA_TrackToActuator::KX_TRACK_UPAXIS_POS_Y);
  KX_MACRO_addTypesToDict(d, KX_TRACK_UPAXIS_POS_Z, SCA_TrackToActuator::KX_TRACK_UPAXIS_POS_Z);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_POS_X, SCA_TrackToActuator::KX_TRACK_TRAXIS_POS_X);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_POS_Y, SCA_TrackToActuator::KX_TRACK_TRAXIS_POS_Y);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_POS_Z, SCA_TrackToActuator::KX_TRACK_TRAXIS_POS_Z);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_NEG_X, SCA_TrackToActuator::KX_TRACK_TRAXIS_NEG_X);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_NEG_Y, SCA_TrackToActuator::KX_TRACK_TRAXIS_NEG_Y);
  KX_MACRO_addTypesToDict(d, KX_TRACK_TRAXIS_NEG_Z, SCA_TrackToActuator::KX_TRACK_TRAXIS_NEG_Z);

  /* Dynamic actuator */
  KX_MACRO_addTypesToDict(
      d, KX_DYN_RESTORE_DYNAMICS, SCA_DynamicActuator::KX_DYN_RESTORE_DYNAMICS);
  KX_MACRO_addTypesToDict(
      d, KX_DYN_DISABLE_DYNAMICS, SCA_DynamicActuator::KX_DYN_DISABLE_DYNAMICS);
  KX_MACRO_addTypesToDict(
      d, KX_DYN_ENABLE_RIGID_BODY, SCA_DynamicActuator::KX_DYN_ENABLE_RIGID_BODY);
  KX_MACRO_addTypesToDict(
      d, KX_DYN_DISABLE_RIGID_BODY, SCA_DynamicActuator::KX_DYN_DISABLE_RIGID_BODY);
  KX_MACRO_addTypesToDict(d, KX_DYN_SET_MASS, SCA_DynamicActuator::KX_DYN_SET_MASS);

  /* Input & Mouse Sensor */
  KX_MACRO_addTypesToDict(d, KX_INPUT_NONE, SCA_InputEvent::NONE);
  KX_MACRO_addTypesToDict(d, KX_INPUT_JUST_ACTIVATED, SCA_InputEvent::JUSTACTIVATED);
  KX_MACRO_addTypesToDict(d, KX_INPUT_ACTIVE, SCA_InputEvent::ACTIVE);
  KX_MACRO_addTypesToDict(d, KX_INPUT_JUST_RELEASED, SCA_InputEvent::JUSTRELEASED);

  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_LEFT, SCA_IInputDevice::LEFTMOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_MIDDLE, SCA_IInputDevice::MIDDLEMOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_RIGHT, SCA_IInputDevice::RIGHTMOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_BUTTON4, SCA_IInputDevice::BUTTON4MOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_BUTTON5, SCA_IInputDevice::BUTTON5MOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_BUTTON6, SCA_IInputDevice::BUTTON6MOUSE);
  KX_MACRO_addTypesToDict(d, KX_MOUSE_BUT_BUTTON7, SCA_IInputDevice::BUTTON7MOUSE);

  /* 2D Filter Actuator */
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_ENABLED, RAS_2DFilterManager::FILTER_ENABLED);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_DISABLED, RAS_2DFilterManager::FILTER_DISABLED);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_NOFILTER, RAS_2DFilterManager::FILTER_NOFILTER);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_MOTIONBLUR, RAS_2DFilterManager::FILTER_MOTIONBLUR);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_BLUR, RAS_2DFilterManager::FILTER_BLUR);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_SHARPEN, RAS_2DFilterManager::FILTER_SHARPEN);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_DILATION, RAS_2DFilterManager::FILTER_DILATION);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_EROSION, RAS_2DFilterManager::FILTER_EROSION);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_LAPLACIAN, RAS_2DFilterManager::FILTER_LAPLACIAN);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_SOBEL, RAS_2DFilterManager::FILTER_SOBEL);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_PREWITT, RAS_2DFilterManager::FILTER_PREWITT);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_GRAYSCALE, RAS_2DFilterManager::FILTER_GRAYSCALE);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_SEPIA, RAS_2DFilterManager::FILTER_SEPIA);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_INVERT, RAS_2DFilterManager::FILTER_INVERT);
  KX_MACRO_addTypesToDict(d, RAS_2DFILTER_CUSTOMFILTER, RAS_2DFilterManager::FILTER_CUSTOMFILTER);

  /* Sound Actuator */
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_PLAYSTOP, SCA_SoundActuator::KX_SOUNDACT_PLAYSTOP);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_PLAYEND, SCA_SoundActuator::KX_SOUNDACT_PLAYEND);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_LOOPSTOP, SCA_SoundActuator::KX_SOUNDACT_LOOPSTOP);
  KX_MACRO_addTypesToDict(d, KX_SOUNDACT_LOOPEND, SCA_SoundActuator::KX_SOUNDACT_LOOPEND);
  KX_MACRO_addTypesToDict(
      d, KX_SOUNDACT_LOOPBIDIRECTIONAL, SCA_SoundActuator::KX_SOUNDACT_LOOPBIDIRECTIONAL);
  KX_MACRO_addTypesToDict(d,
                          KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP,
                          SCA_SoundActuator::KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP);

  /* State Actuator */
  KX_MACRO_addTypesToDict(d, KX_STATE_OP_CPY, SCA_StateActuator::OP_CPY);
  KX_MACRO_addTypesToDict(d, KX_STATE_OP_SET, SCA_StateActuator::OP_SET);
  KX_MACRO_addTypesToDict(d, KX_STATE_OP_CLR, SCA_StateActuator::OP_CLR);
  KX_MACRO_addTypesToDict(d, KX_STATE_OP_NEG, SCA_StateActuator::OP_NEG);

  /* Game Actuator Modes */
  KX_MACRO_addTypesToDict(d, KX_GAME_LOAD, SCA_GameActuator::KX_GAME_LOAD);
  KX_MACRO_addTypesToDict(d, KX_GAME_START, SCA_GameActuator::KX_GAME_START);
  KX_MACRO_addTypesToDict(d, KX_GAME_RESTART, SCA_GameActuator::KX_GAME_RESTART);
  KX_MACRO_addTypesToDict(d, KX_GAME_QUIT, SCA_GameActuator::KX_GAME_QUIT);
  KX_MACRO_addTypesToDict(d, KX_GAME_SAVECFG, SCA_GameActuator::KX_GAME_SAVECFG);
  KX_MACRO_addTypesToDict(d, KX_GAME_LOADCFG, SCA_GameActuator::KX_GAME_LOADCFG);
  KX_MACRO_addTypesToDict(d, KX_GAME_SCREENSHOT, SCA_GameActuator::KX_GAME_SCREENSHOT);

  /* Scene Actuator Modes */
  KX_MACRO_addTypesToDict(d, KX_SCENE_RESTART, SCA_SceneActuator::KX_SCENE_RESTART);
  KX_MACRO_addTypesToDict(d, KX_SCENE_SET_SCENE, SCA_SceneActuator::KX_SCENE_SET_SCENE);
  KX_MACRO_addTypesToDict(d, KX_SCENE_SET_CAMERA, SCA_SceneActuator::KX_SCENE_SET_CAMERA);
  KX_MACRO_addTypesToDict(d, KX_SCENE_REMOVE_SCENE, SCA_SceneActuator::KX_SCENE_REMOVE_SCENE);

  /* Parent Actuator Modes */
  KX_MACRO_addTypesToDict(d, KX_PARENT_SET, SCA_ParentActuator::KX_PARENT_SET);
  KX_MACRO_addTypesToDict(d, KX_PARENT_REMOVE, SCA_ParentActuator::KX_PARENT_REMOVE);

  /* BL_ArmatureConstraint type */
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_TRACKTO, CONSTRAINT_TYPE_TRACKTO);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_KINEMATIC, CONSTRAINT_TYPE_KINEMATIC);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_ROTLIKE, CONSTRAINT_TYPE_ROTLIKE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_LOCLIKE, CONSTRAINT_TYPE_LOCLIKE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_MINMAX, CONSTRAINT_TYPE_MINMAX);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_SIZELIKE, CONSTRAINT_TYPE_SIZELIKE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_LOCKTRACK, CONSTRAINT_TYPE_LOCKTRACK);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_STRETCHTO, CONSTRAINT_TYPE_STRETCHTO);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_CLAMPTO, CONSTRAINT_TYPE_CLAMPTO);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_TRANSFORM, CONSTRAINT_TYPE_TRANSFORM);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_TYPE_DISTLIMIT, CONSTRAINT_TYPE_DISTLIMIT);
  /* BL_ArmatureConstraint ik_type */
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_COPYPOSE, CONSTRAINT_IK_COPYPOSE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_DISTANCE, CONSTRAINT_IK_DISTANCE);
  /* BL_ArmatureConstraint ik_mode */
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_MODE_INSIDE, LIMITDIST_INSIDE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_MODE_OUTSIDE, LIMITDIST_OUTSIDE);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_MODE_ONSURFACE, LIMITDIST_ONSURFACE);
  /* BL_ArmatureConstraint ik_flag */
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_FLAG_TIP, CONSTRAINT_IK_TIP);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_FLAG_ROT, CONSTRAINT_IK_ROT);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_FLAG_STRETCH, CONSTRAINT_IK_STRETCH);
  KX_MACRO_addTypesToDict(d, CONSTRAINT_IK_FLAG_POS, CONSTRAINT_IK_POS);
  /* SCA_ArmatureSensor type */
  KX_MACRO_addTypesToDict(d, KX_ARMSENSOR_STATE_CHANGED, SENS_ARM_STATE_CHANGED);
  KX_MACRO_addTypesToDict(d, KX_ARMSENSOR_LIN_ERROR_BELOW, SENS_ARM_LIN_ERROR_BELOW);
  KX_MACRO_addTypesToDict(d, KX_ARMSENSOR_LIN_ERROR_ABOVE, SENS_ARM_LIN_ERROR_ABOVE);
  KX_MACRO_addTypesToDict(d, KX_ARMSENSOR_ROT_ERROR_BELOW, SENS_ARM_ROT_ERROR_BELOW);
  KX_MACRO_addTypesToDict(d, KX_ARMSENSOR_ROT_ERROR_ABOVE, SENS_ARM_ROT_ERROR_ABOVE);

  /* BL_ArmatureActuator type */
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_RUN, ACT_ARM_RUN);
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_ENABLE, ACT_ARM_ENABLE);
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_DISABLE, ACT_ARM_DISABLE);
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_SETTARGET, ACT_ARM_SETTARGET);
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_SETWEIGHT, ACT_ARM_SETWEIGHT);
  KX_MACRO_addTypesToDict(d, KX_ACT_ARMATURE_SETINFLUENCE, ACT_ARM_SETINFLUENCE);

  /* BL_Armature Channel rotation_mode */
  KX_MACRO_addTypesToDict(d, ROT_MODE_QUAT, ROT_MODE_QUAT);
  KX_MACRO_addTypesToDict(d, ROT_MODE_XYZ, ROT_MODE_XYZ);
  KX_MACRO_addTypesToDict(d, ROT_MODE_XZY, ROT_MODE_XZY);
  KX_MACRO_addTypesToDict(d, ROT_MODE_YXZ, ROT_MODE_YXZ);
  KX_MACRO_addTypesToDict(d, ROT_MODE_YZX, ROT_MODE_YZX);
  KX_MACRO_addTypesToDict(d, ROT_MODE_ZXY, ROT_MODE_ZXY);
  KX_MACRO_addTypesToDict(d, ROT_MODE_ZYX, ROT_MODE_ZYX);

  /* Steering actuator */
  KX_MACRO_addTypesToDict(d, KX_STEERING_SEEK, SCA_SteeringActuator::KX_STEERING_SEEK);
  KX_MACRO_addTypesToDict(d, KX_STEERING_FLEE, SCA_SteeringActuator::KX_STEERING_FLEE);
  KX_MACRO_addTypesToDict(
      d, KX_STEERING_PATHFOLLOWING, SCA_SteeringActuator::KX_STEERING_PATHFOLLOWING);

  /* KX_NavMeshObject render mode */
  KX_MACRO_addTypesToDict(d, RM_WALLS, KX_NavMeshObject::RM_WALLS);
  KX_MACRO_addTypesToDict(d, RM_POLYS, KX_NavMeshObject::RM_POLYS);
  KX_MACRO_addTypesToDict(d, RM_TRIS, KX_NavMeshObject::RM_TRIS);

  /* BL_Action play modes */
  KX_MACRO_addTypesToDict(d, KX_ACTION_MODE_PLAY, BL_Action::ACT_MODE_PLAY);
  KX_MACRO_addTypesToDict(d, KX_ACTION_MODE_LOOP, BL_Action::ACT_MODE_LOOP);
  KX_MACRO_addTypesToDict(d, KX_ACTION_MODE_PING_PONG, BL_Action::ACT_MODE_PING_PONG);

  /* BL_Action blend modes */
  KX_MACRO_addTypesToDict(d, KX_ACTION_BLEND_BLEND, BL_Action::ACT_BLEND_BLEND);
  KX_MACRO_addTypesToDict(d, KX_ACTION_BLEND_ADD, BL_Action::ACT_BLEND_ADD);

  /* Mouse Actuator object axis*/
  KX_MACRO_addTypesToDict(
      d, KX_ACT_MOUSE_OBJECT_AXIS_X, SCA_MouseActuator::KX_ACT_MOUSE_OBJECT_AXIS_X);
  KX_MACRO_addTypesToDict(
      d, KX_ACT_MOUSE_OBJECT_AXIS_Y, SCA_MouseActuator::KX_ACT_MOUSE_OBJECT_AXIS_Y);
  KX_MACRO_addTypesToDict(
      d, KX_ACT_MOUSE_OBJECT_AXIS_Z, SCA_MouseActuator::KX_ACT_MOUSE_OBJECT_AXIS_Z);

  // Check for errors
  if (PyErr_Occurred()) {
    Py_FatalError("can't initialize module bge.logic");
  }

  return m;
}

/**
 * Explanation of
 *
 * - backupPySysObjects()       : stores sys.path in #gp_sys_backup
 * - initPySysObjects(main)     : initializes the blendfile and library paths
 * - restorePySysObjects()      : restores sys.path from #gp_sys_backup
 *
 * These exist so the current blend dir "//" can always be used to import modules from.
 * the reason we need a few functions for this is that python is not only used by the game engine
 * so we cant just add to sys.path all the time, it would leave pythons state in a mess.
 * It would also be incorrect since loading blend files for new levels etc would always add to
 * sys.path
 *
 * To play nice with blenders python, the sys.path is backed up and the current blendfile along
 * with all its lib paths are added to the sys path.
 * When loading a new blendfile, the original sys.path is restored and the new paths are added over
 * the top.
 */

/**
 * So we can have external modules mixed with our blend files.
 */
static void backupPySysObjects(void)
{
  PyObject *sys_path = PySys_GetObject("path");
  PyObject *sys_meta_path = PySys_GetObject("meta_path");
  PyObject *sys_mods = PySys_GetObject("modules");

  /* paths */
  Py_XDECREF(gp_sys_backup.path);                             /* just in case its set */
  gp_sys_backup.path = PyList_GetSlice(sys_path, 0, INT_MAX); /* copy the list */

  /* meta_paths */
  Py_XDECREF(gp_sys_backup.meta_path);                                  /* just in case its set */
  gp_sys_backup.meta_path = PyList_GetSlice(sys_meta_path, 0, INT_MAX); /* copy the list */

  /* modules */
  Py_XDECREF(gp_sys_backup.modules);             /* just in case its set */
  gp_sys_backup.modules = PyDict_Copy(sys_mods); /* copy the dict */

  if (bpy_sys_module_backup) {
    PyDict_Clear(sys_mods);
    // Load a clean generated modules dict from the blender begining.
    PyDict_Update(sys_mods, bpy_sys_module_backup);
  }
}

/* for initPySysObjects only,
 * takes a blend path and adds a scripts dir from it
 *
 * "/home/me/foo.blend" -> "/home/me/scripts"
 */
static void initPySysObjects__append(PyObject *sys_path, const char *filename)
{
  PyObject *item;
  char expanded[FILE_MAX];

  BLI_split_dir_part(filename, expanded, sizeof(expanded)); /* get the dir part of filename only */
  BLI_path_abs(expanded,
               KX_GetMainPath().c_str()); /* filename from lib->filename is (always?) absolute, so
                                             this may not be needed but it wont hurt */
  BLI_path_normalize(
      KX_GetMainPath().c_str(),
      expanded); /* Don't use BLI_cleanup_dir because it adds a slash - BREAKS WIN32 ONLY */
  item = PyC_UnicodeFromByte(expanded);

  if (PySequence_Index(sys_path, item) == -1) {
    PyErr_Clear(); /* PySequence_Index sets a ValueError */
    PyList_Insert(sys_path, 0, item);
  }

  Py_DECREF(item);
}
static void initPySysObjects(Main *maggie)
{
  PyObject *sys_path = PySys_GetObject("path");
  PyObject *sys_meta_path = PySys_GetObject("meta_path");

  if (gp_sys_backup.path == nullptr) {
    /* backup */
    backupPySysObjects();
  }
  else {
    /* get the original sys path when the BGE started */
    PyList_SetSlice(sys_path, 0, INT_MAX, gp_sys_backup.path);
    PyList_SetSlice(sys_meta_path, 0, INT_MAX, gp_sys_backup.meta_path);
  }

  Library *lib = (Library *)maggie->libraries.first;

  while (lib) {
    /* lib->name wont work in some cases (on win32),
     * even when expanding with KX_GetMainPath(), using lib->filename is less trouble */
    initPySysObjects__append(sys_path, lib->filepath);
    lib = (Library *)lib->id.next;
  }

  initPySysObjects__append(sys_path, KX_GetMainPath().c_str());
}

static void restorePySysObjects(void)
{
  if (gp_sys_backup.path == nullptr) {
    return;
  }

  /* will never fail */
  PyObject *sys_path = PySys_GetObject("path");
  PyObject *sys_meta_path = PySys_GetObject("meta_path");
  PyObject *sys_mods = PySys_GetObject("modules");

  /* paths */
  PyList_SetSlice(sys_path, 0, INT_MAX, gp_sys_backup.path);
  Py_DECREF(gp_sys_backup.path);
  gp_sys_backup.path = nullptr;

  /* meta_path */
  PyList_SetSlice(sys_meta_path, 0, INT_MAX, gp_sys_backup.meta_path);
  Py_DECREF(gp_sys_backup.meta_path);
  gp_sys_backup.meta_path = nullptr;

  /* modules */
  PyDict_Clear(sys_mods);
  PyDict_Update(sys_mods, gp_sys_backup.modules);
  Py_DECREF(gp_sys_backup.modules);
  gp_sys_backup.modules = nullptr;
}

void appendPythonPath(const std::string &path)
{
  PyObject *sys_path = PySys_GetObject("path");
  initPySysObjects__append(sys_path, path.c_str());
}

void addImportMain(struct Main *maggie)
{
  bpy_import_main_extra_add(maggie);
}

void removeImportMain(struct Main *maggie)
{
  bpy_import_main_extra_remove(maggie);
}

PyDoc_STRVAR(BGE_module_documentation,
             "This module contains submodules for the Blender Game Engine.\n");

static struct PyModuleDef BGE_module_def = {
    PyModuleDef_HEAD_INIT,
    "bge",                    /* m_name */
    BGE_module_documentation, /* m_doc */
    0,                        /* m_size */
    nullptr,                  /* m_methods */
    nullptr,                  /* m_reload */
    nullptr,                  /* m_traverse */
    nullptr,                  /* m_clear */
    nullptr,                  /* m_free */
};

static void addSubModule(PyObject *modules,
                         PyObject *mod,
                         PyObject *submod,
                         const std::string &modname)
{
  /* PyModule_AddObject doesn't incref the sub module but PyDict_SetItemString increfs
   * the item set. So no incref and decref are needed here. */
  PyModule_AddObject(mod, modname.substr(4).c_str(), submod);
  PyDict_SetItemString(modules, modname.c_str(), submod);
}

PyMODINIT_FUNC initBGE()
{
  PyObject *modules = PyImport_GetModuleDict();
  PyObject *mod = PyModule_Create(&BGE_module_def);

  addSubModule(modules, mod, initApplicationPythonBinding(), "bge.app");
  addSubModule(modules, mod, initConstraintPythonBinding(), "bge.constraints");
  addSubModule(modules, mod, initGameKeysPythonBinding(), "bge.events");
  addSubModule(modules, mod, initGameLogicPythonBinding(), "bge.logic");
  addSubModule(modules, mod, initRasterizerPythonBinding(), "bge.render");
  addSubModule(modules, mod, initGameTypesPythonBinding(), "bge.types");
  addSubModule(modules, mod, initVideoTexturePythonBinding(), "bge.texture");

  return mod;
}

#  ifdef WITH_CYCLES
/* defined in cycles module */
static PyObject *CCL_initPython(void)
{
  return (PyObject *)CCL_python_module_init();
}
#  endif

/* minimal required blender modules to run blenderplayer */
static struct _inittab bge_internal_modules[] = {{"mathutils", PyInit_mathutils},
                                                 {"bgl", BPyInit_bgl},
                                                 {"blf", BPyInit_blf},
#  ifdef WITH_AUDASPACE
                                                 {"aud", AUD_initPython},
#  endif  // WITH_AUDASPACE
                                                 {nullptr, nullptr}};

static struct _inittab bpy_internal_modules[] = {
    {"_bpy_path", BPyInit__bpy_path},
    {"bl_math", BPyInit_bl_math},
    {"imbuf", BPyInit_imbuf},
    {"bmesh", BPyInit_bmesh},
#  ifdef WITH_FLUID
    {"manta", Manta_initPython},
#  endif
#  ifdef WITH_CYCLES
    {"_cycles", CCL_initPython},
#  endif
    {"gpu", BPyInit_gpu},
    {"idprop", BPyInit_idprop},
    {NULL, NULL},
};

/**
 * Convenience function for #initGamePlayerPythonScripting.
 *
 * These should happen so rarely that having comprehensive errors isn't needed.
 * For example if `sys.argv` fails to allocate memory.
 *
 * Show an error just to avoid silent failure in the unlikely event something goes wrong,
 * in this case a developer will need to track down the root cause.
 */
static void pystatus_exit_on_error(PyStatus status)
{
  if (UNLIKELY(PyStatus_Exception(status))) {
    fputs("Internal error initializing Python!\n", stderr);
    /* This calls `exit`. */
    Py_ExitStatusException(status);
  }
}

/**
 * Python is not initialized.
 * see bpy_interface.c's BPY_python_start() which shares the same functionality in blender.
 */
void initGamePlayerPythonScripting(int argc, char **argv, bContext *C)
{
  /* #PyPreConfig (early-configuration).  */
  {
    PyPreConfig preconfig;
    PyStatus status;

    if (BPY_python_get_use_system_env()) {
      PyPreConfig_InitPythonConfig(&preconfig);
    }
    else {
      /* Only use the systems environment variables and site when explicitly requested.
       * Since an incorrect 'PYTHONPATH' causes difficult to debug errors, see: T72807.
       * An alternative to setting `preconfig.use_environment = 0` */
      PyPreConfig_InitIsolatedConfig(&preconfig);
    }

    /* Force `utf-8` on all platforms, since this is what's used for Blender's internal strings,
     * providing consistent encoding behavior across all Blender installations.
     *
     * This also uses the `surrogateescape` error handler ensures any unexpected bytes are escaped
     * instead of raising an error.
     *
     * Without this `sys.getfilesystemencoding()` and `sys.stdout` for example may be set to ASCII
     * or some other encoding - where printing some `utf-8` values will raise an error.
     *
     * This can cause scripts to fail entirely on some systems.
     *
     * This assignment is the equivalent of enabling the `PYTHONUTF8` environment variable.
     * See `PEP-540` for details on exactly what this changes. */
    preconfig.utf8_mode = true;

    /* Note that there is no reason to call #Py_PreInitializeFromBytesArgs here
     * as this is only used so that command line arguments can be handled by Python itself,
     * not for setting `sys.argv` (handled below). */
    status = Py_PreInitialize(&preconfig);
    pystatus_exit_on_error(status);
  }

  /* must run before python initializes, but after #PyPreConfig. */
  PyImport_ExtendInittab(bge_internal_modules);
  /* Must run before python initializes, but after #PyPreConfig. */
  PyImport_ExtendInittab(bpy_internal_modules);

  if (argv) {
    PyConfig config;
    PyStatus status;
    bool has_python_executable = false;

    PyConfig_InitPythonConfig(&config);

    /* Suppress error messages when calculating the module search path.
     * While harmless, it's noisy. */
    config.pathconfig_warnings = 0;

    /* When using the system's Python, allow the site-directory as well. */
    config.user_site_directory = BPY_python_get_use_system_env();

    /* While `sys.argv` is set, we don't want Python to interpret it. */
    config.parse_argv = 0;
    status = PyConfig_SetBytesArgv(&config, argc, (char *const *)argv);
    pystatus_exit_on_error(status);

    /* Needed for Python's initialization for portable Python installations.
     * We could use #Py_SetPath, but this overrides Python's internal logic
     * for calculating it's own module search paths.
     *
     * `sys.executable` is overwritten after initialization to the Python binary. */
    {
      const char *program_path = BKE_appdir_program_path();
      status = PyConfig_SetBytesString(&config, &config.program_name, program_path);
      pystatus_exit_on_error(status);
    }

    /* Setting the program name is important so the 'multiprocessing' module
     * can launch new Python instances. */
    {
      char program_path[FILE_MAX];
      if (BKE_appdir_program_python_search(
              program_path, sizeof(program_path), PY_MAJOR_VERSION, PY_MINOR_VERSION)) {
        status = PyConfig_SetBytesString(&config, &config.executable, program_path);
        pystatus_exit_on_error(status);
        has_python_executable = true;
      }
      else {
        /* Set to `sys.executable = None` below (we can't do before Python is initialized). */
        fprintf(stderr,
                "Unable to find the python binary, "
                "the multiprocessing module may not be functional!\n");
      }
    }

    /* Allow to use our own included Python. `py_path_bundle` may be NULL. */
    {
      const char *py_path_bundle = BKE_appdir_folder_id(BLENDER_SYSTEM_PYTHON, NULL);
      if (py_path_bundle != NULL) {

#  ifdef __APPLE__
        /* Mac-OS allows file/directory names to contain `:` character
         * (represented as `/` in the Finder) but current Python lib (as of release 3.1.1)
         * doesn't handle these correctly. */
        if (strchr(py_path_bundle, ':')) {
          fprintf(stderr,
                  "Warning! Blender application is located in a path containing ':' or '/' chars\n"
                  "This may make python import function fail\n");
        }
#  endif /* __APPLE__ */

        status = PyConfig_SetBytesString(&config, &config.home, py_path_bundle);
        pystatus_exit_on_error(status);
      }
      else {
        /* Common enough to use the system Python on Linux/Unix, warn on other systems. */
#  if defined(__APPLE__) || defined(_WIN32)
        fprintf(stderr,
                "Bundled Python not found and is expected on this platform "
                "(the 'install' target may have not been built)\n");
#  endif
      }
    }

    /* Initialize Python (also acquires lock). */
    status = Py_InitializeFromConfig(&config);
    pystatus_exit_on_error(status);

    if (!has_python_executable) {
      PySys_SetObject("executable", Py_None);
    }
  }
}

void postInitGamePlayerPythonScripting(
    Main *maggie, int argc, char **argv, bContext *C, bool *audioDeviceIsInitialized)
{
  /* Yet another gotcha in the py api
   * Cant run PySys_SetArgv more than once because this adds the
   * binary dir to the sys.path each time.
   * Id have thought python being totally restarted would make this ok but
   * somehow it remembers the sys.path - Campbell
   */
  static bool first_time = true;

  bpy_import_init(PyEval_GetBuiltins());

  bpy_import_main_set(maggie);

#  ifdef WITH_FLUID
  /* Required to prevent assertion error, see:
   * https://stackoverflow.com/questions/27844676 */
  Py_DECREF(PyImport_ImportModule("threading"));
#  endif

  if (first_time) {

    bpy_intern_string_init();

    BPY_rna_init();

    BPy_init_modules(C);

    BPY_python_rna_alloc_types();

    BPY_atexit_register(); /* this can init any time */
  }

  initPySysObjects(maggie);

  /* mathutils types are used by the BGE even if we don't import them */
  {
    PyObject *mod = PyImport_ImportModuleLevel("mathutils", nullptr, nullptr, nullptr, 0);
    Py_DECREF(mod);
  }

#  ifdef WITH_AUDASPACE
  /* accessing a SoundActuator's sound results in a crash if aud is not initialized... */
  {
    PyObject *mod = PyImport_ImportModuleLevel("aud", nullptr, nullptr, nullptr, 0);
    if (mod) {  // avoiding crash if audio device can not be initialized
      Py_DECREF(mod);
      *audioDeviceIsInitialized = true;
    }
    else {
      *audioDeviceIsInitialized = false;
    }
  }
#  endif

  PyDict_SetItemString(PyImport_GetModuleDict(), "bge", initBGE());

  first_time = false;

  EXP_PyObjectPlus::ClearDeprecationWarning();

  BPY_python_reset(C);
}

void exitGamePlayerPythonScripting()
{
  /* Clean up the Python mouse and keyboard */
  delete gp_PythonKeyboard;
  gp_PythonKeyboard = nullptr;

  delete gp_PythonMouse;
  gp_PythonMouse = nullptr;

  for (int i = 0; i < JOYINDEX_MAX; ++i) {
    if (gp_PythonJoysticks[i]) {
      delete gp_PythonJoysticks[i];
      gp_PythonJoysticks[i] = nullptr;
    }
  }

  /* since python restarts we cant let the python backup of the sys.path hang around in a global
   * pointer */
  restorePySysObjects(); /* get back the original sys.path and clear the backup */

  // Py_Finalize();
  bpy_import_main_set(nullptr);
  EXP_PyObjectPlus::ClearDeprecationWarning();
}

/**
 * Python is already initialized.
 */
void initGamePythonScripting(Main *maggie, bContext *C, bool *audioDeviceIsInitialized)
{
  /* Yet another gotcha in the py api
   * Cant run PySys_SetArgv more than once because this adds the
   * binary dir to the sys.path each time.
   * Id have thought python being totally restarted would make this ok but
   * somehow it remembers the sys.path - Campbell
   */

  /* #PyPreConfig (early-configuration).  */
  {
    PyPreConfig preconfig;
    PyStatus status;

    backupPySysObjects();

    if (BPY_python_get_use_system_env()) {
      PyPreConfig_InitPythonConfig(&preconfig);
    }
    else {
      /* Only use the systems environment variables and site when explicitly requested.
       * Since an incorrect 'PYTHONPATH' causes difficult to debug errors, see: T72807.
       * An alternative to setting `preconfig.use_environment = 0` */
      PyPreConfig_InitIsolatedConfig(&preconfig);
    }

    /* Force `utf-8` on all platforms, since this is what's used for Blender's internal strings,
     * providing consistent encoding behavior across all Blender installations.
     *
     * This also uses the `surrogateescape` error handler ensures any unexpected bytes are escaped
     * instead of raising an error.
     *
     * Without this `sys.getfilesystemencoding()` and `sys.stdout` for example may be set to ASCII
     * or some other encoding - where printing some `utf-8` values will raise an error.
     *
     * This can cause scripts to fail entirely on some systems.
     *
     * This assignment is the equivalent of enabling the `PYTHONUTF8` environment variable.
     * See `PEP-540` for details on exactly what this changes. */
    preconfig.utf8_mode = true;

    /* Note that there is no reason to call #Py_PreInitializeFromBytesArgs here
     * as this is only used so that command line arguments can be handled by Python itself,
     * not for setting `sys.argv` (handled below). */
    status = Py_PreInitialize(&preconfig);
    pystatus_exit_on_error(status);
  }

  /* must run before python initializes, but after #PyPreConfig. */
  PyImport_ExtendInittab(bge_internal_modules);
  /* Must run before python initializes, but after #PyPreConfig. */
  PyImport_ExtendInittab(bpy_internal_modules);

  /* #PyConfig (initialize Python). */
  {
    PyConfig config;
    PyStatus status;
    bool has_python_executable = false;

    PyConfig_InitPythonConfig(&config);

    /* Suppress error messages when calculating the module search path.
     * While harmless, it's noisy. */
    config.pathconfig_warnings = 0;

    /* When using the system's Python, allow the site-directory as well. */
    config.user_site_directory = BPY_python_get_use_system_env();

    /* While `sys.argv` is set, we don't want Python to interpret it. */
    config.parse_argv = 0;
    // status = PyConfig_SetBytesArgv(&config, argc, (char *const *)argv);
    // pystatus_exit_on_error(status);

    /* Needed for Python's initialization for portable Python installations.
     * We could use #Py_SetPath, but this overrides Python's internal logic
     * for calculating it's own module search paths.
     *
     * `sys.executable` is overwritten after initialization to the Python binary. */
    {
      const char *program_path = BKE_appdir_program_path();
      status = PyConfig_SetBytesString(&config, &config.program_name, program_path);
      pystatus_exit_on_error(status);
    }

    /* Setting the program name is important so the 'multiprocessing' module
     * can launch new Python instances. */
    {
      char program_path[FILE_MAX];
      if (BKE_appdir_program_python_search(
              program_path, sizeof(program_path), PY_MAJOR_VERSION, PY_MINOR_VERSION)) {
        status = PyConfig_SetBytesString(&config, &config.executable, program_path);
        pystatus_exit_on_error(status);
        has_python_executable = true;
      }
      else {
        /* Set to `sys.executable = None` below (we can't do before Python is initialized). */
        fprintf(stderr,
                "Unable to find the python binary, "
                "the multiprocessing module may not be functional!\n");
      }
    }

    /* Allow to use our own included Python. `py_path_bundle` may be NULL. */
    {
      const char *py_path_bundle = BKE_appdir_folder_id(BLENDER_SYSTEM_PYTHON, NULL);
      if (py_path_bundle != NULL) {

#  ifdef __APPLE__
        /* Mac-OS allows file/directory names to contain `:` character
         * (represented as `/` in the Finder) but current Python lib (as of release 3.1.1)
         * doesn't handle these correctly. */
        if (strchr(py_path_bundle, ':')) {
          fprintf(stderr,
                  "Warning! Blender application is located in a path containing ':' or '/' chars\n"
                  "This may make python import function fail\n");
        }
#  endif /* __APPLE__ */

        status = PyConfig_SetBytesString(&config, &config.home, py_path_bundle);
        pystatus_exit_on_error(status);
      }
      else {
        /* Common enough to use the system Python on Linux/Unix, warn on other systems. */
#  if defined(__APPLE__) || defined(_WIN32)
        fprintf(stderr,
                "Bundled Python not found and is expected on this platform "
                "(the 'install' target may have not been built)\n");
#  endif
      }
    }

    /* Initialize Python (also acquires lock). */
    status = Py_InitializeFromConfig(&config);
    pystatus_exit_on_error(status);

    if (!has_python_executable) {
      PySys_SetObject("executable", Py_None);
    }
  }

#  ifdef WITH_FLUID
  /* Required to prevent assertion error, see:
   * https://stackoverflow.com/questions/27844676 */
  Py_DECREF(PyImport_ImportModule("threading"));
#  endif

  bpy_import_init(PyEval_GetBuiltins());

  bpy_import_main_set(maggie);

  initPySysObjects(maggie);

  /* mathutils types are used by the BGE even if we don't import them */
  {
    PyObject *mod = PyImport_ImportModuleLevel("mathutils", nullptr, nullptr, nullptr, 0);
    Py_DECREF(mod);
  }

#  ifdef WITH_AUDASPACE
  /* accessing a SoundActuator's sound results in a crash if aud is not initialized... */
  {
    PyObject *mod = PyImport_ImportModuleLevel("aud", nullptr, nullptr, nullptr, 0);
    if (mod) {  // avoiding crash if audio device can not be initialized
      Py_DECREF(mod);
      *audioDeviceIsInitialized = true;
    }
    else {
      *audioDeviceIsInitialized = false;
    }
  }
#  endif

  PyDict_SetItemString(PyImport_GetModuleDict(), "bge", initBGE());

  BPY_python_reset(C);

  EXP_PyObjectPlus::ClearDeprecationWarning();
}

void exitGamePythonScripting()
{
  /* Clean up the Python mouse and keyboard */
  delete gp_PythonKeyboard;
  gp_PythonKeyboard = nullptr;

  delete gp_PythonMouse;
  gp_PythonMouse = nullptr;

  for (int i = 0; i < JOYINDEX_MAX; ++i) {
    if (gp_PythonJoysticks[i]) {
      delete gp_PythonJoysticks[i];
      gp_PythonJoysticks[i] = nullptr;
    }
  }

  restorePySysObjects(); /* get back the original sys.path and clear the backup */
  bpy_import_main_set(nullptr);
  EXP_PyObjectPlus::ClearDeprecationWarning();
}

/* similar to the above functions except it sets up the namespace
 * and other more general things */
void setupGamePython(KX_KetsjiEngine *ketsjiengine,
                     Main *blenderdata,
                     PyObject *pyGlobalDict,
                     PyObject **gameLogic,
                     int argc,
                     char **argv,
                     bContext *C,
                     bool *audioDeviceIsInitialized)
{
  PyObject *modules;

  if (argv) /* player only */
    postInitGamePlayerPythonScripting(blenderdata, argc, argv, C, audioDeviceIsInitialized);
  else
    initGamePythonScripting(blenderdata, C, audioDeviceIsInitialized);

  modules = PyImport_GetModuleDict();

  *gameLogic = PyDict_GetItemString(modules, "GameLogic");
  /* is set in initGameLogicPythonBinding so only set here if we want it to persist between scenes
   */
  if (pyGlobalDict)
    PyDict_SetItemString(PyModule_GetDict(*gameLogic),
                         "globalDict",
                         pyGlobalDict);  // Same as importing the module.z

  Scene *startscene = CTX_data_scene(C);

  PyObject *logger = PyImport_ImportModule("bge_extras.logger");

  if (logger) {
    PyObject_CallMethod(logger, "setup", "n", startscene->gm.logLevel);
  }

  if (PyErr_Occurred()) {
    PyErr_Print();
  }

  Py_XDECREF(logger);
}

void createPythonConsole()
{
  // Use an external file, by this way the user can modify it.
  char filepath[FILE_MAX];
  BLI_strncpy(filepath, BKE_appdir_folder_id(BLENDER_SYSTEM_SCRIPTS, "bge"), sizeof(filepath));
  BLI_path_append(filepath, sizeof(filepath), "interpreter.py");

  FILE *fp = fopen(filepath, "r+");
  // Execute the file in python.
  PyRun_SimpleFile(fp, filepath);
}

void updatePythonJoysticks(short (&addrem)[JOYINDEX_MAX])
{
  PyObject *gameLogic = PyImport_ImportModule("GameLogic");
  PyObject *pythonJoyList = PyDict_GetItemString(PyModule_GetDict(gameLogic), "joysticks");

  for (unsigned short i = 0; i < JOYINDEX_MAX; ++i) {
    if (addrem[i] == 0) {
      continue;
    }

    PyObject *item = Py_None;

    if (addrem[i] == 1) {
      DEV_Joystick *joy = DEV_Joystick::GetInstance(i);
      if (joy && joy->Connected()) {
        gp_PythonJoysticks[i] = new SCA_PythonJoystick(joy, i);
        item = gp_PythonJoysticks[i]->GetProxy();
      }
    }
    else if (addrem[i] == 2) {
      if (gp_PythonJoysticks[i]) {
        delete gp_PythonJoysticks[i];
        gp_PythonJoysticks[i] = nullptr;
      }
    }

    PyList_SetItem(pythonJoyList, i, item);
  }

  Py_DECREF(gameLogic);
}

static struct PyModuleDef Rasterizer_module_def = {
    PyModuleDef_HEAD_INIT,
    "Rasterizer",                    /* m_name */
    Rasterizer_module_documentation, /* m_doc */
    0,                               /* m_size */
    rasterizer_methods,              /* m_methods */
    0,                               /* m_reload */
    0,                               /* m_traverse */
    0,                               /* m_clear */
    0,                               /* m_free */
};

PyMODINIT_FUNC initRasterizerPythonBinding()
{
  PyObject *m;
  PyObject *d;

  m = PyModule_Create(&Rasterizer_module_def);
  PyDict_SetItemString(PySys_GetObject("modules"), Rasterizer_module_def.m_name, m);

  // Add some symbolic constants to the module
  d = PyModule_GetDict(m);
  ErrorObject = PyUnicode_FromString("Rasterizer.error");
  PyDict_SetItemString(d, "error", ErrorObject);
  Py_DECREF(ErrorObject);

  KX_MACRO_addTypesToDict(d, RAS_MIPMAP_NONE, RAS_Rasterizer::RAS_MIPMAP_NONE);
  KX_MACRO_addTypesToDict(d, RAS_MIPMAP_NEAREST, RAS_Rasterizer::RAS_MIPMAP_NEAREST);
  KX_MACRO_addTypesToDict(d, RAS_MIPMAP_LINEAR, RAS_Rasterizer::RAS_MIPMAP_LINEAR);

  /* for get/setVsync */
  KX_MACRO_addTypesToDict(d, VSYNC_OFF, VSYNC_OFF);
  KX_MACRO_addTypesToDict(d, VSYNC_ON, VSYNC_ON);
  KX_MACRO_addTypesToDict(d, VSYNC_ADAPTIVE, VSYNC_ADAPTIVE);

  /* stereoscopy */
  KX_MACRO_addTypesToDict(d, LEFT_EYE, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  KX_MACRO_addTypesToDict(d, RIGHT_EYE, RAS_Rasterizer::RAS_STEREO_RIGHTEYE);

  // XXXX Add constants here

  // Check for errors
  if (PyErr_Occurred()) {
    Py_FatalError("can't initialize module Rasterizer");
  }

  return m;
}

/* ------------------------------------------------------------------------- */
/* GameKeys: symbolic constants for key mapping                              */
/* ------------------------------------------------------------------------- */

PyDoc_STRVAR(GameKeys_module_documentation, "This modules provides defines for key-codes");

PyDoc_STRVAR(gPyEventToString_doc,
             "EventToString(event)\n"
             "Take a valid event from the GameKeys module or Keyboard Sensor and return a name");

static PyObject *gPyEventToString(PyObject *, PyObject *value)
{
  PyObject *mod, *dict, *key, *val, *ret = nullptr;
  Py_ssize_t pos = 0;

  mod = PyImport_ImportModule("GameKeys");
  if (!mod)
    return nullptr;

  dict = PyModule_GetDict(mod);

  while (PyDict_Next(dict, &pos, &key, &val)) {
    if (PyObject_RichCompareBool(value, val, Py_EQ)) {
      ret = key;
      break;
    }
  }

  PyErr_Clear();  // in case there was an error clearing
  Py_DECREF(mod);
  if (!ret)
    PyErr_SetString(PyExc_ValueError,
                    "GameKeys.EventToString(int): expected a valid int keyboard event");
  else
    Py_INCREF(ret);

  return ret;
}

PyDoc_STRVAR(
    gPyEventToCharacter_doc,
    "EventToCharacter(event, is_shift)\n"
    "Take a valid event from the GameKeys module or Keyboard Sensor and return a character");

static PyObject *gPyEventToCharacter(PyObject *, PyObject *args)
{
  int event, shift;
  if (!PyArg_ParseTuple(args, "ii:EventToCharacter", &event, &shift)) {
    return nullptr;
  }

  char character[2] = {
      SCA_IInputDevice::ConvertKeyToChar((SCA_IInputDevice::SCA_EnumInputs)event, (bool)shift),
      '\0'};
  return PyUnicode_FromString(character);
}

static struct PyMethodDef gamekeys_methods[] = {
    {"EventToCharacter",
     (PyCFunction)gPyEventToCharacter,
     METH_VARARGS,
     (const char *)gPyEventToCharacter_doc},
    {"EventToString", (PyCFunction)gPyEventToString, METH_O, (const char *)gPyEventToString_doc},
    {nullptr, (PyCFunction) nullptr, 0, nullptr}};

static struct PyModuleDef GameKeys_module_def = {
    PyModuleDef_HEAD_INIT,
    "GameKeys",                    /* m_name */
    GameKeys_module_documentation, /* m_doc */
    0,                             /* m_size */
    gamekeys_methods,              /* m_methods */
    0,                             /* m_reload */
    0,                             /* m_traverse */
    0,                             /* m_clear */
    0,                             /* m_free */
};

PyMODINIT_FUNC initGameKeysPythonBinding()
{
  PyObject *m;
  PyObject *d;

  m = PyModule_Create(&GameKeys_module_def);
  PyDict_SetItemString(PySys_GetObject("modules"), GameKeys_module_def.m_name, m);

  // Add some symbolic constants to the module
  d = PyModule_GetDict(m);

  // XXXX Add constants here

  KX_MACRO_addTypesToDict(d, AKEY, SCA_IInputDevice::AKEY);
  KX_MACRO_addTypesToDict(d, BKEY, SCA_IInputDevice::BKEY);
  KX_MACRO_addTypesToDict(d, CKEY, SCA_IInputDevice::CKEY);
  KX_MACRO_addTypesToDict(d, DKEY, SCA_IInputDevice::DKEY);
  KX_MACRO_addTypesToDict(d, EKEY, SCA_IInputDevice::EKEY);
  KX_MACRO_addTypesToDict(d, FKEY, SCA_IInputDevice::FKEY);
  KX_MACRO_addTypesToDict(d, GKEY, SCA_IInputDevice::GKEY);
  KX_MACRO_addTypesToDict(d, HKEY, SCA_IInputDevice::HKEY_);
  KX_MACRO_addTypesToDict(d, IKEY, SCA_IInputDevice::IKEY);
  KX_MACRO_addTypesToDict(d, JKEY, SCA_IInputDevice::JKEY);
  KX_MACRO_addTypesToDict(d, KKEY, SCA_IInputDevice::KKEY);
  KX_MACRO_addTypesToDict(d, LKEY, SCA_IInputDevice::LKEY);
  KX_MACRO_addTypesToDict(d, MKEY, SCA_IInputDevice::MKEY);
  KX_MACRO_addTypesToDict(d, NKEY, SCA_IInputDevice::NKEY);
  KX_MACRO_addTypesToDict(d, OKEY, SCA_IInputDevice::OKEY);
  KX_MACRO_addTypesToDict(d, PKEY, SCA_IInputDevice::PKEY);
  KX_MACRO_addTypesToDict(d, QKEY, SCA_IInputDevice::QKEY);
  KX_MACRO_addTypesToDict(d, RKEY, SCA_IInputDevice::RKEY);
  KX_MACRO_addTypesToDict(d, SKEY, SCA_IInputDevice::SKEY);
  KX_MACRO_addTypesToDict(d, TKEY, SCA_IInputDevice::TKEY);
  KX_MACRO_addTypesToDict(d, UKEY, SCA_IInputDevice::UKEY);
  KX_MACRO_addTypesToDict(d, VKEY, SCA_IInputDevice::VKEY);
  KX_MACRO_addTypesToDict(d, WKEY, SCA_IInputDevice::WKEY);
  KX_MACRO_addTypesToDict(d, XKEY, SCA_IInputDevice::XKEY);
  KX_MACRO_addTypesToDict(d, YKEY, SCA_IInputDevice::YKEY);
  KX_MACRO_addTypesToDict(d, ZKEY, SCA_IInputDevice::ZKEY);

  KX_MACRO_addTypesToDict(d, ZEROKEY, SCA_IInputDevice::ZEROKEY);
  KX_MACRO_addTypesToDict(d, ONEKEY, SCA_IInputDevice::ONEKEY);
  KX_MACRO_addTypesToDict(d, TWOKEY, SCA_IInputDevice::TWOKEY);
  KX_MACRO_addTypesToDict(d, THREEKEY, SCA_IInputDevice::THREEKEY);
  KX_MACRO_addTypesToDict(d, FOURKEY, SCA_IInputDevice::FOURKEY);
  KX_MACRO_addTypesToDict(d, FIVEKEY, SCA_IInputDevice::FIVEKEY);
  KX_MACRO_addTypesToDict(d, SIXKEY, SCA_IInputDevice::SIXKEY);
  KX_MACRO_addTypesToDict(d, SEVENKEY, SCA_IInputDevice::SEVENKEY);
  KX_MACRO_addTypesToDict(d, EIGHTKEY, SCA_IInputDevice::EIGHTKEY);
  KX_MACRO_addTypesToDict(d, NINEKEY, SCA_IInputDevice::NINEKEY);

  KX_MACRO_addTypesToDict(d, CAPSLOCKKEY, SCA_IInputDevice::CAPSLOCKKEY);

  KX_MACRO_addTypesToDict(d, LEFTCTRLKEY, SCA_IInputDevice::LEFTCTRLKEY);
  KX_MACRO_addTypesToDict(d, LEFTALTKEY, SCA_IInputDevice::LEFTALTKEY);
  KX_MACRO_addTypesToDict(d, RIGHTALTKEY, SCA_IInputDevice::RIGHTALTKEY);
  KX_MACRO_addTypesToDict(d, RIGHTCTRLKEY, SCA_IInputDevice::RIGHTCTRLKEY);
  KX_MACRO_addTypesToDict(d, RIGHTSHIFTKEY, SCA_IInputDevice::RIGHTSHIFTKEY);
  KX_MACRO_addTypesToDict(d, LEFTSHIFTKEY, SCA_IInputDevice::LEFTSHIFTKEY);

  KX_MACRO_addTypesToDict(d, ESCKEY, SCA_IInputDevice::ESCKEY);
  KX_MACRO_addTypesToDict(d, TABKEY, SCA_IInputDevice::TABKEY);
  KX_MACRO_addTypesToDict(d, RETKEY, SCA_IInputDevice::RETKEY);
  KX_MACRO_addTypesToDict(d, ENTERKEY, SCA_IInputDevice::RETKEY);
  KX_MACRO_addTypesToDict(d, SPACEKEY, SCA_IInputDevice::SPACEKEY);
  KX_MACRO_addTypesToDict(d, LINEFEEDKEY, SCA_IInputDevice::LINEFEEDKEY);
  KX_MACRO_addTypesToDict(d, BACKSPACEKEY, SCA_IInputDevice::BACKSPACEKEY);
  KX_MACRO_addTypesToDict(d, DELKEY, SCA_IInputDevice::DELKEY);
  KX_MACRO_addTypesToDict(d, SEMICOLONKEY, SCA_IInputDevice::SEMICOLONKEY);
  KX_MACRO_addTypesToDict(d, PERIODKEY, SCA_IInputDevice::PERIODKEY);
  KX_MACRO_addTypesToDict(d, COMMAKEY, SCA_IInputDevice::COMMAKEY);
  KX_MACRO_addTypesToDict(d, QUOTEKEY, SCA_IInputDevice::QUOTEKEY);
  KX_MACRO_addTypesToDict(d, ACCENTGRAVEKEY, SCA_IInputDevice::ACCENTGRAVEKEY);
  KX_MACRO_addTypesToDict(d, MINUSKEY, SCA_IInputDevice::MINUSKEY);
  KX_MACRO_addTypesToDict(d, SLASHKEY, SCA_IInputDevice::SLASHKEY);
  KX_MACRO_addTypesToDict(d, BACKSLASHKEY, SCA_IInputDevice::BACKSLASHKEY);
  KX_MACRO_addTypesToDict(d, EQUALKEY, SCA_IInputDevice::EQUALKEY);
  KX_MACRO_addTypesToDict(d, LEFTBRACKETKEY, SCA_IInputDevice::LEFTBRACKETKEY);
  KX_MACRO_addTypesToDict(d, RIGHTBRACKETKEY, SCA_IInputDevice::RIGHTBRACKETKEY);

  KX_MACRO_addTypesToDict(d, LEFTARROWKEY, SCA_IInputDevice::LEFTARROWKEY);
  KX_MACRO_addTypesToDict(d, DOWNARROWKEY, SCA_IInputDevice::DOWNARROWKEY);
  KX_MACRO_addTypesToDict(d, RIGHTARROWKEY, SCA_IInputDevice::RIGHTARROWKEY);
  KX_MACRO_addTypesToDict(d, UPARROWKEY, SCA_IInputDevice::UPARROWKEY);

  KX_MACRO_addTypesToDict(d, PAD2, SCA_IInputDevice::PAD2);
  KX_MACRO_addTypesToDict(d, PAD4, SCA_IInputDevice::PAD4);
  KX_MACRO_addTypesToDict(d, PAD6, SCA_IInputDevice::PAD6);
  KX_MACRO_addTypesToDict(d, PAD8, SCA_IInputDevice::PAD8);

  KX_MACRO_addTypesToDict(d, PAD1, SCA_IInputDevice::PAD1);
  KX_MACRO_addTypesToDict(d, PAD3, SCA_IInputDevice::PAD3);
  KX_MACRO_addTypesToDict(d, PAD5, SCA_IInputDevice::PAD5);
  KX_MACRO_addTypesToDict(d, PAD7, SCA_IInputDevice::PAD7);
  KX_MACRO_addTypesToDict(d, PAD9, SCA_IInputDevice::PAD9);

  KX_MACRO_addTypesToDict(d, PADPERIOD, SCA_IInputDevice::PADPERIOD);
  KX_MACRO_addTypesToDict(d, PADSLASHKEY, SCA_IInputDevice::PADSLASHKEY);
  KX_MACRO_addTypesToDict(d, PADASTERKEY, SCA_IInputDevice::PADASTERKEY);

  KX_MACRO_addTypesToDict(d, PAD0, SCA_IInputDevice::PAD0);
  KX_MACRO_addTypesToDict(d, PADMINUS, SCA_IInputDevice::PADMINUS);
  KX_MACRO_addTypesToDict(d, PADENTER, SCA_IInputDevice::PADENTER);
  KX_MACRO_addTypesToDict(d, PADPLUSKEY, SCA_IInputDevice::PADPLUSKEY);

  KX_MACRO_addTypesToDict(d, F1KEY, SCA_IInputDevice::F1KEY);
  KX_MACRO_addTypesToDict(d, F2KEY, SCA_IInputDevice::F2KEY);
  KX_MACRO_addTypesToDict(d, F3KEY, SCA_IInputDevice::F3KEY);
  KX_MACRO_addTypesToDict(d, F4KEY, SCA_IInputDevice::F4KEY);
  KX_MACRO_addTypesToDict(d, F5KEY, SCA_IInputDevice::F5KEY);
  KX_MACRO_addTypesToDict(d, F6KEY, SCA_IInputDevice::F6KEY);
  KX_MACRO_addTypesToDict(d, F7KEY, SCA_IInputDevice::F7KEY);
  KX_MACRO_addTypesToDict(d, F8KEY, SCA_IInputDevice::F8KEY);
  KX_MACRO_addTypesToDict(d, F9KEY, SCA_IInputDevice::F9KEY);
  KX_MACRO_addTypesToDict(d, F10KEY, SCA_IInputDevice::F10KEY);
  KX_MACRO_addTypesToDict(d, F11KEY, SCA_IInputDevice::F11KEY);
  KX_MACRO_addTypesToDict(d, F12KEY, SCA_IInputDevice::F12KEY);
  KX_MACRO_addTypesToDict(d, F13KEY, SCA_IInputDevice::F13KEY);
  KX_MACRO_addTypesToDict(d, F14KEY, SCA_IInputDevice::F14KEY);
  KX_MACRO_addTypesToDict(d, F15KEY, SCA_IInputDevice::F15KEY);
  KX_MACRO_addTypesToDict(d, F16KEY, SCA_IInputDevice::F16KEY);
  KX_MACRO_addTypesToDict(d, F17KEY, SCA_IInputDevice::F17KEY);
  KX_MACRO_addTypesToDict(d, F18KEY, SCA_IInputDevice::F18KEY);
  KX_MACRO_addTypesToDict(d, F19KEY, SCA_IInputDevice::F19KEY);

  KX_MACRO_addTypesToDict(d, OSKEY, SCA_IInputDevice::OSKEY);

  KX_MACRO_addTypesToDict(d, PAUSEKEY, SCA_IInputDevice::PAUSEKEY);
  KX_MACRO_addTypesToDict(d, INSERTKEY, SCA_IInputDevice::INSERTKEY);
  KX_MACRO_addTypesToDict(d, HOMEKEY, SCA_IInputDevice::HOMEKEY);
  KX_MACRO_addTypesToDict(d, PAGEUPKEY, SCA_IInputDevice::PAGEUPKEY);
  KX_MACRO_addTypesToDict(d, PAGEDOWNKEY, SCA_IInputDevice::PAGEDOWNKEY);
  KX_MACRO_addTypesToDict(d, ENDKEY, SCA_IInputDevice::ENDKEY);

  // MOUSE
  KX_MACRO_addTypesToDict(d, LEFTMOUSE, SCA_IInputDevice::LEFTMOUSE);
  KX_MACRO_addTypesToDict(d, MIDDLEMOUSE, SCA_IInputDevice::MIDDLEMOUSE);
  KX_MACRO_addTypesToDict(d, RIGHTMOUSE, SCA_IInputDevice::RIGHTMOUSE);
  KX_MACRO_addTypesToDict(d, BUTTON4MOUSE, SCA_IInputDevice::BUTTON4MOUSE);
  KX_MACRO_addTypesToDict(d, BUTTON5MOUSE, SCA_IInputDevice::BUTTON5MOUSE);
  KX_MACRO_addTypesToDict(d, BUTTON6MOUSE, SCA_IInputDevice::BUTTON6MOUSE);
  KX_MACRO_addTypesToDict(d, BUTTON7MOUSE, SCA_IInputDevice::BUTTON7MOUSE);
  KX_MACRO_addTypesToDict(d, WHEELUPMOUSE, SCA_IInputDevice::WHEELUPMOUSE);
  KX_MACRO_addTypesToDict(d, WHEELDOWNMOUSE, SCA_IInputDevice::WHEELDOWNMOUSE);
  KX_MACRO_addTypesToDict(d, MOUSEX, SCA_IInputDevice::MOUSEX);
  KX_MACRO_addTypesToDict(d, MOUSEY, SCA_IInputDevice::MOUSEY);

  // Check for errors
  if (PyErr_Occurred()) {
    Py_FatalError("can't initialize module GameKeys");
  }

  return m;
}

/* ------------------------------------------------------------------------- */
/* Application: application values that remain unchanged during runtime       */
/* ------------------------------------------------------------------------- */

PyDoc_STRVAR(Application_module_documentation,
             "This module contains application values that remain unchanged during runtime.");

static struct PyModuleDef Application_module_def = {
    PyModuleDef_HEAD_INIT,
    "bge.app",                        /* m_name */
    Application_module_documentation, /* m_doc */
    0,                                /* m_size */
    nullptr,                          /* m_methods */
    0,                                /* m_reload */
    0,                                /* m_traverse */
    0,                                /* m_clear */
    0,                                /* m_free */
};

PyMODINIT_FUNC initApplicationPythonBinding()
{
  PyObject *m;
  PyObject *d;

  m = PyModule_Create(&Application_module_def);

  // Add some symbolic constants to the module
  d = PyModule_GetDict(m);

  PyDict_SetItemString(
      d,
      "version",
      Py_BuildValue("(iii)", BLENDER_VERSION / 100, BLENDER_VERSION % 100, BLENDER_VERSION_PATCH));
  PyDict_SetItemString(d,
                       "version_string",
                       PyUnicode_FromFormat("%d.%02d (sub %d)",
                                            BLENDER_VERSION / 100,
                                            BLENDER_VERSION % 100,
                                            BLENDER_VERSION_PATCH));

  PyDict_SetItemString(d,
                       "has_texture_ffmpeg",
#  ifdef WITH_FFMPEG
                       Py_True
#  else
                       Py_False
#  endif
  );
  PyDict_SetItemString(d,
                       "has_joystick",
#  ifdef WITH_SDL
                       Py_True
#  else
                       Py_False
#  endif
  );
  PyDict_SetItemString(d,
                       "has_physics",
#  ifdef WITH_BULLET
                       Py_True
#  else
                       Py_False
#  endif
  );

  // Check for errors
  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
  }

  return m;
}

// utility function for loading and saving the globalDict
void saveGamePythonConfig()
{
  char *marshal_buffer = nullptr;
  int marshal_length = 0;
  PyObject *gameLogic = PyImport_ImportModule("GameLogic");
  if (gameLogic) {
    PyObject *pyGlobalDict = PyDict_GetItemString(PyModule_GetDict(gameLogic),
                                                  "globalDict");  // Same as importing the module
    if (pyGlobalDict) {
#  ifdef Py_MARSHAL_VERSION
      PyObject *pyGlobalDictMarshal = PyMarshal_WriteObjectToString(
          pyGlobalDict, 2);  // Py_MARSHAL_VERSION == 2 as of Py2.5
#  else
      PyObject *pyGlobalDictMarshal = PyMarshal_WriteObjectToString(pyGlobalDict);
#  endif
      if (pyGlobalDictMarshal) {
        // for testing only
        // PyObject_Print(pyGlobalDictMarshal, stderr, 0);
        char *marshal_cstring;

        marshal_cstring = PyBytes_AsString(pyGlobalDictMarshal);  // py3 uses byte arrays
        marshal_length = PyBytes_Size(pyGlobalDictMarshal);
        marshal_buffer = new char[marshal_length + 1];
        memcpy(marshal_buffer, marshal_cstring, marshal_length);
        Py_DECREF(pyGlobalDictMarshal);
      }
      else {
        CM_Error("bge.logic.globalDict could not be marshal'd");
      }
    }
    else {
      CM_Error("bge.logic.globalDict was removed");
    }
    Py_DECREF(gameLogic);
  }
  else {
    PyErr_Clear();
    CM_Error("bge.logic failed to import bge.logic.globalDict will be lost");
  }

  std::string marshal_path = pathGamePythonConfig();

  if (marshal_length && marshal_buffer) {
    FILE *fp = fopen(marshal_path.c_str(), "wb");

    if (fp) {
      if (fwrite(marshal_buffer, 1, marshal_length, fp) != marshal_length) {
        CM_Error("could not write marshal data");
      }

      fclose(fp);
    }
    else {
      CM_Error("could not open marshal file");
    }
  }
  else {
    CM_Error("could not create marshal buffer");
  }

  if (marshal_buffer) {
    delete[] marshal_buffer;
  }
}

void loadGamePythonConfig()
{
  std::string marshal_path = pathGamePythonConfig();

  FILE *fp = fopen(marshal_path.c_str(), "rb");

  if (fp) {
    // obtain file size:
    fseek(fp, 0, SEEK_END);
    size_t marshal_length = ftell(fp);
    if (marshal_length == -1) {
      CM_Error("could not read position of '" << marshal_path << "'");
      fclose(fp);
      return;
    }
    rewind(fp);

    char *marshal_buffer = (char *)malloc(sizeof(char) * marshal_length);

    int result = fread(marshal_buffer, 1, marshal_length, fp);

    if (result == marshal_length) {
      /* Restore the dict */
      PyObject *gameLogic = PyImport_ImportModule("GameLogic");

      if (gameLogic) {
        PyObject *pyGlobalDict = PyMarshal_ReadObjectFromString(marshal_buffer, marshal_length);
        if (pyGlobalDict) {
          PyObject *pyGlobalDict_orig = PyDict_GetItemString(
              PyModule_GetDict(gameLogic), "globalDict");  // Same as importing the module.
          if (pyGlobalDict_orig) {
            PyDict_Clear(pyGlobalDict_orig);
            PyDict_Update(pyGlobalDict_orig, pyGlobalDict);
          }
          else {
            /* this should not happen, but cant find the original globalDict, just assign it then
             */
            PyDict_SetItemString(PyModule_GetDict(gameLogic),
                                 "globalDict",
                                 pyGlobalDict);  // Same as importing the module.
          }
          Py_DECREF(gameLogic);
          Py_DECREF(pyGlobalDict);
        }
        else {
          Py_DECREF(gameLogic);
          PyErr_Clear();
          CM_Error("could not marshall string");
        }
      }
      else {
        PyErr_Clear();
        CM_Error("bge.logic failed to import bge.logic.globalDict will be lost");
      }
    }
    else {
      CM_Error("could not read all of '" << marshal_path << "'");
    }

    free(marshal_buffer);
    fclose(fp);
  }
  else {
    CM_Error("could not open '" << marshal_path << "'");
  }
}

std::string pathGamePythonConfig()
{
  std::string path = KX_GetOrigPath();
  int len = path.size();

  /* replace extension */
  if (BLI_path_extension_check_n(path.c_str(), ".blend")) {
    path = path.substr(0, len - 6) + std::string(".bgeconf");
  }
  else {
    path += std::string(".bgeconf");
  }

  return path;
}

#endif  // WITH_PYTHON
