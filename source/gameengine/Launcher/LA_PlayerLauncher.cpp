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

/** \file gameengine/Launcher/LA_PlayerLauncher.cpp
 *  \ingroup launcher
 */


#ifdef WIN32
#  pragma warning (disable:4786) // suppress stl-MSVC debug info warning
#  include <windows.h>
#endif

#include "LA_PlayerLauncher.h"
#include "LA_SystemCommandLine.h"

extern "C" {
#  include "BKE_sound.h"

#  include "BLI_fileops.h"

#  include "MEM_guardedalloc.h"
}

#include "KX_PythonInit.h"

#include "GPG_Canvas.h"

#include "GHOST_ISystem.h"

#include "DEV_InputDevice.h"

#include "CM_Message.h"

LA_PlayerLauncher::LA_PlayerLauncher(GHOST_ISystem *system, GHOST_IWindow *window, Main *maggie, Scene *scene, GlobalSettings *gs,
                                     RAS_Rasterizer::StereoMode stereoMode, int samples, int argc, char **argv, const std::string& pythonMainLoop)
	:LA_Launcher(system, maggie, scene, gs, stereoMode, samples, false, argc, argv),
	m_mainWindow(window),
	m_pythonMainLoop(pythonMainLoop)
{
}

LA_PlayerLauncher::~LA_PlayerLauncher()
{
}

#ifdef WITH_PYTHON

bool LA_PlayerLauncher::GetPythonMainLoopCode(std::string& pythonCode, std::string& pythonFileName)
{
#ifndef WITH_GAMEENGINE_SECURITY
	if (!m_pythonMainLoop.empty()) {
		if (BLI_is_file(m_pythonMainLoop.c_str())) {
			size_t filesize = 0;
			char *filecontent = (char *)BLI_file_read_text_as_mem(m_pythonMainLoop.c_str(), 0, &filesize);
			pythonCode = std::string(filecontent, filesize);
			MEM_freeN(filecontent);
			pythonFileName = m_pythonMainLoop;
			return true;
		}
		else {
			CM_Error("cannot yield control to Python: no file named '" << m_pythonMainLoop << "'");
			return false;
		}
	}
#endif
	return LA_Launcher::GetPythonMainLoopCode(pythonCode, pythonFileName);
}

void LA_PlayerLauncher::RunPythonMainLoop(const std::string& pythonCode)
{
	/* If a valid python main loop file exists is that we are running it.
	 * Then we put its path in the python include paths. */
	if (!m_pythonMainLoop.empty()) {
		appendPythonPath(m_pythonMainLoop);
	}
	LA_Launcher::RunPythonMainLoop(pythonCode);
}

#endif  // WITH_PYTHON

RAS_Rasterizer::DrawType LA_PlayerLauncher::GetRasterizerDrawMode()
{
	const SYS_SystemHandle& syshandle = SYS_GetSystem();
	const bool wireframe = SYS_GetCommandLineInt(syshandle, "wireframe", 0);

	if (wireframe) {
		return RAS_Rasterizer::RAS_WIREFRAME;
	}

	return RAS_Rasterizer::RAS_TEXTURED;
}

void LA_PlayerLauncher::InitCamera()
{
}

void LA_PlayerLauncher::SetWindowOrder(short order)
{
	m_mainWindow->setOrder((order == 0) ? GHOST_kWindowOrderBottom : GHOST_kWindowOrderTop);
}

void LA_PlayerLauncher::InitEngine()
{
	BKE_sound_init(m_maggie);
	LA_Launcher::InitEngine();

	m_rasterizer->PrintHardwareInfo();
}

void LA_PlayerLauncher::ExitEngine()
{
	LA_Launcher::ExitEngine();
	BKE_sound_exit();
}

KX_ExitInfo LA_PlayerLauncher::EngineNextFrame()
{
	if (m_inputDevice->GetInput(SCA_IInputDevice::WINRESIZE).Find(SCA_InputEvent::ACTIVE)) {
		GHOST_Rect bnds;
		m_mainWindow->getClientBounds(bnds);
		m_canvas->Resize(bnds.getWidth(), bnds.getHeight());
		m_ketsjiEngine->Resize();
		m_inputDevice->ConvertEvent(SCA_IInputDevice::WINRESIZE, 0, 0);
	}

	return LA_Launcher::EngineNextFrame();
}

RAS_ICanvas *LA_PlayerLauncher::CreateCanvas(RAS_Rasterizer *rasty, const RAS_OffScreen::AttachmentList& attachments)
{
	return (new GPG_Canvas(rasty, attachments, m_mainWindow));
}
