#ifndef __KX_PYTHON_COMPONENT_H__
#define __KX_PYTHON_COMPONENT_H__

#include <vector>

class KX_GameObject;

class KX_LogicManager
{
private:
	std::vector<KX_GameObject *> m_objects;

public:
	KX_LogicManager();
	~KX_LogicManager();

	void RegisterObject(KX_GameObject *gameobj);
	void UnregisterObject(KX_GameObject *gameobj);

	void UpdateComponents();

	void Merge(KX_LogicManager& other);
};

#endif  // __KX_PYTHON_COMPONENT_H__
