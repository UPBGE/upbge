/** \file EXP_ArrayValue.h
 *  \ingroup expressions
 */

#pragma once

#include <vector>

#include "EXP_Value.h"

/** The value of List and Table game properties: values of any property type, in order. */
class EXP_ArrayValue : public EXP_PropValue {
 public:
  EXP_ArrayValue() = default;
  virtual ~EXP_ArrayValue();

  virtual std::string GetText() override;
  /** The number of items. */
  virtual double GetNumber() override;
  virtual int GetValueType() override;
  virtual void SetValue(EXP_Value *newval) override;
  virtual EXP_Value *GetReplica() override;
  virtual void ProcessReplica() override;
  virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val) override;
  virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val) override;

  const std::vector<EXP_Value *> &GetItems() const
  {
    return m_items;
  }
  /** Takes over the caller's reference to \a item. */
  void Append(EXP_Value *item);

#ifdef WITH_PYTHON
  /** A new Python list: changing it does not change the property. */
  virtual PyObject *ConvertValueToPython() override;
#endif

 private:
  std::vector<EXP_Value *> m_items;
};
