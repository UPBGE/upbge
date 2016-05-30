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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file GPG_Application.h
 *  \ingroup player
 *  \brief GHOST Blender Player application declaration file.
 */

#ifdef WIN32
#include <wtypes.h>
#endif

#include "LA_Launcher.h"

#include "GHOST_Types.h"

class GPG_Application : public LA_Launcher
{
protected:
	/// Main window.
	GHOST_IWindow *m_mainWindow;

	virtual RAS_ICanvas *CreateCanvas(RAS_IRasterizer *rasty);
	virtual RAS_IRasterizer::DrawType GetRasterizerDrawMode();
	virtual bool GetUseAlwaysExpandFraming();
	virtual void InitCamera();
	virtual void InitPython();
	virtual void ExitPython();

public:
	GPG_Application(GHOST_ISystem* system, Main *maggie, Scene *scene, GlobalSettings *gs, RAS_IRasterizer::StereoMode stereoMode, int argc, char **argv);
	virtual ~GPG_Application();

	void startWindow(STR_String& title,
	                 int windowLeft, int windowTop,
	                 int windowWidth, int windowHeight,
	                 const bool stereoVisual, const GHOST_TUns16 samples=0);
	void startFullScreen(int width, int height,
	                     int bpp, int frequency,
	                     const bool stereoVisual,
	                     const GHOST_TUns16 samples=0, bool useDesktop=false);
	void startEmbeddedWindow(STR_String& title, const GHOST_TEmbedderWindowID parent_window,
	                         const bool stereoVisual, const GHOST_TUns16 samples=0);
#ifdef WIN32
	void startScreenSaverFullScreen(int width, int height,
	                                int bpp, int frequency,
	                                const bool stereoVisual, const GHOST_TUns16 samples=0);
	void startScreenSaverPreview(HWND parentWindow,
	                             const bool stereoVisual, const GHOST_TUns16 samples=0);
#endif

	virtual void InitEngine();
	virtual void ExitEngine();
};
