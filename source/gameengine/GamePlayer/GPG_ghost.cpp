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
 * Start up of the Blender Player on GHOST.
 */

/** \file gameengine/GamePlayer/GPG_ghost.cpp
 *  \ingroup player
 */

#ifdef __linux__
#  ifdef __alpha__
#    include <signal.h>
#  endif /* __alpha__ */
#endif   /* __linux__ */

#include "BKE_addon.h"
#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_blender_cli_command.hh"
#include "BKE_blendfile.hh"
#include "BKE_brush.hh"
#include "BKE_cachefile.hh"
#include "BKE_callbacks.hh"
#include "BKE_cpp_types.hh"
#include "BKE_global.hh"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_icons.h"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_keyconfig.h"
#include "BKE_lib_remap.hh"
#include "BKE_main.hh"
#include "BKE_mask.h"
#include "BKE_material.hh"
#include "BKE_mball_tessellate.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_particle.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_shader_fx.h"
#include "BKE_sound.h"
#include "BKE_studiolight.h"
#include "BKE_subdiv.hh"
#include "BKE_tracking.h"
#include "BKE_vfont.hh"
#include "BKE_volume.hh"
#include "BKE_workspace.hh"
#include "BLF_api.hh"
#include "BLI_fileops.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_task.h"
#include "BLI_timer.h"
#include "BLO_readfile.hh"
#include "BLO_runtime.hh"
#include "BLT_lang.hh"
#include "BPY_extern_python.hh"
#include "BPY_extern_run.hh"
#include "CLG_log.h"
#include "DEG_depsgraph.hh"
#include "DNA_genfile.h"
#include "DRW_engine.hh"
#include "ED_asset.hh"
#include "ED_datafiles.h"
#include "ED_gpencil_legacy.hh"
#include "ED_grease_pencil.hh"
#include "ED_keyframes_edit.hh"
#include "ED_keyframing.hh"
#include "ED_node.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_util.hh"
#include "GHOST_ISystem.hh"
#include "GHOST_Path-api.hh"
#include "GPU_context.hh"
#include "GPU_init_exit.hh"
#include "GPU_material.hh"
#include "IMB_imbuf.hh"
#include "IMB_thumbs.hh"
#include "MEM_CacheLimiterC-Api.h"
#include "MEM_guardedalloc.h"
#include "MOV_util.hh"
#include "RE_engine.h"
#include "RE_pipeline.h"
#include "RE_texture.h"
#include "RNA_define.hh"
#include "UI_interface.hh"
#include "UI_resources.hh"
#include "WM_api.hh"
#include "wm.hh"
#include "wm_event_system.hh"

#include "wm_message_bus.hh"
#include "wm_surface.hh"
#include "wm_window.hh"
#include "windowmanager/intern/wm_window_private.hh"

#include "CM_Message.h"
#include "KX_Globals.h"
#include "KX_PythonInit.h"
#include "LA_PlayerLauncher.h"
#include "LA_SystemCommandLine.h"

// keep at the bottom for linking
#include "ANIM_keyingsets.hh"

void UV_clipboard_free(void);

#ifdef __APPLE__
extern "C" int GHOST_HACK_getFirstFile(char buf[]);
#endif

#ifdef WIN32
#  include <windows.h>
#  if !defined(DEBUG)
#    include <wincon.h>
#  endif  // !defined(DEBUG)
#  if defined(_MSC_VER) && defined(_M_X64)
#    include <math.h> /* needed for _set_FMA3_enable */
#  endif
#  include "utfconv.hh"
#endif  // WIN32

#ifdef WITH_GAMEENGINE_BPPLAYER
#  include "SpindleEncryption.h"
#endif  // WITH_GAMEENGINE_BPPLAYER

const int kMinWindowWidth = 100;
const int kMinWindowHeight = 100;

/* Previously, boost::split was used but the boost include was causing issues that I didn't managed
 * to solve, and it was also causing Visual studio text highlighting problems in gpg_ghost.cpp,
 * then I choosed to workaround without using boost. The following func has been found on a random
 * blog on the net */
static void custom_split_string(std::vector<std::string> &parts,
                                std::string fullstring,
                                char *separator)
{
  int startIndex = 0, endIndex = 0;
  for (int i = 0; i <= fullstring.size(); i++) {

    // If we reached the end of the word or the end of the input.
    if (fullstring[i] == *separator || i == fullstring.size()) {
      endIndex = i;
      std::string temp;
      temp.append(fullstring, startIndex, endIndex - startIndex);
      parts.push_back(temp);
      startIndex = endIndex + 1;
    }
  }
}

static void mem_error_cb(const char *errorStr)
{
  fprintf(stderr, "%s", errorStr);
  fflush(stderr);
}

#ifdef WIN32
typedef enum {
  SCREEN_SAVER_MODE_NONE = 0,
  SCREEN_SAVER_MODE_PREVIEW,
  SCREEN_SAVER_MODE_SAVER,
  SCREEN_SAVER_MODE_CONFIGURATION,
  SCREEN_SAVER_MODE_PASSWORD,
} ScreenSaverMode;

static ScreenSaverMode scr_saver_mode = SCREEN_SAVER_MODE_NONE;
static HWND scr_saver_hwnd = nullptr;

static BOOL scr_saver_init(int argc, char **argv)
{
  scr_saver_mode = SCREEN_SAVER_MODE_NONE;
  scr_saver_hwnd = nullptr;
  BOOL ret = false;

  int len = ::strlen(argv[0]);
  if (len > 4 && !::stricmp(".scr", argv[0] + len - 4)) {
    scr_saver_mode = SCREEN_SAVER_MODE_CONFIGURATION;
    ret = true;
    if (argc >= 2) {
      if (argc >= 3) {
        scr_saver_hwnd = (HWND)(INT_PTR)::atoi(argv[2]);
      }
      if (!::stricmp("/c", argv[1])) {
        scr_saver_mode = SCREEN_SAVER_MODE_CONFIGURATION;
        if (scr_saver_hwnd == nullptr)
          scr_saver_hwnd = ::GetForegroundWindow();
      }
      else if (!::stricmp("/s", argv[1])) {
        scr_saver_mode = SCREEN_SAVER_MODE_SAVER;
      }
      else if (!::stricmp("/a", argv[1])) {
        scr_saver_mode = SCREEN_SAVER_MODE_PASSWORD;
      }
      else if (!::stricmp("/p", argv[1]) || !::stricmp("/l", argv[1])) {
        scr_saver_mode = SCREEN_SAVER_MODE_PREVIEW;
      }
    }
  }
  return ret;
}

#  define SCR_SAVE_MOUSE_MOVE_THRESHOLD 15

static HWND found_ghost_window_hwnd;
static GHOST_IWindow *ghost_window_to_find;
static WNDPROC ghost_wnd_proc;
static POINT scr_save_mouse_pos;

static LRESULT CALLBACK screenSaverWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  BOOL close = false;
  switch (uMsg) {
    case WM_MOUSEMOVE: {
      POINT pt;
      GetCursorPos(&pt);
      LONG dx = scr_save_mouse_pos.x - pt.x;
      LONG dy = scr_save_mouse_pos.y - pt.y;
      if (abs(dx) > SCR_SAVE_MOUSE_MOVE_THRESHOLD || abs(dy) > SCR_SAVE_MOUSE_MOVE_THRESHOLD) {
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
    PostMessage(hwnd, WM_CLOSE, 0, 0);
  return CallWindowProc(ghost_wnd_proc, hwnd, uMsg, wParam, lParam);
}

BOOL CALLBACK findGhostWindowHWNDProc(HWND hwnd, LPARAM lParam)
{
  GHOST_IWindow *p = (GHOST_IWindow *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
  BOOL ret = true;
  if (p == ghost_window_to_find) {
    found_ghost_window_hwnd = hwnd;
    ret = false;
  }
  return ret;
}

static HWND findGhostWindowHWND(GHOST_IWindow *window)
{
  found_ghost_window_hwnd = nullptr;
  ghost_window_to_find = window;
  LPARAM lParam = 0;
  EnumWindows(findGhostWindowHWNDProc, lParam);
  return found_ghost_window_hwnd;
}

static GHOST_IWindow *startScreenSaverPreview(GHOST_ISystem *system,
                                              HWND parentWindow,
                                              const bool stereoVisual)
{
  RECT rc;
  if (GetWindowRect(parentWindow, &rc)) {
    int windowWidth = rc.right - rc.left;
    int windowHeight = rc.bottom - rc.top;
    const char *title = "";
    GHOST_GPUSettings glSettings = {0};

    if (stereoVisual) {
      glSettings.flags |= GHOST_gpuStereoVisual;
    }
    const eGPUBackendType gpu_backend = GPU_backend_type_selection_get();
    glSettings.context_type = wm_ghost_drawing_context_type(gpu_backend);

    GHOST_IWindow *window = system->createWindow(title,
                                                 0,
                                                 0,
                                                 windowWidth,
                                                 windowHeight,
                                                 GHOST_kWindowStateMinimized,
                                                 glSettings);
    if (!window) {
      CM_Error("could not create main window");
      exit(-1);
    }

    HWND ghost_hwnd = findGhostWindowHWND(window);
    if (!ghost_hwnd) {
      CM_Error("could find main window");
      exit(-1);
    }

    SetParent(ghost_hwnd, parentWindow);
    LONG_PTR style = GetWindowLongPtr(ghost_hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtr(ghost_hwnd, GWL_EXSTYLE);

    RECT adjrc = {0, 0, windowWidth, windowHeight};
    AdjustWindowRectEx(&adjrc, style, false, exstyle);

    style = (style & (~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_TILEDWINDOW))) |
            WS_CHILD;
    SetWindowLongPtr(ghost_hwnd, GWL_STYLE, style);
    SetWindowPos(ghost_hwnd,
                 nullptr,
                 adjrc.left,
                 adjrc.top,
                 0,
                 0,
                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);

    /* Check the size of the client rectangle of the window and resize the window
     * so that the client rectangle has the size requested.
     */
    const float pixelsize = window->getNativePixelSize();
    window->setClientSize(windowWidth / pixelsize, windowHeight / pixelsize);

    return window;
  }

  return nullptr;
}

#endif  // WIN32

static GHOST_IWindow *startFullScreen(GHOST_ISystem *system,
                                      int width,
                                      int height,
                                      int bpp,
                                      int frequency,
                                      const bool stereoVisual,
                                      const int alphaBackground,
                                      bool useDesktop)
{
  uint32_t sysWidth = 0, sysHeight = 0;
  system->getMainDisplayDimensions(sysWidth, sysHeight);
  // Create the main window
  GHOST_DisplaySetting setting;
  setting.xPixels = (useDesktop) ? sysWidth : width;
  setting.yPixels = (useDesktop) ? sysHeight : height;
  setting.bpp = bpp;
  setting.frequency = frequency;

  GHOST_GPUSettings gpu_settings = {0};
  if (stereoVisual) {
    gpu_settings.flags |= GHOST_gpuStereoVisual;
  }

  const eGPUBackendType gpu_backend = GPU_backend_type_selection_get();
  gpu_settings.context_type = wm_ghost_drawing_context_type(gpu_backend);

  GHOST_IWindow *window = nullptr;
  system->beginFullScreen(setting, &window, gpu_settings);
  window->setCursorVisibility(false);
  /* note that X11 ignores this (it uses a window internally for fullscreen) */
  window->setState(GHOST_kWindowStateFullScreen);

  return window;
}

#ifdef WIN32

static GHOST_IWindow *startScreenSaverFullScreen(GHOST_ISystem *system,
                                                 int width,
                                                 int height,
                                                 int bpp,
                                                 int frequency,
                                                 const bool stereoVisual,
                                                 const int alphaBackground)
{
  GHOST_IWindow *window = startFullScreen(
      system, width, height, bpp, frequency, stereoVisual, alphaBackground, 0);
  HWND ghost_hwnd = findGhostWindowHWND(window);
  if (ghost_hwnd != nullptr) {
    GetCursorPos(&scr_save_mouse_pos);
    ghost_wnd_proc = (WNDPROC)GetWindowLongPtr(ghost_hwnd, GWLP_WNDPROC);
    SetWindowLongPtr(ghost_hwnd, GWLP_WNDPROC, (uintptr_t)screenSaverWindowProc);
  }

  return window;
}

#endif  // WIN32

static GHOST_IWindow *startWindow(GHOST_ISystem *system,
                                  const char *title,
                                  int windowLeft,
                                  int windowTop,
                                  int windowWidth,
                                  int windowHeight,
                                  const bool stereoVisual,
                                  const int alphaBackground)
{
  GHOST_GPUSettings glSettings = {0};
  // Create the main window
  // std::string title ("Blender Player - GHOST");
  if (stereoVisual)
    glSettings.flags |= GHOST_gpuStereoVisual;

  const eGPUBackendType gpu_backend = GPU_backend_type_selection_get();
  glSettings.context_type = wm_ghost_drawing_context_type(gpu_backend);

  GHOST_IWindow *window = system->createWindow(title,
                                               windowLeft,
                                               windowTop,
                                               windowWidth,
                                               windowHeight,
                                               GHOST_kWindowStateNormal,
                                               glSettings);
  if (!window) {
    CM_Error("could not create main window");
    exit(-1);
  }

  /* Check the size of the client rectangle of the window and resize the window
   * so that the client rectangle has the size requested.
   */
  const float pixelsize = window->getNativePixelSize();
  window->setClientSize(windowWidth / pixelsize, windowHeight / pixelsize);
  window->setCursorVisibility(false);

  return window;
}

static GHOST_IWindow *startEmbeddedWindow(GHOST_ISystem *system,
                                          const char *title,
                                          const GHOST_TEmbedderWindowID parentWindow,
                                          const bool stereoVisual,
                                          const int alphaBackground)
{
  GHOST_TWindowState state = GHOST_kWindowStateNormal;
  GHOST_GPUSettings glSettings = {0};

  if (stereoVisual)
    glSettings.flags |= GHOST_gpuStereoVisual;

  /*if (parentWindow != 0)
    state = GHOST_kWindowStateEmbedded;*/

  const eGPUBackendType gpu_backend = GPU_backend_type_selection_get();
  glSettings.context_type = wm_ghost_drawing_context_type(gpu_backend);

  GHOST_IWindow *window = system->createWindow(
      title, 0, 0, 0, 0, state, glSettings, false, parentWindow);

  if (!window) {
    CM_Error("could not create main window");
    exit(-1);
  }

  return window;
}

static void usage(const std::string &program, bool isBlenderPlayer)
{
  std::string example_filename = "";
  std::string example_pathname = "";

#ifdef _WIN32
  const std::string consoleoption = "[-c] ";
#else
  const std::string consoleoption = "";
#endif

  if (isBlenderPlayer) {
    example_filename = "filename.blend";
#ifdef _WIN32
    example_pathname = "c:\\";
#else
    example_pathname = "/home/user/";
#endif
  }
  CM_Message(std::endl)
      CM_Message("usage:   " << program << " [--options] " << example_filename << std::endl);
  CM_Message("Available options are: [-w [w h l t]] [-f [fw fh fb ff]] "
             << consoleoption << "[-g gamengineoptions] "
             << "[-s stereomode] [-m aasamples]");
  CM_Message("Optional parameters must be passed in order.");
  CM_Message("Default values are set in the blend file." << std::endl);
  CM_Message("  -h: Prints this command summary" << std::endl);
  CM_Message("  -w: display in a window");
  CM_Message("       --Optional parameters--");
  CM_Message("       w = window width");
  CM_Message("       h = window height");
  CM_Message("       l = window left coordinate");
  CM_Message("       t = window top coordinate");
  CM_Message("       Note: To define 'w' or 'h', both must be used."
             << "Also, to define 'l' or 't', all four parameters must be used.");
  CM_Message("       Example: -w   or  -w 500 300  or  -w 500 300 0 0" << std::endl);
  CM_Message("  -f: start game in fullscreen mode");
  CM_Message("       --Optional parameters--");
  CM_Message("       fw = fullscreen mode pixel width    (use 0 to detect automatically)");
  CM_Message("       fh = fullscreen mode pixel height   (use 0 to detect automatically)");
  CM_Message(
      "       fb = fullscreen mode bits per pixel (default unless set in the blend file: 32)");
  CM_Message(
      "       ff = fullscreen mode frequency      (default unless set in the blend file: 60)");
  CM_Message("       Note: To define 'fw'' or 'fh'', both must be used.");
  CM_Message("       Example: -f  or  -f 1024 768  or  -f 0 0 16  or  -f 1024 728 16 30"
             << std::endl);
  CM_Message("  -s: start player in stereoscopy mode (requires 3D capable hardware)");
  CM_Message(
      "       stereomode: nostereo         (default unless stereo is set in the blend file)");
  CM_Message("                   anaglyph         (Red-Blue glasses)");
  CM_Message("                   sidebyside       (Left Right)");
  CM_Message("                   syncdoubling     (Above Below)");
  CM_Message("                   3dtvtopbottom    (Squashed Top-Bottom for passive glasses)");
  CM_Message("                   interlace        (Interlace horizontally)");
  CM_Message("                   vinterlace       (Vertical interlace for autostereo display)");
  CM_Message("                   hwpageflip       (Quad buffered shutter glasses)");
  CM_Message("       Example: -s sidebyside  or  -s vinterlace" << std::endl);
  CM_Message("  -m: maximum anti-aliasing (eg. 2,4,8,16)" << std::endl);
  CM_Message("  -n: maximum anisotropic filtering (eg. 2,4,8,16)" << std::endl);
  CM_Message("  -i: parent window's ID" << std::endl);
#ifdef _WIN32
  CM_Message("  -c: keep console window open" << std::endl);
#endif
  CM_Message("  -d: debugging options:");
  CM_Message("       memory        Debug memory leaks");
  CM_Message("       gpu           Debug gpu error and warnings" << std::endl);
  CM_Message("  -g: game engine options:" << std::endl);
  CM_Message("       Name                       Default      Description");
  CM_Message("       ------------------------------------------------------------------------");
  CM_Message("       fixedtime                      0         \"Enable all frames\"");
  CM_Message("       wireframe                      0         Wireframe render");
  CM_Message("       show_framerate                 0         Show the frame rate");
  CM_Message("       show_properties                0         Show debug properties");
  CM_Message("       show_profile                   0         Show profiling information");
  CM_Message("       show_bounding_box              0         Show debug bounding box volume");
  CM_Message("       show_armatures                 0         Show debug armatures");
  CM_Message("       show_camera_frustum            0         Show debug camera frustum volume");
  CM_Message(
      "       show_shadow_frustum            0         Show debug light shadow frustum volume");
  CM_Message("       ignore_deprecation_warnings    1         Ignore deprecation warnings"
             << std::endl);
  CM_Message("  -p: override python main loop script");
  CM_Message(std::endl);
  CM_Message(
      "  - : all arguments after this are ignored, allowing python to access them from sys.argv");
  CM_Message(std::endl);
  CM_Message("example: " << program << " -w 320 200 10 10 -g noaudio " << example_pathname
                         << example_filename);
  CM_Message("example: " << program << " -g show_framerate = 0 " << example_pathname
                         << example_filename);
  CM_Message("example: " << program << " -i 232421 -m 16 " << example_pathname
                         << example_filename);
}

static void get_filename(int argc, char **argv, char *filename)
{
#ifdef __APPLE__
  /* On Mac we park the game file (called game.blend) in the application bundle.
   * The executable is located in the bundle as well.
   * Therefore, we can locate the game relative to the executable.
   */
  int srclen = ::strlen(argv[0]);
  int len = 0;
  char *gamefile = nullptr;

  filename[0] = '\0';

  if (argc > 1) {
    if (BLI_exists(argv[argc - 1])) {
      BLI_strncpy(filename, argv[argc - 1], FILE_MAX);
    }
    if (::strncmp(argv[argc - 1], "-psn_", 5) == 0) {
      static char firstfilebuf[512];
      if (GHOST_HACK_getFirstFile(firstfilebuf)) {
        BLI_strncpy(filename, firstfilebuf, FILE_MAX);
      }
    }
  }

  srclen -= ::strlen("MacOS/Blenderplayer");
  if (srclen > 0) {
    len = srclen + ::strlen("Resources/game.blend");
    gamefile = new char[len + 1];
    ::strcpy(gamefile, argv[0]);
    ::strcpy(gamefile + srclen, "Resources/game.blend");

    if (BLI_exists(gamefile))
      BLI_strncpy(filename, gamefile, FILE_MAX);

    delete[] gamefile;
  }

#else
  filename[0] = '\0';

  if (argc > 1)
    BLI_strncpy(filename, argv[argc - 1], FILE_MAX);
#endif  // !_APPLE
}

static BlendFileData *load_game_data(const char *progname,
                                     char *filename = nullptr,
                                     char *relativename = nullptr)
{
  ReportList reports;
  BlendFileData *bfd = nullptr;

  BKE_reports_init(&reports, RPT_STORE);

  BlendFileReadReport breports;
  breports.reports = &reports;

  /* try to load ourself, will only work if we are a runtime */
  if (BLO_is_a_runtime(progname)) {
    bfd = BLO_read_runtime(progname, &breports);
    if (bfd) {
      bfd->type = BLENFILETYPE_RUNTIME;
      BLI_strncpy(bfd->main->filepath, progname, sizeof(bfd->main->filepath));
    }
  }
  else {
    bfd = BLO_read_from_file(progname, BLO_READ_SKIP_NONE, &breports);
  }

  if (!bfd && filename) {
    bfd = load_game_data(filename);
    if (!bfd) {
      CM_Error("loading " << filename << " failed: ");
      BKE_reports_print(&reports, RPT_ERROR);
    }
  }

  BKE_reports_free(&reports);

  return bfd;
}

/// Return true when the exit code ask to quit the engine.
static bool quitGame(KX_ExitRequest exitcode)
{
  // Exit the game engine if we are not restarting the game or loading an other file.
  return (exitcode != KX_ExitRequest::RESTART_GAME &&
          exitcode != KX_ExitRequest::START_OTHER_GAME);
}

#ifdef WITH_GAMEENGINE_BPPLAYER

static BlendFileData *load_encrypted_game_data(const char *filename, std::string encryptKey)
{
  ReportList reports;
  BlendFileData *bfd = NULL;
  char *fileData = NULL;
  int fileSize;
  std::string localPath(SPINDLE_GetFilePath());
  BKE_reports_init(&reports, RPT_STORE);

  if (filename == NULL) {
    return NULL;
  }

  if (!localPath.empty() && !encryptKey.empty()) {
    // Load file and decrypt.
    fileData = SPINDLE_DecryptFromFile(filename, &fileSize, encryptKey.c_str(), 0);
  }

  if (fileData) {
    bfd = BLO_read_from_memory(fileData, fileSize, BLO_READ_SKIP_USERDEF, &reports);
    delete[] fileData;
  }

  if (!bfd) {
    BKE_reports_print(&reports, RPT_ERROR);
  }

  BKE_reports_clear(&reports);
  return bfd;
}

#endif  // WITH_GAMEENGINE_BPPLAYER

static void callback_clg_fatal(void *fp)
{
  BLI_system_backtrace((FILE *)fp);
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

static int GetShadingTypeRuntime(bContext *C)
{
  View3D *v3d = CTX_wm_view3d(C);
  bool not_eevee = (v3d->shading.type != OB_RENDER) && (v3d->shading.type != OB_MATERIAL);

  if (not_eevee) {
    return OB_RENDER;
  }
  return v3d->shading.type;
}

int main(int argc,
#ifdef WIN32
         char **argv_c
#else
         char **argv
#endif
)
{
  int i;
  int argc_py_clamped = argc; /* use this so python args can be added after ' - ' */
  bool error = false;
  SYS_SystemHandle syshandle = SYS_GetSystem();
  bool fullScreen = false;
  bool fullScreenParFound = false;
  bool windowParFound = false;
#ifdef WIN32
  bool closeConsole = false; // Changed for testing (best to leave console opened for debugging)
#endif

#ifdef WITH_GAMEENGINE_BPPLAYER
  bool useLocalPath = false;
  std::string hexKey;
#endif  // WITH_GAMEENGINE_BPPLAYER
  RAS_Rasterizer::StereoMode stereomode = RAS_Rasterizer::RAS_STEREO_NOSTEREO;
  bool stereoWindow = false;
  bool stereoParFound = false;
  int windowLeft = 100;
  int windowTop = 100;
  int windowWidth = 640;
  int windowHeight = 480;
  uint32_t fullScreenWidth = 0;
  uint32_t fullScreenHeight = 0;
  GHOST_IWindow *window = nullptr;
  int fullScreenBpp = 32;
  int fullScreenFrequency = 60;
  GHOST_TEmbedderWindowID parentWindow = 0;
  bool isBlenderPlayer =
      false;  // true when lauching from blender or command line. false for bundled player
  int validArguments = 0;
  bool samplesParFound = false;
  std::string pythonControllerFile;
  uint16_t aasamples = 0;
  int alphaBackground = 0;

#ifdef WIN32
  char **argv;
  int argv_num;

  /* We delay loading of openmp so we can set the policy here. */
#  if defined(_MSC_VER)
  _putenv_s("OMP_WAIT_POLICY", "PASSIVE");
#  endif

  /* FMA3 support in the 2013 CRT is broken on Vista and Windows 7 RTM (fixed in SP1). Just disable
   * it. */
#  if defined(_MSC_VER) && defined(_M_X64)
  _set_FMA3_enable(0);
#  endif

  /* Win32 Unicode Args */
  /* NOTE: cannot use guardedalloc malloc here, as it's not yet initialized
   *       (it depends on the args passed in, which is what we're getting here!)
   */
  {
    wchar_t **argv_16 = CommandLineToArgvW(GetCommandLineW(), &argc);
    argv = (char **)malloc(argc * sizeof(char *));
    for (argv_num = 0; argv_num < argc; argv_num++) {
      argv[argv_num] = alloc_utf_8_from_16(argv_16[argv_num], 0);
    }
    LocalFree(argv_16);
  }
#endif /* WIN32 */

#ifdef __linux__
#  ifdef __alpha__
  signal(SIGFPE, SIG_IGN);
#  endif /* __alpha__ */
#endif   /* __linux__ */

  //GPU_backend_type_selection_set(GPU_BACKEND_VULKAN);

  BlendFileData *bfd = nullptr;

  /* creator.c */

  /* Initialize logging */
  CLG_init();
  CLG_fatal_fn_set(callback_clg_fatal);

  bContext *C = CTX_create();

  /* Initialize path to executable. */
  BKE_appdir_program_path_init(argv[0]);

  BKE_tempdir_init(nullptr);
  BLI_threadapi_init();

  DNA_sdna_current_init();

  BKE_blender_globals_init(); /* blender.c */

  MEM_CacheLimiter_set_disabled(true);

  BKE_cpp_types_init();
  BKE_idtype_init();
  BKE_cachefiles_init();
  BKE_modifier_init();
  BKE_shaderfx_init();
  BKE_volumes_init();
  DEG_register_node_types();

  BKE_brush_system_init();
  RE_texture_rng_init();

  BKE_callback_global_init();


  /* After parsing #ARG_PASS_ENVIRONMENT such as `--env-*`,
   * since they impact `BKE_appdir` behavior. */
  BKE_appdir_init();

  /* After parsing number of threads argument. */
  BLI_task_scheduler_init();

  /* Initialize sub-systems that use `BKE_appdir.h`. */
  IMB_init();

  /* Keep after #ARG_PASS_SETTINGS since debug flags are checked. */
  MOV_init();

  RNA_init();

  RE_engines_init();
  blender::bke::node_system_init();
  BKE_particle_init_rng();

  BKE_sound_init_once();

  // Initialize a default material for meshes without materials.
  BKE_materials_init();

  /* wm_init_exit */
  GHOST_CreateSystemPaths();

  BKE_addon_pref_type_init();
  BKE_keyconfig_pref_type_init();

  wm_operatortypes_register();

  WM_paneltype_init(); /* Lookup table only. */
  WM_menutype_init();
  WM_uilisttype_init();
  wm_gizmotype_init();
  wm_gizmogrouptype_init();

  ED_undosys_type_init();

  BKE_library_callback_free_notifier_reference_set(
      WM_main_remove_notifier_reference);                    /* lib_id.c */
  BKE_region_callback_free_gizmomap_set(wm_gizmomap_remove); /* screen.c */
  BKE_region_callback_refresh_tag_gizmomap_set(WM_gizmomap_tag_refresh);
  BKE_library_callback_remap_editor_id_reference_set(
      WM_main_remap_editor_id_reference);                     /* lib_id.c */
  BKE_spacedata_callback_id_remap_set(ED_spacedata_id_remap_single); /* screen.c */
  DEG_editors_set_update_cb(ED_render_id_flush_update, ED_render_scene_update);

  ED_spacetypes_init(); /* editors/space_api/spacetype.c */

  ED_node_init_butfuncs();

  BLF_init(); /* Please update source/gamengine/GamePlayer/GPG_ghost.cpp if you change this */

  BLT_lang_init();
  /* Must call first before doing any '.blend' file reading,
   * since versioning code may create new IDs... See T57066. */
  BLT_lang_set("");

  /* Init icons before reading .blend files for preview icons, which can
   * get triggered by the depsgraph. This is also done in background mode
   * for scripts that do background processing with preview icons. */
  BKE_icons_init(BIFICONID_LAST_STATIC);
  BKE_preview_images_init();

  WM_msgbus_types_init();

  /* Studio-lights needs to be init before we read the home-file,
   * otherwise the versioning cannot find the default studio-light. */
  BKE_studiolight_init();

  BLI_assert((G.fileflags & G_FILE_NO_UI) == 0);

  /* We skip this in blenderplayer
   * We'll just init userpreferences
   * a bit later
   */
#if 0
  wm_homefile_read(C,
                   NULL,
                   G.factory_startup,
                   false,
                   use_data,
                   use_userdef,
                   NULL,
                   WM_init_state_app_template_get(),
                   &is_factory_startup);

  /* Call again to set from userpreferences... */
  BLT_lang_set(NULL);
#endif

  ED_file_init(); /* for fsmenu */

  /* For WM_init_opengl and UI_init, we need to initialize later in blenderplayer pipeline */

#if 0
    if (!G.background) {

#  ifdef WITH_INPUT_NDOF
    /* sets 3D mouse deadzone */
    WM_ndof_deadzone_set(U.ndof_deadzone);
#  endif
    WM_init_opengl();

    if (!WM_platform_support_perform_checks()) {
      exit(-1);
    }

    UI_init();
  }
#endif

  blender::bke::subdiv::init();

  ED_spacemacros_init();

  /* The following code is done in InitGamePlayerPythonScripting */

#if 0
   /* note: there is a bug where python needs initializing before loading the
   * startup.blend because it may contain PyDrivers. It also needs to be after
   * initializing space types and other internal data.
   *
   * However cant redo this at the moment. Solution is to load python
   * before wm_homefile_read() or make py-drivers check if python is running.
   * Will try fix when the crash can be repeated. - campbell. */

#  ifdef WITH_PYTHON
  BPY_python_start(C, argc, argv);
  BPY_python_reset(C);
#  else
  (void)argc; /* unused */
  (void)argv; /* unused */
#  endif

  if (!G.background && !wm_start_with_console) {
    GHOST_toggleConsole(3);
  }
#endif

  ED_render_clear_mtex_copybuf();

  /* Skip wm_init_exit code here
   * in blenderplayer
   */

  /* We load our own G_MAIN in blenderplayer,
   * so free the one that BKE_blender_globals_init() gives us
   */
  //BKE_main_free(G_MAIN);
  //G_MAIN = nullptr;

  /* background render uses this font too */
  BKE_vfont_builtin_register(datatoc_bfont_pfb, datatoc_bfont_pfb_size);

  const bool unique = false;
  BLF_load_default(unique);
  if (blf_mono_font == -1)
    blf_mono_font = BLF_load_mono_default(true);

    // Parse command line options
#if defined(DEBUG)
  CM_Debug("argv[0] = '" << argv[0] << "'");
#endif

#ifdef WIN32
  if (scr_saver_init(argc, argv)) {
    switch (scr_saver_mode) {
      case SCREEN_SAVER_MODE_CONFIGURATION:
        MessageBox(scr_saver_hwnd,
                   "This screen saver has no options that you can set",
                   "Screen Saver",
                   MB_OK);
        break;
      case SCREEN_SAVER_MODE_PASSWORD:
        /* This is W95 only, which we currently do not support.
         * Fall-back to normal screen saver behavior in that case... */
      case SCREEN_SAVER_MODE_SAVER:
        fullScreen = true;
        fullScreenParFound = true;
        break;

      case SCREEN_SAVER_MODE_PREVIEW:
      case SCREEN_SAVER_MODE_NONE:
        /* This will actually be handled somewhere below... */
        break;
    }
  }
#endif

  /* Try to load existing user preferences from config folder:
   * Either from exported Game folder, either from blender userprefs file
   */
  char filepath_userdef[FILE_MAX] = "";
  UserDef *userdef = nullptr;

  const std::optional<std::string> cfgdir = BKE_appdir_folder_id(BLENDER_USER_CONFIG, nullptr);
  if (cfgdir.has_value()) {
    BLI_path_join(
        filepath_userdef, sizeof(filepath_userdef), cfgdir->c_str(), BLENDER_USERPREF_FILE);

    /* load preferences */
    if (BLI_exists(filepath_userdef)) {
      userdef = BKE_blendfile_userdef_read(filepath_userdef, nullptr);
    }
  }

  if (!userdef) {
    userdef = BKE_blendfile_userdef_from_defaults();
  }

  BKE_blender_userdef_data_set_and_free(userdef);
  userdef = nullptr;

  GPU_backend_type_selection_set(eGPUBackendType(U.gpu_backend));

  /* Call again to set from userpreferences... */
  BLT_lang_set("");

  /* if running blenderplayer the last argument can't be parsed since it has to be the filename.
   * else it is bundled */
  isBlenderPlayer = !BLO_is_a_runtime(argv[0]);
  if (isBlenderPlayer)
    validArguments = argc - 1;
  else
    validArguments = argc;

    /* Parsing command line arguments (can be set from WM_OT_blenderplayer_start) */
#if defined(DEBUG)
  CM_Debug("parsing command line arguments...");
  CM_Debug("num of arguments is: " << validArguments - 1);  //-1 because i starts at 1
#endif

  for (i = 1; (i < validArguments) && !error
#ifdef WIN32
              && scr_saver_mode == SCREEN_SAVER_MODE_NONE
#endif
       ;)

  {
#if defined(DEBUG)
    CM_Debug("argv[" << i << "] = '" << argv[i] << "'");
#endif
    if (argv[i][0] == '-') {
      /* ignore all args after " - ", allow python to have own args */
      if (argv[i][1] == '\0') {
        argc_py_clamped = i;
        break;
      }

      switch (argv[i][1]) {
        case 'g':  // game engine options (show_framerate, fixedtime, etc)
        {
          i++;
          if (i <= validArguments) {
            char *paramname = argv[i];
            // Check for single value versus assignment
            if (i + 1 <= validArguments && (*(argv[i + 1]) == '=')) {
              i++;
              if (i + 1 <= validArguments) {
                i++;
                // Assignment
                SYS_WriteCommandLineInt(syshandle, paramname, atoi(argv[i]));
                SYS_WriteCommandLineFloat(syshandle, paramname, atof(argv[i]));
                SYS_WriteCommandLineString(syshandle, paramname, argv[i]);
#if defined(DEBUG)
                CM_Debug(paramname << " = '" << argv[i] << "'");
#endif
                i++;
              }
              else {
                error = true;
                CM_Error("argument assignment " << paramname << " without value.");
              }
            }
            else {
              //						SYS_WriteCommandLineInt(syshandle, argv[i++], 1);
            }
          }
          break;
        }
        case 'd':  // debug on
        {
          ++i;

          if (strcmp(argv[i], "gpu") == 0) {
            G.debug |= G_DEBUG_GPU | G_DEBUG;
            ++i;
          }
          else if (strcmp(argv[i], "memory") == 0) {
            G.debug |= G_DEBUG;

            CM_Debug("Switching to fully guarded memory allocator.");
            MEM_use_guarded_allocator();

            MEM_set_memory_debug();
#ifndef NDEBUG
            BLI_mempool_set_memory_debug();
#endif
            ++i;
          }
          else {
            CM_Error("debug mode '" << argv[i] << "' unrecognized.");
          }

          break;
        }
#ifdef WITH_GAMEENGINE_BPPLAYER
        case 'L': {
          // Find the requested base file directory.
          if (!useLocalPath) {
            SPINDLE_SetFilePath(&argv[i][2]);
            useLocalPath = true;
          }
          i++;
          break;
        }
        case 'K': {
          // Find and set keys
          hexKey = SPINDLE_FindAndSetEncryptionKeys(argv, i);
          i++;
          break;
        }
#endif             // WITH_GAMEENGINE_BPPLAYER
        case 'f':  // fullscreen mode
        {
          i++;
          fullScreen = true;
          fullScreenParFound = true;
          if ((i + 2) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
            fullScreenWidth = atoi(argv[i++]);
            fullScreenHeight = atoi(argv[i++]);
            if ((i + 1) <= validArguments && argv[i][0] != '-') {
              fullScreenBpp = atoi(argv[i++]);
              if ((i + 1) <= validArguments && argv[i][0] != '-')
                fullScreenFrequency = atoi(argv[i++]);
            }
          }
          else if ((i + 1) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
            error = true;
            CM_Error("to define fullscreen width or height, both options must be used.");
          }
          break;
        }
        case 'w':  // display in a window
        {
          i++;
          fullScreen = false;
          windowParFound = true;

          // Parse window position and size options
          if ((i + 2) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
            windowWidth = atoi(argv[i++]);
            windowHeight = atoi(argv[i++]);

            if ((i + 2) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
              windowLeft = atoi(argv[i++]);
              windowTop = atoi(argv[i++]);
            }
            else if ((i + 1) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
              error = true;
              CM_Error(
                  "to define the window left or right coordinates, both options must be used.");
            }
          }
          else if ((i + 1) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
            error = true;
            CM_Error("to define the window's width or height, both options must be used.");
          }
          break;
        }
        case 'h':  // display help
        {
          usage(argv[0], isBlenderPlayer);
          return 0;
          break;
        }
        case 'i':  // parent window ID
        {
          i++;
          if ((i + 1) <= validArguments)
            parentWindow = (GHOST_TEmbedderWindowID)atoll(argv[i++]);
          else {
            error = true;
            CM_Error("too few options for parent window argument.");
          }
#if defined(DEBUG)
          CM_Debug("XWindows ID = " << int(parentWindow));
#endif  // defined(DEBUG)
          break;
        }
        case 'm':  // maximum anti-aliasing (eg. 2,4,8,16)
        {
          i++;
          samplesParFound = true;
          if ((i + 1) <= validArguments)
            aasamples = atoi(argv[i++]);
          else {
            error = true;
            CM_Error("no argument supplied for -m");
          }
          break;
        }
        case 'n': {
          ++i;
          if ((i + 1) <= validArguments) {
            U.anisotropic_filter = atoi(argv[i++]);
          }
          else {
            error = true;
            CM_Error("no argument supplied for -n");
          }
          break;
        }
        case 'c':  // keep console (windows only)
        {
          i++;
#ifdef WIN32
          closeConsole = false;
#endif
          break;
        }
        case 's':  // stereo mode
        {
          i++;
          if ((i + 1) <= validArguments) {
            stereoParFound = true;

            if (!strcmp(argv[i],
                        "nostereo"))  // may not be redundant if the file has different setting
            {
              stereomode = RAS_Rasterizer::RAS_STEREO_NOSTEREO;
            }

            // only the hardware pageflip method needs a stereo window
            else if (!strcmp(argv[i], "hwpageflip")) {
              stereomode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
              stereoWindow = true;
            }
            else if (!strcmp(argv[i], "syncdoubling"))
              stereomode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;

            else if (!strcmp(argv[i], "3dtvtopbottom"))
              stereomode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;

            else if (!strcmp(argv[i], "anaglyph"))
              stereomode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;

            else if (!strcmp(argv[i], "sidebyside"))
              stereomode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;

            else if (!strcmp(argv[i], "interlace"))
              stereomode = RAS_Rasterizer::RAS_STEREO_INTERLACED;

            else if (!strcmp(argv[i], "vinterlace"))
              stereomode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;

#if 0
//					// future stuff
//					else if (!strcmp(argv[i], "stencil")
//						stereomode = RAS_STEREO_STENCIL;
#endif
            else {
              error = true;
              CM_Error("stereomode '" << argv[i] << "' unrecognized.");
            }

            i++;
          }
          else {
            error = true;
            CM_Error("too few options for stereo argument.");
          }
          break;
        }
        case 'a':  // allow window to blend with display background
        {
          i++;
          alphaBackground = 1;
          break;
        }
        case 'p': {
          ++i;
          pythonControllerFile = argv[i++];
          break;
        }
        default:  // not recognized
        {
          CM_Warning("unknown argument: " << argv[i++]);
          break;
        }
      }
    }
    else {
      i++;
    }
  }

  if ((windowWidth < kMinWindowWidth) || (windowHeight < kMinWindowHeight)) {
    error = true;
    CM_Error("window size too small.");
  }

  if (error) {
    usage(argv[0], isBlenderPlayer);
    return 0;
  }
  GHOST_ISystem *system = nullptr;
#ifdef WIN32
  if (scr_saver_mode != SCREEN_SAVER_MODE_CONFIGURATION)
#endif
  {
    // Create the system
    if (GHOST_ISystem::createSystem(true, false) == GHOST_kSuccess) {
      system = GHOST_ISystem::getSystem();
      BLI_assert(system);

      if (!fullScreenWidth || !fullScreenHeight)
        system->getMainDisplayDimensions(fullScreenWidth, fullScreenHeight);
      // process first batch of events. If the user
      // drops a file on top off the blenderplayer icon, we
      // receive an event with the filename

      system->processEvents(0);

      // this bracket is needed for app (see below) to get out
      // of scope before GHOST_ISystem::disposeSystem() is called.
      {
        KX_ExitRequest exitcode = KX_ExitRequest::NO_REQUEST;
        std::string exitstring = "";
        bool firstTimeRunning = true;
        char filename[FILE_MAX];
        char pathname[FILE_MAX];
        char *titlename;

        get_filename(argc_py_clamped, argv, filename);
        if (filename[0])
          BLI_path_abs_from_cwd(filename, sizeof(filename));

        // fill the GlobalSettings with the first scene files
        // those may change during the game and persist after using Game Actuator
        GlobalSettings gs;

#ifdef WITH_PYTHON
        PyObject *globalDict = nullptr;
#endif
        /* Set Viewport render mode and shading type for the whole runtime */
        bool first_time_window = true;
        int shadingTypeRuntime = 0;
        bool useViewportRender = false;

        /* We don't want to use other windows than the one where is the 3D view */
        std::vector<wmWindow *> unused_windows = {};

        do {
          // Read the Blender file

          // if we got an exitcode 3 (KX_ExitRequest::START_OTHER_GAME) load a different file
          if (exitcode == KX_ExitRequest::START_OTHER_GAME ||
              exitcode == KX_ExitRequest::RESTART_GAME) {

            /* This normally exits/close the GHOST_IWindow */
            if (bfd) {
              /* Hack to not free the win->ghosting AND win->gpu_ctx when we restart/load new
               * .blend */
              CTX_wm_window(C)->ghostwin = nullptr;
              /* Hack to not free wm->message_bus when we restart/load new .blend */
              CTX_wm_manager(C)->message_bus = nullptr;

              //BLO_blendfiledata_free(bfd);
              if (bfd->user) {
                MEM_freeN(bfd->user);
              }

              MEM_delete(bfd);
            }

            char basedpath[FILE_MAX];

            // base the actuator filename relative to the last file
            if (exitcode == KX_ExitRequest::RESTART_GAME) {
              /* We have weird issues with exitstring ("~" in the exitstring which mess the path)
               * when we use Game Restart actuator).
               * Then instead of using exitstring we can recycle filename
               * However this is not a proper fix but a temp fix and it would need to
               * understand why when we start blenderplayer from blender (not when we start
               * blenderplayer from Visual Studio), the exitstring can be corrupted.
               */
              BLI_strncpy(basedpath, filename[0] ? filename : NULL, sizeof(basedpath));
            }
            else {
              BLI_strncpy(basedpath, exitstring.c_str(), sizeof(basedpath));
            }
            BLI_path_abs(basedpath, pathname);
            bfd = load_game_data(basedpath);

            if (!bfd) {
              // just add "//" in front of it
              char temppath[FILE_MAX] = "//";
              BLI_strncpy(temppath + 2, basedpath, FILE_MAX - 2);

              BLI_path_abs(temppath, pathname);
              bfd = load_game_data(temppath);
            }
          }
          else {
#ifdef WITH_GAMEENGINE_BPPLAYER
            if (useLocalPath) {
              bfd = load_encrypted_game_data(filename[0] ? filename : NULL, hexKey);

              // The file is valid and it's the original file name.
              if (bfd) {
                remove(filename);
                KX_SetOrigPath(bfd->main->filepath);
              }
            }
            else
#endif  // WITH_GAMEENGINE_BPPLAYER
            {
              bfd = load_game_data(BKE_appdir_program_path(), filename[0] ? filename : NULL);
              // The file is valid and it's the original file name.
              if (bfd) {
                /* Without this step, the bmain->name can be ".blend~"
                 * and as I don't understand why and as the bug has been
                 * reported, we ensure the extension is ".blend"
                 * else this is causing issues with globalDict. (youle)
                 */
                char *blend_name = bfd->main->filepath;
                BLI_path_extension_ensure(blend_name, FILE_MAX, ".blend");

                KX_SetOrigPath(blend_name);
              }
            }
          }

#if defined(DEBUG)
          CM_Debug("game data loaded from " << filename);
#endif

          if (!bfd) {
            usage(argv[0], isBlenderPlayer);
            error = true;
            exitcode = KX_ExitRequest::QUIT_GAME;
          }
          else {
            /* Setting options according to the blend file if not overriden in the command line */
#ifdef WIN32
#  if !defined(DEBUG)
            if (closeConsole && firstTimeRunning) {
              system->setConsoleWindowState(GHOST_kConsoleWindowStateHide);  // Close a console window
            }
#  endif  // !defined(DEBUG)
#endif    // WIN32

            /* We load our own G_MAIN in blenderplayer,
             * so free the one that BKE_blender_globals_init() gives us.
             */
            /* Free old main and replace it with loaded one (was done outside main loop
             * previously...) */
            BKE_blender_globals_main_replace(bfd->main);

            Main *maggie = bfd->main;
            Scene *scene = bfd->curscene;
            CTX_data_main_set(C, maggie);
            CTX_data_scene_set(C, scene);
            G.main = maggie;
            G_MAIN = G.main;

            if (firstTimeRunning) {
              G.fileflags = bfd->fileflags;
              gs.glslflag = scene->gm.flag;
            }

            titlename = maggie->filepath;

            // Check whether the game should be displayed full-screen
            if ((!fullScreenParFound) && (!windowParFound)) {
              // Only use file settings when command line did not override
              if ((scene->gm.playerflag & GAME_PLAYER_FULLSCREEN)) {
                fullScreen = true;
                fullScreenWidth = scene->gm.xplay;
                fullScreenHeight = scene->gm.yplay;
                fullScreenFrequency = scene->gm.freqplay;
                fullScreenBpp = scene->gm.depth;
              }
              else {
                fullScreen = false;
                windowWidth = scene->gm.xplay;
                windowHeight = scene->gm.yplay;
              }
            }

            // Check whether the game should be displayed in stereo
            if (!stereoParFound) {
              // Only use file settings when command line did not override
              if (scene->gm.stereoflag == STEREO_ENABLED) {
                switch (scene->gm.stereomode) {
                  case STEREO_QUADBUFFERED: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
                    break;
                  }
                  case STEREO_ABOVEBELOW: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;
                    break;
                  }
                  case STEREO_INTERLACED: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_INTERLACED;
                    break;
                  }
                  case STEREO_ANAGLYPH: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;
                    break;
                  }
                  case STEREO_SIDEBYSIDE: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;
                    break;
                  }
                  case STEREO_VINTERLACE: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;
                    break;
                  }
                  case STEREO_3DTVTOPBOTTOM: {
                    stereomode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;
                    break;
                  }
                }
                if (stereomode == RAS_Rasterizer::RAS_STEREO_QUADBUFFERED)
                  stereoWindow = true;
              }
            }
            else {
              scene->gm.stereoflag = STEREO_ENABLED;
            }

            if (!samplesParFound)
              aasamples = scene->gm.aasamples;

            BLI_strncpy(pathname, maggie->filepath, sizeof(pathname));
            if (firstTimeRunning) {
              firstTimeRunning = false;

              if (fullScreen) {
#ifdef WIN32
                if (scr_saver_mode == SCREEN_SAVER_MODE_SAVER) {
                  window = startScreenSaverFullScreen(system,
                                                      fullScreenWidth,
                                                      fullScreenHeight,
                                                      fullScreenBpp,
                                                      fullScreenFrequency,
                                                      stereoWindow,
                                                      alphaBackground);
                }
                else
#endif
                {
                  window = startFullScreen(
                      system,
                      fullScreenWidth,
                      fullScreenHeight,
                      fullScreenBpp,
                      fullScreenFrequency,
                      stereoWindow,
                      alphaBackground,
                      (scene->gm.playerflag & GAME_PLAYER_DESKTOP_RESOLUTION));
                }
              }
              else {
#ifdef __APPLE__
                // on Mac's we'll show the executable name instead of the 'game.blend' name
                char tempname[1024], *appstring;
                ::strcpy(tempname, titlename);

                appstring = strstr(tempname, ".app/");
                if (appstring) {
                  appstring[2] = 0;
                  titlename = &tempname[0];
                }
#endif
                // Strip the path so that we have the name of the game file
                std::string path = titlename;
                std::vector<std::string> parts;
#ifndef WIN32
                custom_split_string(parts, path, (char *)("/"));
#else   // WIN32
                custom_split_string(parts, path, (char *)("\\"));
#endif  // WIN32
                std::string title;
                if (parts.size()) {
                  title = parts[parts.size() - 1];
                  std::vector<std::string> sublastparts;
                  custom_split_string(sublastparts, title, (char *)("."));
                  if (sublastparts.size() > 1) {
                    title = sublastparts[0];
                  }
                }
                else {
                  title = "blenderplayer";
                }
#ifdef WIN32
                if (scr_saver_mode == SCREEN_SAVER_MODE_PREVIEW) {
                  window = startScreenSaverPreview(system, scr_saver_hwnd, stereoWindow);
                }
                else
#endif
                {
                  const char *strtitle = title.c_str();
                  if (parentWindow != 0)
                    window = startEmbeddedWindow(
                        system, strtitle, parentWindow, stereoWindow, alphaBackground);
                  else
                    window = startWindow(system,
                                         strtitle,
                                         windowLeft,
                                         windowTop,
                                         windowWidth,
                                         windowHeight,
                                         stereoWindow,
                                         alphaBackground);
                }
              }
              /* wm context */
              wmWindowManager *wm = (wmWindowManager *)G_MAIN->wm.first;
              wmWindow *win = (wmWindow *)wm->windows.first;
              CTX_wm_manager_set(C, wm);
              CTX_wm_window_set(C, win);
            }

            wmWindowManager *wm = (wmWindowManager *)bfd->main->wm.first;
            wmWindow *win = (wmWindow *)wm->windows.first;
            CTX_wm_manager_set(C, wm);
            CTX_wm_window_set(C, win);
            InitBlenderContextVariables(C, wm, bfd->curscene);
            wm_window_ghostwindow_blenderplayer_ensure(wm, win, window, first_time_window);

            /* Get rid of windows which are not the 3D view windows */
            LISTBASE_FOREACH (wmWindow *, win_in_list, &wm->windows) {
              if (win_in_list == win) {
                continue;
              }
              unused_windows.push_back(win_in_list);
              BLI_remlink(&wm->windows, win_in_list);
            }

            if (first_time_window) {
#  ifdef WITH_PYTHON
              BPY_python_start(C, argc, (const char **)argv);
              CTX_py_init_set(C, true);
#  endif WITH_PYTHON

              /* We need to have first an ogl context bound and it's done
               * in wm_window_ghostwindow_blenderplayer_ensure.
               */
              WM_init_gpu_blenderplayer(system);

              UI_theme_init_default();
              UI_init();
              /* To have blf_monofont_render available for generated textures checkerboard */
              UI_reinit_font();

              /* Set Viewport render mode and shading type for the whole runtime */
              useViewportRender = scene->gm.flag & GAME_USE_VIEWPORT_RENDER;
              shadingTypeRuntime = GetShadingTypeRuntime(C);
            }
            first_time_window = false;

            WM_keyconfig_update_postpone_begin();

            WM_keyconfig_init(C);

            // wm_init_scripts_extensions_once(C);

            WM_keyconfig_update_postpone_end();
            WM_keyconfig_update(static_cast<wmWindowManager *>(G_MAIN->wm.first));

            bScreen *screen = WM_window_get_active_screen(win);
            screen->state = SCREENFULL;

            if ((wm->init_flag & WM_INIT_FLAG_WINDOW) == 0) {
              ED_screens_init(C, G_MAIN, wm);
              wm->init_flag |= WM_INIT_FLAG_WINDOW;
              /* ED_screen_init can change bContext then we need to restore it again
               * after... b9907cb60b3c37e55cc8ea186e6cca26e333a039 */
              InitBlenderContextVariables(C, wm, bfd->curscene);
            }

            WorkSpace *workspace = BKE_workspace_active_get(win->workspace_hook);
            WM_window_set_active_screen(win, workspace, screen);

            /* Make all ARegion invisible as we only want 3D view to be drawn - also, drawing everything
               would need more code to be initialized */
            ED_screen_areas_iter (win, screen, area_iter) {
              /* this is to prevent a bug with gizmos drawing - if gizmos drawing is enabled by user during runtime */
              area_iter->full = screen;
              /* tag all areas for full redraw at least 1 time to prevent bugs during wm_draw_update */
              ED_area_tag_redraw(area_iter);
              LISTBASE_FOREACH (ARegion *, region, &area_iter->regionbase) {
                region->runtime->visible = 0;
              }
            }

            /* Don't draw gizmos by default */
            CTX_wm_view3d(C)->gizmo_flag |= V3D_GIZMO_HIDE;
            if (useViewportRender) {
              /* Don't draw overlays by default */
              CTX_wm_view3d(C)->flag2 |= V3D_HIDE_OVERLAYS;
            }

            // This argc cant be argc_py_clamped, since python uses it.
            LA_PlayerLauncher launcher(system,
                                       window,
                                       maggie,
                                       scene,
                                       &gs,
                                       stereomode,
                                       aasamples,
                                       argc,
                                       argv,
                                       pythonControllerFile,
                                       C,
                                       useViewportRender,
                                       shadingTypeRuntime);
#ifdef WITH_PYTHON
            // Acquire Python's GIL (global interpreter lock)
            // so we can safely run Python code and API calls
            PyGILState_Ensure();
            if (!globalDict) {
              globalDict = PyDict_New();
            }
            launcher.SetPythonGlobalDict(globalDict);
#endif  // WITH_PYTHON

            launcher.InitEngine();

            // Enter main loop
            launcher.EngineMainLoop();

            exitcode = launcher.GetExitRequested();
            exitstring = launcher.GetExitString();
            gs = *launcher.GetGlobalSettings();

            launcher.ExitEngine();
          }

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
        } while (!quitGame(exitcode));

        /* Restore the windows we disabled during standalone runtime to free it
         * (normally) in standalone exit pipeline */
        for (wmWindow *tmp_win : unused_windows) {
          BLI_addtail(&CTX_wm_manager(C)->windows, tmp_win);
        }

#ifdef WITH_PYTHON
        // Free globalDict OUTSIDE runtime loop.
        if (globalDict) {
          PyDict_Clear(globalDict);
          Py_DECREF(globalDict);
        }
#endif
      }
    }
    else {
      error = true;
      CM_Error("couldn't create a system.");
    }
  }

#if defined(WITH_PYTHON) && !defined(WITH_PYTHON_MODULE)
  /* Without this, we there isn't a good way to manage false-positive resource leaks
   * where a #PyObject references memory allocated with guarded-alloc, #71362.
   *
   * This allows add-ons to free resources when unregistered (which is good practice anyway).
   *
   * Don't run this code when built as a Python module as this runs when Python is in the
   * process of shutting down, where running a snippet like this will crash, see #82675.
   * Instead use the `atexit` module, installed by #BPY_python_start.
   *
   * Don't run this code when `C` is null because #pyrna_unregister_class
   * passes in `CTX_data_main(C)` to un-registration functions.
   * Further: `addon_utils.disable_all()` may call into functions that expect a valid context,
   * supporting all these code-paths with a null context is quite involved for such a corner-case.
   *
   * Check `CTX_py_init_get(C)` in case this function runs before Python has been initialized.
   * Which can happen when the GPU backend fails to initialize.
   */
  if (C && CTX_py_init_get(C)) {
    /* Calls `addon_utils.disable_all()` as well as unregistering all "startup" modules.  */
    const char *imports[] = {"bpy", "bpy.utils", nullptr};
    BPY_run_string_eval(C, imports, "bpy.utils._on_exit()");
  }
#endif

  /* Perform this early in case commands reference other data freed later in this function.
   * This most run:
   * - After add-ons are disabled because they may unregister commands.
   * - Before Python exits so Python objects can be de-referenced.
   * - Before #BKE_blender_atexit runs they free the `argv` on WIN32.
   */
  BKE_blender_cli_command_free_all();

  BLI_timer_free();

  WM_paneltype_clear();

  BKE_addon_pref_type_free();
  BKE_keyconfig_pref_type_free();
  BKE_materials_exit();

  wm_operatortype_free();
  wm_surfaces_free();
  wm_dropbox_free();
  WM_menutype_free();

  /* all non-screen and non-space stuff editors did, like editmode */
  if (C) {
    Main *bmain = CTX_data_main(C);
    ED_editors_exit(bmain, true);
  }

  BKE_mball_cubeTable_free();

  /* render code might still access databases */
  RE_FreeAllRender();
  RE_engines_exit();

  ED_preview_free_dbase(); /* frees a Main dbase, before BKE_blender_free! */
  ED_preview_restart_queue_free();
  blender::ed::asset::list::storage_exit();

  BKE_tracking_clipboard_free();
  BKE_mask_clipboard_free();
  BKE_vfont_clipboard_free();
  ED_node_clipboard_free();
  blender::ed::greasepencil::clipboard_free();
  UV_clipboard_free();
  wm_clipboard_free();

#ifdef WITH_COMPOSITOR_CPU
  COM_deinitialize();
#endif

  blender::bke::subdiv::exit();

  BKE_image_free_unused_gpu_textures();

  /* Frees the entire library (#G_MAIN) and space-types. */
  BKE_blender_free();

  /* Important this runs after #BKE_blender_free because the window manager may be allocated
   * when `C` is null, holding references to undo steps which will fail to free if their types
   * have been freed first. */
  ED_undosys_type_free();

  if (bfd && bfd->user) {
    MEM_freeN(bfd->user);
  }

  MEM_delete(bfd);
  /* G_MAIN == bfd->main, it gets referenced in free_nodesystem so we can't have a dangling pointer
   */
  G_MAIN = nullptr;

  blender::draw::DRW_subdiv_free();

  ANIM_fcurves_copybuf_free();
  ANIM_drivers_copybuf_free();
  ANIM_driver_vars_copybuf_free();
  ANIM_fmodifiers_copybuf_free();
  ED_gpencil_anim_copybuf_free();
  ED_gpencil_strokes_copybuf_free();

  /* free gizmo-maps after freeing blender,
   * so no deleted data get accessed during cleaning up of areas. */
  wm_gizmomaptypes_free();
  wm_gizmogrouptype_free();
  wm_gizmotype_free();
  /* Same for UI-list types. */
  WM_uilisttype_free();

  BLF_exit();

#ifdef WITH_INTERNATIONAL
  BLT_lang_free();
#endif

  blender::animrig::keyingset_infos_exit();

#ifdef WITH_PYTHON
  BPY_python_end(true);
#endif

  ED_file_exit(); /* for fsmenu */

  DRW_gpu_context_enable_ex(false);
  UI_exit();
  GPU_pass_cache_free();
  GPU_shader_cache_dir_clear_old();
  GPU_exit();
  DRW_gpu_context_disable_ex(false);
  DRW_gpu_context_destroy();

  if (window) {
    system->disposeWindow(window);
  }

  // Dispose the system
  GHOST_ISystem::disposeSystem();

  BKE_blender_userdef_data_free(&U, false);

  RNA_exit(); /* should be after BPY_python_end so struct python slots are cleared */

  SYS_DeleteSystem(syshandle);

  wm_ghost_exit();

  if (C) {
    CTX_free(C);
  }

  DNA_sdna_current_free();

  BLI_threadapi_exit();
  BLI_task_scheduler_exit();

  /* No need to call this early, rather do it late so that other
   * pieces of Blender using sound may exit cleanly, see also T50676. */
  BKE_sound_exit();

  BKE_appdir_exit();

  BKE_blender_atexit();

  wm_autosave_delete();

  BKE_tempdir_session_purge();

  /* Logging cannot be called after exiting (#CLOG_INFO, #CLOG_WARN etc will crash).
   * So postpone exiting until other sub-systems that may use logging have shut down. */
  CLG_exit();

  int totblock = MEM_get_memory_blocks_in_use();
  if (totblock != 0) {
    CM_Error("totblock: " << totblock);
    MEM_set_error_callback(mem_error_cb);
    MEM_printmemlist();
  }

#ifdef WIN32
  while (argv_num) {
    free(argv[--argv_num]);
  }
  free(argv);
  argv = nullptr;
#endif

  return error ? -1 : 0;
}
