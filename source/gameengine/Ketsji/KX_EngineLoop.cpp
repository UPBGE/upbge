#include "KX_EngineLoop.h"
#include "KX_KetsjiEngine.h"
#include "KX_PythonInit.h"
#include "KX_Globals.h"
#include "KX_NetworkMessageManager.h"
#include "BL_BlenderConverter.h"
#include "SCA_IInputDevice.h"
#include "DEV_Joystick.h"

KX_EngineLoop::KX_EngineLoop(KX_KetsjiEngine *engine, KX_TimeCategoryLogger *logger)
	:m_engine(engine),
	m_logger(logger)
{
}

KX_EngineLoop::~KX_EngineLoop()
{
}

void KX_EngineLoop::MergeAsyncLoading()
{
	m_engine->GetConverter()->MergeAsyncLoads();
}

void KX_EngineLoop::UpdateInputEvents()
{
	m_engine->GetInputDevice()->ReleaseMoveEvent();
#ifdef WITH_SDL
	// Handle all SDL Joystick events here to share them for all scenes properly.
	short addrem[JOYINDEX_MAX] = {0};
	if (DEV_Joystick::HandleEvents(addrem)) {
#  ifdef WITH_PYTHON
		updatePythonJoysticks(addrem);
#  endif  // WITH_PYTHON
	}
#endif  // WITH_SDL
}

void KX_EngineLoop::EndInputEvents()
{
	m_engine->GetInputDevice()->ClearInputs();
}

void KX_EngineLoop::UpdateNetwork()
{
	m_engine->GetNetworkMessageManager()->ClearMessages();
}

void KX_EngineLoop::ProcessScheduledScenes()
{
	m_engine->ProcessScheduledScenes();
}

void KX_EngineLoop::InitEnvironment(KX_Scene *scene)
{
	// set Python hooks for each scene
#ifdef WITH_PYTHON
	PHY_SetActiveEnvironment(scene->GetPhysicsEnvironment());
#endif
	KX_SetActiveScene(scene);
}

void KX_EngineLoop::BeginLogic(KX_Scene *scene)
{
	scene->LogicBeginFrame(m_frameTime, framestep);
}

void KX_EngineLoop::UpdateLogic(KX_Scene *scene)
{
	scene->LogicUpdateFrame(m_frameTime);
}

void KX_EngineLoop::EndLogic(KX_Scene *scene)
{
	scene->LogicEndFrame();
}

void KX_EngineLoop::UpdateActivity(KX_Scene *scene)
{
	scene->UpdateObjectActivity();
}

void KX_EngineLoop::UpdateParents(KX_Scene *scene)
{
	scene->UpdateParents(m_frameTime);
}

void KX_EngineLoop::UpdatePhysics(KX_Scene *scene)
{
	/* Perform physics calculations on the scene. This can involve
	 * many iterations of the physics solver. */
	scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, timestep, framestep);
}

void KX_EngineLoop::UpdateSuspended(KX_Scene *scene)
{
	/** Set scene's total pause duration for animations process.
	 * This is done in a separate loop to get the proper state of each scenes.
	 * eg: There's 2 scenes, the first is suspended and the second is active.
	 * If the second scene resume the first, the first scene will be not proceed
	 * in 'NextFrame' for one frame, but set as active.
	 * The render functions, called after and which update animations,
	 * will see the first scene as active and will proceed to it,
	 * but it will cause some negative current frame on actions because of the
	 * total pause duration not set.
	 */ // TODO !!!
	scene->SetSuspendedDelta(scene->GetSuspendedDelta() + framestep);
}

void KX_DefaultEngineLoop::NextFrame()
{
}
