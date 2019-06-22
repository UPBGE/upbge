/** \file gameengine/Expressions/PropValue.cpp
 *  \ingroup expressions
 */

#include "EXP_PropValue.h"
#include "EXP_PropBool.h"
#include "EXP_PropString.h"
#include "EXP_PropInt.h"
#include "EXP_PropFloat.h"
#include "EXP_PropPython.h"

EXP_PropValue *EXP_PropValue::ConvertPythonToValue(PyObject *pyobj)
{
	// Note: Boolean check should go before Int check [#34677].
	if (PyBool_Check(pyobj)) {
		return new EXP_PropBool((bool)PyLong_AsLongLong(pyobj));
	}
	else if (PyFloat_Check(pyobj)) {
		const double tval = PyFloat_AsDouble(pyobj);
		return new EXP_PropFloat(tval);
	}
	else if (PyLong_Check(pyobj)) {
		return new EXP_PropInt(PyLong_AsLongLong(pyobj));
	}
	else if (PyUnicode_Check(pyobj)) {
		return new EXP_PropString(_PyUnicode_AsString(pyobj));
	}

	// Fall down in python property.
	return new EXP_PropPython(pyobj);
}
