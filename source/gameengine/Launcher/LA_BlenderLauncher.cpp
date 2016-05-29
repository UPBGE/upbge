#include "LA_BlenderLauncher.h"

#include "KX_BlenderCanvas.h"

#include "KX_PythonInit.h"

extern "C" {
#  include "BKE_context.h"

// avoid c++ conflict with 'new'
#  define new _new
#  include "BKE_screen.h"
#  undef new

#  include "DNA_scene_types.h"
#  include "DNA_object_types.h"
#  include "DNA_view3d_types.h"
}

LA_BlenderLauncher::LA_BlenderLauncher(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs, RAS_IRasterizer::StereoMode stereoMode, 
									   int argc, char **argv, bContext *context, rcti *camframe, ARegion *ar, int alwaysUseExpandFraming)
	:LA_Launcher(system, maggie, scene, gs, stereoMode, argc, argv),
	m_context(context),
	m_ar(ar),
	m_camFrame(camframe),
	m_alwaysUseExpandFraming(alwaysUseExpandFraming)
{
	m_windowManager = CTX_wm_manager(m_context);
	m_window = CTX_wm_window(m_context);
	m_view3d = CTX_wm_view3d(m_context);

	m_areaRect.SetLeft(m_camFrame->xmin);
	m_areaRect.SetBottom(m_camFrame->ymin);
	m_areaRect.SetRight(m_camFrame->xmax);
	m_areaRect.SetTop(m_camFrame->ymax);
}

LA_BlenderLauncher::~LA_BlenderLauncher()
{
}

RAS_ICanvas *LA_BlenderLauncher::CreateCanvas(RAS_IRasterizer *rasty)
{
	return (new KX_BlenderCanvas(rasty, m_windowManager, m_window, m_areaRect, m_ar));
}

RAS_IRasterizer::DrawType LA_BlenderLauncher::GetRasterizerDrawMode()
{
	View3D *v3d = CTX_wm_view3d(m_context);

	RAS_IRasterizer::DrawType drawmode = RAS_IRasterizer::RAS_TEXTURED;
	switch(v3d->drawtype) {
		case OB_BOUNDBOX:
		case OB_WIRE:
		{
			drawmode = RAS_IRasterizer::RAS_WIREFRAME;
			break;
		}
		case OB_SOLID:
		{
			drawmode = RAS_IRasterizer::RAS_SOLID;
			break;
		}
		case OB_MATERIAL:
		{
			drawmode = RAS_IRasterizer::RAS_TEXTURED;
			break;
		}
	}
	return drawmode;
}

bool LA_BlenderLauncher::GetUseAlwaysExpandFraming()
{
	return m_alwaysUseExpandFraming;
}

void LA_BlenderLauncher::InitCamera()
{
	RegionView3D *rv3d = CTX_wm_region_view3d(m_context);

	// Some blender stuff.
	float camzoom = 1.0f;
	bool draw_letterbox = false;

	if (rv3d->persp == RV3D_CAMOB) {
		if (m_startScene->gm.framing.type == SCE_GAMEFRAMING_BARS) { /* Letterbox */
			draw_letterbox = true;
		}
		else {
			camzoom = 1.0f / BKE_screen_view3d_zoom_to_fac(rv3d->camzoom);
		}
	}

	m_ketsjiEngine->SetCameraZoom(camzoom);
	m_ketsjiEngine->SetCameraOverrideZoom(2.0f);

	if (rv3d->persp != RV3D_CAMOB) {
		m_ketsjiEngine->EnableCameraOverride(m_startSceneName);
		m_ketsjiEngine->SetCameraOverrideUseOrtho((rv3d->persp == RV3D_ORTHO));
		m_ketsjiEngine->SetCameraOverrideProjectionMatrix(MT_CmMatrix4x4(rv3d->winmat));
		m_ketsjiEngine->SetCameraOverrideViewMatrix(MT_CmMatrix4x4(rv3d->viewmat));
		m_ketsjiEngine->SetCameraOverrideClipping(m_view3d->near, m_view3d->far);
		m_ketsjiEngine->SetCameraOverrideLens(m_view3d->lens);
	}
}


void LA_BlenderLauncher::InitPython()
{
#ifdef WITH_PYTHON

#endif  // WITH_PYTHON
}
void LA_BlenderLauncher::ExitPython()
{
#ifdef WITH_PYTHON

	exitGamePythonScripting();

#endif  // WITH_PYTHON
}

bool LA_BlenderLauncher::InitEngine()
{
	// Lock frame and camera enabled - storing global values.
	m_savedBlenderData.sceneLayer = m_startScene->lay;
	m_savedBlenderData.camera = m_startScene->camera;

	if (m_view3d->scenelock == 0) {
		m_startScene->lay = m_view3d->lay;
		m_startScene->camera = m_view3d->camera;
	}

	return LA_Launcher::InitEngine();
}

void LA_BlenderLauncher::ExitEngine()
{
	LA_Launcher::ExitEngine();

	// Lock frame and camera enabled - restoring global values.
	if (m_view3d->scenelock == 0) {
		m_startScene->lay = m_savedBlenderData.sceneLayer;
		m_startScene->camera= m_savedBlenderData.camera;
	}
}
