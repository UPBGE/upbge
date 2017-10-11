#ifndef __EXP_ATTRIBUTE_DEF_H__
#define __EXP_ATTRIBUTE_DEF_H__

#include <string>

#include "BLI_utildefines.h"

#include "EXP_PyObjectPlus.h"
#include "EXP_Attribute.h"
#include "EXP_PythonUtils.h"

enum EXP_GetSetFlags
{
	EXP_GETSET_NONE = 0,
	EXP_GETSET_READONLY = (1 << 0),
	EXP_GETSET_RANGE = (1 << 1),
	EXP_GETSET_CLAMP = (1 << 2)
};

template <class>
struct EXP_AttributeFunctionTrait;
template <class>
struct EXP_AttributeFunctionSetterTrait;

template <class _First, class ... Args>
struct EXP_AttributeFunctionFirstArgument
{
	using FirstType = _First;
};

/** Struct used to identify the return type, object and the number of arguments*
 * of a pointer to a member getter function.
 */
template <class _ReturnType, class _Object, class ... _Args>
struct EXP_AttributeFunctionTrait<_ReturnType (_Object::*)(_Args ...)>
{
	using ReturnType = _ReturnType;
	using Signature = _ReturnType (_Object::*)(_Args ...);
	enum {
		ARGS_COUNT = sizeof ... (_Args)
	};
};

struct EXP_AttributeFunctionTraitDummy
{
	using ReturnType = bool;
	using Signature = bool *;
	enum {
		ARGS_COUNT = -1
	};
};

/** Struct used to identify the return type, object and the number of arguments
 * of a pointer to a member function.
 */
template <class _ReturnType, class _Object, class ... _Args>
struct EXP_AttributeFunctionSetterTrait<_ReturnType (_Object::*)(_Args ...)> : public EXP_AttributeFunctionTrait<_ReturnType (_Object::*)(_Args ...)>
{
	using FirstArg = typename EXP_AttributeFunctionFirstArgument<_Args ...>::FirstType;
};

struct EXP_AttributeFunctionSetterTraitDummy : public EXP_AttributeFunctionTraitDummy
{
	using FirstArg = bool;
};

/** Struct used to identify the type, object a member.
 */
template <class>
struct EXP_AttributeMemberTrait;

template <class _Type, class _Object>
struct EXP_AttributeMemberTrait<_Type (_Object::*)>
{
	using Type = _Type;
	using Object = _Object;
	using Signature = Type (Object::*);
};

template <class ParentClass, EXP_GetSetFlags Flags, class SetType, 
		class CustomCheckFunctionTrait,
		typename CustomCheckFunctionTrait::Signature CustomCheckFunction>
class EXP_AttributeDefBase
{
public:
	template <EXP_GetSetFlags _Flags = Flags>
	static typename std::enable_if<(_Flags & EXP_GETSET_RANGE) == 0, bool>::type
	RangeCheckClamp(SetType& value, const EXP_Attribute *attrdef)
	{
		return true;
	}

	// Check if the value is in a range, if not depending on RangeInfo::Clamp, clamp or raise.
	template <EXP_GetSetFlags _Flags = Flags>
	static typename std::enable_if<(_Flags & EXP_GETSET_RANGE) != 0, bool>::type
	RangeCheckClamp(SetType& value, const EXP_Attribute *attrdef)
	{
		const float lower = attrdef->m_lower;
		const float upper = attrdef->m_upper;
		if (value < lower || value > upper) {
			if (Flags & EXP_GETSET_CLAMP) {
				attrdef->PrintLimitError<SetType>(value, lower, upper);
				return false;
			}

			// Try to clamp.
			value = mt::Clamp(value, lower, upper);
		}

		return true;
	}

	/// Call a custom function without passing the attribute as argument.
	template <class Trait, typename Trait::Signature Function, class ... Args>
	static typename std::enable_if<(sizeof ... (Args) == (Trait::ARGS_COUNT)),
		typename Trait::ReturnType>::type
	CallCustomFunction(ParentClass *self, const EXP_Attribute *attrdef, Args ... args)
	{
		return (self->*Function)(args ...);
	}

	/** Call a custom function and pass the attribute argument, check that the custom
	 * function have one more argument.
	 */
	template <class Trait, typename Trait::Signature Function, class ... Args>
	static typename std::enable_if<((sizeof ... (Args) + 1) == Trait::ARGS_COUNT),
		typename Trait::ReturnType>::type
	CallCustomFunction(ParentClass *self, const EXP_Attribute *attrdef, Args ... args)
	{
		return (self->*Function)(args ..., attrdef);
	}

	template <typename CustomCheckFunctionTrait::Signature Function = CustomCheckFunction>
	static typename std::enable_if<Function == nullptr, bool>::type
	Check(ParentClass *self, const EXP_Attribute *attrdef)
	{
		return true;
	}

	template <typename CustomCheckFunctionTrait::Signature Function = CustomCheckFunction>
	static typename std::enable_if<Function != nullptr, bool>::type
	Check(ParentClass *self, const EXP_Attribute *attrdef)
	{
		return CallCustomFunction<CustomCheckFunctionTrait, CustomCheckFunction>(self, attrdef);
	}
};

template <class _ParentClass, EXP_GetSetFlags _Flags,
		class Trait, typename Trait::Signature Member,
		class CustomCheckFunctionTrait = EXP_AttributeFunctionTraitDummy,
		typename CustomCheckFunctionTrait::Signature CustomCheckFunction = nullptr>
class EXP_AttributeDefMember : public EXP_AttributeDefBase<_ParentClass, _Flags, typename Trait::Type,
		CustomCheckFunctionTrait, CustomCheckFunction>
{
public:
	using ParentClass = _ParentClass;
	static const EXP_GetSetFlags Flags = _Flags;
	using Type = typename Trait::Type;
	using GetType = Type;
	using SetType = Type;

	static Type& GetValue(ParentClass *self, const EXP_Attribute *attrdef)
	{
		return self->*Member;
	}

	static bool SetValue(ParentClass *self, const EXP_Attribute *attrdef, Type value)
	{
		self->*Member = value;
		return true;
	}
};

template <class _ParentClass, EXP_GetSetFlags _Flags,
		class CustomGetterFunctionTrait = EXP_AttributeFunctionTraitDummy,
		typename CustomGetterFunctionTrait::Signature CustomGetterFunction = nullptr,
		class CustomSetterFunctionTrait = EXP_AttributeFunctionSetterTraitDummy,
		typename CustomSetterFunctionTrait::Signature CustomSetterFunction = nullptr,
		class CustomCheckFunctionTrait = EXP_AttributeFunctionTraitDummy,
		typename CustomCheckFunctionTrait::Signature CustomCheckFunction = nullptr>
class EXP_AttributeDefFunction : public EXP_AttributeDefBase<_ParentClass, _Flags, typename CustomSetterFunctionTrait::FirstArg,
		CustomCheckFunctionTrait, CustomCheckFunction>
{
public:
	using ParentClass = _ParentClass;
	static const EXP_GetSetFlags Flags = _Flags;
	using GetType = typename CustomGetterFunctionTrait::ReturnType;
	using SetType = typename CustomSetterFunctionTrait::FirstArg;

	static GetType GetValue(ParentClass *self, const EXP_Attribute *attrdef)
	{
		return EXP_AttributeDefFunction::template CallCustomFunction<CustomGetterFunctionTrait, CustomGetterFunction>(self, attrdef);
	}

	template <class Trait = CustomSetterFunctionTrait, typename Trait::Signature Function = CustomSetterFunction>
	static typename std::enable_if<Function != nullptr && !std::is_void<typename Trait::ReturnType>::value, bool>::type
	SetValue(ParentClass *self, const EXP_Attribute *attrdef, SetType value)
	{
		return EXP_AttributeDefFunction::template CallCustomFunction<CustomSetterFunctionTrait, CustomSetterFunction>(self, attrdef, value);
	}

	// Defaulting return status to true for function returning void.
	template <class Trait = CustomSetterFunctionTrait, typename Trait::Signature Function = CustomSetterFunction>
	static typename std::enable_if<Function != nullptr && std::is_void<typename Trait::ReturnType>::value, bool>::type
	SetValue(ParentClass *self, const EXP_Attribute *attrdef, SetType value)
	{
		EXP_AttributeDefFunction::template CallCustomFunction<CustomSetterFunctionTrait, CustomSetterFunction>(self, attrdef, value);
		return true;
	}
};

template <class Def>
class EXP_AttributeDef : public Def
{
	using ParentClass = typename Def::ParentClass;
	static const EXP_GetSetFlags Flags = Def::Flags;
	using SetType = typename Def::SetType;
	using SetTypeNoRef = typename std::decay<SetType>::type;

	static PyObject *Getter(PyObject *self_py, void *closure)
	{
		// Check if the proxy is still valid.
		ParentClass *self = static_cast<ParentClass *>(EXP_PROXY_REF(self_py));
		if (!EXP_Attribute::IsValid(self)) {
			return nullptr;
		}

		const EXP_Attribute *attrdef = static_cast<EXP_Attribute *>(closure);

		// First call check function.
		if (!Def::Check(self, attrdef)) {
			return nullptr;
		}

		return EXP_ConvertToPython(Def::GetValue(self, attrdef));
	}

	template <EXP_GetSetFlags _Flags = Flags>
	static typename std::enable_if<(_Flags & EXP_GETSET_READONLY) != 0, int>::type
	Setter(PyObject *self_py, PyObject *value, void *closure)
	{
		// Should never been called.
		BLI_assert(false);
		return PY_SET_ATTR_FAIL;
	}

	template <EXP_GetSetFlags _Flags = Flags>
	static typename std::enable_if<(_Flags & EXP_GETSET_READONLY) == 0, int>::type
	Setter(PyObject *self_py, PyObject *value, void *closure)
	{
		// Check if the proxy is still valid.
		ParentClass *self = static_cast<ParentClass *>(EXP_PROXY_REF(self_py));
		if (!EXP_Attribute::IsValid(self)) {
			return PY_SET_ATTR_FAIL;
		}

		const EXP_Attribute *attrdef = static_cast<EXP_Attribute *>(closure);

		// First call check function.
		if (!Def::Check(self, attrdef)) {
			return PY_SET_ATTR_FAIL;
		}

		// Try to convert from python.
		SetTypeNoRef temp;
		if (EXP_ConvertFromPython(value, temp)) {
			if (!Def::RangeCheckClamp(temp, attrdef)) {
				return PY_SET_ATTR_FAIL;
			}

			if (!Def::SetValue(self, attrdef, temp)) {
				return PY_SET_ATTR_FAIL;
			}
		}
		else {
			attrdef->PrintSetterError<SetTypeNoRef>();
			return PY_SET_ATTR_FAIL;
		}

		return PY_SET_ATTR_SUCCESS;
	}

public:
	static EXP_Attribute GetAttribute(const std::string& name, const std::array<float, 2>& range = {{0.0f, 0.0f}})
	{
		EXP_Attribute::GetterFunction getter = Getter;
		EXP_Attribute::SetterFunction setter = Setter;
		return EXP_Attribute(name, getter, (Flags & EXP_GETSET_READONLY) ? nullptr : setter, range);
	}
};

#define _FUNCTION_PARAMETER(function) \
	EXP_AttributeFunctionTrait<decltype(&SelfType::function)>, &SelfType::function

#define _FUNCTION_SETTER_PARAMETER(function) \
	EXP_AttributeFunctionSetterTrait<decltype(&SelfType::function)>, &SelfType::function

#define _FUNCTION_NULL \
	EXP_AttributeFunctionTraitDummy, nullptr

#define _MEMBER_PARAMETER(member) \
	EXP_AttributeMemberTrait<decltype(&SelfType::member)>, &SelfType::member

#define _ADD_CLAMP(flags, clamp) \
	(EXP_GetSetFlags)((clamp ? flags & (EXP_GETSET_CLAMP & EXP_GETSET_RANGE) : flags & EXP_GETSET_RANGE))

#define EXP_ATTRIBUTE_RW(name, member) \
	EXP_AttributeDef<EXP_AttributeDefMember<SelfType, EXP_GETSET_NONE, _MEMBER_PARAMETER(member)> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RO(name, member) \
	EXP_AttributeDef<EXP_AttributeDefMember<SelfType, EXP_GETSET_READONLY, _MEMBER_PARAMETER(member)> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RW_RANGE(name, member, lower, upper, clamp) \
	EXP_AttributeDef<EXP_AttributeDefMember<SelfType, _ADD_CLAMP(EXP_GETSET_NONE, clamp), _MEMBER_PARAMETER(member) \
	> >::GetAttribute(name, {{lower, upper}})

#define EXP_ATTRIBUTE_RW_CHECK(name, member, check) \
	EXP_AttributeDef<EXP_AttributeDefMember<SelfType, EXP_GETSET_NONE, _MEMBER_PARAMETER(member), _FUNCTION_SETTER_PARAMETER(check) \
	> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RW_FUNCTION(name, getter, setter) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, EXP_GETSET_NONE, \
		_FUNCTION_PARAMETER(getter), _FUNCTION_SETTER_PARAMETER(setter) \
	> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RO_FUNCTION(name, getter) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, EXP_GETSET_READONLY, _FUNCTION_PARAMETER(getter) \
	> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RW_FUNCTION_CHECK(name, getter, setter, check) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, EXP_GETSET_NONE, \
		_FUNCTION_PARAMETER(getter), \
		_FUNCTION_SETTER_PARAMETER(setter), \
		_FUNCTION_PARAMETER(check) \
	> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RO_FUNCTION_CHECK(name, getter, check) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, EXP_GETSET_READONLY, \
		_FUNCTION_PARAMETER(getter), \
		_FUNCTION_NULL, \
		_FUNCTION_PARAMETER(check) \
	> >::GetAttribute(name)

#define EXP_ATTRIBUTE_RW_FUNCTION_RANGE(name, getter, setter, lower, upper, clamp) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, _ADD_CLAMP(EXP_GETSET_NONE, clamp), \
		_FUNCTION_PARAMETER(getter), \
		_FUNCTION_SETTER_PARAMETER(setter) \
	> >::GetAttribute(name, {{lower, upper}})

#define EXP_ATTRIBUTE_RW_FUNCTION_CHECK_RANGE(name, getter, setter, check, lower, upper, clamp) \
	EXP_AttributeDef<EXP_AttributeDefFunction<SelfType, _ADD_CLAMP(EXP_GETSET_NONE, clamp), \
		_FUNCTION_PARAMETER(getter), \
		_FUNCTION_SETTER_PARAMETER(setter), \
		_FUNCTION_PARAMETER(check)  \
	> >::GetAttribute(name, {{lower, upper}})

#define EXP_ATTRIBUTE_NULL \
	EXP_Attribute()

#endif  // __EXP_ATTRIBUTE_DEF_H__
