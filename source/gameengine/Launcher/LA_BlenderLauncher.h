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
	bool m_drawLetterBox;
	RAS_Rect m_areaRect;

	/// Saved blender data to restore at the game end as m_savedData from LA_Launcher.
	struct SavedBlenderData {
		int sceneLayer;
		Object *camera;
	} m_savedBlenderData;

	virtual bool InitEngine();
	virtual void ExitEngine();
	virtual void RenderEngine();

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
