/** \file gameengine/Expressions/Value.cpp
 *  \ingroup expressions
 */
// Value.cpp: implementation of the CValue class.
// developed at Eindhoven University of Technology, 1997
// by the OOPS team
//////////////////////////////////////////////////////////////////////
/*
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
#include "EXP_Value.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_ListValue.h"

#ifdef WITH_PYTHON

PyTypeObject CValue::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"CValue",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	0, 0, 0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef CValue::Methods[] = {
	{nullptr, nullptr} // Sentinel
};
#endif  // WITH_PYTHON

CValue::CValue()
	:m_pNamedPropertyArray(nullptr),
	m_error(false)
{
}

CValue::~CValue()
{
	ClearProperties();
}

std::string CValue::op2str(VALUE_OPERATOR op)
{
	std::string opmsg;
	switch (op) {
		case VALUE_MOD_OPERATOR:
		{
			opmsg = " % ";
			break;
		}
		case VALUE_ADD_OPERATOR:
		{
			opmsg = " + ";
			break;
		}
		case VALUE_SUB_OPERATOR:
		{
			opmsg = " - ";
			break;
		}
		case VALUE_MUL_OPERATOR:
		{
			opmsg = " * ";
			break;
		}
		case VALUE_DIV_OPERATOR:
		{
			opmsg = " / ";
			break;
		}
		case VALUE_NEG_OPERATOR:
		{
			opmsg = " -";
			break;
		}
		case VALUE_POS_OPERATOR:
		{
			opmsg = " +";
			break;
		}
		case VALUE_AND_OPERATOR:
		{
			opmsg = " & ";
			break;
		}
		case VALUE_OR_OPERATOR:
		{
			opmsg = " | ";
			break;
		}
		case VALUE_EQL_OPERATOR:
		{
			opmsg = " = ";
			break;
		}
		case VALUE_NEQ_OPERATOR:
		{
			opmsg = " != ";
			break;
		}
		case VALUE_NOT_OPERATOR:
		{
			opmsg = " !";
			break;
		}
		default:
		{
			opmsg = "Error in Errorhandling routine.";
			break;
		}
	}
	return opmsg;
}

//---------------------------------------------------------------------------------------------------------------------
//	Property Management
//---------------------------------------------------------------------------------------------------------------------

/// Set property <ioProperty>, overwrites and releases a previous property with the same name if needed.
void CValue::SetProperty(const std::string & name, CValue *ioProperty)
{
	// Check if somebody is setting an empty property.
	if (ioProperty == nullptr) {
		trace("Warning:trying to set empty property!");
		return;
	}

	// Try to replace property (if so -> exit as soon as we replaced it).
	if (m_pNamedPropertyArray) {
		CValue *oldval = (*m_pNamedPropertyArray)[name];
		if (oldval) {
			oldval->Release();
		}
	}
	// Make sure we have a property array.
	else {
		m_pNamedPropertyArray = new std::map<std::string, CValue *>;
	}

	// Add property at end of array.
	(*m_pNamedPropertyArray)[name] = ioProperty->AddRef();
}

/// Get pointer to a property with name <inName>, returns nullptr if there is no property named <inName>.
CValue *CValue::GetProperty(const std::string & inName)
{
	if (m_pNamedPropertyArray) {
		std::map<std::string, CValue *>::iterator it = m_pNamedPropertyArray->find(inName);
		if (it != m_pNamedPropertyArray->end()) {
			return (*it).second;
		}
	}
	return nullptr;
}

/// Get text description of property with name <inName>, returns an empty string if there is no property named <inName>.
const std::string CValue::GetPropertyText(const std::string & inName)
{
	CValue *property = GetProperty(inName);
	if (property) {
		return property->GetText();
	}
	else {
		return "";
	}
}

float CValue::GetPropertyNumber(const std::string& inName, float defnumber)
{
	CValue *property = GetProperty(inName);
	if (property) {
		return property->GetNumber();
	}
	else {
		return defnumber;
	}
}

/// Remove the property named <inName>, returns true if the property was succesfully removed, false if property was not found or could not be removed.
bool CValue::RemoveProperty(const std::string& inName)
{
	// Check if there are properties at all which can be removed.
	if (m_pNamedPropertyArray) {
		std::map<std::string, CValue *>::iterator it = m_pNamedPropertyArray->find(inName);
		if (it != m_pNamedPropertyArray->end()) {
			((*it).second)->Release();
			m_pNamedPropertyArray->erase(it);
			return true;
		}
	}

	return false;
}

/// Get Property Names.
std::vector<std::string> CValue::GetPropertyNames()
{
	std::vector<std::string> result;
	if (!m_pNamedPropertyArray) {
		return result;
	}
	result.reserve(m_pNamedPropertyArray->size());

	std::map<std::string, CValue *>::iterator it;
	for (it = m_pNamedPropertyArray->begin(); (it != m_pNamedPropertyArray->end()); it++)
	{
		result.push_back((*it).first);
	}
	return result;
}

/// Clear all properties.
void CValue::ClearProperties()
{
	// Check if we have any properties.
	if (m_pNamedPropertyArray == nullptr) {
		return;
	}

	// Remove all properties.
	std::map<std::string, CValue *>::iterator it;
	for (it = m_pNamedPropertyArray->begin(); (it != m_pNamedPropertyArray->end()); it++)
	{
		CValue *tmpval = (*it).second;
		tmpval->Release();
	}

	// Delete property array.
	delete m_pNamedPropertyArray;
	m_pNamedPropertyArray = nullptr;
}

/// Get property number <inIndex>.
CValue *CValue::GetProperty(int inIndex)
{
	int count = 0;
	CValue *result = nullptr;

	if (m_pNamedPropertyArray) {
		std::map<std::string, CValue *>::iterator it;
		for (it = m_pNamedPropertyArray->begin(); (it != m_pNamedPropertyArray->end()); it++) {
			if (count++ == inIndex) {
				result = (*it).second;
				break;
			}
		}
	}
	return result;
}

/// Get the amount of properties assiocated with this value.
int CValue::GetPropertyCount()
{
	if (m_pNamedPropertyArray) {
		return m_pNamedPropertyArray->size();
	}
	else {
		return 0;
	}
}

void CValue::DestructFromPython()
{
#ifdef WITH_PYTHON
	// Avoid decrefing freed proxy in destructor.
	m_proxy = nullptr;
	Release();
#endif  // WITH_PYTHON
}

void CValue::ProcessReplica()
{
	PyObjectPlus::ProcessReplica();

	// Copy all props.
	if (m_pNamedPropertyArray) {
		std::map<std::string, CValue *> *pOldArray = m_pNamedPropertyArray;
		m_pNamedPropertyArray = nullptr;
		std::map<std::string, CValue *>::iterator it;
		for (it = pOldArray->begin(); (it != pOldArray->end()); it++)
		{
			CValue *val = (*it).second->GetReplica();
			SetProperty((*it).first, val);
			val->Release();
		}
	}
}

int CValue::GetValueType()
{
	return VALUE_NO_TYPE;
}

CValue *CValue::FindIdentifier(const std::string& identifiername)
{
	CValue *result = nullptr;

	int pos = 0;
	// if a dot exists, explode the name into pieces to get the subcontext
	if ((pos = identifiername.find('.')) != std::string::npos) {
		const std::string rightstring = identifiername.substr(pos + 1);
		const std::string leftstring = identifiername.substr(0, pos);
		CValue *tempresult = GetProperty(leftstring);
		if (tempresult) {
			result = tempresult->FindIdentifier(rightstring);
		}
	}
	else {
		result = GetProperty(identifiername);
		if (result) {
			return result->AddRef();
		}
	}
	if (!result) {
		result = new CErrorValue(identifiername + " not found");
	}
	return result;
}

#ifdef WITH_PYTHON

PyAttributeDef CValue::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("name",  CValue, pyattr_get_name),
	KX_PYATTRIBUTE_NULL // Sentinel
};

PyObject *CValue::pyattr_get_name(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	CValue *self = static_cast<CValue *> (self_v);
	return PyUnicode_FromStdString(self->GetName());
}

/**
 * There are 2 reasons this could return nullptr
 * - unsupported type.
 * - error converting (overflow).
 *
 * \param do_type_exception Use to skip raising an exception for unknown types.
 */
CValue *CValue::ConvertPythonToValue(PyObject *pyobj, const bool do_type_exception, const char *error_prefix)
{

	CValue *vallie;
	// Note: Boolean check should go before Int check [#34677].
	if (PyBool_Check(pyobj)) {
		vallie = new CBoolValue( (bool)PyLong_AsLongLong(pyobj) );
	}
	else if (PyFloat_Check(pyobj)) {
		const double tval = PyFloat_AsDouble(pyobj);
		if (tval > (double)FLT_MAX || tval < (double)-FLT_MAX) {
			PyErr_Format(PyExc_OverflowError, "%soverflow converting from float, out of internal range", error_prefix);
			vallie = nullptr;
		}
		else {
			vallie = new CFloatValue((float)tval);
		}
	}
	else if (PyLong_Check(pyobj)) {
		vallie = new CIntValue( (cInt)PyLong_AsLongLong(pyobj) );
	}
	else if (PyUnicode_Check(pyobj)) {
		vallie = new CStringValue(_PyUnicode_AsString(pyobj), "");
	}
	// Note, don't let these get assigned to GameObject props, must check elsewhere.
	else if (PyObject_TypeCheck(pyobj, &CValue::Type)) {
		vallie = (static_cast<CValue *>(BGE_PROXY_REF(pyobj)))->AddRef();
	}
	else {
		if (do_type_exception) {
			// Return an error value from the caller.
			PyErr_Format(PyExc_TypeError, "%scould convert python value to a game engine property", error_prefix);
		}
		vallie = nullptr;
	}
	return vallie;

}

PyObject *CValue::ConvertKeysToPython(void)
{
	if (m_pNamedPropertyArray) {
		PyObject *pylist = PyList_New(m_pNamedPropertyArray->size());
		Py_ssize_t i = 0;

		std::map<std::string, CValue *>::iterator it;
		for (it = m_pNamedPropertyArray->begin(); (it != m_pNamedPropertyArray->end()); it++) {
			PyList_SET_ITEM(pylist, i++, PyUnicode_FromStdString((*it).first));
		}

		return pylist;
	}
	else {
		return PyList_New(0);
	}
}

#endif  // WITH_PYTHON

CValue *CValue::Calc(VALUE_OPERATOR op, CValue *val)
{
	return nullptr;
}

CValue *CValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return nullptr;
}

void CValue::SetValue(CValue *newval)
{
	// No one should get here.
	BLI_assert(false);
}

std::string CValue::GetText()
{
	return GetName();
}

double CValue::GetNumber()
{
	return -1.0;
}

void CValue::SetName(const std::string& name)
{
}

CValue *CValue::GetReplica()
{
	return nullptr;
}
