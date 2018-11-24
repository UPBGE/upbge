/** \file gameengine/Expressions/EXP_Dictionary.cpp
 *  \ingroup expressions
 */

#include "EXP_Dictionary.h"

EXP_Dictionary::EXP_Dictionary(const EXP_Dictionary& other)
	:EXP_Value(other)
{
	for (const auto& pair : other.m_properties) {
		m_properties.emplace(pair.first, pair.second->GetReplica());
	}
}

void EXP_Dictionary::SetProperty(const std::string& name, EXP_PropValue *prop)
{
	// Try to replace property.
	const auto& it = m_properties.find(name);
	if (it != m_properties.end()) {
		it->second.reset(prop);
	}
	// Add property into the array.
	else {
		m_properties.emplace(name, prop);
	}
}

EXP_PropValue *EXP_Dictionary::GetProperty(const std::string& name) const
{
	const auto& it = m_properties.find(name);
	if (it != m_properties.end()) {
		return it->second.get();
	}
	return nullptr;
}

bool EXP_Dictionary::RemoveProperty(const std::string& name)
{
	const auto& it = m_properties.find(name);
	if (it != m_properties.end()) {
		m_properties.erase(it);
		return true;
	}

	return false;
}

std::vector<std::string> EXP_Dictionary::GetPropertyNames() const
{
	const unsigned short size = m_properties.size();
	std::vector<std::string> result(size);

	unsigned short i = 0;
	for (const auto& pair : m_properties) {
		result[i++] = pair.first;
	}
	return result;
}

void EXP_Dictionary::ClearProperties()
{
	m_properties.clear();
}

EXP_PropValue *EXP_Dictionary::GetProperty(unsigned short inIndex) const
{
	unsigned short count = 0;
	for (const auto& pair : m_properties) {
		if (count++ == inIndex) {
			return pair.second.get();
		}
	}
	return nullptr;
}

unsigned short EXP_Dictionary::GetPropertyCount() const
{
	return m_properties.size();
}

bool EXP_Dictionary::FindPropertyRegex(const std::regex& regex) const
{
	for (const auto& pair : m_properties) {
		if (std::regex_match(pair.first, regex)) {
			return true;
		}
	}

	return false;
}

bool EXP_Dictionary::IsDictionary() const
{
	return true;
}

#ifdef WITH_PYTHON

PyObject *EXP_Dictionary::ConvertKeysToPython()
{
	PyObject *pylist = PyList_New(m_properties.size());

	Py_ssize_t i = 0;
	for (const auto& pair : m_properties) {
		PyList_SET_ITEM(pylist, i++, PyUnicode_FromStdString(pair.first));
	}

	return pylist;
}


static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
	EXP_Dictionary *self = static_cast<EXP_Dictionary *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "value = dict[key]: EXP_Dictionary, " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	const char *name = _PyUnicode_AsString(item);
	if (!name) {
		PyErr_SetString(PyExc_KeyError, "value = dict[key]: EXP_Dictionary, key must be a string");
		return nullptr;
	}

	EXP_PropValue *prop = self->GetProperty(name);

	if (!prop) {
		PyErr_Format(PyExc_KeyError, "value = dict[key]: EXP_Dictionary, key \"%s\" does not exist", name);
		return nullptr;
	}

	return prop->ConvertValueToPython();
}


static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
	EXP_Dictionary *self = static_cast<EXP_Dictionary *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "dict[key] = value: EXP_Dictionary, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	const char *name = _PyUnicode_AsString(key);
	if (!name) {
		PyErr_SetString(PyExc_KeyError, "dict[key] = value: EXP_Dictionary, key must be a string");
		return -1;
	}

	// del ob["key"]
	if (!val) {
		if (!self->RemoveProperty(name)) {
			PyErr_Format(PyExc_KeyError, "dict[key] = value: EXP_Dictionary, key \"%s\" does not exist", name);
			return -1;
		}
	}
	// ob["key"] = value
	else {
		EXP_PropValue *prop = EXP_PropValue::ConvertPythonToValue(val);
		self->SetProperty(name, prop);
	}

	return 0;
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
	EXP_Dictionary *self = static_cast<EXP_Dictionary *>EXP_PROXY_REF(self_v);

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in dict: EXP_Dictionary, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	const char *name = _PyUnicode_AsString(value);
	if (!name) {
		PyErr_SetString(PyExc_KeyError, "val in dict: EXP_Dictionary, key must be a string");
		return -1;
	}

	if (self->GetProperty(name)) {
		return 1;
	}

	return 0;
}

PyMappingMethods EXP_Dictionary::Mapping = {
	(lenfunc)nullptr,                               /*inquiry mp_length */
	(binaryfunc)Map_GetItem,        /*binaryfunc mp_subscript */
	(objobjargproc)Map_SetItem, /*objobjargproc mp_ass_subscript */
};

PySequenceMethods EXP_Dictionary::Sequence = {
	nullptr,        /* Cant set the len otherwise it can evaluate as false */
	nullptr,        /* sq_concat */
	nullptr,        /* sq_repeat */
	nullptr,        /* sq_item */
	nullptr,        /* sq_slice */
	nullptr,        /* sq_ass_item */
	nullptr,        /* sq_ass_slice */
	(objobjproc)Seq_Contains,   /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

PyTypeObject EXP_Dictionary::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"EXP_Dictionary",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&Sequence,
	&Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef EXP_Dictionary::Methods[] = {
	{"getPropertyNames", (PyCFunction)EXP_Dictionary::sPyGetPropertyNames, METH_NOARGS},
	{"get", (PyCFunction)EXP_Dictionary::sPyget, METH_VARARGS},
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef EXP_Dictionary::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *EXP_Dictionary::PyGetPropertyNames()
{
	return ConvertKeysToPython();
}

// Matches python dict.get(key, [default])
PyObject *EXP_Dictionary::Pyget(PyObject *args)
{
	PyObject *key;
	PyObject *def = Py_None;

	if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
		return nullptr;
	}

	if (PyUnicode_Check(key)) {
		EXP_PropValue *item = GetProperty(_PyUnicode_AsString(key));
		if (item) {
			return item->ConvertValueToPython();
		}
	}

	Py_INCREF(def);
	return def;
}

#endif  // WITH_PYTHON
