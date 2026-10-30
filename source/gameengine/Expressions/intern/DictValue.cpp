/** \file gameengine/Expressions/intern/DictValue.cpp
 *  \ingroup expressions
 */

#include "EXP_DictValue.h"

#include "EXP_ErrorValue.h"

EXP_DictValue::~EXP_DictValue()
{
  for (auto &entry : m_entries) {
    entry.second->Release();
  }
}

std::string EXP_DictValue::GetText()
{
  std::string text = "{";
  for (auto &entry : m_entries) {
    text += (text.size() > 1 ? ", " : "") + entry.first + ": " + entry.second->GetText();
  }
  return text + "}";
}

double EXP_DictValue::GetNumber()
{
  return double(m_entries.size());
}

int EXP_DictValue::GetValueType()
{
  return VALUE_DICT_TYPE;
}

void EXP_DictValue::SetValue(EXP_Value *newval)
{
  if (newval == this || newval->GetValueType() != VALUE_DICT_TYPE) {
    return;
  }
  std::map<std::string, EXP_Value *> entries;
  for (auto &entry : static_cast<EXP_DictValue *>(newval)->m_entries) {
    entries.emplace(entry.first, entry.second->GetReplica());
  }
  for (auto &entry : m_entries) {
    entry.second->Release();
  }
  m_entries = std::move(entries);
}

EXP_Value *EXP_DictValue::GetReplica()
{
  EXP_DictValue *replica = new EXP_DictValue(*this);
  replica->ProcessReplica();
  return replica;
}

void EXP_DictValue::ProcessReplica()
{
  EXP_PropValue::ProcessReplica();
  for (auto &entry : m_entries) {
    entry.second = entry.second->GetReplica();
  }
}

EXP_Value *EXP_DictValue::Calc(VALUE_OPERATOR op, EXP_Value * /*val*/)
{
  return new EXP_ErrorValue(op2str(op) + " not defined for dictionaries");
}

EXP_Value *EXP_DictValue::CalcFinal(VALUE_DATA_TYPE /*dtype*/,
                                    VALUE_OPERATOR op,
                                    EXP_Value * /*val*/)
{
  return new EXP_ErrorValue(op2str(op) + " not defined for dictionaries");
}

void EXP_DictValue::Set(const std::string &key, EXP_Value *value)
{
  auto [entry, inserted] = m_entries.try_emplace(key, value);
  if (!inserted) {
    entry->second->Release();
    entry->second = value;
  }
}

#ifdef WITH_PYTHON
PyObject *EXP_DictValue::ConvertValueToPython()
{
  PyObject *dict = PyDict_New();
  for (auto &entry : m_entries) {
    PyObject *value = entry.second->ConvertValueToPython();
    if (value == nullptr) {
      value = entry.second->GetProxy();
    }
    PyDict_SetItemString(dict, entry.first.c_str(), value);
    Py_DECREF(value);
  }
  return dict;
}
#endif
