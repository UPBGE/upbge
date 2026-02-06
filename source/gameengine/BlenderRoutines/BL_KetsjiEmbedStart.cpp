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

#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLO_readfile.hh"
#include "ED_screen.hh"
#include "WM_api.hh"
#include "WM_keymap.hh"
#include "wm_window.hh"

#include "CM_Message.h"
#include "GHOST_ISystem.hh"
#include "KX_Globals.h"
#include "LA_BlenderLauncher.h"

extern "C" {

void StartKetsjiShell(blender::bContext *C,
                      blender::ARegion *ar,
                      blender::rcti *cam_frame,
                      int always_use_expand_framing);
}

using namespace blender;

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

static void InitBlenderContextVariables(blender::bContext *C, blender::wmWindowManager *wm, blender::Scene *scene)
{
  blender::wmWindow *win = (blender::wmWindow *)wm->windows.first;
  blender::bScreen *screen = WM_window_get_active_screen(win);

  for (blender::ScrArea &sa : screen->areabase) {
    if (sa.spacetype == SPACE_VIEW3D) {
      const ListBaseT<blender::ARegion>&regionbase = sa.regionbase;
      for (blender::ARegion &region : regionbase) {
        if (region.regiontype == RGN_TYPE_WINDOW) {
          if (region.regiondata) {
            CTX_wm_screen_set(C, screen);
            CTX_wm_area_set(C, &sa);
            CTX_wm_region_set(C, &region);
            CTX_data_scene_set(C, scene);
            win->scene = scene;
            return;
          }
        }
      }
    }
  }
}

static int GetShadingTypeRuntime(blender::bContext *C, bool useViewportRender)
{
  blender::View3D *v3d = CTX_wm_view3d(C);
  if (useViewportRender) {
    return v3d->shading.type;
  }
  bool not_eevee = (v3d->shading.type != OB_RENDER) && (v3d->shading.type != OB_MATERIAL);

  if (not_eevee) {
    return OB_RENDER;
  }
  return v3d->shading.type;
}

static void RefreshContextAndScreen(blender::bContext *C, blender::wmWindowManager *wm, blender::wmWindow *win, blender::Scene *scene)
{
  blender::bScreen *screen = WM_window_get_active_screen(win);
  InitBlenderContextVariables(C, wm, scene);

  WM_check(C);
  ED_screen_change(C, screen);
  ED_screen_refresh(C, wm, win);

  ED_screen_areas_iter (win, screen, area_iter) {
    ED_area_tag_redraw(area_iter);
  }
}

/**
 * Free a BlendFileData whose WM and window have borrowed resources
 * (ghostwin, gpuctx, message_bus) from the original WM.
 * Nullifies all borrowed pointers before freeing so that the loaded
 * WM's destructor does not free resources owned by the backup WM.
 */
static void bge_blendfiledata_free(BlendFileData *bfd)
{
  if (bfd->main) {
    blender::wmWindowManager *wm = (blender::wmWindowManager *)bfd->main->wm.first;
    if (wm) {
      /* Null out borrowed runtime pointers so they are not freed
       * when the loaded WM is destroyed. */
      wm->runtime->message_bus = nullptr;

      /* The keyconfigs list owns the same keyconfig objects that defaultconf/addonconf/userconf
       * point to. Clear the entire list so the destructor doesn't free borrowed keyconfigs.
       * This mirrors what wm_file_read_setup_wm_use_new does in wm_files.cc. */
      BLI_listbase_clear(&wm->runtime->keyconfigs);
      wm->runtime->defaultconf = nullptr;
      wm->runtime->addonconf = nullptr;
      wm->runtime->userconf = nullptr;

      for (blender::wmWindow &win : wm->windows) {
        win.runtime->ghostwin = nullptr;
        win.runtime->gpuctx = nullptr;
      }
    }
  }
  BLO_blendfiledata_free(bfd);
}

extern "C" void StartKetsjiShell(blender::bContext *C,
                                 blender::ARegion *ar,
                                 blender::rcti *cam_frame,
                                 int always_use_expand_framing)
{
  /* context values */
  blender::Scene *startscene = CTX_data_scene(C);
  blender::Main *maggie1 = CTX_data_main(C);

  KX_ExitRequest exitrequested = KX_ExitRequest::NO_REQUEST;
  blender::Main *blenderdata = maggie1;

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
    BKE_undosys_step_push(CTX_wm_manager(C)->runtime->undo_stack, C, "bge_start");
    /* Temp hack to fix issue with undo https://github.com/UPBGE/upbge/issues/1516 */
    /* https://github.com/UPBGE/upbge/commit/1b4d5c7a35597a70411515f721a405416244b540 */
    BKE_undosys_step_push(CTX_wm_manager(C)->runtime->undo_stack, C, "pre");
  }

  /* Make sure we'll use old undo at bge exit to properly restore scene
   * See memfile_undo.c and search for keyword UPBGE
   */
  G.is_undo_at_exit = true;

  blender::wmWindowManager *wm_backup = CTX_wm_manager(C);
  blender::wmWindow *win_backup = CTX_wm_window(C);
  void *msgbus_backup = wm_backup->runtime->message_bus;
  void *gpuctx_backup = win_backup->runtime->gpuctx;
  void *ghostwin_backup = win_backup->runtime->ghostwin;

  /* Set Viewport render mode and shading type for the whole runtime */
  bool useViewportRender = startscene->gm.flag & GAME_USE_VIEWPORT_RENDER;
  int shadingTypeRuntime = GetShadingTypeRuntime(C, useViewportRender);
  int shadingTypeBackup = CTX_wm_view3d(C)->shading.type;

  /* Global Undo behaviour change since ebb5643e598a17b2f21b4e50acac35afe82dbd55
   * We now need to backup v3d->camera and restore it manually at ge exit as
   * v3d->camera can be changed during bge pipeline (RenderAfterCameraSetup) */
  blender::Object *backup_cam = nullptr;
  if (CTX_wm_view3d(C)->camera != nullptr) {
    backup_cam = CTX_wm_view3d(C)->camera;
  }

  do {
    // if we got an exitcode 3 (KX_ExitRequest::START_OTHER_GAME) load a different file
    if (exitrequested == KX_ExitRequest::START_OTHER_GAME ||
        exitrequested == KX_ExitRequest::RESTART_GAME) {
      exitrequested = KX_ExitRequest::NO_REQUEST;
      if (bfd) {
        /* Swap G_MAIN back to maggie1 before freeing the loaded data,
         * to avoid leaving G_MAIN as a dangling pointer. */
        BKE_blender_globals_main_swap(maggie1);
        bge_blendfiledata_free(bfd);
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
        BKE_blender_globals_main_swap(bfd->main);
        CTX_data_main_set(C, bfd->main);
        blender::wmWindowManager *wm = (blender::wmWindowManager *)bfd->main->wm.first;
        blender::wmWindow *win = (blender::wmWindow *)wm->windows.first;
        CTX_wm_manager_set(C, wm);
        CTX_wm_window_set(C, win);
        win->runtime->ghostwin = ghostwin_backup;
        win->runtime->gpuctx = gpuctx_backup;
        wm->runtime->message_bus = (wmMsgBus *)msgbus_backup;

        /* Transfer key configurations from backup WM to loaded WM.
         * First free any keyconfigs the loaded WM may already own,
         * then move the entire list from the backup. This mirrors
         * wm_file_read_setup_wm_use_new in wm_files.cc. */
        while (wmKeyConfig *kc = (wmKeyConfig *)BLI_pophead(&wm->runtime->keyconfigs)) {
          WM_keyconfig_free(kc);
        }
        wm->runtime->keyconfigs = wm_backup->runtime->keyconfigs;
        wm->runtime->defaultconf = wm_backup->runtime->defaultconf;
        wm->runtime->addonconf = wm_backup->runtime->addonconf;
        wm->runtime->userconf = wm_backup->runtime->userconf;
        wm->init_flag |= WM_INIT_FLAG_KEYCONFIG;

        wm_window_ghostwindow_embedded_ensure(wm, win);

        /* We need to init Blender blender::bContext environment here
         * to because in embedded, ar, v3d...
         * are needed for launcher creation + Refresh Screen
         * to be able to draw blender areas.
         */
        RefreshContextAndScreen(C, wm, win, bfd->curscene);

        /* ED_screen_init can change blender::bContext then we need to restore it again after...
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

    blender::Scene *scene = bfd ? bfd->curscene :
                         (blender::Scene *)BLI_findstring(
                             &blenderdata->scenes, startscenename, offsetof(blender::ID, name) + 2);

    GHOST_ISystem *system = GHOST_ISystem::getSystem();
    LA_BlenderLauncher launcher = LA_BlenderLauncher(system,
                                                     blenderdata,
                                                     scene,
                                                     &gs,
                                                     RAS_Rasterizer::RAS_STEREO_NOSTEREO,
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
    blender::wmWindowManager *wm = CTX_wm_manager(C);
    WM_jobs_kill_all(wm);

    for (blender::wmWindow &win : wm->windows) {
      CTX_wm_window_set(C, &win); /* needed by operator close callbacks */
      WM_event_remove_handlers(C, &win.runtime->handlers);
      WM_event_remove_handlers(C, &win.runtime->modalhandlers);
      ED_screen_exit(C, &win, WM_window_get_active_screen(&win));
    }
  } while (exitrequested == KX_ExitRequest::RESTART_GAME ||
           exitrequested == KX_ExitRequest::START_OTHER_GAME);

  if (bfd) {
    /* Swap G_MAIN back to maggie1 before freeing the loaded data. */
    BKE_blender_globals_main_swap(maggie1);
    bge_blendfiledata_free(bfd);

    /* Restore context to use the original Main and WM. */
    CTX_data_main_set(C, maggie1);
    CTX_wm_manager_set(C, wm_backup);
  }

  /* Always restore the borrowed resources to the backup WM/window,
   * even if no bfd was loaded (restart of the same file case). */
  win_backup->runtime->ghostwin = ghostwin_backup;
  win_backup->runtime->gpuctx = gpuctx_backup;
  wm_backup->runtime->message_bus = (wmMsgBus *)msgbus_backup;
  CTX_wm_window_set(C, win_backup);  // Fix for crash at exit when we have preferences window open

  RefreshContextAndScreen(C, wm_backup, win_backup, startscene);

  /* ED_screen_init must be called to fix https://github.com/UPBGE/upbge/issues/1388 */
  ED_screens_init(C,maggie1, wm_backup);

  /* ED_screen_init can change blender::bContext then we need to restore it again after...
   * b9907cb60b3c37e55cc8ea186e6cca26e333a039 */
  InitBlenderContextVariables(C, wm_backup, startscene);

  /* Restore shading type we had before game start */
  CTX_wm_view3d(C)->shading.type = shadingTypeBackup;

  /* Restore saved v3d->camera before bge start */
  CTX_wm_view3d(C)->camera = backup_cam;

  /* Undo System */
  if (startscene->gm.flag & GAME_USE_UNDO) {
    UndoStep *step_data_from_name = NULL;
    step_data_from_name = BKE_undosys_step_find_by_name(CTX_wm_manager(C)->runtime->undo_stack,
                                                        "bge_start");
    if (step_data_from_name) {
      BKE_undosys_step_undo_with_data(
          CTX_wm_manager(C)->runtime->undo_stack, C, step_data_from_name);
    }
    else {
      BKE_undosys_step_undo(CTX_wm_manager(C)->runtime->undo_stack, C);
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
