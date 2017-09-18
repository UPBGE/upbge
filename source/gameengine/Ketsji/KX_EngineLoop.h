#ifndef __KX_ENGINE_LOOP_H__
#define __KX_ENGINE_LOOP_H__

#include "EXP_Value.h"

class KX_Scene;
class KX_TimeCategoryLogger;
class KX_KetsjiEngine;

class KX_EngineLoop
{
private:
	KX_KetsjiEngine *m_engine;
	KX_TimeCategoryLogger *m_logger;

	void MergeAsyncLoading();
	void UpdateInputEvents();
	void EndInputEvents();
	void UpdateNetwork();
	void ProcessScheduledScenes();

	void InitEnvironment(KX_Scene *scene);
	void BeginLogic(KX_Scene *scene);
	void UpdateLogic(KX_Scene *scene);
	void EndLogic(KX_Scene *scene);
	void UpdateActivity(KX_Scene *scene);
	void UpdateParents(KX_Scene *scene);
	void UpdatePhysics(KX_Scene *scene);
	void UpdateSuspended(KX_Scene *scene);

public:
	KX_EngineLoop(KX_KetsjiEngine *engine, KX_TimeCategoryLogger *logger);
	virtual ~KX_EngineLoop();

	virtual void NextFrame() = 0;
};

class KX_DefaultEngineLoop : public KX_EngineLoop
{
public:
	virtual void NextFrame();
};

class KX_PythonEngineLoop : public KX_EngineLoop, CValue
{
public:
	virtual void NextFrame();
};

#endif  // __KX_ENGINE_LOOP_H__
