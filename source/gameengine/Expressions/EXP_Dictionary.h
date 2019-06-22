/** \file EXP_Dictionary.h
 *  \ingroup expressions
 */

#ifndef __EXP_DICTIONARY_H__
#define __EXP_DICTIONARY_H__

#include "EXP_Value.h"
#include "EXP_PropValue.h"

#include <vector>
#include <map>
#include <regex>

class EXP_Dictionary : public EXP_Value
{
	Py_Header

public:
	EXP_Dictionary() = default;
	EXP_Dictionary(const EXP_Dictionary& other);
	virtual ~EXP_Dictionary() = default;

#ifdef WITH_PYTHON
	virtual PyObject *ConvertKeysToPython();
#endif  // WITH_PYTHON

	/// Property Management
	/** Set property <prop>, overwrites and releases a previous property with the same name if needed.
	 * Stall the owning of the property.
	 */
	void SetProperty(const std::string& name, EXP_PropValue *prop);
	/// Get pointer to a property with name <name>, returns nullptr if there is no property named <name>.
	EXP_PropValue *GetProperty(const std::string & name) const;
	/// Remove the property named <name>, returns true if the property was succesfully removed, false if property was not found or could not be removed.
	bool RemoveProperty(const std::string& name);
	std::vector<std::string> GetPropertyNames() const;
	/// Clear all properties.
	void ClearProperties();

	// TODO to remove in the same time timer management is refactored.
	/// Get property number <inIndex>.
	EXP_PropValue *GetProperty(unsigned short inIndex) const;
	/// Get the amount of properties assiocated with this value.
	unsigned short GetPropertyCount() const;

	bool FindPropertyRegex(const std::regex& regex) const;

	virtual bool IsDictionary() const;

#ifdef WITH_PYTHON
	EXP_PYMETHOD_NOARGS(EXP_Dictionary, GetPropertyNames);
	EXP_PYMETHOD_VARARGS(EXP_Dictionary, get);

	/// get/set python dictionary interface.
	static PyMappingMethods Mapping;
	static PySequenceMethods Sequence;
#endif

private:
	/// Properties for user/game etc.
	std::map<std::string, std::unique_ptr<EXP_PropValue> > m_properties;
};

#endif  // __EXP_DICTIONARY_H__
