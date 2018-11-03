/*
 * Execute Python scripts
 *
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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_PythonController.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PythonController.h"
#include "SCA_LogicManager.h"
#include "SCA_ISensor.h"
#include "SCA_IActuator.h"
#include "EXP_PyObjectPlus.h"

extern "C" {
#  ifdef WITH_PYTHON
#    include "compile.h"
#    include "eval.h"
#    include "py_capi_utils.h"
#  endif  // WITH_PYTHON
}

#include "CM_Message.h"

// initialize static member variables
SCA_PythonController *SCA_PythonController::m_sCurrentController = nullptr;


SCA_PythonController::SCA_PythonController(SCA_IObject *gameobj, int mode)
	:SCA_IController(gameobj),
#ifdef WITH_PYTHON
	m_bytecode(nullptr),
	m_function(nullptr),
#endif
	m_function_argc(0),
	m_bModified(true),
	m_debug(false),
	m_mode(mode)
#ifdef WITH_PYTHON
	, m_pythondictionary(nullptr)
#endif

{

}

SCA_PythonController::~SCA_PythonController()
{

#ifdef WITH_PYTHON
	Py_XDECREF(m_bytecode);
	Py_XDECREF(m_function);

	if (m_pythondictionary) {
		// break any circular references in the dictionary
		PyDict_Clear(m_pythondictionary);
		Py_DECREF(m_pythondictionary);
	}
#endif
}



EXP_Value *SCA_PythonController::GetReplica()
{
	SCA_PythonController *replica = new SCA_PythonController(*this);

#ifdef WITH_PYTHON
	/* why is this needed at all??? - m_bytecode is nullptr'd below so this doesnt make sense
	 * but removing it crashes blender (with YoFrankie). so leave in for now - Campbell */
	Py_XINCREF(replica->m_bytecode);

	Py_XINCREF(replica->m_function); // this is ok since its not set to nullptr
	replica->m_bModified = replica->m_bytecode == nullptr;

	// The replica->m_pythondictionary is stolen - replace with a copy.
	if (m_pythondictionary) {
		replica->m_pythondictionary = PyDict_Copy(m_pythondictionary);
	}

#if 0
	// The other option is to incref the replica->m_pythondictionary -
	// the replica objects can then share data.
	if (m_pythondictionary) {
		Py_INCREF(replica->m_pythondictionary);
	}
#endif

#endif /* WITH_PYTHON */

	// this will copy properties and so on...
	replica->ProcessReplica();

	return replica;
}



void SCA_PythonController::SetScriptText(const std::string& text)
{
	m_scriptText = text;
	m_bModified = true;
}



void SCA_PythonController::SetScriptName(const std::string& name)
{
	m_scriptName = name;
}

bool SCA_PythonController::IsTriggered(class SCA_ISensor *sensor)
{
	return (std::find(m_triggeredSensors.begin(), m_triggeredSensors.end(), sensor) != m_triggeredSensors.end());
}

#ifdef WITH_PYTHON

/* warning, self is not the SCA_PythonController, its a EXP_PyObjectPlus_Proxy */
PyObject *SCA_PythonController::sPyGetCurrentController(PyObject *self)
{
	if (m_sCurrentController == nullptr) {
		PyErr_SetString(PyExc_SystemError, "bge.logic.getCurrentController(), this function is being run outside the python controllers context, or blenders internal state is corrupt.");
		return nullptr;
	}
	return m_sCurrentController->GetProxy();
}

SCA_IActuator *SCA_PythonController::LinkedActuatorFromPy(PyObject *value)
{
	// for safety, todo: only allow for registered actuators (pointertable)
	// we don't want to crash gameengine/blender by python scripts
	std::vector<SCA_IActuator *> lacts =  m_sCurrentController->GetLinkedActuators();
	std::vector<SCA_IActuator *>::iterator it;

	if (PyUnicode_Check(value)) {
		/* get the actuator from the name */
		const char *name = _PyUnicode_AsString(value);
		for (it = lacts.begin(); it != lacts.end(); ++it) {
			if (name == (*it)->GetName()) {
				return *it;
			}
		}
	}
	else if (PyObject_TypeCheck(value, &SCA_IActuator::Type)) {
		EXP_PyObjectPlus *value_plus = EXP_PROXY_REF(value);
		for (it = lacts.begin(); it != lacts.end(); ++it) {
			if (static_cast<SCA_IActuator *>(value_plus) == (*it)) {
				return *it;
			}
		}
	}

	/* set the exception */
	PyErr_Format(PyExc_ValueError,
	             "%R not in this python controllers actuator list", value);

	return nullptr;
}

const char *SCA_PythonController::sPyGetCurrentController__doc__ = "getCurrentController()";

PyTypeObject SCA_PythonController::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_PythonController",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&SCA_IController::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef SCA_PythonController::Methods[] = {
	{"activate", (PyCFunction)SCA_PythonController::sPyActivate, METH_O},
	{"deactivate", (PyCFunction)SCA_PythonController::sPyDeActivate, METH_O},
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef SCA_PythonController::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("script", SCA_PythonController, pyattr_get_script, pyattr_set_script),
	EXP_PYATTRIBUTE_INT_RO("mode", SCA_PythonController, m_mode),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

void SCA_PythonController::ErrorPrint(const char *error_msg)
{
	// If GetParent() is nullptr, then most likely the object this controller
	// was attached to is gone (e.g., removed by LibFree()). Also, GetName()
	// can be a bad pointer if GetParent() is nullptr, so better be safe and
	// flag it as unavailable as well
	CM_LogicBrickError(this, error_msg);
	PyErr_Print();

	/* Added in 2.48a, the last_traceback can reference Objects for example, increasing
	 * their user count. Not to mention holding references to wrapped data.
	 * This is especially bad when the PyObject for the wrapped data is freed, after blender
	 * has already dealocated the pointer */
	PySys_SetObject("last_traceback", nullptr);
	PyErr_Clear(); /* just to be sure */
}

bool SCA_PythonController::Compile()
{
	m_bModified = false;

	// if a script already exists, decref it before replace the pointer to a new script
	if (m_bytecode) {
		Py_DECREF(m_bytecode);
		m_bytecode = nullptr;
	}

	// recompile the scripttext into bytecode
	m_bytecode = Py_CompileString(m_scriptText.c_str(), m_scriptName.c_str(), Py_file_input);

	if (m_bytecode) {
		return true;
	}
	else {
		ErrorPrint("Python error compiling script");
		return false;
	}
}

bool SCA_PythonController::Import()
{
	m_bModified = false;

	/* in case we re-import */
	Py_XDECREF(m_function);
	m_function = nullptr;

	std::string mod_path = m_scriptText; /* just for storage, use C style string access */
	std::string function_string;

	const int pos = mod_path.rfind('.');
	if (pos != std::string::npos) {
		function_string = mod_path.substr(pos + 1);
		mod_path = mod_path.substr(0, pos);
	}

	if (function_string.empty()) {
		CM_LogicBrickError(this, "python module name formatting expected 'SomeModule.Func', got '" << m_scriptText << "'");
		return false;
	}

	// Try to get the module by name
	PyObject *mod = PyImport_GetModule(mod_path.c_str());

	if (mod == nullptr) {

		// Module not already imported, trying to import it now
		mod = PyImport_ImportModule(mod_path.c_str());
		if (mod == nullptr) {
			ErrorPrint("Python module can't be imported");
			return false;
		}

	} else {

		// Module was already imported, let's reload it
		mod = PyImport_ReloadModule(mod);
		if (mod == nullptr) {
			ErrorPrint("Python module can't be reloaded");
			return false;
		}

	}

	// Get the function object
	m_function = PyObject_GetAttrString(mod, function_string.c_str());

	// DECREF the module as we don't need it anymore
	Py_DECREF(mod);

	if (m_function == nullptr) {
		if (PyErr_Occurred()) {
			ErrorPrint("python controller found the module but could not access the function");
		}
		else {
			CM_LogicBrickError(this, "python module '" << m_scriptText << "' found but function missing");
		}
		return false;
	}

	if (!PyCallable_Check(m_function)) {
		Py_DECREF(m_function);
		m_function = nullptr;
		CM_LogicBrickError(this, "python module function '" << m_scriptText << "' not callable");
		return false;
	}

	m_function_argc = 0; /* rare cases this could be a function that isn't defined in python, assume zero args */
	if (PyFunction_Check(m_function)) {
		m_function_argc = ((PyCodeObject *)PyFunction_GET_CODE(m_function))->co_argcount;
	}

	if (m_function_argc > 1) {
		Py_DECREF(m_function);
		m_function = nullptr;
		CM_LogicBrickError(this, "python module function:\n '" << m_scriptText << "' takes " << m_function_argc
		                                                       << " args, should be zero or 1 controller arg");
		return false;
	}

	return true;
}


void SCA_PythonController::Trigger(SCA_LogicManager *logicmgr)
{
	m_sCurrentController = this;

	PyObject *excdict =      nullptr;
	PyObject *resultobj =    nullptr;

	switch (m_mode) {
		case SCA_PYEXEC_SCRIPT:
		{
			if (m_bModified) {
				if (Compile() == false) { // sets m_bModified to false
					return;
				}
			}
			if (!m_bytecode) {
				return;
			}

			/*
			 * This part here with excdict is a temporary patch
			 * to avoid python/gameengine crashes when python
			 * inadvertently holds references to game objects
			 * in global variables.
			 *
			 * The idea is always make a fresh dictionary, and
			 * destroy it right after it is used to make sure
			 * python won't hold any gameobject references.
			 *
			 * Note that the PyDict_Clear _is_ necessary before
			 * the Py_DECREF() because it is possible for the
			 * variables inside the dictionary to hold references
			 * to the dictionary (ie. generate a cycle), so we
			 * break it by hand, then DECREF (which in this case
			 * should always ensure excdict is cleared).
			 */

			if (!m_pythondictionary) {
				/* Without __file__ set the sys.argv[0] is used for the filename
				 * which ends up with lines from the blender binary being printed in the console */
				m_pythondictionary = PyDict_Copy(PyC_DefaultNameSpace(m_scriptName.c_str())); /* new reference */
			}

			excdict = PyDict_Copy(m_pythondictionary);

			resultobj = PyEval_EvalCode((PyObject *)m_bytecode, excdict, excdict);

			/* PyRun_SimpleString(m_scriptText.Ptr()); */
			break;
		}
		case SCA_PYEXEC_MODULE:
		{
			if (m_bModified || m_debug) {
				if (Import() == false) { // sets m_bModified to false
					return;
				}
			}
			if (!m_function) {
				return;
			}

			PyObject *args = nullptr;

			if (m_function_argc == 1) {
				args = PyTuple_New(1);
				PyTuple_SET_ITEM(args, 0, GetProxy());
			}

			resultobj = PyObject_CallObject(m_function, args);
			Py_XDECREF(args);
			break;
		}

	} /* end switch */


	/* Free the return value and print the error */
	if (resultobj) {
		Py_DECREF(resultobj);
	}
	else {
		ErrorPrint("Python script error");
	}

	if (excdict) { /* Only for SCA_PYEXEC_SCRIPT types */
		/* clear after PyErrPrint - seems it can be using
		 * something in this dictionary and crash? */
		// This doesn't appear to be needed anymore
		//PyDict_Clear(excdict);
		Py_DECREF(excdict);
	}

	m_triggeredSensors.clear();
	m_sCurrentController = nullptr;
}

PyObject *SCA_PythonController::PyActivate(PyObject *value)
{
	if (m_sCurrentController != this) {
		PyErr_SetString(PyExc_SystemError, "Cannot activate an actuator from a non-active controller");
		return nullptr;
	}

	SCA_IActuator *actu = LinkedActuatorFromPy(value);
	if (actu == nullptr) {
		return nullptr;
	}

	m_logicManager->AddActiveActuator((SCA_IActuator *)actu, true);
	Py_RETURN_NONE;
}

PyObject *SCA_PythonController::PyDeActivate(PyObject *value)
{
	if (m_sCurrentController != this) {
		PyErr_SetString(PyExc_SystemError, "Cannot deactivate an actuator from a non-active controller");
		return nullptr;
	}

	SCA_IActuator *actu = LinkedActuatorFromPy(value);
	if (actu == nullptr) {
		return nullptr;
	}

	m_logicManager->AddActiveActuator((SCA_IActuator *)actu, false);
	Py_RETURN_NONE;
}

PyObject *SCA_PythonController::pyattr_get_script(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	//SCA_PythonController* self = static_cast<SCA_PythonController*>(static_cast<SCA_IController*>(static_cast<SCA_ILogicBrick*>(static_cast<EXP_Value*>(static_cast<EXP_PyObjectPlus*>(self_v)))));
	// static_cast<void *>(dynamic_cast<Derived *>(obj)) - static_cast<void *>(obj)

	SCA_PythonController *self = static_cast<SCA_PythonController *>(self_v);
	return PyUnicode_FromStdString(self->m_scriptText);
}



int SCA_PythonController::pyattr_set_script(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	SCA_PythonController *self = static_cast<SCA_PythonController *>(self_v);

	const char *scriptArg = _PyUnicode_AsString(value);

	if (scriptArg == nullptr) {
		PyErr_SetString(PyExc_TypeError, "controller.script = string: Python Controller, expected a string script text");
		return PY_SET_ATTR_FAIL;
	}

	/* set scripttext sets m_bModified to true,
	 * so next time the script is needed, a reparse into byte code is done */
	self->SetScriptText(scriptArg);

	return PY_SET_ATTR_SUCCESS;
}

#else // WITH_PYTHON

void SCA_PythonController::Trigger(SCA_LogicManager *logicmgr)
{
	/* intentionally blank */
}

#endif // WITH_PYTHON

/* eof */
