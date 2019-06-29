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

/** \file LA_PlayerLauncher.h
 *  \ingroup launcher
 */

#ifdef WIN32
#include <wtypes.h>
#endif

#include "LA_Launcher.h"

#include "GHOST_Types.h"

class GHOST_IWindow;

class LA_PlayerLauncher : public LA_Launcher
{
protected:
	/// Main window.
	GHOST_IWindow *m_mainWindow;

	/// Override python script main loop file name.
	std::string m_pythonMainLoop;

#ifdef WITH_PYTHON
	virtual bool GetPythonMainLoopCode(std::string& pythonCode, std::string& pythonFileName);
	virtual void RunPythonMainLoop(const std::string& pythonCode);
#endif  // WITH_PYTHON

	virtual RAS_ICanvas *CreateCanvas(RAS_Rasterizer *rasty, const RAS_OffScreen::AttachmentList& attachments);
	virtual RAS_Rasterizer::DrawType GetRasterizerDrawMode();
	virtual void InitCamera();

	virtual void SetWindowOrder(short order);

public:
	LA_PlayerLauncher(GHOST_ISystem *system, GHOST_IWindow *window, Main *maggie, Scene *scene, GlobalSettings *gs,
					  RAS_Rasterizer::StereoMode stereoMode, int samples, int argc, char **argv, const std::string& pythonMainLoop);
	virtual ~LA_PlayerLauncher();

	virtual void InitEngine();
	virtual void ExitEngine();

	virtual KX_ExitInfo EngineNextFrame();
};
