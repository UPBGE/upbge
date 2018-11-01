#include "LOG_Object.h"
#include "LOG_Tree.h"
#include "KX_PythonComponent.h"

LOG_Object::LOG_Object()
{
}

LOG_Object::LOG_Object(const LOG_Object& other)
	:m_components(nullptr),
	m_logicTree(nullptr)
{
#ifdef WITH_PYTHON
	if (other.m_components) {
		m_components.reset(other.m_components->GetReplica());

		for (KX_PythonComponent *component : m_components) {
			component->SetObject(this);
		}
	}

	if (other.m_logicTree) {
		m_logicTree.reset(new LOG_Tree(*other.m_logicTree));
		m_logicTree->SetObject(this);
	}
#endif  // WITH_PYTHON
}

LOG_Object::~LOG_Object()
{
}

EXP_ListValue<KX_PythonComponent> *LOG_Object::GetComponents() const
{
	return m_components.get();
}

void LOG_Object::SetComponents(EXP_ListValue<KX_PythonComponent> *components)
{
	m_components.reset(components);
}

LOG_Tree *LOG_Object::GetLogicTree() const
{
	return m_logicTree.get();
}

void LOG_Object::SetLogicTree(LOG_Tree *tree)
{
	m_logicTree.reset(tree);
}

bool LOG_Object::UseLogic() const
{
	return ((m_components != nullptr) || (m_logicTree != nullptr));
}

void LOG_Object::UpdateLogic()
{
	if (m_components) {
		for (KX_PythonComponent *component : m_components) {
			component->Update();
		}
	}

	if (m_logicTree) {
		m_logicTree->Update();
	}
}

#ifdef WITH_PYTHON

PyTypeObject LOG_Object::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_Object",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef LOG_Object::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_Object::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("components", LOG_Object, pyattr_get_components),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyObject *LOG_Object::pyattr_get_components(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Object *self = static_cast<LOG_Object *>(self_v);
	EXP_ListValue<KX_PythonComponent> *components = self->GetComponents();
	return components ? components->GetProxy() : (new EXP_ListValue<KX_PythonComponent>())->NewProxy(true);
}

#endif  // WITH_PYTHON
