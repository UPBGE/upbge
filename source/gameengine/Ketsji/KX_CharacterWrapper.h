
/** \file KX_CharacterWrapper.h
 *  \ingroup ketsji
 */

#ifndef __KX_CHARACTERWRAPPER_H__
#define __KX_CHARACTERWRAPPER_H__

#include "EXP_Value.h"
#include "PHY_DynamicTypes.h"
class PHY_ICharacter;


///Python interface to character physics
class	KX_CharacterWrapper : public EXP_Value
{
	Py_Header

public:
	KX_CharacterWrapper(PHY_ICharacter* character);
	virtual ~KX_CharacterWrapper();

	virtual std::string GetName() const;
#ifdef WITH_PYTHON
	EXP_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, jump);
	EXP_PYMETHOD_DOC(KX_CharacterWrapper, setVelocity);
	EXP_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, reset);

	static PyObject* pyattr_get_onground(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	
	static PyObject*	pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_fallSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_fallSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_max_jumps(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_max_jumps(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_maxSlope(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_maxSlope(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_jump_count(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_jumpSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_jumpSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_walk_dir(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_walk_dir(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif // WITH_PYTHON

private:
	PHY_ICharacter*			 m_character;
};

#endif  /* __KX_CHARACTERWRAPPER_H__ */
