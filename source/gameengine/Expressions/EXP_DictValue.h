/** \file EXP_DictValue.h
 *  \ingroup expressions
 */

#pragma once

#include <map>

#include "EXP_Value.h"

/** A dictionary game property value, and each row of a Table property: text keys, any values. */
class EXP_DictValue : public EXP_PropValue {
 public:
  EXP_DictValue() = default;
  virtual ~EXP_DictValue();

  virtual std::string GetText() override;
  /** The number of entries. */
  virtual double GetNumber() override;
  virtual int GetValueType() override;
  virtual void SetValue(EXP_Value *newval) override;
  virtual EXP_Value *GetReplica() override;
  virtual void ProcessReplica() override;
  virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val) override;
  virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val) override;

  const std::map<std::string, EXP_Value *> &GetEntries() const
  {
    return m_entries;
  }
  /** Takes over the caller's reference to \a value, replacing any entry of \a key. */
  void Set(const std::string &key, EXP_Value *value);

#ifdef WITH_PYTHON
  /** A new Python dict: changing it does not change the property. */
  virtual PyObject *ConvertValueToPython() override;
#endif

 private:
  std::map<std::string, EXP_Value *> m_entries;
};
