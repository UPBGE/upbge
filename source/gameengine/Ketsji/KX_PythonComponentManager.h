#pragma once


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
};

