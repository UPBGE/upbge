#ifndef __KX_PYTHON_RESSOURCE_H__
#define __KX_PYTHON_RESSOURCE_H__

#include "EXP_PyObjectPlus.h"

#include "BL_ResourceCollection.h"

#include "KX_Scene.h"
#include "KX_Camera.h"
#include "KX_KetsjiEngine.h"

#ifdef WITH_PYTHON

/** Find a ressource based on its name in a scene.
 * \param scene Used to fetch the ressource.
 * \param name Name of the ressource.
 * \return the ressource of the corresponding name if found, else null.
 */
template <class Object>
inline Object *FindRessourceByName(KX_Scene *scene, const std::string& name)
{
	return scene->GetResources().Find<Object>(name);
}

/** Find a camera in scene, fetch only scene active object of camera type.
 * \param scene Used to fetch the camera.
 * \param name Name of the camera.
 * \return the camera of the corresponding name if found, else null.
 */
template <>
inline KX_Camera *FindRessourceByName(KX_Scene *scene, const std::string& name)
{
	return scene->GetCameraList().FindValue(name);
}

/** Convert python value to a derived type of EXP_PyObjectPlus.
 * \param value The python value to convert.
 * \param object The converted object.
 * \param py_none_ok True if None is accepted and converted to nullptr.
 * \param error_prefix Message prefix to print in case of error.
 * \return True if conversion succeeded.
 */
template <class Object>
inline bool ConvertFromPython(PyObject *value, Object * &object, bool py_none_ok, const char *error_prefix)
{
	const char *typeName = Object::Type.tp_name;

	if (!value) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		object = nullptr;
		return false;
	}

	if (value == Py_None) {
		object = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected %s or a %s name, None is invalid", error_prefix, typeName, typeName);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &Object::Type)) {
		object = static_cast<Object *>(EXP_PROXY_REF(value));

		/* sets the error */
		if (!object) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a %s or None", error_prefix, typeName);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a %s", error_prefix, typeName);
	}

	return false;
}

/** Convert python value to a derived type of EXP_PyObjectPlus.
 * \param scene The scene to fetch ressource in case of python string (name) passed.
 * \param value The python value to convert.
 * \param object The converted object.
 * \param py_none_ok True if None is accepted and converted to nullptr.
 * \param error_prefix Message prefix to print in case of error.
 * \return True if conversion succeeded.
 */
template <class Object>
inline bool ConvertFromPython(KX_Scene *scene, PyObject *value, Object * &object, bool py_none_ok, const char *error_prefix)
{
	const char *typeName = Object::Type.tp_name;

	if (!value) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		object = nullptr;
		return false;
	}

	if (value == Py_None) {
		object = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected %s or a %s name, None is invalid", error_prefix, typeName, typeName);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		object = FindRessourceByName<Object>(scene, std::string(_PyUnicode_AsString(value)));

		if (object) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any %s in this scene", error_prefix, _PyUnicode_AsString(value), typeName);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &Object::Type)) {
		object = static_cast<Object *>(EXP_PROXY_REF(value));

		/* sets the error */
		if (!object) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a %s, a string or None", error_prefix, typeName);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a %s or a string", error_prefix, typeName);
	}

	return false;
}

/** Convert python value to a scene.
 * \param value The python value to convert.
 * \param scene The converted scene.
 * \param py_none_ok True if None is accepted and converted to nullptr.
 * \param error_prefix Message prefix to print in case of error.
 * \return True if conversion succeeded.
 */
template <>
inline bool ConvertFromPython(PyObject *value, KX_Scene * &scene, bool py_none_ok, const char *error_prefix)
{
	if (!value) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		scene = nullptr;
		return false;
	}

	if (value == Py_None) {
		scene = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_Scene or a KX_Scene name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		scene = KX_GetActiveEngine()->FindScene(std::string(_PyUnicode_AsString(value)));

		if (scene) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any in game", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_Scene::Type)) {
		scene = static_cast<KX_Scene *>EXP_PROXY_REF(value);

		// Sets the error.
		if (scene == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	scene = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene or a string", error_prefix);
	}

	return false;
}

#endif  // WITH_PYTHON

#endif  // __KX_PYTHON_RESSOURCE_H__
