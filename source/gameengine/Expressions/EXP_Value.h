/*
 * Value.h: interface for the EXP_Value class.
 * Copyright (c) 1996-2000 Erwin Coumans <coockie@acm.org>
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Erwin Coumans makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 */

/** \file EXP_Value.h
 *  \ingroup expressions
 */

#ifndef __EXP_VALUE_H__
#define __EXP_VALUE_H__

#include "EXP_PyObjectPlus.h"

#ifdef WITH_PYTHON
#  include "object.h"
#endif

#include <map>
#include <vector>
#include <string>

/// Property class base.
class EXP_PropValue
{
public:
	enum DATA_TYPE {
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
	virtual DATA_TYPE GetValueType() const = 0;

	virtual EXP_PropValue *GetReplica() = 0;

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython() = 0;
#endif  // WITH_PYTHON
};

/**
 * Baseclass EXP_Value
 *
 * Base class for all editor functionality, flexible object type that allows
 * calculations and uses reference counting for memory management.
 *
 * Features:
 * - Property system (SetProperty() / GetProperty() / FindIdentifier())
 * - Replication (GetReplica())
 *
 */
class EXP_Value : public EXP_PyObjectPlus
{
	Py_Header
public:
	EXP_Value();
	EXP_Value(const EXP_Value& other);
	virtual ~EXP_Value();

	EXP_Value& operator=(const EXP_Value& other) = delete;
	EXP_Value& operator=(EXP_Value&& other) = default;

#ifdef WITH_PYTHON
	virtual PyObject *py_repr(void)
	{
		return PyUnicode_FromStdString(GetText());
	}

	static PyObject *pyattr_get_name(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef);

	virtual PyObject *ConvertKeysToPython();
#endif  // WITH_PYTHON

	/// Property Management
	/** Set property <ioProperty>, overwrites and releases a previous property with the same name if needed.
	 * Stall the owning of the property.
	 */
	void SetProperty(const std::string& name, EXP_PropValue *ioProperty);
	/// Get pointer to a property with name <inName>, returns nullptr if there is no property named <inName>.
	EXP_PropValue *GetProperty(const std::string & inName) const;
	/// Remove the property named <inName>, returns true if the property was succesfully removed, false if property was not found or could not be removed.
	bool RemoveProperty(const std::string& inName);
	std::vector<std::string> GetPropertyNames() const;
	/// Clear all properties.
	void ClearProperties();

	// TODO to remove in the same time timer management is refactored.
	/// Get property number <inIndex>.
	EXP_PropValue *GetProperty(int inIndex) const;
	/// Get the amount of properties assiocated with this value.
	int GetPropertyCount();

	virtual std::string GetText() const;

	/// Retrieve the name of the value.
	virtual std::string GetName() const = 0;
	/// Set the name of the value.
	virtual void SetName(const std::string& name);

	virtual EXP_Value *GetReplica();
	virtual void ProcessReplica();

protected:
	virtual void DestructFromPython();

private:
	/// Properties for user/game etc.
	std::map<std::string, std::unique_ptr<EXP_PropValue> > m_properties;
};

#endif  // __EXP_VALUE_H__
