#include "EXP_Attribute.h"
#include "EXP_PyObjectPlus.h"

#include "BLI_math_vector.h"

EXP_Attribute::EXP_Attribute()
	:m_getter(nullptr),
	m_setter(nullptr)
{
}

EXP_Attribute::EXP_Attribute(const std::string& name, GetterFunction getter, SetterFunction setter, const std::array<float, 2>& range)
	:m_name(name),
	m_getter(getter),
	m_setter(setter),
	m_lower(range[0]),
	m_upper(range[1])
{
}

bool EXP_Attribute::IsValid(EXP_PyObjectPlus *self)
{
	if (!self || !self->py_is_valid()) {
		PyErr_SetString(PyExc_SystemError, EXP_PROXY_ERROR_MSG);
		return false;
	}

	return true;
}

void EXP_Attribute::PrintError(const std::string& msg) const
{
	PyErr_Format(PyExc_AttributeError, "%s%s", m_name.c_str(), msg.c_str());
}
