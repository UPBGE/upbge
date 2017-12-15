#ifndef __CM_TEMPLATE_H__
#define __CM_TEMPLATE_H__

#include <tuple>
#include <type_traits>

template <class BaseObject, template<typename> class Object, class Tuple, class Key, unsigned int index, class ... Args>
typename std::enable_if<std::tuple_size<Tuple>::value == index, BaseObject *>::type
_CM_InstantiateTemplateCase(const Key& UNUSED(key), Args ... UNUSED(args))
{
	return nullptr;
}

template <class BaseObject, template<typename> class Object, class Tuple, class Key, unsigned int index, class ... Args>
typename std::enable_if<std::tuple_size<Tuple>::value != index, BaseObject *>::type
_CM_InstantiateTemplateCase(const Key& key, Args ... args)
{
	using KeyType = typename std::tuple_element<index, Tuple>::type;
	if (KeyType() == key) {
		return (new Object<KeyType>(args ...));
	}
	else {
		return _CM_InstantiateTemplateCase<BaseObject, Object, Tuple, Key, index + 1, Args ...>(key, args ...);
	}
	return nullptr;
}

template <class BaseObject, template<typename> class Object, class Tuple, class Key, class ... Args>
BaseObject *CM_InstantiateTemplateSwitch(const Key& key, Args ... args)
{
	return _CM_InstantiateTemplateCase<BaseObject, Object, Tuple, Key, 0, Args ...>(key, args ...);
}

#endif  // __CM_TEMPLATE_H__
