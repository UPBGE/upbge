#ifndef __EXP_ATTRIBUTE_H__
#define __EXP_ATTRIBUTE_H__

#include <string>
#include <sstream>
#include "mathfu.h"
#include "EXP_Python.h"

class EXP_PyObjectPlus;

class EXP_Attribute
{
public:
	using GetterFunction = PyObject *(*)(PyObject *, void *);
	using SetterFunction = int (*)(PyObject *, PyObject *, void *);

	/// Name of the python attribute with class, class.name.
	std::string m_name;

	GetterFunction m_getter;
	SetterFunction m_setter;

	float m_lower;
	float m_upper;

	EXP_Attribute();

	EXP_Attribute(const std::string& name, GetterFunction getter, SetterFunction setter, const std::array<float, 2>& range);

	static bool IsValid(EXP_PyObjectPlus *self);

	template <typename Type>
	void PrintSetterError() const;
	template <typename Type>
	void PrintLimitError(const Type& value, const Type& Lower, const Type& Upper) const
	{
		std::stringstream stream;
		stream << " : Value (" << value << ") out of range [" << Lower << ", " << Upper << "]";
		PrintError(stream.str());
	}

	void PrintError(const std::string& msg) const;
};

template <>
inline void EXP_Attribute::PrintSetterError<bool>() const
{
	PrintError(" = bool: Excepted a boolean.");
}

template <>
inline void EXP_Attribute::PrintSetterError<int>() const
{
	PrintError(" = int: Excepted a int.");
}

template <>
inline void EXP_Attribute::PrintSetterError<unsigned int>() const
{
	PrintError(" = int: Excepted a int.");
}

template <>
inline void EXP_Attribute::PrintSetterError<short>() const
{
	PrintError(" = int: Excepted a int.");
}

template <>
inline void EXP_Attribute::PrintSetterError<unsigned short>() const
{
	PrintError(" = int: Excepted a int.");
}

template <>
inline void EXP_Attribute::PrintSetterError<float>() const
{
	PrintError(" = float: Excepted a float.");
}

template <>
inline void EXP_Attribute::PrintSetterError<std::string>() const
{
	PrintError(" = str: Excepted a string.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec2>() const
{
	PrintError(" = Vector: Excepted a 2d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec3>() const
{
	PrintError(" = Vector: Excepted a 3d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec4>() const
{
	PrintError(" = Vector: Excepted a 4d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec2_packed>() const
{
	PrintError(" = Vector: Excepted a 2d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec3_packed>() const
{
	PrintError(" = Vector: Excepted a 3d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::vec4_packed>() const
{
	PrintError(" = Vector: Excepted a 4d vector.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::mat3>() const
{
	PrintError(" = Vector: Excepted a 3x3 matrix.");
}

template <>
inline void EXP_Attribute::PrintSetterError<mt::mat4>() const
{
	PrintError(" = Vector: Excepted a 4x4 matrix.");
}

template <>
inline void EXP_Attribute::PrintSetterError<PyObject *>() const
{
}

#endif  // __EXP_ATTRIBUTE_H__
