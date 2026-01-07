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

/** \file LA_BlenderLauncher.h
 *  \ingroup launcher
 */

#include "LA_Launcher.h"
#include "RAS_Rect.h"
namespace blender { struct Object; }
namespace blender { struct bContext; }
namespace blender { struct ARegion; }
namespace blender { struct View3D; }
namespace blender { struct rcti; }
namespace blender { struct wmWindowManager; }
namespace blender { struct wmWindow; }

class LA_BlenderLauncher : public LA_Launcher {
 protected:
  blender::bContext *m_context;
  blender::ARegion *m_ar;
  blender::rcti *m_camFrame;
  blender::View3D *m_view3d;
  blender::wmWindowManager *m_windowManager;
  blender::wmWindow *m_window;
  int m_alwaysUseExpandFraming;
  bool m_drawLetterBox;

  /// Saved blender data to restore at the game end as m_savedData from LA_Launcher.
  struct SavedBlenderData {
    int sceneLayer;
    blender::Object *camera;
  } m_savedBlenderData;

  virtual RAS_ICanvas *CreateCanvas();
  virtual bool GetUseAlwaysExpandFraming();
  virtual void InitCamera();
  virtual void InitPython();
  virtual void ExitPython();

 public:
  LA_BlenderLauncher(GHOST_ISystem *system,
                     blender::Main *maggie,
                     blender::Scene *scene,
                     GlobalSettings *gs,
                     RAS_Rasterizer::StereoMode stereoMode,
                     int argc,
                     char **argv,
                     blender::bContext *context,
                     blender::rcti *camframe,
                     blender::ARegion *ar,
                     int alwaysUseExpandFraming,
                     bool useViewportRender,
                     int shadingTypeRuntime);
  virtual ~LA_BlenderLauncher();

  virtual void InitEngine();
  virtual void ExitEngine();

  virtual bool EngineNextFrame();
};
