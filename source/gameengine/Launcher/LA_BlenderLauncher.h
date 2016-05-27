#include "LA_Launcher.h"

#include "RAS_Rect.h"

struct Object;
struct bContext;
struct ARegion;
struct View3D;
struct rcti;
struct wmWindowManager;
struct wmWindow;

class LA_BlenderLauncher : public LA_Launcher
{
protected:
	bContext *m_context;
	ARegion *m_ar;
	rcti *m_camFrame;
	View3D *m_view3d;
	wmWindowManager *m_windowManager;
	wmWindow *m_window;
	int m_alwaysUseExpandFraming;
	RAS_Rect m_areaRect;

	struct SavedData {
		int sceneLayer;
		Object *camera;
		int vsync;
		int mipmap;
	} m_savedData;

	virtual bool InitEngine();
	virtual void ExitEngine();

	virtual RAS_ICanvas *CreateCanvas(RAS_IRasterizer *rasty);
	virtual RAS_IRasterizer::DrawType GetRasterizerDrawMode();
	virtual bool GetUseAlwaysExpandFraming();
	virtual void InitCamera();
	virtual void InitPython();
	virtual void ExitPython();

public:
	LA_BlenderLauncher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs, RAS_IRasterizer::StereoMode stereoMode, 
					   int argc, char **argv, bContext *context, rcti *camframe, ARegion *ar, int alwaysUseExpandFraming);
	virtual ~LA_BlenderLauncher();
};
