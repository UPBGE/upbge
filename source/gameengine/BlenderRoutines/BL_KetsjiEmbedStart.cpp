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

#ifdef _MSC_VER
/* don't show stl-warnings */
#  pragma warning(disable : 4786)
#endif

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLO_readfile.hh"
#include "ED_screen.hh"
#include "GPU_context.hh"
#include "WM_api.hh"
#include "wm_window.hh"

#include "CM_Message.h"
#include "GHOST_ISystem.hh"
#include "KX_Globals.h"
#include "LA_BlenderLauncher.h"

extern "C" {

void StartKetsjiShell(struct bContext *C,
                      struct ARegion *ar,
                      rcti *cam_frame,
                      int always_use_expand_framing);
}

#ifdef WITH_AUDASPACE
#  include <AUD_Device.h>
#endif

static BlendFileData *load_game_data(const char *filename)
{
  ReportList reports;
  BlendFileData *bfd;

  BKE_reports_init(&reports, RPT_STORE);

  BlendFileReadReport breports;
  breports.reports = &reports;

  bfd = BLO_read_from_file(filename, BLO_READ_SKIP_USERDEF, &breports);

  if (!bfd) {
    CM_Error("loading " << filename << " failed: ");
    BKE_reports_print(&reports, RPT_ERROR);
  }

  BKE_reports_free(&reports);

  return bfd;
}

static void InitBlenderContextVariables(bContext *C, wmWindowManager *wm, Scene *scene)
{
  wmWindow *win = (wmWindow *)wm->windows.first;
  bScreen *screen = WM_window_get_active_screen(win);

  LISTBASE_FOREACH (ScrArea *, sa, &screen->areabase) {
    if (sa->spacetype == SPACE_VIEW3D) {
      ListBase *regionbase = &sa->regionbase;
      LISTBASE_FOREACH (ARegion *, region, regionbase) {
        if (region->regiontype == RGN_TYPE_WINDOW) {
          if (region->regiondata) {
            CTX_wm_screen_set(C, screen);
            CTX_wm_area_set(C, sa);
            CTX_wm_region_set(C, region);
            CTX_data_scene_set(C, scene);
            win->scene = scene;
            return;
          }
        }
      }
    }
  }
}

static int GetShadingTypeRuntime(bContext *C, bool useViewportRender)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (useViewportRender) {
    return v3d->shading.type;
  }
  bool not_eevee = (v3d->shading.type != OB_RENDER) && (v3d->shading.type != OB_MATERIAL);

  if (not_eevee) {
    return OB_RENDER;
  }
  return v3d->shading.type;
}

static void RefreshContextAndScreen(bContext *C, wmWindowManager *wm, wmWindow *win, Scene *scene)
{
  bScreen *screen = WM_window_get_active_screen(win);
  InitBlenderContextVariables(C, wm, scene);

  WM_check(C);
  ED_screen_change(C, screen);
  ED_screen_refresh(C, wm, win);

  ED_screen_areas_iter (win, screen, area_iter) {
    ED_area_tag_redraw(area_iter);
  }
}

extern "C" void StartKetsjiShell(struct bContext *C,
                                 struct ARegion *ar,
                                 rcti *cam_frame,
                                 int always_use_expand_framing)
{
  /* context values */
  Scene *startscene = CTX_data_scene(C);
  Main *maggie1 = CTX_data_main(C);

  KX_ExitRequest exitrequested = KX_ExitRequest::NO_REQUEST;
  Main *blenderdata = maggie1;

  char *startscenename = startscene->id.name + 2;
  char pathname[FILE_MAXDIR + FILE_MAXFILE];
  char prevPathName[FILE_MAXDIR + FILE_MAXFILE];
  std::string exitstring = "";
  BlendFileData *bfd = nullptr;

  BLI_strncpy(pathname, blenderdata->filepath, sizeof(pathname));
  BLI_strncpy(prevPathName, G_MAIN->filepath, sizeof(prevPathName));

  /* Without this step, the bmain->name can be ".blend~"
   * and as I don't understand why and as the bug has been
   * reported, we ensure the extension is ".blend"
   * else this is causing issues with globalDict. (youle)
   */
  char *blend_name = blenderdata->filepath;
  BLI_path_extension_ensure(blend_name, FILE_MAX, ".blend");

  KX_SetOrigPath(std::string(blend_name));

#ifdef WITH_PYTHON

  // Acquire Python's GIL (global interpreter lock)
  // so we can safely run Python code and API calls
  PyGILState_STATE gilstate = PyGILState_Ensure();

  PyObject *globalDict = PyDict_New();

#endif

  GlobalSettings gs;
  gs.glslflag = startscene->gm.flag;

  if (startscene->gm.flag & GAME_USE_UNDO) {
    BKE_undosys_step_push(CTX_wm_manager(C)->undo_stack, C, "bge_start");
    /* Temp hack to fix issue with undo https://github.com/UPBGE/upbge/issues/1516 */
    /* https://github.com/UPBGE/upbge/commit/1b4d5c7a35597a70411515f721a405416244b540 */
    BKE_undosys_step_push(CTX_wm_manager(C)->undo_stack, C, "pre");
  }

  /* Make sure we'll use old undo at bge exit to properly restore scene
   * See memfile_undo.c and search for keyword UPBGE
   */
  G.is_undo_at_exit = true;

  wmWindowManager *wm_backup = CTX_wm_manager(C);
  wmWindow *win_backup = CTX_wm_window(C);
  void *msgbus_backup = wm_backup->message_bus;
  void *gpuctx_backup = win_backup->gpuctx;
  void *ghostwin_backup = win_backup->ghostwin;

  /* Set Viewport render mode and shading type for the whole runtime */
  bool useViewportRender = startscene->gm.flag & GAME_USE_VIEWPORT_RENDER;
  int shadingTypeRuntime = GetShadingTypeRuntime(C, useViewportRender);
  int shadingTypeBackup = CTX_wm_view3d(C)->shading.type;

  /* Global Undo behaviour change since ebb5643e598a17b2f21b4e50acac35afe82dbd55
   * We now need to backup v3d->camera and restore it manually at ge exit as
   * v3d->camera can be changed during bge pipeline (RenderAfterCameraSetup) */
  Object *backup_cam = nullptr;
  if (CTX_wm_view3d(C)->camera != nullptr) {
    backup_cam = CTX_wm_view3d(C)->camera;
  }

  do {
    // if we got an exitcode 3 (KX_ExitRequest::START_OTHER_GAME) load a different file
    if (exitrequested == KX_ExitRequest::START_OTHER_GAME ||
        exitrequested == KX_ExitRequest::RESTART_GAME) {
      exitrequested = KX_ExitRequest::NO_REQUEST;
      if (bfd) {
        /* Hack to not free the win->ghosting AND win->gpu_ctx when we restart/load new
         * .blend */
        CTX_wm_window(C)->ghostwin = nullptr;
        /* Hack to not free wm->message_bus when we restart/load new .blend */
        CTX_wm_manager(C)->message_bus = nullptr;
        BLO_blendfiledata_free(bfd);
      }

      char basedpath[FILE_MAX];
      // base the actuator filename with respect
      // to the original file working directory

      if (!exitstring.empty()) {
        BLI_strncpy(basedpath, exitstring.c_str(), sizeof(basedpath));
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

        /* If we don't change G_MAIN, bpy won't work in loaded .blends */
        G_MAIN = G.main = bfd->main;
        CTX_data_main_set(C, bfd->main);
        wmWindowManager *wm = (wmWindowManager *)bfd->main->wm.first;
        wmWindow *win = (wmWindow *)wm->windows.first;
        CTX_wm_manager_set(C, wm);
        CTX_wm_window_set(C, win);
        win->ghostwin = ghostwin_backup;
        win->gpuctx = gpuctx_backup;
        wm->message_bus = (wmMsgBus *)msgbus_backup;

        wm->defaultconf = wm_backup->defaultconf;
        wm->addonconf = wm_backup->addonconf;
        wm->userconf = wm_backup->userconf;
        wm->init_flag |= WM_INIT_FLAG_KEYCONFIG;

        wm_window_ghostwindow_embedded_ensure(wm, win);

        /* We need to init Blender bContext environment here
         * to because in embedded, ar, v3d...
         * are needed for launcher creation + Refresh Screen
         * to be able to draw blender areas.
         */
        RefreshContextAndScreen(C, wm, win, bfd->curscene);

        /* ED_screen_init can change bContext then we need to restore it again after...
         * b9907cb60b3c37e55cc8ea186e6cca26e333a039 */
        InitBlenderContextVariables(C, wm, bfd->curscene);

        if (blenderdata) {
          BLI_strncpy(pathname, blenderdata->filepath, sizeof(pathname));
          // Change G_MAIN path to ensure loading of data using relative paths.
          BLI_strncpy(G_MAIN->filepath, pathname, sizeof(G_MAIN->filepath));
        }
      }
      // else forget it, we can't find it
      else {
        exitrequested = KX_ExitRequest::QUIT_GAME;
      }
    }

    Scene *scene = bfd ? bfd->curscene :
                         (Scene *)BLI_findstring(
                             &blenderdata->scenes, startscenename, offsetof(ID, name) + 2);

    RAS_Rasterizer::StereoMode stereoMode = RAS_Rasterizer::RAS_STEREO_NOSTEREO;
    if (scene) {
      // Quad buffered needs a special window.
      if (scene->gm.stereoflag == STEREO_ENABLED) {
        if (scene->gm.stereomode != RAS_Rasterizer::RAS_STEREO_QUADBUFFERED) {
          switch (scene->gm.stereomode) {
            case STEREO_QUADBUFFERED: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
              break;
            }
            case STEREO_ABOVEBELOW: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;
              break;
            }
            case STEREO_INTERLACED: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_INTERLACED;
              break;
            }
            case STEREO_ANAGLYPH: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;
              break;
            }
            case STEREO_SIDEBYSIDE: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;
              break;
            }
            case STEREO_VINTERLACE: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;
              break;
            }
            case STEREO_3DTVTOPBOTTOM: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;
              break;
            }
          }
        }
      }
    }

    GHOST_ISystem *system = GHOST_ISystem::getSystem();
    LA_BlenderLauncher launcher = LA_BlenderLauncher(system,
                                                     blenderdata,
                                                     scene,
                                                     &gs,
                                                     stereoMode,
                                                     0,
                                                     nullptr,
                                                     C,
                                                     cam_frame,
                                                     CTX_wm_region(C),
                                                     always_use_expand_framing,
                                                     useViewportRender,
                                                     shadingTypeRuntime);
#ifdef WITH_PYTHON
    launcher.SetPythonGlobalDict(globalDict);
#endif  // WITH_PYTHON

    launcher.InitEngine();

    CM_Message(std::endl << "Blender Game Engine Started");
    launcher.EngineMainLoop();
    CM_Message("Blender Game Engine Finished");

    exitrequested = launcher.GetExitRequested();
    exitstring = launcher.GetExitString();
    gs = *launcher.GetGlobalSettings();

    launcher.ExitEngine();

    /* refer to WM_exit_ext() and BKE_blender_free(),
     * these are not called in the player but we need to match some of there behavior here,
     * if the order of function calls or blenders state isn't matching that of blender
     * proper, we may get troubles later on */
    wmWindowManager *wm = CTX_wm_manager(C);
    WM_jobs_kill_all(wm);

    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      CTX_wm_window_set(C, win); /* needed by operator close callbacks */
      WM_event_remove_handlers(C, &win->handlers);
      WM_event_remove_handlers(C, &win->modalhandlers);
      ED_screen_exit(C, win, WM_window_get_active_screen(win));
    }
  } while (exitrequested == KX_ExitRequest::RESTART_GAME ||
           exitrequested == KX_ExitRequest::START_OTHER_GAME);

  if (bfd) {
    /* Hack to not free the win->ghosting AND win->gpu_ctx when we restart/load new
     * .blend */
    CTX_wm_window(C)->ghostwin = nullptr;
    /* Hack to not free wm->message_bus when we restart/load new .blend */
    CTX_wm_manager(C)->message_bus = nullptr;
    BLO_blendfiledata_free(bfd);

    /* Restore Main and Scene used before ge start */
    G_MAIN = G.main = maggie1;
    CTX_data_main_set(C, maggie1);
    CTX_wm_manager_set(C, wm_backup);
    win_backup->ghostwin = ghostwin_backup;
    win_backup->gpuctx = gpuctx_backup;
    wm_backup->message_bus = (wmMsgBus *)msgbus_backup;
  }
  CTX_wm_window_set(C, win_backup);  // Fix for crash at exit when we have preferences window open

  RefreshContextAndScreen(C, wm_backup, win_backup, startscene);

  /* ED_screen_init must be called to fix https://github.com/UPBGE/upbge/issues/1388 */
  ED_screens_init(C,maggie1, wm_backup);

  /* ED_screen_init can change bContext then we need to restore it again after...
   * b9907cb60b3c37e55cc8ea186e6cca26e333a039 */
  InitBlenderContextVariables(C, wm_backup, startscene);

  /* Restore shading type we had before game start */
  CTX_wm_view3d(C)->shading.type = shadingTypeBackup;

  /* Restore saved v3d->camera before bge start */
  CTX_wm_view3d(C)->camera = backup_cam;

  /* Undo System */
  if (startscene->gm.flag & GAME_USE_UNDO) {
    UndoStep *step_data_from_name = NULL;
    step_data_from_name = BKE_undosys_step_find_by_name(CTX_wm_manager(C)->undo_stack,
                                                        "bge_start");
    if (step_data_from_name) {
      BKE_undosys_step_undo_with_data(CTX_wm_manager(C)->undo_stack, C, step_data_from_name);
    }
    else {
      BKE_undosys_step_undo(CTX_wm_manager(C)->undo_stack, C);
    }
  }

  /* Make sure we'll use new undo in viewport because faster */
  G.is_undo_at_exit = false;

#ifdef WITH_PYTHON

  PyDict_Clear(globalDict);
  Py_DECREF(globalDict);

  // Release Python's GIL
  PyGILState_Release(gilstate);
#endif

  // Restore G_MAIN path.
  BLI_strncpy(G_MAIN->filepath, prevPathName, sizeof(G_MAIN->filepath));
}
