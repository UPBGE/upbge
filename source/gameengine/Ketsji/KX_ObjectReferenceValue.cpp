/** \file gameengine/Ketsji/KX_ObjectReferenceValue.cpp
 *  \ingroup ketsji
 */

#include "KX_ObjectReferenceValue.h"

#include "BL_SceneConverter.h"
#include "EXP_ArrayValue.h"
#include "EXP_BoolValue.h"
#include "EXP_DictValue.h"
#include "EXP_ErrorValue.h"
#include "KX_GameObject.h"

KX_ObjectReferenceValue::KX_ObjectReferenceValue(KX_GameObject *object)
{
  SetObject(object);
}

KX_ObjectReferenceValue::KX_ObjectReferenceValue(blender::Object *converted_object)
    : m_convertedObject(converted_object)
{
}

KX_ObjectReferenceValue::~KX_ObjectReferenceValue()
{
  SetObject(nullptr);
}

void KX_ObjectReferenceValue::SetObject(KX_GameObject *object)
{
  m_convertedObject = nullptr;
  if (object == m_object) {
    return;
  }
  if (m_object != nullptr) {
    m_object->RemoveReferenceValue(this);
  }
  m_object = object;
  if (m_object != nullptr) {
    m_object->AddReferenceValue(this);
  }
}

void KX_ObjectReferenceValue::ResolveConverted(BL_SceneConverter &converter)
{
  if (m_convertedObject != nullptr) {
    SetObject(converter.FindGameObject(m_convertedObject));
  }
}

std::string KX_ObjectReferenceValue::GetText()
{
  return m_object ? m_object->GetName() : "None";
}

double KX_ObjectReferenceValue::GetNumber()
{
  return m_object ? 1.0 : 0.0;
}

int KX_ObjectReferenceValue::GetValueType()
{
  return VALUE_OBJECT_TYPE;
}

void KX_ObjectReferenceValue::SetValue(EXP_Value *newval)
{
  if (newval->GetValueType() == VALUE_OBJECT_TYPE) {
    SetObject(static_cast<KX_ObjectReferenceValue *>(newval)->m_object);
  }
}

EXP_Value *KX_ObjectReferenceValue::GetReplica()
{
  KX_ObjectReferenceValue *replica = new KX_ObjectReferenceValue(*this);
  replica->ProcessReplica();
  return replica;
}

void KX_ObjectReferenceValue::ProcessReplica()
{
  EXP_PropValue::ProcessReplica();
  /* The copy refers to the same object and must be cleared with it too. */
  KX_GameObject *object = m_object;
  m_object = nullptr;
  SetObject(object);
}

EXP_Value *KX_ObjectReferenceValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  return val->CalcFinal(VALUE_OBJECT_TYPE, op, this);
}

EXP_Value *KX_ObjectReferenceValue::CalcFinal(VALUE_DATA_TYPE dtype,
                                              VALUE_OPERATOR op,
                                              EXP_Value *val)
{
  if (dtype == VALUE_OBJECT_TYPE && (op == VALUE_EQL_OPERATOR || op == VALUE_NEQ_OPERATOR)) {
    const bool same = static_cast<KX_ObjectReferenceValue *>(val)->m_object == m_object;
    return new EXP_BoolValue(op == VALUE_EQL_OPERATOR ? same : !same);
  }
  return new EXP_ErrorValue(op2str(op) + " not defined for objects");
}

#ifdef WITH_PYTHON
PyObject *KX_ObjectReferenceValue::ConvertValueToPython()
{
  if (m_object == nullptr) {
    Py_RETURN_NONE;
  }
  return m_object->GetProxy();
}
#endif

void KX_RelinkObjectReferences(EXP_Value *value,
                               const std::map<SCA_IObject *, SCA_IObject *> &copies)
{
  switch (value->GetValueType()) {
    case VALUE_OBJECT_TYPE: {
      KX_ObjectReferenceValue *reference = static_cast<KX_ObjectReferenceValue *>(value);
      const auto copy = copies.find(reference->GetObject());
      if (copy != copies.end()) {
        reference->SetObject(static_cast<KX_GameObject *>(copy->second));
      }
      break;
    }
    case VALUE_ARRAY_TYPE:
      for (EXP_Value *item : static_cast<EXP_ArrayValue *>(value)->GetItems()) {
        KX_RelinkObjectReferences(item, copies);
      }
      break;
    case VALUE_DICT_TYPE:
      for (auto &entry : static_cast<EXP_DictValue *>(value)->GetEntries()) {
        KX_RelinkObjectReferences(entry.second, copies);
      }
      break;
    default:
      break;
  }
}

void KX_ResolveObjectReferences(EXP_Value *value, BL_SceneConverter &converter)
{
  switch (value->GetValueType()) {
    case VALUE_OBJECT_TYPE:
      static_cast<KX_ObjectReferenceValue *>(value)->ResolveConverted(converter);
      break;
    case VALUE_ARRAY_TYPE:
      for (EXP_Value *item : static_cast<EXP_ArrayValue *>(value)->GetItems()) {
        KX_ResolveObjectReferences(item, converter);
      }
      break;
    case VALUE_DICT_TYPE:
      for (auto &entry : static_cast<EXP_DictValue *>(value)->GetEntries()) {
        KX_ResolveObjectReferences(entry.second, converter);
      }
      break;
    default:
      break;
  }
}
