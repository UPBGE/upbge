
/** \file KX_CharacterWrapper.h
 *  \ingroup ketsji
 */

#ifndef __KX_CHARACTERWRAPPER_H__
#define __KX_CHARACTERWRAPPER_H__

#include "EXP_Value.h"
#include "PHY_DynamicTypes.h"

class PHY_ICharacter;

/// Python interface to character physics
class KX_CharacterWrapper : public CValue {
  Py_Header

      public : KX_CharacterWrapper(PHY_ICharacter *character);
  virtual ~KX_CharacterWrapper();

  virtual std::string GetName();
#ifdef WITH_PYTHON
  KX_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, jump);
  KX_PYMETHOD_DOC(KX_CharacterWrapper, setVelocity);
  KX_PYMETHOD_DOC_NOARGS(KX_CharacterWrapper, reset);

  static PyObject *pyattr_get_onground(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

  static PyObject *pyattr_get_gravity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_gravity(PyObjectPlus *self_v,
                                const KX_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);
  static PyObject *pyattr_get_fallSpeed(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_fallSpeed(PyObjectPlus *self_v,
                                  const KX_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value);
  static PyObject *pyattr_get_max_jumps(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_max_jumps(PyObjectPlus *self_v,
                                  const KX_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value);
  static PyObject *pyattr_get_maxSlope(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_maxSlope(PyObjectPlus *self_v,
                                 const KX_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value);
  static PyObject *pyattr_get_jump_count(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_jumpSpeed(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_jumpSpeed(PyObjectPlus *self_v,
                                  const KX_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value);
  static PyObject *pyattr_get_walk_dir(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_walk_dir(PyObjectPlus *self_v,
                                 const KX_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value);
#endif  // WITH_PYTHON

 private:
  PHY_ICharacter *m_character;
};

#endif /* __KX_CHARACTERWRAPPER_H__ */
