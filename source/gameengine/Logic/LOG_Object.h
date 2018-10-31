#ifndef __LOG_OBJECT_H__
#define __LOG_OBJECT_H__

#include "EXP_Value.h"
#include "EXP_ListValue.h"

#include <memory>

class LOG_Tree;
class KX_PythonComponent;

class LOG_Object : public EXP_Value
{
	Py_Header

private:
	std::unique_ptr<EXP_ListValue<KX_PythonComponent> > m_components;
	std::uniq_ptr<LOG_Tree> m_logicTree;

public:
	LOG_Object();
	LOG_Object(const LOG_Object& other);
	virtual ~LOG_Object();

	EXP_ListValue<KX_PythonComponent> *GetComponents() const;
	void SetComponents(EXP_ListValue<KX_PythonComponent> *components);

	LOG_Tree *GetLogicTree() const;
	void SetLogicTree(LOG_Tree *tree);

	bool UseLogic() const;
	/// Updates components and logic tree.
	void UpdateLogic();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_components(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON
};

#endif  // __LOG_OBJECT_H__
