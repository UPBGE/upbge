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
 * Blender's Ketsji startpoint
 */

/** \file gameengine/BlenderRoutines/BL_KetsjiEmbedStart.cpp
 *  \ingroup blroutines
 */


#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _MSC_VER
   /* don't show stl-warnings */
#  pragma warning (disable:4786)
#endif

#include "KX_BlenderCanvas.h"

#include "KX_KetsjiEngine.h"
#include "KX_NetworkMessageManager.h"
#include "KX_BlenderSceneConverter.h"
#include "KX_PythonInit.h"
#include "KX_PyConstraintBinding.h"
#include "KX_PythonMain.h"
#include "KX_Globals.h"

#include "RAS_OpenGLRasterizer.h"

#include "BL_BlenderDataConversion.h"

#include "GPU_extensions.h"
#include "EXP_Value.h"

#include "GHOST_ISystem.h"
#include "GH_EventConsumer.h"
#include "GH_InputDevice.h"

#include "LA_BlenderLauncher.h"

extern "C" {
	#include "DNA_object_types.h"
	#include "DNA_view3d_types.h"
	#include "DNA_screen_types.h"
	#include "DNA_userdef_types.h"
	#include "DNA_scene_types.h"
	#include "DNA_windowmanager_types.h"

	#include "BKE_global.h"
	#include "BKE_report.h"
	#include "BKE_ipo.h"
	#include "BKE_main.h"
	#include "BKE_context.h"
	#include "BKE_sound.h"

	/* avoid c++ conflict with 'new' */
	#define new _new
	#include "BKE_screen.h"
	#undef new

	#include "MEM_guardedalloc.h"

	#include "BLI_blenlib.h"
	#include "BLO_readfile.h"

	#include "../../blender/windowmanager/WM_types.h"
	#include "../../blender/windowmanager/wm_window.h"

/* avoid more includes (not used by BGE) */
typedef void * wmUIHandlerFunc;
typedef void * wmUIHandlerRemoveFunc;

	#include "../../blender/windowmanager/wm_event_system.h"
}

#ifdef WITH_AUDASPACE
#  include AUD_DEVICE_H
#endif

static BlendFileData *load_game_data(char *filename)
{
	ReportList reports;
	BlendFileData *bfd;
	
	BKE_reports_init(&reports, RPT_STORE);
	bfd= BLO_read_from_file(filename, &reports);

	if (!bfd) {
		printf("Loading %s failed: ", filename);
		BKE_reports_print(&reports, RPT_ERROR);
	}

	BKE_reports_clear(&reports);

	return bfd;
}

static int BL_KetsjiNextFrame(KX_KetsjiEngine *ketsjiengine, bContext *C, wmWindow *win, Scene *scene,
							  ARegion *ar, GHOST_ISystem *system, int draw_letterbox)
{
	// first check if we want to exit
	int exitrequested = ketsjiengine->GetExitCode();

	// kick the engine
	bool render = ketsjiengine->NextFrame();

	if (render) {
		if (draw_letterbox) {
			RAS_IRasterizer *rasty = ketsjiengine->GetRasterizer();

			// Clear screen to border color
			// We do this here since we set the canvas to be within the frames. This means the engine
			// itself is unaware of the extra space, so we clear the whole region for it.
			rasty->SetClearColor(scene->gm.framing.col[0], scene->gm.framing.col[1], scene->gm.framing.col[2]);
			rasty->SetViewport(ar->winrct.xmin, ar->winrct.ymin,
			           BLI_rcti_size_x(&ar->winrct), BLI_rcti_size_y(&ar->winrct));
			rasty->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT);
		}

		// render the frame
		ketsjiengine->Render();
	}

	system->processEvents(false);
	system->dispatchEvents();

	SCA_IInputDevice *inputDevice = ketsjiengine->GetInputDevice();

	if (inputDevice->GetEvent((SCA_IInputDevice::SCA_EnumInputs)ketsjiengine->GetExitKey()).Find(SCA_InputEvent::KX_ACTIVE)) {
		exitrequested = KX_EXIT_REQUEST_BLENDER_ESC;
	}
	else if (inputDevice->GetEvent(SCA_IInputDevice::KX_WINCLOSE).Find(SCA_InputEvent::KX_ACTIVE) ||
		inputDevice->GetEvent(SCA_IInputDevice::KX_WINQUIT).Find(SCA_InputEvent::KX_ACTIVE))
	{
		exitrequested = KX_EXIT_REQUEST_OUTSIDE;
	}

	// test for the ESC key
	//XXX while (qtest())
	/*while (wmEvent *event= (wmEvent *)win->queue.first) {
		short val = 0;
		//unsigned short event = 0; //XXX extern_qread(&val);
		unsigned int unicode = event->utf8_buf[0] ? BLI_str_utf8_as_unicode(event->utf8_buf) : event->ascii;

		if (keyboarddevice->ConvertBlenderEvent(event->type, event->val, unicode))
			exitrequested = KX_EXIT_REQUEST_BLENDER_ESC;*/

		/* Coordinate conversion... where
		 * should this really be?
		 */
		/*if (event->type == MOUSEMOVE) {*/
			/* Note, not nice! XXX 2.5 event hack */
			/*val = event->x - ar->winrct.xmin;
			mousedevice->ConvertBlenderEvent(MOUSEX, val, 0);

			val = ar->winy - (event->y - ar->winrct.ymin) - 1;
			mousedevice->ConvertBlenderEvent(MOUSEY, val, 0);
		}
		else {
			mousedevice->ConvertBlenderEvent(event->type, event->val, 0);
		}

		BLI_remlink(&win->queue, event);
		wm_event_free(event);
	}*/

	if (win != CTX_wm_window(C)) {
		exitrequested= KX_EXIT_REQUEST_OUTSIDE; /* window closed while bge runs */
	}
	return exitrequested;
}


#ifdef WITH_PYTHON
static struct BL_KetsjiNextFrameState {
	class KX_KetsjiEngine* ketsjiengine;
	struct bContext *C;
	struct wmWindow* win;
	struct Scene* scene;
	struct ARegion *ar;
	GHOST_ISystem *system;
	int draw_letterbox;
} ketsjinextframestate;

static int BL_KetsjiPyNextFrame(void *state0)
{
	BL_KetsjiNextFrameState *state = (BL_KetsjiNextFrameState *) state0;
	return BL_KetsjiNextFrame(
		state->ketsjiengine, 
		state->C, 
		state->win, 
		state->scene, 
		state->ar,
		state->system,
		state->draw_letterbox);
}
#endif


extern "C" void StartKetsjiShell(struct bContext *C, struct ARegion *ar, rcti *cam_frame, int always_use_expand_framing)
{
	/* context values */
	Scene *startscene = CTX_data_scene(C);
	Main* maggie1 = CTX_data_main(C);

	int exitrequested = KX_EXIT_REQUEST_NO_REQUEST;
	Main* blenderdata = maggie1;

	char* startscenename = startscene->id.name + 2;
	char pathname[FILE_MAXDIR+FILE_MAXFILE], oldsce[FILE_MAXDIR + FILE_MAXFILE];
	STR_String exitstring = "";
	BlendFileData *bfd = NULL;

	BLI_strncpy(pathname, blenderdata->name, sizeof(pathname));
	BLI_strncpy(oldsce, G.main->name, sizeof(oldsce));
#ifdef WITH_PYTHON
	resetGamePythonPath(); // need this so running a second time wont use an old blendfiles path
	setGamePythonPath(G.main->name);

	// Acquire Python's GIL (global interpreter lock)
	// so we can safely run Python code and API calls
	PyGILState_STATE gilstate = PyGILState_Ensure();

#endif

	GlobalSettings gs;
	gs.glslflag = startscene->gm.flag;

	do
	{
		// if we got an exitcode 3 (KX_EXIT_REQUEST_START_OTHER_GAME) load a different file
		if (exitrequested == KX_EXIT_REQUEST_START_OTHER_GAME || exitrequested == KX_EXIT_REQUEST_RESTART_GAME) {
			exitrequested = KX_EXIT_REQUEST_NO_REQUEST;
			if (bfd) {
				BLO_blendfiledata_free(bfd);
			}
			
			char basedpath[FILE_MAX];
			// base the actuator filename with respect
			// to the original file working directory

			if (exitstring != "") {
				BLI_strncpy(basedpath, exitstring.ReadPtr(), sizeof(basedpath));
			}

			// load relative to the last loaded file, this used to be relative
			// to the first file but that makes no sense, relative paths in
			// blend files should be relative to that file, not some other file
			// that happened to be loaded first
			BLI_path_abs(basedpath, pathname);
			bfd = load_game_data(basedpath);
			
			// if it wasn't loaded, try it forced relative
			if (!bfd) {
				// just add "//" in front of it
				char temppath[FILE_MAX] = "//";
				BLI_strncpy(temppath + 2, basedpath, FILE_MAX - 2);
				
				BLI_path_abs(temppath, pathname);
				bfd = load_game_data(temppath);
			}
			
			// if we got a loaded blendfile, proceed
			if (bfd) {
				blenderdata = bfd->main;
				startscenename = bfd->curscene->id.name + 2;

				if (blenderdata) {
					BLI_strncpy(G.main->name, blenderdata->name, sizeof(G.main->name));
					BLI_strncpy(pathname, blenderdata->name, sizeof(pathname));
#ifdef WITH_PYTHON
					setGamePythonPath(G.main->name);
#endif
				}
			}
			// else forget it, we can't find it
			else {
				exitrequested = KX_EXIT_REQUEST_QUIT_GAME;
			}
		}

		Scene *scene = bfd ? bfd->curscene : (Scene *)BLI_findstring(&blenderdata->scene, startscenename, offsetof(ID, name) + 2);

		RAS_IRasterizer::StereoMode stereoMode = RAS_IRasterizer::RAS_STEREO_NOSTEREO;
		if (scene) {
			// Quad buffered needs a special window.
			if (scene->gm.stereoflag == STEREO_ENABLED) {
				if (scene->gm.stereomode != RAS_IRasterizer::RAS_STEREO_QUADBUFFERED) {
					stereoMode = (RAS_IRasterizer::StereoMode)scene->gm.stereomode;
				}
			}
		}

		GHOST_ISystem *system = GHOST_ISystem::getSystem();
		LA_BlenderLauncher launcher = LA_BlenderLauncher(system, blenderdata, startscene, &gs, stereoMode, 0, NULL, C, cam_frame, ar, always_use_expand_framing);
		launcher.StartGameEngine();

		bool run = true;
		while (run) {
			run = launcher.EngineNextFrame();
		}

		launcher.StopGameEngine();
	
	} while (exitrequested == KX_EXIT_REQUEST_RESTART_GAME || exitrequested == KX_EXIT_REQUEST_START_OTHER_GAME);
	
	if (bfd) {
		BLO_blendfiledata_free(bfd);
	}

	BLI_strncpy(G.main->name, oldsce, sizeof(G.main->name));

#ifdef WITH_PYTHON

	// Release Python's GIL
	PyGILState_Release(gilstate);
#endif

}
