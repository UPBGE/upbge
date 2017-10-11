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
#include "EXP_Attribute.h"

#include <string>
#include <initializer_list>

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
 * each PyC++ class must be registered in KX_PythonInitTypes.cpp
 */
#define Py_Header(Class) \
public: \
	using SelfType = Class; \
	static PyTypeObject Type; \
	static PyMethodDef Methods[]; \
	static EXP_Attribute Attributes[]; \
	virtual PyTypeObject *GetType(void) \
	{ \
		return &Type; \
	} \
	virtual PyObject *GetProxy() \
	{ \
		return GetProxyPlus_Ext(&Type); \
	} \
	virtual PyObject *NewProxy(bool py_owns) \
	{ \
		return NewProxyPlus_Ext(&Type, py_owns); \
	}

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

#include "EXP_AttributeDef.h"

/*------------------------------
 * EXP_PyObjectPlus
 *------------------------------ */
#else  // WITH_PYTHON

#define Py_Header \
public: \

#endif

/// The EXP_PyObjectPlus abstract class.
class EXP_PyObjectPlus
{
	Py_Header(EXP_PyObjectPlus) // Always start with Py_Header

protected:
	/** Called when the object is freed from a python owner proxy.
	 * It has effect to use reference count for deletion and to not
	 * be every time deleted in EXP_Value.
	 */
	virtual void DestructFromPython();

	static bool m_ignore_deprecation_warnings;

#ifdef WITH_PYTHON
	PyObject *m_proxy; // Actually a EXP_PyObjectPlus_Proxy.
#endif  // WITH_PYTHON

public:
	EXP_PyObjectPlus();
	virtual ~EXP_PyObjectPlus();

#ifdef WITH_PYTHON

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

	PyObject *GetProxyPlus_Ext(PyTypeObject *tp);
	PyObject *NewProxyPlus_Ext(PyTypeObject *tp, bool py_owns);

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
