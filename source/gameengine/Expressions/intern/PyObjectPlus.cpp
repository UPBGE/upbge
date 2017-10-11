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

/** \file gameengine/Expressions/EXP_PyObjectPlus.cpp
 *  \ingroup expressions
 */


/*------------------------------
 * EXP_PyObjectPlus cpp
 *
 * C++ library routines for Crawl 3.2
 *
 * Derived from work by
 * David Redish
 * graduate student
 * Computer Science Department
 * Carnegie Mellon University (CMU)
 * Center for the Neural Basis of Cognition (CNBC)
 * http://www.python.org/doc/PyCPP.html
 *
 * ----------------------------- */

#include "EXP_PyObjectPlus.h"
#include "EXP_AttributeDef.h"

#include "CM_Message.h"

EXP_PyObjectPlus::EXP_PyObjectPlus()
{
	#ifdef WITH_PYTHON
	m_proxy = nullptr;
	#endif
}

EXP_PyObjectPlus::~EXP_PyObjectPlus()
{
#ifdef WITH_PYTHON
	InvalidateProxy();
#endif
}

void EXP_PyObjectPlus::ProcessReplica()
{
#ifdef WITH_PYTHON
	/* Clear the proxy, will be created again if needed with GetProxy()
	 * otherwise the PyObject will point to the wrong reference */
	m_proxy = nullptr;
#endif
}

/* Sometimes we might want to manually invalidate a BGE type even if
 * it hasn't been released by the BGE, say for example when an object
 * is removed from a scene, accessing it may cause problems.
 *
 * In this case the current proxy is made invalid, disowned,
 * and will raise an error on access. However if python can get access
 * to this class again it will make a new proxy and work as expected.
 */
void EXP_PyObjectPlus::InvalidateProxy()        // check typename of each parent
{
#ifdef WITH_PYTHON
	if (m_proxy) {
		EXP_PROXY_REF(m_proxy) = nullptr;
		// Decrement proxy only if python doesn't own it.
		if (!EXP_PROXY_PYOWNS(m_proxy)) {
			Py_DECREF(m_proxy);
		}
		m_proxy = nullptr;
	}
#endif
}

void EXP_PyObjectPlus::DestructFromPython()
{
#ifdef WITH_PYTHON
	delete this;
#endif
}

#ifdef WITH_PYTHON

/*------------------------------
 * EXP_PyObjectPlus Type		-- Every class, even the abstract one should have a Type
 * ----------------------------- */


PyTypeObject EXP_PyObjectPlus::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"EXP_PyObjectPlus",                 /*tp_name*/
	sizeof(EXP_PyObjectPlus_Proxy),     /*tp_basicsize*/
	0,                              /*tp_itemsize*/
	/* methods */
	py_base_dealloc,                /* tp_dealloc */
	0,                              /* printfunc tp_print; */
	0,                              /* getattrfunc tp_getattr; */
	0,                              /* setattrfunc tp_setattr; */
	0,                              /* tp_compare */ /* DEPRECATED in python 3.0! */
	py_base_repr,                   /* tp_repr */
	0, 0, 0, 0, 0, 0, 0, 0, 0,      /* Method suites for standard classes */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* long tp_flags; */
	0, 0, 0, 0,
	/* weak reference enabler */
#ifdef USE_WEAKREFS
	offsetof(EXP_PyObjectPlus_Proxy, in_weakreflist),   /* long tp_weaklistoffset; */
#else
	0,
#endif
	0, 0,
	Methods,
	0,
	0,
	nullptr // no subtype
};

/// This should be the entry in Type.
PyObject *EXP_PyObjectPlus::py_base_repr(PyObject *self)
{
	EXP_PyObjectPlus *self_plus = EXP_PROXY_REF(self);
	if (self_plus == nullptr) {
		PyErr_SetString(PyExc_SystemError, EXP_PROXY_ERROR_MSG);
		return nullptr;
	}
	return self_plus->py_repr();
}


PyObject *EXP_PyObjectPlus::py_base_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyTypeObject *base_type;

	// One or more args is needed.
	if (!PyTuple_GET_SIZE(args)) {
		PyErr_SetString(PyExc_TypeError, "Expected at least one argument");
		return nullptr;
	}

	EXP_PyObjectPlus_Proxy *base = (EXP_PyObjectPlus_Proxy *)PyTuple_GET_ITEM(args, 0);

	/**
	 * the 'base' PyObject may be subclassed (multiple times even)
	 * we need to find the first C++ defined class to check 'type'
	 * is a subclass of the base arguments type.
	 *
	 * This way we can share one tp_new function for every EXP_PyObjectPlus
	 *
	 * eg.
	 *
	 * # CustomOb is called 'type' in this C code
	 * \code{.py}
	 * class CustomOb(GameTypes.KX_GameObject):
	 *     pass
	 *
	 * # this calls py_base_new(...), the type of 'CustomOb' is checked to be a subclass of the 'cont.owner' type
	 * ob = CustomOb(cont.owner)
	 * \endcode
	 * */
	base_type = Py_TYPE(base);
	while (base_type && !EXP_PROXY_CHECK_TYPE(base_type)) {
		base_type = base_type->tp_base;
	}

	if (base_type == nullptr || !EXP_PROXY_CHECK_TYPE(base_type)) {
		PyErr_SetString(PyExc_TypeError, "can't subclass from a blender game type because the argument given is not a game class or subclass");
		return nullptr;
	}

	// Use base_type rather than Py_TYPE(base) because we could already be subtyped.
	if (!PyType_IsSubtype(type, base_type)) {
		PyErr_Format(PyExc_TypeError, "can't subclass blender game type <%s> from <%s> because it is not a subclass", base_type->tp_name, type->tp_name);
		return nullptr;
	}

	/* invalidate the existing base and return a new subclassed one,
	 * this is a bit dodgy in that it also attaches its self to the existing object
	 * which is not really 'correct' python OO but for our use its OK. */

	EXP_PyObjectPlus_Proxy *ret = (EXP_PyObjectPlus_Proxy *)type->tp_alloc(type, 0); // Starts with 1 ref, used for the return ref'.
	ret->ref = base->ref;
	ret->py_owns = base->py_owns;
	ret->py_ref = base->py_ref;

	if (ret->py_ref) {
		base->ref = nullptr; // Invalidate! disallow further access.
		if (ret->ref) {
			ret->ref->m_proxy = nullptr;
		}
		/* 'base' may be freed after this func finished but not necessarily
		 * there is no reference to the BGE data now so it will throw an error on access */
		Py_DECREF(base);
		if (ret->ref) {
			ret->ref->m_proxy = (PyObject *)ret;

			// Incref the proxy in case the python doesn't own the ref.
			if (!ret->py_owns) {
				Py_INCREF(ret);
			}
		}
	}

	return (PyObject *)ret;
}

/**
 * \param self A EXP_PyObjectPlus_Proxy
 */
void EXP_PyObjectPlus::py_base_dealloc(PyObject *self)
{
#ifdef USE_WEAKREFS
	if (EXP_PROXY_WKREF(self) != nullptr) {
		PyObject_ClearWeakRefs((PyObject *)self);
	}
#endif

	if (EXP_PROXY_PYREF(self)) {
		EXP_PyObjectPlus *self_plus = EXP_PROXY_REF(self);
		if (self_plus) {
			// Does python own this?, then delete it.
			if (EXP_PROXY_PYOWNS(self)) {
				self_plus->DestructFromPython();
			}
			EXP_PROXY_REF(self) = nullptr; // Not really needed.
		}
	}

	/* is ok normally but not for subtyping, use tp_free instead.
	 * PyObject_DEL(self);
	 */
	Py_TYPE(self)->tp_free(self);
};

/*------------------------------
* EXP_PyObjectPlus Methods  -- Every class, even the abstract one should have a Methods
   ------------------------------*/
PyMethodDef EXP_PyObjectPlus::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

#define BGE_PY_ATTR_INVALID (&(PyObjectPlus::Attributes[0]))
EXP_Attribute EXP_PyObjectPlus::Attributes[] = {
	//EXP_ATTRIBUTE_RO_CUSTOM(bool, PyObjectPlus, "invalid", pyattr_get_invalid), TODO 
	EXP_ATTRIBUTE_NULL
};

#if 0
bool EXP_PyObjectPlus::pyattr_get_invalid(const EXP_Attribute *attrdef)
{
	return PyBool_FromLong(self_v ? 0 : 1);
}
#endif

PyObject *EXP_PyObjectPlus::py_repr(void)
{
	PyErr_SetString(PyExc_SystemError, "Representation not overridden by object.");
	return nullptr;
}

bool EXP_PyObjectPlus::py_is_valid(void)
{
	return true;
}

PyObject *EXP_PyObjectPlus::GetProxyPlus_Ext(PyTypeObject *tp)
{
	if (!m_proxy) {
		m_proxy = reinterpret_cast<PyObject *>PyObject_NEW(EXP_PyObjectPlus_Proxy, tp);
		EXP_PROXY_PYOWNS(m_proxy) = false;
		EXP_PROXY_PYREF(m_proxy) = true;
#ifdef USE_WEAKREFS
		EXP_PROXY_WKREF(m_proxy) = nullptr;
#endif
	}

	EXP_PROXY_REF(m_proxy) = this; // Its possible this was set to nullptr, so set it back here.
	Py_INCREF(m_proxy); // We own one, thos ones fore the return.
	return m_proxy;
}

PyObject *EXP_PyObjectPlus::NewProxyPlus_Ext(PyTypeObject *tp, bool py_owns)
{
	if (m_proxy) {
		if (py_owns) { // Free
			EXP_PROXY_REF(m_proxy) = nullptr;
			Py_DECREF(m_proxy);
			m_proxy = nullptr;
		}
		else {
			Py_INCREF(m_proxy);
			return m_proxy;
		}

	}

	GetProxyPlus_Ext(tp);
	if (py_owns) {
		EXP_PROXY_PYOWNS(m_proxy) = py_owns;
		Py_DECREF(m_proxy); // Could avoid thrashing here but for now its ok.
	}
	return m_proxy;
}

// TODO move these functions in a separate file.
// Deprecation warning management.
bool EXP_PyObjectPlus::m_ignore_deprecation_warnings = false;

bool EXP_PyObjectPlus::GetDerprecationWarnings()
{
	return m_ignore_deprecation_warnings;
}

void EXP_PyObjectPlus::SetDeprecationWarnings(bool ignoreDeprecationWarnings)
{
	m_ignore_deprecation_warnings = ignoreDeprecationWarnings;
}

void EXP_PyObjectPlus::ShowDeprecationWarning_func(const std::string& old_way, const std::string& new_way)
{
	CM_PythonWarning("method " << old_way << " is deprecated, please use " << new_way << " instead.");
}

void EXP_PyObjectPlus::ClearDeprecationWarning()
{
	EXP_WarnLink *wlink_next;
	EXP_WarnLink *wlink = GetDeprecationWarningLinkFirst();

	while (wlink) {
		wlink->warn_done = false; // No need to nullptr the link, its cleared before adding to the list next time round.
		wlink_next = reinterpret_cast<EXP_WarnLink *>(wlink->link);
		wlink->link = nullptr;
		wlink = wlink_next;
	}
	NullDeprecationWarning();
}

static EXP_WarnLink *m_base_wlink_first = nullptr;
static EXP_WarnLink *m_base_wlink_last = nullptr;

EXP_WarnLink *EXP_PyObjectPlus::GetDeprecationWarningLinkFirst(void)
{
	return m_base_wlink_first;
}

EXP_WarnLink *EXP_PyObjectPlus::GetDeprecationWarningLinkLast(void)
{
	return m_base_wlink_last;
}

void EXP_PyObjectPlus::SetDeprecationWarningFirst(EXP_WarnLink *wlink)
{
	m_base_wlink_first = wlink;
}

void EXP_PyObjectPlus::SetDeprecationWarningLinkLast(EXP_WarnLink *wlink)
{
	m_base_wlink_last = wlink;
}

void EXP_PyObjectPlus::NullDeprecationWarning()
{
	m_base_wlink_first = m_base_wlink_last = nullptr;
}

#endif  // WITH_PYTHON
