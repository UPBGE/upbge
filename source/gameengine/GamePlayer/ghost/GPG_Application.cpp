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
 * GHOST Blender Player application implementation file.
 */

/** \file gameengine/GamePlayer/ghost/GPG_Application.cpp
 *  \ingroup player
 */


#ifdef WIN32
#  pragma warning (disable:4786) // suppress stl-MSVC debug info warning
#  include <windows.h>
#endif

#include "GPU_extensions.h"
#include "GPU_init_exit.h"

#include "GPG_Application.h"
#include "BL_BlenderDataConversion.h"

#include <iostream>
#include <BLI_utildefines.h>
#include <stdlib.h>

/**********************************
 * Begin Blender include block
 **********************************/
#ifdef __cplusplus
extern "C"
{
#endif  // __cplusplus
#include "BLI_blenlib.h"
#include "BLO_readfile.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_sound.h"
#include "IMB_imbuf.h"
#include "DNA_scene_types.h"
#ifdef __cplusplus
}
#endif // __cplusplus
/**********************************
 * End Blender include block
 **********************************/

#include "KX_KetsjiEngine.h"

// include files needed by "KX_BlenderSceneConverter.h"
#include "SCA_IActuator.h"
#include "RAS_MeshObject.h"
#include "RAS_OpenGLRasterizer.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"
#include "KX_PyConstraintBinding.h"

#include "KX_NetworkMessageManager.h"
#include "KX_BlenderSceneConverter.h"

#include "GH_InputDevice.h"
#include "GH_EventConsumer.h"

#include "LA_System.h"
#include "LA_SystemCommandLine.h"

#include "GPG_Canvas.h" 

#include "STR_String.h"

#include "GHOST_ISystem.h"
#include "GHOST_IEvent.h"
#include "GHOST_IEventConsumer.h"
#include "GHOST_IWindow.h"
#include "GHOST_Rect.h"

#ifdef WITH_AUDASPACE
#  include AUD_DEVICE_H
#endif

GPG_Application::GPG_Application(GHOST_ISystem *system, Main *maggie, Scene *scene, GlobalSettings *gs,
								 RAS_IRasterizer::StereoMode stereoMode, int argc, char **argv)
	:LA_Launcher(system, maggie, scene, gs, stereoMode, argc, argv)
{
}

GPG_Application::~GPG_Application()
{
}

#ifdef WIN32
#define SCR_SAVE_MOUSE_MOVE_THRESHOLD 15

static HWND found_ghost_window_hwnd;
static GHOST_IWindow* ghost_window_to_find;
static WNDPROC ghost_wnd_proc;
static POINT scr_save_mouse_pos;

static LRESULT CALLBACK screenSaverWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	BOOL close = false;
	switch (uMsg)
	{
		case WM_MOUSEMOVE:
		{ 
			POINT pt; 
			GetCursorPos(&pt);
			LONG dx = scr_save_mouse_pos.x - pt.x;
			LONG dy = scr_save_mouse_pos.y - pt.y;
			if (abs(dx) > SCR_SAVE_MOUSE_MOVE_THRESHOLD
			        || abs(dy) > SCR_SAVE_MOUSE_MOVE_THRESHOLD)
			{
				close = true;
			}
			scr_save_mouse_pos = pt;
			break;
		}
		case WM_LBUTTONDOWN: 
		case WM_MBUTTONDOWN: 
		case WM_RBUTTONDOWN: 
		case WM_KEYDOWN:
			close = true;
	}
	if (close)
		PostMessage(hwnd,WM_CLOSE,0,0);
	return CallWindowProc(ghost_wnd_proc, hwnd, uMsg, wParam, lParam);
}

BOOL CALLBACK findGhostWindowHWNDProc(HWND hwnd, LPARAM lParam)
{
	GHOST_IWindow *p = (GHOST_IWindow*) GetWindowLongPtr(hwnd, GWLP_USERDATA);
	BOOL ret = true;
	if (p == ghost_window_to_find)
	{
		found_ghost_window_hwnd = hwnd;
		ret = false;
	}
	return ret;
}

static HWND findGhostWindowHWND(GHOST_IWindow* window)
{
	found_ghost_window_hwnd = NULL;
	ghost_window_to_find = window;
	EnumWindows(findGhostWindowHWNDProc, NULL);
	return found_ghost_window_hwnd;
}

bool GPG_Application::startScreenSaverPreview(
	HWND parentWindow,
	const bool stereoVisual,
	const GHOST_TUns16 samples)
{
	bool success = false;

	RECT rc;
	if (GetWindowRect(parentWindow, &rc))
	{
		int windowWidth = rc.right - rc.left;
		int windowHeight = rc.bottom - rc.top;
		STR_String title = "";
		GHOST_GLSettings glSettings = {0};

		if (stereoVisual) {
			glSettings.flags |= GHOST_glStereoVisual;
		}
		glSettings.numOfAASamples = samples;

		m_mainWindow = m_system->createWindow(title, 0, 0, windowWidth, windowHeight, GHOST_kWindowStateMinimized,
		                                     GHOST_kDrawingContextTypeOpenGL, glSettings);
		if (!m_mainWindow) {
			printf("error: could not create main window\n");
			exit(-1);
		}

		HWND ghost_hwnd = findGhostWindowHWND(m_mainWindow);
		if (!ghost_hwnd) {
			printf("error: could find main window\n");
			exit(-1);
		}

		SetParent(ghost_hwnd, parentWindow);
		LONG_PTR style = GetWindowLongPtr(ghost_hwnd, GWL_STYLE);
		LONG_PTR exstyle = GetWindowLongPtr(ghost_hwnd, GWL_EXSTYLE);

		RECT adjrc = { 0, 0, windowWidth, windowHeight };
		AdjustWindowRectEx(&adjrc, style, false, exstyle);

		style = (style & (~(WS_POPUP|WS_OVERLAPPEDWINDOW|WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_TILEDWINDOW ))) | WS_CHILD;
		SetWindowLongPtr(ghost_hwnd, GWL_STYLE, style);
		SetWindowPos(ghost_hwnd, NULL, adjrc.left, adjrc.top, 0, 0, SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);

		/* Check the size of the client rectangle of the window and resize the window
		 * so that the client rectangle has the size requested.
		 */
		m_mainWindow->setClientSize(windowWidth, windowHeight);

		success = InitEngine();
		if (success) {
			success = StartEngine();
		}

	}
	return success;
}

bool GPG_Application::startScreenSaverFullScreen(
		int width,
		int height,
		int bpp,int frequency,
		const bool stereoVisual,
		const GHOST_TUns16 samples)
{
	bool ret = startFullScreen(width, height, bpp, frequency, stereoVisual, samples);
	if (ret)
	{
		HWND ghost_hwnd = findGhostWindowHWND(m_mainWindow);
		if (ghost_hwnd != NULL)
		{
			GetCursorPos(&scr_save_mouse_pos);
			ghost_wnd_proc = (WNDPROC) GetWindowLongPtr(ghost_hwnd, GWLP_WNDPROC);
			SetWindowLongPtr(ghost_hwnd,GWLP_WNDPROC, (uintptr_t) screenSaverWindowProc);
		}
	}
	return ret;
}

#endif

bool GPG_Application::startWindow(
        STR_String& title,
        int windowLeft,
        int windowTop,
        int windowWidth,
        int windowHeight,
        const bool stereoVisual,
        const GHOST_TUns16 samples)
{
	GHOST_GLSettings glSettings = {0};
	bool success;
	// Create the main window
	//STR_String title ("Blender Player - GHOST");
	if (stereoVisual)
		glSettings.flags |= GHOST_glStereoVisual;
	glSettings.numOfAASamples = samples;

	m_mainWindow = m_system->createWindow(title, windowLeft, windowTop, windowWidth, windowHeight, GHOST_kWindowStateNormal,
	                                     GHOST_kDrawingContextTypeOpenGL, glSettings);
	if (!m_mainWindow) {
		printf("error: could not create main window\n");
		exit(-1);
	}

	/* Check the size of the client rectangle of the window and resize the window
	 * so that the client rectangle has the size requested.
	 */
	m_mainWindow->setClientSize(windowWidth, windowHeight);
	m_mainWindow->setCursorVisibility(false);

	success = InitEngine();
	if (success) {
		success = StartEngine();
	}
	return success;
}

bool GPG_Application::startEmbeddedWindow(
        STR_String& title,
        const GHOST_TEmbedderWindowID parentWindow,
        const bool stereoVisual,
        const GHOST_TUns16 samples)
{
	GHOST_TWindowState state = GHOST_kWindowStateNormal;
	GHOST_GLSettings glSettings = {0};

	if (stereoVisual)
		glSettings.flags |= GHOST_glStereoVisual;
	glSettings.numOfAASamples = samples;

	if (parentWindow != 0)
		state = GHOST_kWindowStateEmbedded;
	m_mainWindow = m_system->createWindow(title, 0, 0, 0, 0, state,
	                                     GHOST_kDrawingContextTypeOpenGL, glSettings, parentWindow);

	if (!m_mainWindow) {
		printf("error: could not create main window\n");
		exit(-1);
	}
	m_isEmbedded = true;

	bool success = InitEngine();
	if (success) {
		success = StartEngine();
	}
	return success;
}


bool GPG_Application::startFullScreen(
        int width,
        int height,
        int bpp,int frequency,
        const bool stereoVisual,
        const GHOST_TUns16 samples,
        bool useDesktop)
{
	bool success;
	GHOST_TUns32 sysWidth=0, sysHeight=0;
	m_system->getMainDisplayDimensions(sysWidth, sysHeight);
	// Create the main window
	GHOST_DisplaySetting setting;
	setting.xPixels = (useDesktop) ? sysWidth : width;
	setting.yPixels = (useDesktop) ? sysHeight : height;
	setting.bpp = bpp;
	setting.frequency = frequency;

	m_system->beginFullScreen(setting, &m_mainWindow, stereoVisual, samples);
	m_mainWindow->setCursorVisibility(false);
	/* note that X11 ignores this (it uses a window internally for fullscreen) */
	m_mainWindow->setState(GHOST_kWindowStateFullScreen);

	success = InitEngine();
	if (success) {
		success = StartEngine();
	}
	return success;
}

void GPG_Application::InitPython()
{
	
}

void GPG_Application::ExitPython()
{
#ifdef WITH_PYTHON
	exitGamePlayerPythonScripting();
#endif  // WITH_PYTHON
}

bool GPG_Application::InitEngine()
{
	GPU_init();
	BKE_sound_init(m_maggie);
	return LA_Launcher::InitEngine();
}

void GPG_Application::ExitEngine()
{
	GPU_exit();
	BKE_sound_exit();
	LA_Launcher::ExitEngine();
}

RAS_ICanvas *GPG_Application::CreateCanvas(RAS_IRasterizer *rasty)
{
	return (new GPG_Canvas(rasty, m_mainWindow));
}
