
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
	Py_Header(KX_CharacterWrapper)

public:
	KX_CharacterWrapper(PHY_ICharacter* character);
	virtual ~KX_CharacterWrapper();

	virtual std::string GetName() const;
#ifdef WITH_PYTHON
	EXP_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, jump);
	EXP_PYMETHOD_DOC(KX_CharacterWrapper, setVelocity);
	EXP_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, reset);

	bool pyattr_get_onground();
	
	mt::vec3 pyattr_get_gravity();
	void pyattr_set_gravity(const mt::vec3& value);
	float pyattr_get_fallSpeed();
	void pyattr_set_fallSpeed(float value);
	float pyattr_get_maxSlope();
	void pyattr_set_maxSlope(float value);
	int pyattr_get_max_jumps();
	void pyattr_set_max_jumps(int value);
	int pyattr_get_jump_count();
	float pyattr_get_jumpSpeed();
	void pyattr_set_jumpSpeed(float value);
	mt::vec3 pyattr_get_walk_dir();
	void pyattr_set_walk_dir(const mt::vec3& value);
#endif // WITH_PYTHON

private:
	PHY_ICharacter*			 m_character;
};

#endif  /* __KX_CHARACTERWRAPPER_H__ */
