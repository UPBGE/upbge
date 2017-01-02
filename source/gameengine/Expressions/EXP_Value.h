/*
 * Value.h: interface for the CValue class.
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

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include <map>		// array functionality for the propertylist
#include <vector>
#include <string>	// std::string class

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

#ifndef GEN_NO_ASSERT
#undef  assert
#define	assert(exp)			((void)NULL)
#endif


#ifndef GEN_NO_TRACE
#undef  trace
#define	trace(exp)			((void)NULL)
#endif

#ifndef GEN_NO_DEBUG
#undef  debug
#define	debug(exp)			((void)NULL)
#endif

#ifndef GEN_NO_ASSERTD
#undef  BLI_assert
#define	BLI_assert(exp)			((void)NULL)
#endif

enum VALUE_OPERATOR {
	
	VALUE_MOD_OPERATOR,			// %
	VALUE_ADD_OPERATOR,			// +
	VALUE_SUB_OPERATOR,			// -
	VALUE_MUL_OPERATOR,			// *
	VALUE_DIV_OPERATOR,			// /
	VALUE_NEG_OPERATOR,			// -
	VALUE_POS_OPERATOR,			// +
	VALUE_AND_OPERATOR,			// &&
	VALUE_OR_OPERATOR,			// ||
	VALUE_EQL_OPERATOR,			// ==
	VALUE_NEQ_OPERATOR,			// !=
	VALUE_GRE_OPERATOR,			// >
	VALUE_LES_OPERATOR,			// <
	VALUE_GEQ_OPERATOR,			// >=
	VALUE_LEQ_OPERATOR,			// <=
	VALUE_NOT_OPERATOR,			// !
	VALUE_NO_OPERATOR			// no operation at all
};

enum VALUE_DATA_TYPE {
	VALUE_NO_TYPE,				// abstract baseclass
	VALUE_INT_TYPE,
	VALUE_FLOAT_TYPE,
	VALUE_STRING_TYPE,
	VALUE_BOOL_TYPE,
	VALUE_ERROR_TYPE,
	VALUE_EMPTY_TYPE,
	VALUE_LIST_TYPE,
	VALUE_VOID_TYPE,
	VALUE_VECTOR_TYPE,
	VALUE_MAX_TYPE				//only here to provide number of types
};

#include "EXP_PyObjectPlus.h"
#ifdef WITH_PYTHON
#include "object.h"
#endif

/**
 * Baseclass CValue
 *
 * Together with CExpression, CValue and it's derived classes can be used to
 * parse expressions into a parsetree with error detecting/correcting capabilities
 * also expandable by a CFactory pluginsystem 
 *
 * Base class for all editor functionality, flexible object type that allows
 * calculations and uses reference counting for memory management.
 * 
 * Features:
 * - Reference Counting (AddRef() / Release())
 * - Calculations (Calc() / CalcFinal())
 * - Property system (SetProperty() / GetProperty() / FindIdentifier())
 * - Replication (GetReplica())
 * - Flags (IsError())
 * 
 * - Some small editor-specific things added
 * - A helperclass CompressorArchive handles the serialization
 * 
 */
class CValue  : public PyObjectPlus

{
Py_Header
public:
	// Construction / Destruction
	CValue();

#ifdef WITH_PYTHON
	//static PyObject *PyMake(PyObject *, PyObject *);
	virtual PyObject *py_repr(void)
	{
		return PyUnicode_FromStdString(GetText());
	}

	virtual PyObject *ConvertValueToPython() {
		return NULL;
	}

	virtual CValue *ConvertPythonToValue(PyObject *pyobj, const bool do_type_exception, const char *error_prefix);
	
	static PyObject *pyattr_get_name(void *self, const KX_PYATTRIBUTE_DEF *attrdef);
	
	virtual PyObject *ConvertKeysToPython( void );
#endif  /* WITH_PYTHON */

	
	
	// Expression Calculation
	virtual CValue*		Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue*		CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);

	/// Reference Counting
	int GetRefCount()
	{ 
		return m_refcount; 
	}

	// Add a reference to this value
	CValue *AddRef()
	{
		// Increase global reference count, used to see at the end of the program
		// if all CValue-derived classes have been dereferenced to 0
		m_refcount++; 
		return this;
	}

	// Release a reference to this value (when reference count reaches 0, the value is removed from the heap)
	int			Release()
	{
		// Decrease global reference count, used to see at the end of the program
		// if all CValue-derived classes have been dereferenced to 0
		// Decrease local reference count, if it reaches 0 the object should be freed
		if (--m_refcount > 0)
		{
			// Reference count normal, return new reference count
			return m_refcount;
		}
		else
		{
			// Reference count reached 0, delete ourselves and return 0
	//		BLI_assert(m_refcount==0, "Reference count reached sub-zero, object released too much");
			
			delete this;
			return 0;
		}
	}


	/// Property Management
	virtual void		SetProperty(const std::string& name,CValue* ioProperty);						// Set property <ioProperty>, overwrites and releases a previous property with the same name if needed
	virtual CValue*		GetProperty(const std::string & inName);
	const std::string GetPropertyText(const std::string & inName);						// Get text description of property with name <inName>, returns an empty string if there is no property named <inName>
	float				GetPropertyNumber(const std::string& inName,float defnumber);
	virtual bool		RemoveProperty(const std::string& inName);						// Remove the property named <inName>, returns true if the property was succesfully removed, false if property was not found or could not be removed
	virtual std::vector<std::string>	GetPropertyNames();
	virtual void		ClearProperties();										// Clear all properties

	virtual CValue*		GetProperty(int inIndex);								// Get property number <inIndex>
	virtual int			GetPropertyCount();										// Get the amount of properties assiocated with this value

	virtual CValue*		FindIdentifier(const std::string& identifiername);

	virtual const std::string GetText();
	virtual double		GetNumber();
	virtual int			GetValueType();												// Get Prop value type

	virtual std::string GetName() = 0;											// Retrieve the name of the value
	virtual void		SetName(const std::string& name);								// Set the name of the value
	/** Sets the value to this cvalue.
	 * \attention this particular function should never be called. Why not abstract? */
	virtual void		SetValue(CValue* newval);
	virtual CValue*		GetReplica();
	virtual void			ProcessReplica();
	//virtual CValue*		Copy() = 0;
	
	std::string				op2str(VALUE_OPERATOR op);

	inline void			SetError(bool err)										{ m_error=err; }
	inline bool			IsError()												{ return m_error; }

protected:
	virtual void DestructFromPython();

	virtual				~CValue();
private:
	// Member variables
	std::map<std::string,CValue*>*		m_pNamedPropertyArray;									// Properties for user/game etc
	bool m_error;
	int					m_refcount;												// Reference Counter
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// CPropValue is a CValue derived class, that implements the identification (String name)
// SetName() / GetName(), 
// normal classes should derive from CPropValue, real lightweight classes straight from CValue


class CPropValue : public CValue
{
public:
	CPropValue() :
	  CValue(),
		m_strNewName()

	{
	}
	
	virtual ~CPropValue()
	{
	}
	
	virtual void			SetName(const std::string& name) {
		m_strNewName = name;
	}
	
	virtual std::string GetName() {
		//std::string namefromprop = GetPropertyText("Name");
		//if (namefromprop.size() > 0)
		//	return namefromprop;
		return m_strNewName;
	}						// name of Value
	
protected:
	std::string					m_strNewName;				    // Identification

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:CPropValue")
#endif
};

#endif  /* __EXP_VALUE_H__ */
