#include "EXP_PythonUtils.h"
#include "EXP_ListWrapper.h"

EXP_ValuePythonOwn::EXP_ValuePythonOwn(EXP_Value *value)
    :m_value(value)
{
}

PyObject *EXP_ValuePythonOwn::GetProxy() const
{
    if (m_value) {
        return m_value->NewProxy(true);
    }

    Py_RETURN_NONE;
}

PyObject *EXP_ConvertToPython(EXP_BaseListWrapper *ptr)
{
	return ptr->NewProxy(true);
}

PyObject *EXP_ConvertToPython(EXP_Value *ptr)
{
	if (ptr) {
		return ptr->GetProxy();
	}
	Py_RETURN_NONE;
}

PyObject *EXP_ConvertToPython(EXP_Value &ptr)
{
    return EXP_ConvertToPython(&ptr);
}

PyObject *EXP_ConvertToPython(const EXP_ValuePythonOwn &ptr)
{
    return ptr.GetProxy();
}
