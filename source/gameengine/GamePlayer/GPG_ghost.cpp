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


#include <math.h>
#include <fstream>

#ifdef __linux__
#  ifdef __alpha__
#    include <signal.h>
#  endif /* __alpha__ */
#endif /* __linux__ */

extern "C"
{
#  include "MEM_guardedalloc.h"
#  include "MEM_CacheLimiterC-Api.h"

#  include "BLI_threads.h"
#  include "BLI_mempool.h"
#  include "BLI_blenlib.h"

#  include "DNA_scene_types.h"
#  include "DNA_genfile.h"

#  include "BLO_readfile.h"
#  include "BLO_runtime.h"

#  include "BKE_appdir.h"
#  include "BKE_blender.h"
#  include "BKE_depsgraph.h"
#  include "BKE_global.h"
#  include "BKE_icons.h"
#  include "BKE_image.h"
#  include "BKE_node.h"
#  include "BKE_report.h"
#  include "BKE_library.h"
#  include "BKE_library_remap.h"
#  include "BKE_modifier.h"
#  include "BKE_material.h"
#  include "BKE_text.h"
#  include "BKE_sound.h"

#  include "IMB_imbuf.h"
#  include "IMB_moviecache.h"

#  ifdef __APPLE__
int GHOST_HACK_getFirstFile(char buf[]);
#  endif

// For BLF
#  include "BLF_api.h"
#  include "BLT_translation.h"
#  include "BLT_lang.h"

extern int datatoc_bfont_ttf_size;
extern char datatoc_bfont_ttf[];
extern int datatoc_bmonofont_ttf_size;
extern char datatoc_bmonofont_ttf[];

}

#include "GPU_init_exit.h"
#include "GPU_draw.h"

#include "KX_Globals.h"
#include "KX_PythonInit.h"

#include "LA_SystemCommandLine.h"
#include "LA_PlayerLauncher.h"

#include "GHOST_ISystem.h"

#include "BKE_main.h"

#include "RNA_define.h"

#ifdef WIN32
#  include <windows.h>
#  if !defined(DEBUG)
#    include <wincon.h>
#  endif // !defined(DEBUG)
#  if defined(_MSC_VER) && defined(_M_X64)
#    include <math.h> /* needed for _set_FMA3_enable */
#  endif
#  include "utfconv.h"
#endif // WIN32

#ifdef WITH_SDL_DYNLOAD
#  include "sdlew.h"
#endif

#ifdef WITH_GAMEENGINE_BPPLAYER
#  include "SpindleEncryption.h"
#endif  // WITH_GAMEENGINE_BPPLAYER

#include <boost/algorithm/string.hpp>

#include "CM_Message.h"

const int kMinWindowWidth = 100;
const int kMinWindowHeight = 100;

static void mem_error_cb(const char *errorStr)
{
	fprintf(stderr, "%s", errorStr);
	fflush(stderr);
}

// library.c will only free window managers with a callback function.
// We don't actually use a wmWindowManager, but loading a blendfile
// loads wmWindows, so we need to free those.
static void wm_free(bContext *C, wmWindowManager *wm)
{
	BLI_freelistN(&wm->windows);
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
				scr_saver_hwnd = (HWND)::atoi(argv[2]);
			}
			if (!::stricmp("/c", argv[1])) {
				scr_saver_mode = SCREEN_SAVER_MODE_CONFIGURATION;
				if (scr_saver_hwnd == nullptr) {
					scr_saver_hwnd = ::GetForegroundWindow();
				}
			}
			else if (!::stricmp("/s", argv[1])) {
				scr_saver_mode = SCREEN_SAVER_MODE_SAVER;
			}
			else if (!::stricmp("/a", argv[1])) {
				scr_saver_mode = SCREEN_SAVER_MODE_PASSWORD;
			}
			else if (!::stricmp("/p", argv[1])
			         || !::stricmp("/l", argv[1])) {
				scr_saver_mode = SCREEN_SAVER_MODE_PREVIEW;
			}
		}
	}
	return ret;
}

#define SCR_SAVE_MOUSE_MOVE_THRESHOLD 15

static HWND found_ghost_window_hwnd;
static GHOST_IWindow *ghost_window_to_find;
static WNDPROC ghost_wnd_proc;
static POINT scr_save_mouse_pos;

static LRESULT CALLBACK screenSaverWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	BOOL close = false;
	switch (uMsg) {
		case WM_MOUSEMOVE:
		{
			POINT pt;
			GetCursorPos(&pt);
			LONG dx = scr_save_mouse_pos.x - pt.x;
			LONG dy = scr_save_mouse_pos.y - pt.y;
			if (abs(dx) > SCR_SAVE_MOUSE_MOVE_THRESHOLD
			    || abs(dy) > SCR_SAVE_MOUSE_MOVE_THRESHOLD) {
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
	if (close) {
		PostMessage(hwnd, WM_CLOSE, 0, 0);
	}
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
		std::string title = "";
		GHOST_GLSettings glSettings = {0};

		if (stereoVisual) {
			glSettings.flags |= GHOST_glStereoVisual;
		}

		GHOST_IWindow *window = system->createWindow(title, 0, 0, windowWidth, windowHeight, GHOST_kWindowStateMinimized,
		                                             GHOST_kDrawingContextTypeOpenGL, glSettings);
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

		RECT adjrc = { 0, 0, windowWidth, windowHeight };
		AdjustWindowRectEx(&adjrc, style, false, exstyle);

		style = (style & (~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_TILEDWINDOW))) | WS_CHILD;
		SetWindowLongPtr(ghost_hwnd, GWL_STYLE, style);
		SetWindowPos(ghost_hwnd, nullptr, adjrc.left, adjrc.top, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);

		/* Check the size of the client rectangle of the window and resize the window
		 * so that the client rectangle has the size requested.
		 */
		window->setClientSize(windowWidth, windowHeight);

		return window;
	}

	return nullptr;
}

#endif  // WIN32

static GHOST_IWindow *startFullScreen(GHOST_ISystem *system,
                                      int width,
                                      int height,
                                      int bpp, int frequency,
                                      const bool stereoVisual,
                                      const int alphaBackground,
                                      bool useDesktop)
{
	GHOST_TUns32 sysWidth = 0, sysHeight = 0;
	system->getMainDisplayDimensions(sysWidth, sysHeight);
	// Create the main window
	GHOST_DisplaySetting setting;
	setting.xPixels = (useDesktop) ? sysWidth : width;
	setting.yPixels = (useDesktop) ? sysHeight : height;
	setting.bpp = bpp;
	setting.frequency = frequency;

	GHOST_IWindow *window = nullptr;
	system->beginFullScreen(setting, &window, stereoVisual, alphaBackground);
	window->setCursorVisibility(false);
	/* note that X11 ignores this (it uses a window internally for fullscreen) */
	window->setState(GHOST_kWindowStateFullScreen);

	return window;
}

#ifdef WIN32

static GHOST_IWindow *startScreenSaverFullScreen(GHOST_ISystem *system,
                                                 int width,
                                                 int height,
                                                 int bpp, int frequency,
                                                 const bool stereoVisual,
                                                 const int alphaBackground)
{
	GHOST_IWindow *window = startFullScreen(system, width, height, bpp, frequency, stereoVisual, alphaBackground, 0);
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
                                  std::string& title,
                                  int windowLeft,
                                  int windowTop,
                                  int windowWidth,
                                  int windowHeight,
                                  const bool stereoVisual,
                                  const int alphaBackground)
{
	GHOST_GLSettings glSettings = {0};
	// Create the main window
	//std::string title ("Blender Player - GHOST");
	if (stereoVisual) {
		glSettings.flags |= GHOST_glStereoVisual;
	}
	if (alphaBackground) {
		glSettings.flags |= GHOST_glAlphaBackground;
	}

	GHOST_IWindow *window = system->createWindow(title, windowLeft, windowTop, windowWidth, windowHeight, GHOST_kWindowStateNormal,
	                                             GHOST_kDrawingContextTypeOpenGL, glSettings);
	if (!window) {
		CM_Error("could not create main window");
		exit(-1);
	}

	/* Check the size of the client rectangle of the window and resize the window
	 * so that the client rectangle has the size requested.
	 */
	window->setClientSize(windowWidth, windowHeight);
	window->setCursorVisibility(false);

	return window;
}

static GHOST_IWindow *startEmbeddedWindow(GHOST_ISystem *system,
                                          std::string& title,
                                          const GHOST_TEmbedderWindowID parentWindow,
                                          const bool stereoVisual,
                                          const int alphaBackground)
{
	GHOST_TWindowState state = GHOST_kWindowStateNormal;
	GHOST_GLSettings glSettings = {0};

	if (stereoVisual) {
		glSettings.flags |= GHOST_glStereoVisual;
	}
	if (alphaBackground) {
		glSettings.flags |= GHOST_glAlphaBackground;
	}

	if (parentWindow != 0) {
		state = GHOST_kWindowStateEmbedded;
	}
	GHOST_IWindow *window = system->createWindow(title, 0, 0, 0, 0, state,
	                                             GHOST_kDrawingContextTypeOpenGL, glSettings, false, parentWindow);

	if (!window) {
		CM_Error("could not create main window");
		exit(-1);
	}

	return window;
}

static void usage(const std::string& program, bool isBlenderPlayer)
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
	CM_Message("Available options are: [-w [w h l t]] [-f [fw fh fb ff]] " << consoleoption << "[-g gamengineoptions] "
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
	CM_Message("       fb = fullscreen mode bits per pixel (default unless set in the blend file: 32)");
	CM_Message("       ff = fullscreen mode frequency      (default unless set in the blend file: 60)");
	CM_Message("       Note: To define 'fw'' or 'fh'', both must be used.");
	CM_Message("       Example: -f  or  -f 1024 768  or  -f 0 0 16  or  -f 1024 728 16 30" << std::endl);
	CM_Message("  -s: start player in stereoscopy mode (requires 3D capable hardware)");
	CM_Message("       stereomode: nostereo         (default unless stereo is set in the blend file)");
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
	CM_Message("       nomipmap                       0         Disable mipmaps");
	CM_Message("       wireframe                      0         Wireframe render");
	CM_Message("       show_framerate                 0         Show the frame rate");
	CM_Message("       show_render_queries            0         Show the render queries");
	CM_Message("       show_properties                0         Show debug properties");
	CM_Message("       show_profile                   0         Show profiling information");
	CM_Message("       show_bounding_box              0         Show debug bounding box volume");
	CM_Message("       show_armatures                 0         Show debug armatures");
	CM_Message("       show_camera_frustum            0         Show debug camera frustum volume");
	CM_Message("       show_shadow_frustum            0         Show debug light shadow frustum volume");
	CM_Message("       ignore_deprecation_warnings    1         Ignore deprecation warnings" << std::endl);
	CM_Message("  -p: override python main loop script");
	CM_Message(std::endl);
	CM_Message("  - : all arguments after this are ignored, allowing python to access them from sys.argv");
	CM_Message(std::endl);
	CM_Message("example: " << program << " -w 320 200 10 10 -g noaudio " << example_pathname << example_filename);
	CM_Message("example: " << program << " -g show_framerate = 0 " << example_pathname << example_filename);
	CM_Message("example: " << program << " -i 232421 -m 16 " << example_pathname << example_filename);
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

	srclen -= ::strlen("MacOS/blenderplayer");
	if (srclen > 0) {
		len = srclen + ::strlen("Resources/game.blend");
		gamefile = new char[len + 1];
		::strcpy(gamefile, argv[0]);
		::strcpy(gamefile + srclen, "Resources/game.blend");

		if (BLI_exists(gamefile)) {
			BLI_strncpy(filename, gamefile, FILE_MAX);
		}

		delete[] gamefile;
	}

#else
	filename[0] = '\0';

	if (argc > 1) {
		BLI_strncpy(filename, argv[argc - 1], FILE_MAX);
	}
#endif // !_APPLE
}

static BlendFileData *load_game_data(const char *progname, char *filename = nullptr)
{
	ReportList reports;
	BlendFileData *bfd = nullptr;

	BKE_reports_init(&reports, RPT_STORE);

	/* try to load ourself, will only work if we are a runtime */
	if (BLO_is_a_runtime(progname)) {
		bfd = BLO_read_runtime(progname, &reports);
		if (bfd) {
			bfd->type = BLENFILETYPE_RUNTIME;
			BLI_strncpy(bfd->main->name, progname, sizeof(bfd->main->name));
		}
	}
	else {
		bfd = BLO_read_from_file(progname, &reports, BLO_READ_SKIP_NONE);
	}

	if (!bfd && filename) {
		bfd = load_game_data(filename);
		if (!bfd) {
			CM_Error("loading " << filename << " failed: ");
			BKE_reports_print(&reports, RPT_ERROR);
		}
	}

	BKE_reports_clear(&reports);

	return bfd;
}

#ifdef WITH_GAMEENGINE_BPPLAYER

static BlendFileData *load_encrypted_game_data(const char *filename, std::string encryptKey)
{
	ReportList reports;
	BlendFileData *bfd = nullptr;
	char *fileData = nullptr;
	int fileSize;
	std::string localPath(SPINDLE_GetFilePath());
	BKE_reports_init(&reports, RPT_STORE);

	if (filename == nullptr) {
		return nullptr;
	}

	if (!localPath.empty() && !encryptKey.empty()) {
		// Load file and decrypt.
		fileData = SPINDLE_DecryptFromFile(filename, &fileSize, encryptKey.c_str(), 0);
	}

	if (fileData) {
		bfd = BLO_read_from_memory(fileData, fileSize, &reports, BLO_READ_SKIP_USERDEF);
		delete[] fileData;
	}

	if (!bfd) {
		BKE_reports_print(&reports, RPT_ERROR);
	}

	BKE_reports_clear(&reports);
	return bfd;
}

#endif  // WITH_GAMEENGINE_BPPLAYER

int main(int argc,
#ifdef WIN32
         char **UNUSED(argv_c)
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
	bool closeConsole = true;
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
	GHOST_TUns32 fullScreenWidth = 0;
	GHOST_TUns32 fullScreenHeight = 0;
	GHOST_IWindow *window = nullptr;
	int fullScreenBpp = 32;
	int fullScreenFrequency = 60;
	GHOST_TEmbedderWindowID parentWindow = 0;
	bool isBlenderPlayer = false; //true when lauching from blender or command line. false for bundled player
	int validArguments = 0;
	bool samplesParFound = false;
	std::string pythonControllerFile;
	GHOST_TUns16 aasamples = 0;
	int alphaBackground = 0;

#ifdef WIN32
	char **argv;
	int argv_num;

	/* We delay loading of openmp so we can set the policy here. */
# if defined(_MSC_VER)
	_putenv_s("OMP_WAIT_POLICY", "PASSIVE");
# endif

	/* FMA3 support in the 2013 CRT is broken on Vista and Windows 7 RTM (fixed in SP1). Just disable it. */
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
#endif  /* WIN32 */

#ifdef __linux__
#ifdef __alpha__
	signal(SIGFPE, SIG_IGN);
#endif /* __alpha__ */
#endif /* __linux__ */

#ifdef WITH_SDL_DYNLOAD
	sdlewInit();
#endif

	BKE_appdir_program_path_init(argv[0]);
	BKE_tempdir_init(nullptr);

	// We don't use threads directly in the BGE, but we need to call this so things like
	// freeing up GPU_Textures works correctly.
	BLI_threadapi_init();

	DNA_sdna_current_init();

	RNA_init();

	init_nodesystem();

	BKE_blender_globals_init();

	SET_FLAG_FROM_TEST(G.f, (U.flag & USER_SCRIPT_AUTOEXEC_DISABLE) == 0, G_SCRIPT_AUTOEXEC);

	// We load our own G.main, so free the one that BKE_blender_globals_init() gives us
	BKE_main_free(G.main);
	G.main = nullptr;

	MEM_CacheLimiter_set_disabled(true);
	IMB_init();
	BKE_images_init();
	BKE_modifier_init();
	DAG_init();

#ifdef WITH_FFMPEG
	IMB_ffmpeg_init();
#endif

	// Setup builtin font for BLF (mostly copied from creator.c, wm_init_exit.c and interface_style.c)
	BLF_init();
	BLT_lang_init();
	BLT_lang_set("");

	BLF_load_mem("default", (unsigned char *)datatoc_bfont_ttf, datatoc_bfont_ttf_size);
	if (blf_mono_font == -1) {
		blf_mono_font = BLF_load_mem_unique("monospace", (unsigned char *)datatoc_bmonofont_ttf, datatoc_bmonofont_ttf_size);
	}

	// Parse command line options
#if defined(DEBUG)
	CM_Debug("argv[0] = '" << argv[0] << "'");
#endif

#ifdef WIN32
	if (scr_saver_init(argc, argv)) {
		switch (scr_saver_mode) {
			case SCREEN_SAVER_MODE_CONFIGURATION:
			{
				MessageBox(scr_saver_hwnd, "This screen saver has no options that you can set", "Screen Saver", MB_OK);
				break;
			}
			case SCREEN_SAVER_MODE_PASSWORD:
			/* This is W95 only, which we currently do not support.
			 * Fall-back to normal screen saver behavior in that case... */
			case SCREEN_SAVER_MODE_SAVER:
			{
				fullScreen = true;
				fullScreenParFound = true;
				break;
			}

			case SCREEN_SAVER_MODE_PREVIEW:
			case SCREEN_SAVER_MODE_NONE:
			{
				/* This will actually be handled somewhere below... */
				break;
			}

		}
	}
#endif
	// XXX add the ability to change this values to the command line parsing.
	U.mixbufsize = 2048;
	U.audiodevice = 2;
	U.audiorate = 48000;
	U.audioformat = 0x24;
	U.audiochannels = 2;

	U.anisotropic_filter = 2;
	// enable fast mipmap generation
	U.use_gpu_mipmap = 1;

	BKE_sound_init_once();

	// Initialize a default material for meshes without materials.
	init_def_material();

	BKE_library_callback_free_window_manager_set(wm_free);

	/* if running blenderplayer the last argument can't be parsed since it has to be the filename. else it is bundled */
	isBlenderPlayer = !BLO_is_a_runtime(argv[0]);
	if (isBlenderPlayer) {
		validArguments = argc - 1;
	}
	else {
		validArguments = argc;
	}


	/* Parsing command line arguments (can be set from WM_OT_blenderplayer_start) */
#if defined(DEBUG)
	CM_Debug("parsing command line arguments...");
	CM_Debug("num of arguments is: " << validArguments - 1);     //-1 because i starts at 1
#endif

	for (i = 1; (i < validArguments) && !error
#ifdef WIN32
	     && scr_saver_mode == SCREEN_SAVER_MODE_NONE
#endif
	     ; )

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
				case 'g': //game engine options (show_framerate, fixedtime, etc)
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
				case 'd': //debug on
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
				case 'L':
				{
					// Find the requested base file directory.
					if (!useLocalPath) {
						SPINDLE_SetFilePath(&argv[i][2]);
						useLocalPath = true;
					}
					i++;
					break;
				}
				case 'K':
				{
					//Find and set keys
					hexKey = SPINDLE_FindAndSetEncryptionKeys(argv, i);
					i++;
					break;
				}
#endif  // WITH_GAMEENGINE_BPPLAYER
				case 'f': //fullscreen mode
				{
					i++;
					fullScreen = true;
					fullScreenParFound = true;
					if ((i + 2) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
						fullScreenWidth = atoi(argv[i++]);
						fullScreenHeight = atoi(argv[i++]);
						if ((i + 1) <= validArguments && argv[i][0] != '-') {
							fullScreenBpp = atoi(argv[i++]);
							if ((i + 1) <= validArguments && argv[i][0] != '-') {
								fullScreenFrequency = atoi(argv[i++]);
							}
						}
					}
					else if ((i + 1) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
						error = true;
						CM_Error("to define fullscreen width or height, both options must be used.");
					}
					break;
				}
				case 'w': //display in a window
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
							CM_Error("to define the window left or right coordinates, both options must be used.");
						}
					}
					else if ((i + 1) <= validArguments && argv[i][0] != '-' && argv[i + 1][0] != '-') {
						error = true;
						CM_Error("to define the window's width or height, both options must be used.");
					}
					break;
				}
				case 'h': //display help
				{
					usage(argv[0], isBlenderPlayer);
					return 0;
					break;
				}
				case 'i': //parent window ID
				{
					i++;
					if ((i + 1) <= validArguments) {
						parentWindow = (GHOST_TEmbedderWindowID)atoll(argv[i++]);
					}
					else {
						error = true;
						CM_Error("too few options for parent window argument.");
					}
#if defined(DEBUG)
					CM_Debug("XWindows ID = " << int(parentWindow));
#endif // defined(DEBUG)
					break;
				}
				case 'm': //maximum anti-aliasing (eg. 2,4,8,16)
				{
					i++;
					samplesParFound = true;
					if ((i + 1) <= validArguments) {
						aasamples = atoi(argv[i++]);
					}
					else {
						error = true;
						CM_Error("no argument supplied for -m");
					}
					break;
				}
				case 'n':
				{
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
				case 'c': //keep console (windows only)
				{
					i++;
#ifdef WIN32
					closeConsole = false;
#endif
					break;
				}
				case 's': //stereo mode
				{
					i++;
					if ((i + 1) <= validArguments) {
						stereoParFound = true;

						if (!strcmp(argv[i], "nostereo")) { // may not be redundant if the file has different setting
							stereomode = RAS_Rasterizer::RAS_STEREO_NOSTEREO;
						}

						// only the hardware pageflip method needs a stereo window
						else if (!strcmp(argv[i], "hwpageflip")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
							stereoWindow = true;
						}
						else if (!strcmp(argv[i], "syncdoubling")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;
						}

						else if (!strcmp(argv[i], "3dtvtopbottom")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;
						}

						else if (!strcmp(argv[i], "anaglyph")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;
						}

						else if (!strcmp(argv[i], "sidebyside")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;
						}

						else if (!strcmp(argv[i], "interlace")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_INTERLACED;
						}

						else if (!strcmp(argv[i], "vinterlace")) {
							stereomode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;
						}

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
				case 'a': // allow window to blend with display background
				{
					i++;
					alphaBackground = 1;
					break;
				}
				case 'p':
				{
					++i;
					pythonControllerFile = argv[i++];
					break;
				}
				default: //not recognized
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

#ifdef WIN32
	if (scr_saver_mode != SCREEN_SAVER_MODE_CONFIGURATION)
#endif
	{
		// Create the system
		if (GHOST_ISystem::createSystem() == GHOST_kSuccess) {
			GHOST_ISystem *system = GHOST_ISystem::getSystem();
			BLI_assert(system);

			if (!fullScreenWidth || !fullScreenHeight) {
				system->getMainDisplayDimensions(fullScreenWidth, fullScreenHeight);
			}
			// process first batch of events. If the user
			// drops a file on top off the blenderplayer icon, we
			// receive an event with the filename

			system->processEvents(0);

#ifdef WITH_PYTHON
			// Initialize python and the global dictionary.
			initPlayerPython(argc, argv);
			PyObject *globalDict = PyDict_New();
#endif  // WITH_PYTHON

			// this bracket is needed for app (see below) to get out
			// of scope before GHOST_ISystem::disposeSystem() is called.
			{
				KX_ExitInfo exitInfo;
				bool firstTimeRunning = true;
				char filename[FILE_MAX];
				char pathname[FILE_MAX];
				char *titlename;

				get_filename(argc_py_clamped, argv, filename);
				if (filename[0]) {
					BLI_path_cwd(filename, sizeof(filename));
				}


				// fill the GlobalSettings with the first scene files
				// those may change during the game and persist after using Game Actuator
				GlobalSettings gs;

				do {
					// Read the Blender file
					BlendFileData *bfd;

					// if we got an exitcode 3 (KX_ExitInfo::START_OTHER_GAME) load a different file
					if (exitInfo.m_code == KX_ExitInfo::START_OTHER_GAME) {
						char basedpath[FILE_MAX];

						// base the actuator filename relative to the last file
						BLI_strncpy(basedpath, exitInfo.m_fileName.c_str(), sizeof(basedpath));
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
							bfd = load_encrypted_game_data(filename[0] ? filename : nullptr, hexKey);

							// The file is valid and it's the original file name.
							if (bfd) {
								remove(filename);
								KX_SetOrigPath(bfd->main->name);
							}
						}
						else
#endif  // WITH_GAMEENGINE_BPPLAYER
						{
							bfd = load_game_data(BKE_appdir_program_path(), filename[0] ? filename : nullptr);
							// The file is valid and it's the original file name.
							if (bfd) {
								KX_SetOrigPath(bfd->main->name);
							}
						}
					}

#if defined(DEBUG)
					CM_Debug("game data loaded from " << filename);
#endif

					if (!bfd) {
						usage(argv[0], isBlenderPlayer);
						error = true;
						exitInfo.m_code = KX_ExitInfo::QUIT_GAME;
					}
					else {
						/* Setting options according to the blend file if not overriden in the command line */
#ifdef WIN32
#if !defined(DEBUG)
						if (closeConsole) {
							system->toggleConsole(0); // Close a console window
						}
#endif // !defined(DEBUG)
#endif // WIN32
						Main *maggie = bfd->main;
						Scene *scene = bfd->curscene;
						G.main = maggie;

						if (firstTimeRunning) {
							G.fileflags  = bfd->fileflags;

							gs.glslflag = scene->gm.flag;
						}

						//Seg Fault; icon.c gIcons == 0
						BKE_icons_init(1);

						titlename = maggie->name;

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
									case STEREO_QUADBUFFERED:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
										break;
									}
									case STEREO_ABOVEBELOW:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;
										break;
									}
									case STEREO_INTERLACED:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_INTERLACED;
										break;
									}
									case STEREO_ANAGLYPH:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;
										break;
									}
									case STEREO_SIDEBYSIDE:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;
										break;
									}
									case STEREO_VINTERLACE:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;
										break;
									}
									case STEREO_3DTVTOPBOTTOM:
									{
										stereomode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;
										break;
									}
								}
								if (stereomode == RAS_Rasterizer::RAS_STEREO_QUADBUFFERED) {
									stereoWindow = true;
								}
							}
						}
						else {
							scene->gm.stereoflag = STEREO_ENABLED;
						}

						if (!samplesParFound) {
							aasamples = scene->gm.aasamples;
						}

						BLI_strncpy(pathname, maggie->name, sizeof(pathname));
						if (firstTimeRunning) {
							firstTimeRunning = false;

							if (fullScreen) {
#ifdef WIN32
								if (scr_saver_mode == SCREEN_SAVER_MODE_SAVER) {
									window = startScreenSaverFullScreen(system, fullScreenWidth, fullScreenHeight,
									                                    fullScreenBpp, fullScreenFrequency, stereoWindow,
									                                    alphaBackground);
								}
								else
#endif
								{
									window = startFullScreen(system, fullScreenWidth, fullScreenHeight, fullScreenBpp,
									                         fullScreenFrequency, stereoWindow, alphaBackground,
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
								boost::split(parts, path, boost::is_any_of("/"));
#else  // WIN32
								boost::split(parts, path, boost::is_any_of("\\"));
#endif // WIN32
								std::string title;
								if (parts.size()) {
									title = parts[parts.size() - 1];
									std::vector<std::string> sublastparts;
									boost::split(sublastparts, title, boost::is_any_of("."));
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
									if (parentWindow != 0) {
										window = startEmbeddedWindow(system, title, parentWindow, stereoWindow, alphaBackground);
									}
									else {
										window = startWindow(system, title, windowLeft, windowTop, windowWidth,
										                     windowHeight, stereoWindow, alphaBackground);
									}
								}
							}

							GPU_init();

							if (SYS_GetCommandLineInt(syshandle, "nomipmap", 0)) {
								GPU_set_mipmap(0);
							}

							GPU_set_anisotropic(U.anisotropic_filter);
							GPU_set_gpu_mipmapping(U.use_gpu_mipmap);
							GPU_set_linear_mipmap(true);
						}

						// This argc cant be argc_py_clamped, since python uses it.
						LA_PlayerLauncher launcher(system, window, maggie, scene, &gs, stereomode, aasamples,
						                           argc, argv, pythonControllerFile);

#ifdef WITH_PYTHON
						launcher.SetPythonGlobalDict(globalDict);
#endif  // WITH_PYTHON

						launcher.InitEngine();

						// Enter main loop
						exitInfo = launcher.EngineMainLoop();

						gs = *launcher.GetGlobalSettings();

						launcher.ExitEngine();

						BLO_blendfiledata_free(bfd);
						/* G.main == bfd->main, it gets referenced in free_nodesystem so we can't have a dangling pointer */
						G.main = nullptr;
					}
				} while (ELEM(exitInfo.m_code, KX_ExitInfo::RESTART_GAME, KX_ExitInfo::START_OTHER_GAME));
			}

			GPU_exit();

#ifdef WITH_PYTHON
			PyDict_Clear(globalDict);
			Py_DECREF(globalDict);
			exitPlayerPython();
#endif  // WITH_PYTHON

			// Seg Fault; icon.c gIcons == 0
			BKE_icons_free();

			window->setCursorShape(GHOST_kStandardCursorDefault);
			window->setCursorVisibility(true);

			if (window) {
				system->disposeWindow(window);
			}

			// Dispose the system
			GHOST_ISystem::disposeSystem();
		}
		else {
			error = true;
			CM_Error("couldn't create a system.");
		}
	}

	/* refer to WM_exit_ext() and BKE_blender_free(),
	 * these are not called in the player but we need to match some of there behavior here,
	 * if the order of function calls or blenders state isn't matching that of blender proper,
	 * we may get troubles later on */

	free_nodesystem();

	// Cleanup
	RNA_exit();
	DNA_sdna_current_free();
	BLF_exit();

#ifdef WITH_INTERNATIONAL
	BLF_free_unifont();
	BLF_free_unifont_mono();
	BLT_lang_free();
#endif

	IMB_exit();
	BKE_images_exit();
	DAG_exit();
	IMB_moviecache_destruct();

	SYS_DeleteSystem(syshandle);

	BLI_threadapi_exit();

	int totblock = MEM_get_memory_blocks_in_use();
	if (totblock != 0) {
		CM_Error("totblock: " << totblock);
		MEM_set_error_callback(mem_error_cb);
		MEM_printmemlist();
	}

	BKE_tempdir_session_purge();

#ifdef WIN32
	while (argv_num) {
		free(argv[--argv_num]);
	}
	free(argv);
	argv = nullptr;
#endif

	return error ? -1 : 0;
}
