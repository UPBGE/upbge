#ifndef __EXP_PYTHON_UTILS_H__
#define __EXP_PYTHON_UTILS_H__

#include "EXP_Python.h"

#include <vector>
#include <memory>

#include "BLI_utildefines.h"

#include "mathfu.h"

class EXP_Value;
class EXP_BaseListWrapper;

inline PyObject *PyUnicode_FromStdString(const std::string& str)
{
	return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

inline PyObject *EXP_ConvertToPython(bool ptr)
{
	return PyBool_FromLong(ptr);
}

inline PyObject *EXP_ConvertToPython(int ptr)
{
	return PyLong_FromLong(ptr);
}

inline PyObject *EXP_ConvertToPython(unsigned int ptr)
{
	return PyLong_FromLong(ptr);
}

inline PyObject *EXP_ConvertToPython(short ptr)
{
	return PyLong_FromLong(ptr);
}

inline PyObject *EXP_ConvertToPython(unsigned short ptr)
{
	return PyLong_FromLong(ptr);
}

inline PyObject *EXP_ConvertToPython(float ptr)
{
	return PyFloat_FromDouble(ptr);
}

inline PyObject *EXP_ConvertToPython(const std::string& ptr)
{
	return PyUnicode_FromStdString(ptr);
}

inline PyObject *EXP_ConvertToPython(const std::wstring& ptr)
{
	return PyUnicode_FromWideChar(ptr.c_str(), ptr.size());
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec2& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec3& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec4& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec2_packed& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec3_packed& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::vec4_packed& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::mat3& ptr)
{
	return nullptr;
}

 // TODO
inline PyObject *EXP_ConvertToPython(const mt::mat4& ptr)
{
	return nullptr;
}

PyObject *EXP_ConvertToPython(EXP_BaseListWrapper *ptr);
PyObject *EXP_ConvertToPython(EXP_Value *ptr);
PyObject *EXP_ConvertToPython(EXP_Value &ptr);

inline PyObject *EXP_ConvertToPython(PyObject *ptr)
{
	return ptr;
}

template <template <class, class ...> class List, class Item, class ... Args>
inline PyObject *EXP_ConvertToPythonListHelper(const List<Item, Args ...>& list)
{
	const unsigned int size = list.size();
	PyObject *pylist = PyList_New(size);

	unsigned int i = 0;
	for (const Item& item : list) {
		PyList_SET_ITEM(pylist, i, EXP_ConvertToPython(item));
		++i;
	}

	return pylist;
}

template <class Type>
inline PyObject *EXP_ConvertToPython(const std::vector<Type>& list)
{
	return EXP_ConvertToPythonListHelper(list);
}

template <class Type>
inline PyObject *EXP_ConvertToPython(const std::initializer_list<Type>& list)
{
	return EXP_ConvertToPythonListHelper(list);
}

template <class Type>
inline PyObject *EXP_ConvertToPython(std::unique_ptr<Type>& ptr)
{
	return EXP_ConvertToPython(ptr.get());
}


template <typename ValueType, typename PtrType>
inline bool EXP_ConvertFromPythonHelper(const ValueType value, PtrType& ptr)
{
	if (value == -1 && PyErr_Occurred()) {
		return false;
	}

	ptr = (PtrType)value;

	return true;
}


inline bool EXP_ConvertFromPython(PyObject *value, bool& ptr)
{
	return EXP_ConvertFromPythonHelper(PyObject_IsTrue(value), ptr);
}

inline bool EXP_ConvertFromPython(PyObject *value, int& ptr)
{
	return EXP_ConvertFromPythonHelper(PyLong_AsLong(value), ptr);
}

inline bool EXP_ConvertFromPython(PyObject *value, unsigned int& ptr)
{
	return EXP_ConvertFromPythonHelper(PyLong_AsLong(value), ptr);
}

inline bool EXP_ConvertFromPython(PyObject *value, short& ptr)
{
	return EXP_ConvertFromPythonHelper(PyLong_AsLong(value), ptr);
}

inline bool EXP_ConvertFromPython(PyObject *value, unsigned short& ptr)
{
	return EXP_ConvertFromPythonHelper(PyLong_AsLong(value), ptr);
}

inline bool EXP_ConvertFromPython(PyObject *value, float& ptr)
{
	return EXP_ConvertFromPythonHelper(PyFloat_AsDouble(value), ptr);
}

 // TODO
inline bool EXP_ConvertFromPython(PyObject *value, std::string& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec2& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec3& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec4& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec2_packed& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec3_packed& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::vec4_packed& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::mat3& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, mt::mat4& ptr)
{
	return false;
}

// TODO
inline bool EXP_ConvertFromPython(PyObject *value, PyObject *&ptr)
{
	ptr = value;
	return true;
}

template <class ... Args>
inline bool EXP_ParseTupleArgsAndKeywords(PyObject *pyargs, PyObject *pykwds, const char *format, std::initializer_list<const char *> keyword, Args ... args)
{
	BLI_assert((keyword.size() - 1) == (sizeof...(Args)));
	static _PyArg_Parser _parser = {format, keyword.begin(), 0};
	return _PyArg_ParseTupleAndKeywordsFast(pyargs, pykwds, &_parser, args ...);
}


#endif  // __EXP_PYTHON_UTILS_H__
