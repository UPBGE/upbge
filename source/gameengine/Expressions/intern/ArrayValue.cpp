/** \file gameengine/Expressions/intern/ArrayValue.cpp
 *  \ingroup expressions
 */

#include "EXP_ArrayValue.h"

#include "EXP_DictValue.h"
#include "EXP_ErrorValue.h"

EXP_ArrayValue::~EXP_ArrayValue()
{
  for (EXP_Value *item : m_items) {
    item->Release();
  }
}

std::string EXP_ArrayValue::GetText()
{
  std::string text = "[";
  for (size_t index = 0; index < m_items.size(); index++) {
    text += (index ? ", " : "") + m_items[index]->GetText();
  }
  return text + "]";
}

double EXP_ArrayValue::GetNumber()
{
  return double(m_items.size());
}

int EXP_ArrayValue::GetValueType()
{
  return VALUE_ARRAY_TYPE;
}

void EXP_ArrayValue::SetValue(EXP_Value *newval)
{
  if (newval == this || newval->GetValueType() != VALUE_ARRAY_TYPE) {
    return;
  }
  std::vector<EXP_Value *> items;
  items.reserve(static_cast<EXP_ArrayValue *>(newval)->m_items.size());
  for (EXP_Value *item : static_cast<EXP_ArrayValue *>(newval)->m_items) {
    items.push_back(item->GetReplica());
  }
  for (EXP_Value *item : m_items) {
    item->Release();
  }
  m_items = std::move(items);
}

EXP_Value *EXP_ArrayValue::GetReplica()
{
  EXP_ArrayValue *replica = new EXP_ArrayValue(*this);
  replica->ProcessReplica();
  return replica;
}

void EXP_ArrayValue::ProcessReplica()
{
  EXP_PropValue::ProcessReplica();
  /* Each copy owns its items, so changing one object's list leaves the others. */
  for (EXP_Value *&item : m_items) {
    item = item->GetReplica();
  }
}

EXP_Value *EXP_ArrayValue::Calc(VALUE_OPERATOR op, EXP_Value * /*val*/)
{
  return new EXP_ErrorValue(op2str(op) + " not defined for lists");
}

EXP_Value *EXP_ArrayValue::CalcFinal(VALUE_DATA_TYPE /*dtype*/,
                                     VALUE_OPERATOR op,
                                     EXP_Value * /*val*/)
{
  return new EXP_ErrorValue(op2str(op) + " not defined for lists");
}

void EXP_ArrayValue::Append(EXP_Value *item)
{
  m_items.push_back(item);
}

#ifdef WITH_PYTHON
PyObject *EXP_ArrayValue::ConvertValueToPython()
{
  PyObject *list = PyList_New(m_items.size());
  for (size_t index = 0; index < m_items.size(); index++) {
    PyObject *item = m_items[index]->ConvertValueToPython();
    PyList_SET_ITEM(list, index, item ? item : m_items[index]->GetProxy());
  }
  return list;
}
#endif
