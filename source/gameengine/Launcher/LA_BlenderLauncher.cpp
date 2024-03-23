/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Launcher/LA_BlenderLauncher.cpp
 *  \ingroup launcher
 */

#include "LA_BlenderLauncher.h"

#include "BKE_screen.hh"
#include "BLI_rect.h"
#include "DNA_scene_types.h"
#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_state.hh"
#include "WM_api.hh"
#include "wm_event_system.hh"
#include "wm_window.hh"

#include "CM_Message.h"
#include "KX_BlenderCanvas.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"

LA_BlenderLauncher::LA_BlenderLauncher(GHOST_ISystem *system,
                                       Main *maggie,
                                       Scene *scene,
                                       GlobalSettings *gs,
                                       RAS_Rasterizer::StereoMode stereoMode,
                                       int argc,
                                       char **argv,
                                       bContext *context,
                                       rcti *camframe,
                                       ARegion *ar,
                                       int alwaysUseExpandFraming,
                                       bool useViewportRender,
                                       int shadingTypeRuntime)
    : LA_Launcher(system,
                  maggie,
                  scene,
                  gs,
                  stereoMode,
                  scene->gm.aasamples,
                  argc,
                  argv,
                  context,
                  useViewportRender,
                  shadingTypeRuntime),
      m_context(context),
      m_ar(ar),
      m_camFrame(camframe),
      m_alwaysUseExpandFraming(alwaysUseExpandFraming),
      m_drawLetterBox(false)
{
  m_windowManager = CTX_wm_manager(m_context);
  m_window = CTX_wm_window(m_context);
  m_view3d = CTX_wm_view3d(m_context);
  CM_Debug(ar->winx << ", " << ar->winy);
  print_rcti("rcti: ", &ar->winrct);
}

LA_BlenderLauncher::~LA_BlenderLauncher()
{
}

RAS_ICanvas *LA_BlenderLauncher::CreateCanvas()
{
  return (new KX_BlenderCanvas(m_rasterizer, m_windowManager, m_window, m_camFrame, m_ar, m_useViewportRender));
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

  if (rv3d->persp == RV3D_CAMOB) {
    if (m_startScene->gm.framing.type == SCE_GAMEFRAMING_BARS) { /* Letterbox */
      m_drawLetterBox = true;
    }
    else {
      camzoom = 1.0f / BKE_screen_view3d_zoom_to_fac(rv3d->camzoom);
    }
  }

  m_ketsjiEngine->SetCameraZoom(camzoom);
  m_ketsjiEngine->SetCameraOverrideZoom(2.0f);

  if (rv3d->persp != RV3D_CAMOB) {
    RAS_CameraData camdata = RAS_CameraData();
    camdata.m_lens = m_view3d->lens;
    camdata.m_clipstart = m_view3d->clip_start;
    camdata.m_clipend = m_view3d->clip_end;
    camdata.m_perspective = (rv3d->persp != RV3D_ORTHO);

    m_ketsjiEngine->EnableCameraOverride(m_startSceneName,
                                         MT_Matrix4x4(&rv3d->winmat[0][0]),
                                         MT_Matrix4x4(&rv3d->viewmat[0][0]),
                                         camdata);
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

void LA_BlenderLauncher::InitEngine()
{
  // Lock frame and camera enabled - storing global values.
  m_savedBlenderData.sceneLayer = m_startScene->lay;
  m_savedBlenderData.camera = m_startScene->camera;

  if (m_view3d->scenelock == 0) {
    // m_startScene->lay = m_view3d->local_view_uuid;
    m_startScene->camera = m_view3d->camera;
  }

  LA_Launcher::InitEngine();
}

void LA_BlenderLauncher::ExitEngine()
{
  LA_Launcher::ExitEngine();

  // Lock frame and camera enabled - restoring global values.
  if (m_view3d->scenelock == 0) {
    m_startScene->lay = m_savedBlenderData.sceneLayer;
    m_startScene->camera = m_savedBlenderData.camera;
  }

  // Free all window manager events unused.
  wm_event_free_all(m_window);
}

bool LA_BlenderLauncher::EngineNextFrame()
{
  // Free all window manager events unused.
  wm_event_free_all(m_window);

  return LA_Launcher::EngineNextFrame();
}
