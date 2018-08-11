#ifndef __KX_PYTHON_COMPONENT_H__
#define __KX_PYTHON_COMPONENT_H__

#include <vector>

class KX_GameObject;

class KX_PythonComponentManager
{
private:
	std::vector<KX_GameObject *> m_objects;

public:
	KX_PythonComponentManager();
	~KX_PythonComponentManager();

	void RegisterObject(KX_GameObject *gameobj);
	void UnregisterObject(KX_GameObject *gameobj);

	void UpdateComponents();

	void Merge(KX_PythonComponentManager& other);
};

#endif  // __KX_PYTHON_COMPONENT_H__
