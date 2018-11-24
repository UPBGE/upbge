/** \file EXP_PropValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_PROP_VALUE_H__
#define __EXP_PROP_VALUE_H__

#include "EXP_Python.h"

#include <string>

/// Property class base.
class EXP_PropValue
{
public:
	enum DataType {
		TYPE_INT,
		TYPE_FLOAT,
		TYPE_STRING,
		TYPE_BOOL,
#ifdef WITH_PYTHON
		TYPE_PYTHON,
#endif  // WITH_PYTHON
		TYPE_MAX
	};

	static EXP_PropValue *ConvertPythonToValue(PyObject *pyobj);

	virtual std::string GetText() const = 0;
	/// Get property value type.
	virtual DataType GetValueType() const = 0;

	virtual EXP_PropValue *GetReplica() = 0;

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython() = 0;
#endif  // WITH_PYTHON
};

#endif  // __EXP_PROP_VALUE_H__
