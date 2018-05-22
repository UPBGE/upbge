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
 */

/** \file EXP_PyObjectPlus.h
 *  \ingroup expressions
 */

#ifndef __EXP_PYOBJECTPLUS_H__
#define __EXP_PYOBJECTPLUS_H__

// For now keep weakrefs optional.
#define USE_WEAKREFS

#include "EXP_Python.h"

#include "BLI_utildefines.h"

#include <string>
#include <initializer_list>
#include <cstddef> // For offsetof.
#include "mathfu.h"

#ifdef WITH_PYTHON
#ifdef USE_MATHUTILS
extern "C" {
#  include "../../blender/python/mathutils/mathutils.h" // So we can have mathutils callbacks.
}
#endif

#define MAX_PROP_NAME 64

// Use with EXP_ShowDeprecationWarning macro.
typedef struct {
	bool warn_done;
	void *link;
} EXP_WarnLink;

#define EXP_ShowDeprecationWarning(old_way, new_way) \
	{ \
		static EXP_WarnLink wlink = {false, nullptr}; \
		if ((EXP_PyObjectPlus::GetDerprecationWarnings() || wlink.warn_done) == 0) \
		{ \
			EXP_PyObjectPlus::ShowDeprecationWarning_func(old_way, new_way); \
\
			EXP_WarnLink *wlink_last = EXP_PyObjectPlus::GetDeprecationWarningLinkLast(); \
			wlink.warn_done = true; \
			wlink.link = nullptr; \
\
			if (wlink_last) { \
				wlink_last->link = (void *)&(wlink); \
				EXP_PyObjectPlus::SetDeprecationWarningLinkLast(&(wlink)); \
			} \
			else { \
				EXP_PyObjectPlus::SetDeprecationWarningFirst(&(wlink)); \
				EXP_PyObjectPlus::SetDeprecationWarningLinkLast(&(wlink)); \
			} \
		} \
	}

typedef struct EXP_PyObjectPlus_Proxy {
	/// Required python macro.
	PyObject_HEAD
	/// Pointer to GE object, it holds a reference to this proxy.
	class EXP_PyObjectPlus *ref;
	/// Optional pointer to generic structure, the structure holds no reference to this proxy.
	void *ptr;
	/// True if the object pointed by ref should be deleted when the proxy is deleted.
	bool py_owns;
	/// True if proxy is connected to a GE object (ref is used).
	bool py_ref;
#ifdef USE_WEAKREFS
	/// Weak reference enabler.
	PyObject *in_weakreflist;
#endif
} EXP_PyObjectPlus_Proxy;

#define EXP_PROXY_ERROR_MSG "Blender Game Engine data has been freed, cannot use this python variable"
#define EXP_PROXY_REF(_self) (((EXP_PyObjectPlus_Proxy *)_self)->ref)
#define EXP_PROXY_PTR(_self) (((EXP_PyObjectPlus_Proxy *)_self)->ptr)
#define EXP_PROXY_PYOWNS(_self) (((EXP_PyObjectPlus_Proxy *)_self)->py_owns)
#define EXP_PROXY_PYREF(_self) (((EXP_PyObjectPlus_Proxy *)_self)->py_ref)
#ifdef USE_WEAKREFS
#  define EXP_PROXY_WKREF(_self) (((EXP_PyObjectPlus_Proxy *)_self)->in_weakreflist)
#endif

/// Note, sometimes we don't care what BGE type this is as long as its a proxy.
#define EXP_PROXY_CHECK_TYPE(_type) ((_type)->tp_dealloc == EXP_PyObjectPlus::py_base_dealloc)

/// Opposite of EXP_PROXY_REF.
#define EXP_PROXY_FROM_REF(_self) (((EXP_PyObjectPlus *)_self)->GetProxy())
/// Same as 'EXP_PROXY_REF' but doesn't incref.
#define EXP_PROXY_FROM_REF_BORROW(_self) _bge_proxy_from_ref_borrow(_self)


/** This must be the first line of each
 * PyC++ class
 * AttributesPtr correspond to attributes of proxy generic pointer
 * each PyC++ class must be registered in KX_PythonInitTypes.cpp
 */
#define Py_Header \
public: \
	static PyTypeObject Type; \
	static PyMethodDef Methods[]; \
	static PyAttributeDef Attributes[]; \
	virtual PyTypeObject *GetType(void) \
	{ \
		return &Type; \
	} \
	virtual PyObject *GetProxy() \
	{ \
		return GetProxyPlus_Ext(this, &Type, nullptr); \
	} \
	virtual PyObject *NewProxy(bool py_owns) \
	{ \
		return NewProxyPlus_Ext(this, &Type, nullptr, py_owns); \
	}

/** Use this macro for class that use generic pointer in proxy
 * GetProxy() and NewProxy() must be defined to set the correct pointer in the proxy.
 */
#define Py_HeaderPtr \
public: \
	static PyTypeObject Type; \
	static PyMethodDef Methods[]; \
	static PyAttributeDef Attributes[]; \
	static PyAttributeDef AttributesPtr[]; \
	virtual PyTypeObject *GetType(void) \
	{ \
		return &Type; \
	} \
	virtual PyObject *GetProxy(); \
	virtual PyObject *NewProxy(bool py_owns);

/** Nonzero values are an error for setattr
 * however because of the nested lookups we need to know if the errors
 * was because the attribute didnt exits of if there was some problem setting the value.
 */
#define PY_SET_ATTR_FAIL 1
#define PY_SET_ATTR_SUCCESS 0

/** These macros are helpful when embedding Python routines. The second
 * macro is one that also requires a documentation string
 */
#define EXP_PYMETHOD(class_name, method_name) \
	PyObject * Py##method_name(PyObject * args, PyObject * kwds); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * args, PyObject * kwds) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "() - " \
							EXP_PROXY_ERROR_MSG); \
			return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(args, kwds); \
	}

#define EXP_PYMETHOD_VARARGS(class_name, method_name) \
	PyObject * Py##method_name(PyObject * args); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * args) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "() - " \
							EXP_PROXY_ERROR_MSG); return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(args); \
	}

#define EXP_PYMETHOD_NOARGS(class_name, method_name) \
	PyObject * Py##method_name(); \
	static PyObject * \
	sPy##method_name(PyObject * self) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "() - " \
							EXP_PROXY_ERROR_MSG); return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(); \
	}

#define EXP_PYMETHOD_O(class_name, method_name) \
	PyObject * Py##method_name(PyObject * value); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * value) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "(value) - " \
							EXP_PROXY_ERROR_MSG); return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(value); \
	}

#define EXP_PYMETHOD_DOC(class_name, method_name) \
	PyObject * Py##method_name(PyObject * args, PyObject * kwds); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * args, PyObject * kwds) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "(...) - " \
							EXP_PROXY_ERROR_MSG); return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(args, kwds); \
	} \
	static const char method_name##_doc[];

#define EXP_PYMETHOD_DOC_VARARGS(class_name, method_name) \
	PyObject * Py##method_name(PyObject * args); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * args) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "(...) - " \
							EXP_PROXY_ERROR_MSG); \
			return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(args); \
	} \
	static const char method_name##_doc[];

#define EXP_PYMETHOD_DOC_O(class_name, method_name) \
	PyObject * Py##method_name(PyObject * value); \
	static PyObject * \
	sPy##method_name(PyObject * self, PyObject * value) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "(value) - " \
							EXP_PROXY_ERROR_MSG); \
			return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(value); \
	} \
	static const char method_name##_doc[];

#define EXP_PYMETHOD_DOC_NOARGS(class_name, method_name) \
	PyObject * Py##method_name(); \
	static PyObject * \
	sPy##method_name(PyObject * self) \
	{ \
		if (EXP_PROXY_REF(self) == nullptr) { \
			PyErr_SetString(PyExc_RuntimeError, \
							#class_name "." #method_name "() - " \
							EXP_PROXY_ERROR_MSG); \
			return nullptr; \
		} \
		return ((class_name *)EXP_PROXY_REF(self))->Py##method_name(); \
	} \
	static const char method_name##_doc[];

/// Method table macro (with doc).
#define EXP_PYMETHODTABLE(class_name, method_name) \
	{#method_name, (PyCFunction) class_name::sPy##method_name, METH_VARARGS, (const char *)class_name::method_name##_doc}

#define EXP_PYMETHODTABLE_O(class_name, method_name) \
	{#method_name, (PyCFunction) class_name::sPy##method_name, METH_O, (const char *)class_name::method_name##_doc}

#define EXP_PYMETHODTABLE_NOARGS(class_name, method_name) \
	{#method_name, (PyCFunction) class_name::sPy##method_name, METH_NOARGS, (const char *)class_name::method_name##_doc}

#define EXP_PYMETHODTABLE_KEYWORDS(class_name, method_name) \
	{#method_name, (PyCFunction) class_name::sPy##method_name, METH_VARARGS | METH_KEYWORDS, (const char *)class_name::method_name##_doc}

/// Function implementation macro.
#define EXP_PYMETHODDEF_DOC(class_name, method_name, doc_string) \
	const char class_name::method_name##_doc[] = doc_string; \
	PyObject *class_name::Py##method_name(PyObject * args, PyObject * kwds)

#define EXP_PYMETHODDEF_DOC_VARARGS(class_name, method_name, doc_string) \
	const char class_name::method_name##_doc[] = doc_string; \
	PyObject *class_name::Py##method_name(PyObject * args)

#define EXP_PYMETHODDEF_DOC_O(class_name, method_name, doc_string) \
	const char class_name::method_name##_doc[] = doc_string; \
	PyObject *class_name::Py##method_name(PyObject * value)

#define EXP_PYMETHODDEF_DOC_NOARGS(class_name, method_name, doc_string) \
	const char class_name::method_name##_doc[] = doc_string; \
	PyObject *class_name::Py##method_name()

/// Attribute management.
enum EXP_PYATTRIBUTE_TYPE {
	EXP_PYATTRIBUTE_TYPE_BOOL,
	EXP_PYATTRIBUTE_TYPE_ENUM,
	EXP_PYATTRIBUTE_TYPE_SHORT,
	EXP_PYATTRIBUTE_TYPE_INT,
	EXP_PYATTRIBUTE_TYPE_FLOAT,
	EXP_PYATTRIBUTE_TYPE_STRING,
	EXP_PYATTRIBUTE_TYPE_FUNCTION,
	EXP_PYATTRIBUTE_TYPE_VECTOR,
	EXP_PYATTRIBUTE_TYPE_FLAG,
	EXP_PYATTRIBUTE_TYPE_CHAR
};

enum EXP_PYATTRIBUTE_ACCESS {
	EXP_PYATTRIBUTE_RW,
	EXP_PYATTRIBUTE_RO
};

struct EXP_PYATTRIBUTE_DEF;
typedef int (*EXP_PYATTRIBUTE_CHECK_FUNCTION)(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);
typedef int (*EXP_PYATTRIBUTE_SET_FUNCTION)(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
typedef PyObject *(*EXP_PYATTRIBUTE_GET_FUNCTION)(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);

typedef struct EXP_PYATTRIBUTE_DEF {
	/// Name of the python attribute.
	const std::string m_name;
	/// Type of value.
	EXP_PYATTRIBUTE_TYPE m_type;
	/// Read/write access or read-only.
	EXP_PYATTRIBUTE_ACCESS m_access;
	/** Minimum value in case of integer attributes
	 * (for string: minimum string length, for flag: mask value, for float: matrix row size).
	 */
	int m_imin;
	/** Maximum value in case of integer attributes
	 * (for string: maximum string length, for flag: 1 if flag is negative, float: vector/matrix col size).
	 */
	int m_imax;
	/// Minimum value in case of float attributes.
	float m_fmin;
	/// Maximum value in case of float attributes.
	float m_fmax;
	/// Enforce min/max value by clamping.
	bool m_clamp;
	/// The attribute uses the proxy generic pointer, set at runtime.
	bool m_usePtr;
	/// Position of field in structure.
	size_t m_offset;
	/// Size of field for runtime verification (enum only).
	size_t m_size;
	/// Length of array, 1=simple attribute.
	size_t m_length;
	/// Static function to check the assignment, returns 0 if no error.
	EXP_PYATTRIBUTE_CHECK_FUNCTION m_checkFunction;
	/// Static function to check the assignment, returns 0 if no error.
	EXP_PYATTRIBUTE_SET_FUNCTION m_setFunction;
	/// Static function to check the assignment, returns 0 if no error.
	EXP_PYATTRIBUTE_GET_FUNCTION m_getFunction;
} PyAttributeDef;

#define EXP_PYATTRIBUTE_NULL \
	{"", EXP_PYATTRIBUTE_TYPE_BOOL, EXP_PYATTRIBUTE_RW, 0, 1, 0.f, 0.f, false, false, 0, 0, 1, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_BOOL_RW(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_BOOL, EXP_PYATTRIBUTE_RW, 0, 1, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_BOOL_RW_CHECK(name, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_BOOL, EXP_PYATTRIBUTE_RW, 0, 1, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_BOOL_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_BOOL, EXP_PYATTRIBUTE_RO, 0, 1, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}

/// Attribute points to a single bit of an integer field, attribute=true if bit is set.
#define EXP_PYATTRIBUTE_FLAG_RW(name, object, field, bit) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RW, bit, 0, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLAG_RW_CHECK(name, object, field, bit, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RW, bit, 0, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLAG_RO(name, object, field, bit) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RO, bit, 0, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}

/// Attribute points to a single bit of an integer field, attribute=true if bit is set.
#define EXP_PYATTRIBUTE_FLAG_NEGATIVE_RW(name, object, field, bit) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RW, bit, 1, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLAG_NEGATIVE_RW_CHECK(name, object, field, bit, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RW, bit, 1, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLAG_NEGATIVE_RO(name, object, field, bit) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLAG, EXP_PYATTRIBUTE_RO, bit, 1, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}

/** Enum field cannot be mapped to pointer (because we would need a pointer for each enum)
 * use field size to verify mapping at runtime only, assuming enum size is equal to int size.
 */
#define EXP_PYATTRIBUTE_ENUM_RW(name, min, max, clamp, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_ENUM, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_ENUM_RW_CHECK(name, min, max, clamp, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_ENUM, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_ENUM_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_ENUM, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_SHORT_RW(name, min, max, clamp, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_RW_CHECK(name, min, max, clamp, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_ARRAY_RW(name, min, max, clamp, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_ARRAY_RW_CHECK(name, min, max, clamp, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_ARRAY_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_SHORT_LIST_RW(name, min, max, clamp, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_LIST_RW_CHECK(name, min, max, clamp, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_SHORT_LIST_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_SHORT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_INT_RW(name, min, max, clamp, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_RW_CHECK(name, min, max, clamp, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_ARRAY_RW(name, min, max, clamp, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_ARRAY_RW_CHECK(name, min, max, clamp, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_ARRAY_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_INT_LIST_RW(name, min, max, clamp, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_LIST_RW_CHECK(name, min, max, clamp, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, length, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_INT_LIST_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_INT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}

/// Always clamp for float.
#define EXP_PYATTRIBUTE_FLOAT_RW(name, min, max, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_RW_CHECK(name, min, max, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), 0, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
/// Field must be float[n], returns a sequence.
#define EXP_PYATTRIBUTE_FLOAT_ARRAY_RW(name, min, max, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_ARRAY_RW_CHECK(name, min, max, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), 0, length, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_ARRAY_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, length, nullptr, nullptr, nullptr}
/// Field must be float[n], returns a vector.
#define EXP_PYATTRIBUTE_FLOAT_VECTOR_RW(name, min, max, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, length, min, max, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_VECTOR_RW_CHECK(name, min, max, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, 0, length, min, max, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_VECTOR_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RO, 0, length, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
/// Field must be float[n][n], returns a matrix.
#define EXP_PYATTRIBUTE_FLOAT_MATRIX_RW(name, min, max, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, length, length, min, max, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_MATRIX_RW_CHECK(name, min, max, object, field, length, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RW, length, length, min, max, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_FLOAT_MATRIX_RO(name, object, field, length) \
	{ name, EXP_PYATTRIBUTE_TYPE_FLOAT, EXP_PYATTRIBUTE_RO, length, length, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}

/// Only for std::string member.
#define EXP_PYATTRIBUTE_STRING_RW(name, min, max, clamp, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_STRING, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_STRING_RW_CHECK(name, min, max, clamp, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_STRING, EXP_PYATTRIBUTE_RW, min, max, 0.f, 0.f, clamp, false, offsetof(object, field), 0, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_STRING_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_STRING, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), 0, 1, nullptr, nullptr, nullptr}

/// Only for char [] array.
#define EXP_PYATTRIBUTE_CHAR_RW(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_CHAR, EXP_PYATTRIBUTE_RW, 0, 0, 0.f, 0.f, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_CHAR_RW_CHECK(name, object, field, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_CHAR, EXP_PYATTRIBUTE_RW, 0, 0, 0.f, 0.f, true, false, offsetof(object, field), sizeof(((object *)0)->field), 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_CHAR_RO(name, object, field) \
	{ name, EXP_PYATTRIBUTE_TYPE_CHAR, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), sizeof(((object *)0)->field), 1, nullptr, nullptr, nullptr}

/// For mt::vec3 member.
#define EXP_PYATTRIBUTE_VECTOR_RW(name, min, max, object, field, size) \
	{ name, EXP_PYATTRIBUTE_TYPE_VECTOR, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), size, 1, nullptr, nullptr, nullptr}
#define EXP_PYATTRIBUTE_VECTOR_RW_CHECK(name, min, max, clamp, object, field, size, function) \
	{ name, EXP_PYATTRIBUTE_TYPE_VECTOR, EXP_PYATTRIBUTE_RW, 0, 0, min, max, true, false, offsetof(object, field), size, 1, &object::function, nullptr, nullptr}
#define EXP_PYATTRIBUTE_VECTOR_RO(name, object, field, size) \
	{ name, EXP_PYATTRIBUTE_TYPE_VECTOR, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, offsetof(object, field), size, 1, nullptr, nullptr, nullptr}

#define EXP_PYATTRIBUTE_RW_FUNCTION(name, object, getfunction, setfunction) \
	{ name, EXP_PYATTRIBUTE_TYPE_FUNCTION, EXP_PYATTRIBUTE_RW, 0, 0, 0.f, 0.f, false, false, 0, 0, 1, nullptr, &object::setfunction, &object::getfunction}
#define EXP_PYATTRIBUTE_RO_FUNCTION(name, object, getfunction) \
	{ name, EXP_PYATTRIBUTE_TYPE_FUNCTION, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0.f, false, false, 0, 0, 1, nullptr, nullptr, &object::getfunction}
#define EXP_PYATTRIBUTE_ARRAY_RW_FUNCTION(name, object, length, getfunction, setfunction) \
	{ name, EXP_PYATTRIBUTE_TYPE_FUNCTION, EXP_PYATTRIBUTE_RW, 0, 0, 0.f, 0, f, false, false, 0, 0, length, nullptr, &object::setfunction, &object::getfunction}
#define EXP_PYATTRIBUTE_ARRAY_RO_FUNCTION(name, object, length, getfunction) \
	{ name, EXP_PYATTRIBUTE_TYPE_FUNCTION, EXP_PYATTRIBUTE_RO, 0, 0, 0.f, 0, f, false, false, 0, 0, length, nullptr, nullptr, &object::getfunction}

template <class ... Args>
inline bool EXP_ParseTupleArgsAndKeywords(PyObject *pyargs, PyObject *pykwds, const char *format, std::initializer_list<const char *> keyword, Args ... args)
{
	BLI_assert((keyword.size() - 1) == (sizeof...(Args)));
	static _PyArg_Parser _parser = {format, keyword.begin(), 0};
	return _PyArg_ParseTupleAndKeywordsFast(pyargs, pykwds, &_parser, args ...);
}

/*------------------------------
 * EXP_PyObjectPlus
 *------------------------------ */
#else  // WITH_PYTHON

#define Py_Header \
public: \

#define Py_HeaderPtr \
public: \

#endif

/// The EXP_PyObjectPlus abstract class.
class EXP_PyObjectPlus
{
	Py_Header // Always start with Py_Header

protected:
	/** Called when the object is freed from a python owner proxy.
	 * It has effect to use reference count for deletion and to not
	 * be every time deleted in EXP_Value.
	 */
	virtual void DestructFromPython();

	static bool m_ignore_deprecation_warnings;

public:
	EXP_PyObjectPlus();
	virtual ~EXP_PyObjectPlus();

#ifdef WITH_PYTHON
	PyObject *m_proxy; // Actually a EXP_PyObjectPlus_Proxy.

	/* These static functions are referenced by ALL EXP_PyObjectPlus_Proxy types
	 * they take the C++ reference from the EXP_PyObjectPlus_Proxy and call
	 * its own virtual py_repr, py_base_dealloc, etc. functions.
	 */
	static PyObject *py_base_new(PyTypeObject *type, PyObject *args, PyObject *kwds); // Allows subclassing.
	static void py_base_dealloc(PyObject *self);
	static PyObject *py_base_repr(PyObject *self);

	/* These are all virtual python methods that are defined in each class
	 * Our own fake subclassing calls these on each class, then calls the parent */
	virtual PyObject *py_repr();
	/// Subclass may overwrite this function to implement more sophisticated method of validating a proxy.
	virtual bool py_is_valid();

	static PyObject *py_get_attrdef(PyObject *self_py, const PyAttributeDef *attrdef);
	static int py_set_attrdef(PyObject *self_py, PyObject *value, const PyAttributeDef *attrdef);

	/// Kindof dumb, always returns True, the false case is checked for, before this function gets accessed.
	static PyObject *pyattr_get_invalid(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	static PyObject *GetProxyPlus_Ext(EXP_PyObjectPlus *self, PyTypeObject *tp, void *ptr);
	/** self=nullptr => proxy to generic pointer detached from GE object
	 * if py_owns is true, the memory pointed by ptr will be deleted automatically with MEM_freeN
	 * self!=nullptr=> proxy attached to GE object, ptr is optional and point to a struct from which attributes can be defined
	 * if py_owns is true, the object will be deleted automatically, ptr will NOT be deleted
	 * (assume object destructor takes care of it) */
	static PyObject *NewProxyPlus_Ext(EXP_PyObjectPlus *self, PyTypeObject *tp, void *ptr, bool py_owns);

	static EXP_WarnLink *GetDeprecationWarningLinkFirst();
	static EXP_WarnLink *GetDeprecationWarningLinkLast();
	static void SetDeprecationWarningFirst(EXP_WarnLink *wlink);
	static void SetDeprecationWarningLinkLast(EXP_WarnLink *wlink);
	static void NullDeprecationWarning();

	static bool GetDerprecationWarnings();
	/// Enable/disable display of deprecation warnings.
	static void SetDeprecationWarnings(bool ignoreDeprecationWarnings);
	/// Shows a deprecation warning.
	static void ShowDeprecationWarning_func(const std::string& old_way, const std::string& new_way);
	static void ClearDeprecationWarning();

#endif

	void InvalidateProxy();

	/// Makes sure any internal data owned by this class is deep copied.
	virtual void ProcessReplica();
};

#ifdef WITH_PYTHON
PyObject *PyUnicode_FromStdString(const std::string& str);

inline PyObject *_bge_proxy_from_ref_borrow(EXP_PyObjectPlus *self_v)
{
	PyObject *self_proxy = EXP_PROXY_FROM_REF(self_v);
	/* this is typically _very_ bad practice,
	 * however we know the proxy is owned by 'self_v' */
	self_proxy->ob_refcnt--;
	return self_proxy;
}

#endif

#endif  // __EXP_PYOBJECTPLUS_H__
