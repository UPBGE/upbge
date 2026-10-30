/** \file KX_ObjectReferenceValue.h
 *  \ingroup ketsji
 */

#pragma once

#include <map>

#include "EXP_Value.h"

class BL_SceneConverter;
class KX_GameObject;
class SCA_IObject;

namespace blender {
struct Object;
}

/**
 * The value of Object game properties: a reference that becomes empty when its object is removed.
 * Properties converted from the editor first hold the Blender object, found in the game once all
 * objects are converted (#ResolveConverted).
 */
class KX_ObjectReferenceValue : public EXP_PropValue {
 public:
  explicit KX_ObjectReferenceValue(KX_GameObject *object = nullptr);
  explicit KX_ObjectReferenceValue(blender::Object *converted_object);
  virtual ~KX_ObjectReferenceValue();

  KX_GameObject *GetObject() const
  {
    return m_object;
  }
  void SetObject(KX_GameObject *object);
  /** Finds the converted object, for references made from the editor. */
  void ResolveConverted(BL_SceneConverter &converter);

  /** The object's name, or "None". */
  virtual std::string GetText() override;
  virtual double GetNumber() override;
  virtual int GetValueType() override;
  virtual void SetValue(EXP_Value *newval) override;
  virtual EXP_Value *GetReplica() override;
  virtual void ProcessReplica() override;
  virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val) override;
  virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val) override;

#ifdef WITH_PYTHON
  /** The game object, or None. */
  virtual PyObject *ConvertValueToPython() override;
#endif

 private:
  friend KX_GameObject;

  KX_GameObject *m_object = nullptr;
  blender::Object *m_convertedObject = nullptr;
};

/** Finds the game objects of editor Object properties, also inside Lists and Tables. */
void KX_ResolveObjectReferences(EXP_Value *value, BL_SceneConverter &converter);
/** In a copy made by Add Object, references to objects of the copied group go to their copies. */
void KX_RelinkObjectReferences(EXP_Value *value,
                               const std::map<SCA_IObject *, SCA_IObject *> &copies);
