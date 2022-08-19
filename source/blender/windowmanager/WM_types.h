/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup wm
 *
 *
 * Overview of WM structs
 * ======================
 *
 * - #wmWindowManager.windows -> #wmWindow <br>
 *   Window manager stores a list of windows.
 *
 *   - #wmWindow.screen -> #bScreen <br>
 *     Window has an active screen.
 *
 *     - #bScreen.areabase -> #ScrArea <br>
 *       Link to #ScrArea.
 *
 *       - #ScrArea.spacedata <br>
 *         Stores multiple spaces via space links.
 *
 *         - #SpaceLink <br>
 *           Base struct for space data for all different space types.
 *
 *       - #ScrArea.regionbase -> #ARegion <br>
 *         Stores multiple regions.
 *
 *     - #bScreen.regionbase -> #ARegion <br>
 *       Global screen level regions, e.g. popups, popovers, menus.
 *
 *   - #wmWindow.global_areas -> #ScrAreaMap <br>
 *     Global screen via 'areabase', e.g. top-bar & status-bar.
 *
 *
 * Window Layout
 * =============
 *
 * <pre>
 * wmWindow -> bScreen
 * +----------------------------------------------------------+
 * |+-----------------------------------------+-------------+ |
 * ||ScrArea (links to 3D view)               |ScrArea      | |
 * ||+-------++----------+-------------------+|(links to    | |
 * |||ARegion||          |ARegion (quad view)|| properties) | |
 * |||(tools)||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       |+----------+-------------------+|             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * |||       ||          |                   ||             | |
 * ||+-------++----------+-------------------+|             | |
 * |+-----------------------------------------+-------------+ |
 * +----------------------------------------------------------+
 * </pre>
 *
 * Space Data
 * ==========
 *
 * <pre>
 * ScrArea's store a list of space data (SpaceLinks), each of unique type.
 * The first one is the displayed in the UI, others are added as needed.
 *
 * +----------------------------+  <-- area->spacedata.first;
 * |                            |
 * |                            |---+  <-- other inactive SpaceLink's stored.
 * |                            |   |
 * |                            |   |---+
 * |                            |   |   |
 * |                            |   |   |
 * |                            |   |   |
 * |                            |   |   |
 * +----------------------------+   |   |
 *    |                             |   |
 *    +-----------------------------+   |
 *       |                              |
 *       +------------------------------+
 * </pre>
 *
 * A common way to get the space from the ScrArea:
 * \code{.c}
 * if (area->spacetype == SPACE_VIEW3D) {
 *     View3D *v3d = area->spacedata.first;
 *     ...
 * }
 * \endcode
 */

#pragma once

struct ID;
struct ImBuf;
struct bContext;
struct wmDrag;
struct wmDropBox;
struct wmEvent;
struct wmOperator;
struct wmWindowManager;

#include "BLI_compiler_attrs.h"
#include "BLI_utildefines.h"
#include "DNA_listBase.h"
#include "DNA_uuid_types.h"
#include "DNA_vec_types.h"
#include "DNA_xr_types.h"
#include "RNA_types.h"

/* exported types for WM */
#include "gizmo/WM_gizmo_types.h"
#include "wm_cursors.h"
#include "wm_event_types.h"

/* Include external gizmo API's */
#include "gizmo/WM_gizmo_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wmGenericUserDataFreeFn)(void *data);

typedef struct wmGenericUserData {
  void *data;
  /** When NULL, use #MEM_freeN. */
  wmGenericUserDataFreeFn free_fn;
  bool use_free;
} wmGenericUserData;

typedef void (*wmGenericCallbackFn)(struct bContext *C, void *user_data);

typedef struct wmGenericCallback {
  wmGenericCallbackFn exec;
  void *user_data;
  wmGenericUserDataFreeFn free_user_data;
} wmGenericCallback;

/* ************** wmOperatorType ************************ */

/** #wmOperatorType.flag */
enum {
  /** Register operators in stack after finishing (needed for redo). */
  OPTYPE_REGISTER = (1 << 0),
  /** Do an undo push after the operator runs. */
  OPTYPE_UNDO = (1 << 1),
  /** Let Blender grab all input from the WM (X11). */
  OPTYPE_BLOCKING = (1 << 2),
  OPTYPE_MACRO = (1 << 3),

  /** Grabs the cursor and optionally enables continuous cursor wrapping. */
  OPTYPE_GRAB_CURSOR_XY = (1 << 4),
  /** Only warp on the X axis. */
  OPTYPE_GRAB_CURSOR_X = (1 << 5),
  /** Only warp on the Y axis. */
  OPTYPE_GRAB_CURSOR_Y = (1 << 6),

  /** Show preset menu. */
  OPTYPE_PRESET = (1 << 7),

  /**
   * Some operators are mainly for internal use and don't make sense
   * to be accessed from the search menu, even if poll() returns true.
   * Currently only used for the search toolbox.
   */
  OPTYPE_INTERNAL = (1 << 8),

  /** Allow operator to run when interface is locked. */
  OPTYPE_LOCK_BYPASS = (1 << 9),
  /** Special type of undo which doesn't store itself multiple times. */
  OPTYPE_UNDO_GROUPED = (1 << 10),

  /**
   * Depends on the cursor location, when activated from a menu wait for mouse press.
   *
   * In practice these operators often end up being accessed:
   * - Directly from key bindings.
   * - As tools in the toolbar.
   *
   * Even so, accessing from the menu should behave usefully.
   */
  OPTYPE_DEPENDS_ON_CURSOR = (1 << 11),
};

/** For #WM_cursor_grab_enable wrap axis. */
enum {
  WM_CURSOR_WRAP_NONE = 0,
  WM_CURSOR_WRAP_X,
  WM_CURSOR_WRAP_Y,
  WM_CURSOR_WRAP_XY,
};

/**
 * Context to call operator in for #WM_operator_name_call.
 * rna_ui.c contains EnumPropertyItem's of these, keep in sync.
 */
typedef enum wmOperatorCallContext {
  /* if there's invoke, call it, otherwise exec */
  WM_OP_INVOKE_DEFAULT,
  WM_OP_INVOKE_REGION_WIN,
  WM_OP_INVOKE_REGION_CHANNELS,
  WM_OP_INVOKE_REGION_PREVIEW,
  WM_OP_INVOKE_AREA,
  WM_OP_INVOKE_SCREEN,
  /* only call exec */
  WM_OP_EXEC_DEFAULT,
  WM_OP_EXEC_REGION_WIN,
  WM_OP_EXEC_REGION_CHANNELS,
  WM_OP_EXEC_REGION_PREVIEW,
  WM_OP_EXEC_AREA,
  WM_OP_EXEC_SCREEN,
} wmOperatorCallContext;

#define WM_OP_CONTEXT_HAS_AREA(type) \
  (CHECK_TYPE_INLINE(type, wmOperatorCallContext), \
   !ELEM(type, WM_OP_INVOKE_SCREEN, WM_OP_EXEC_SCREEN))
#define WM_OP_CONTEXT_HAS_REGION(type) \
  (WM_OP_CONTEXT_HAS_AREA(type) && !ELEM(type, WM_OP_INVOKE_AREA, WM_OP_EXEC_AREA))

/* property tags for RNA_OperatorProperties */
typedef enum eOperatorPropTags {
  OP_PROP_TAG_ADVANCED = (1 << 0),
} eOperatorPropTags;
#define OP_PROP_TAG_ADVANCED ((eOperatorPropTags)OP_PROP_TAG_ADVANCED)

/* -------------------------------------------------------------------- */
/** \name #wmKeyMapItem
 * \{ */

/**
 * Modifier keys, not actually used for #wmKeyMapItem (never stored in DNA), used for:
 * - #wmEvent.modifier without the `KM_*_ANY` flags.
 * - #WM_keymap_add_item & #WM_modalkeymap_add_item
 */
enum {
  KM_SHIFT = (1 << 0),
  KM_CTRL = (1 << 1),
  KM_ALT = (1 << 2),
  /** Use for Windows-Key on MS-Windows, Command-key on macOS and Super on Linux. */
  KM_OSKEY = (1 << 3),

  /* Used for key-map item creation function arguments. */
  KM_SHIFT_ANY = (1 << 4),
  KM_CTRL_ANY = (1 << 5),
  KM_ALT_ANY = (1 << 6),
  KM_OSKEY_ANY = (1 << 7),
};

/* `KM_MOD_*` flags for #wmKeyMapItem and `wmEvent.alt/shift/oskey/ctrl`. */
/* Note that #KM_ANY and #KM_NOTHING are used with these defines too. */
#define KM_MOD_HELD 1

/**
 * #wmKeyMapItem.type
 * NOTE: most types are defined in `wm_event_types.h`.
 */
enum {
  KM_TEXTINPUT = -2,
};

/** #wmKeyMapItem.val */
enum {
  KM_ANY = -1,
  KM_NOTHING = 0,
  KM_PRESS = 1,
  KM_RELEASE = 2,
  KM_CLICK = 3,
  KM_DBL_CLICK = 4,
  /**
   * \note The cursor location at the point dragging starts is set to #wmEvent.prev_press_xy
   * some operators such as box selection should use this location instead of #wmEvent.xy.
   */
  KM_CLICK_DRAG = 5,
};

/**
 * #wmKeyMapItem.direction
 *
 * Direction set for #KM_CLICK_DRAG key-map items. #KM_ANY (-1) to ignore direction.
 */
enum {
  KM_DIRECTION_N = 1,
  KM_DIRECTION_NE = 2,
  KM_DIRECTION_E = 3,
  KM_DIRECTION_SE = 4,
  KM_DIRECTION_S = 5,
  KM_DIRECTION_SW = 6,
  KM_DIRECTION_W = 7,
  KM_DIRECTION_NW = 8,
};

/** \} */

/* ************** UI Handler ***************** */

#define WM_UI_HANDLER_CONTINUE 0
#define WM_UI_HANDLER_BREAK 1

/* ************** Notifiers ****************** */

typedef struct wmNotifier {
  struct wmNotifier *next, *prev;

  const struct wmWindow *window;

  unsigned int category, data, subtype, action;

  void *reference;

} wmNotifier;

/* 4 levels
 *
 * 0xFF000000; category
 * 0x00FF0000; data
 * 0x0000FF00; data subtype (unused?)
 * 0x000000FF; action
 */

/* category */
#define NOTE_CATEGORY 0xFF000000
#define NC_WM (1 << 24)
#define NC_WINDOW (2 << 24)
#define NC_WORKSPACE (3 << 24)
#define NC_SCREEN (4 << 24)
#define NC_SCENE (5 << 24)
#define NC_OBJECT (6 << 24)
#define NC_MATERIAL (7 << 24)
#define NC_TEXTURE (8 << 24)
#define NC_LAMP (9 << 24)
#define NC_GROUP (10 << 24)
#define NC_IMAGE (11 << 24)
#define NC_BRUSH (12 << 24)
#define NC_TEXT (13 << 24)
#define NC_WORLD (14 << 24)
#define NC_ANIMATION (15 << 24)
/* When passing a space as reference data with this (e.g. `WM_event_add_notifier(..., space)`),
 * the notifier will only be sent to this space. That avoids unnecessary updates for unrelated
 * spaces. */
#define NC_SPACE (16 << 24)
#define NC_GEOM (17 << 24)
#define NC_NODE (18 << 24)
#define NC_ID (19 << 24)
#define NC_PAINTCURVE (20 << 24)
#define NC_MOVIECLIP (21 << 24)
#define NC_MASK (22 << 24)
#define NC_GPENCIL (23 << 24)
#define NC_LINESTYLE (24 << 24)
#define NC_CAMERA (25 << 24)
#define NC_LIGHTPROBE (26 << 24)
/* Changes to asset data in the current .blend. */
#define NC_ASSET (27 << 24)
#define NC_LOGIC (28 << 24)

/* data type, 256 entries is enough, it can overlap */
#define NOTE_DATA 0x00FF0000

/* NC_WM windowmanager */
#define ND_FILEREAD (1 << 16)
#define ND_FILESAVE (2 << 16)
#define ND_DATACHANGED (3 << 16)
#define ND_HISTORY (4 << 16)
#define ND_JOB (5 << 16)
#define ND_UNDO (6 << 16)
#define ND_XR_DATA_CHANGED (7 << 16)
#define ND_LIB_OVERRIDE_CHANGED (8 << 16)

/* NC_SCREEN */
#define ND_LAYOUTBROWSE (1 << 16)
#define ND_LAYOUTDELETE (2 << 16)
#define ND_ANIMPLAY (4 << 16)
#define ND_GPENCIL (5 << 16)
#define ND_LAYOUTSET (6 << 16)
#define ND_SKETCH (7 << 16)
#define ND_WORKSPACE_SET (8 << 16)
#define ND_WORKSPACE_DELETE (9 << 16)

/* NC_SCENE Scene */
#define ND_SCENEBROWSE (1 << 16)
#define ND_MARKERS (2 << 16)
#define ND_FRAME (3 << 16)
#define ND_RENDER_OPTIONS (4 << 16)
#define ND_NODES (5 << 16)
#define ND_SEQUENCER (6 << 16)
/* NOTE: If an object was added, removed, merged/joined, ..., it is not enough to notify with
 * this. This affects the layer so also send a layer change notifier (e.g. ND_LAYER_CONTENT)! */
#define ND_OB_ACTIVE (7 << 16)
/* See comment on ND_OB_ACTIVE. */
#define ND_OB_SELECT (8 << 16)
#define ND_OB_VISIBLE (9 << 16)
#define ND_OB_RENDER (10 << 16)
#define ND_MODE (11 << 16)
#define ND_RENDER_RESULT (12 << 16)
#define ND_COMPO_RESULT (13 << 16)
#define ND_KEYINGSET (14 << 16)
#define ND_TOOLSETTINGS (15 << 16)
#define ND_LAYER (16 << 16)
#define ND_FRAME_RANGE (17 << 16)
#define ND_TRANSFORM_DONE (18 << 16)
#define ND_WORLD (92 << 16)
#define ND_LAYER_CONTENT (101 << 16)

/* NC_OBJECT Object */
#define ND_TRANSFORM (18 << 16)
#define ND_OB_SHADING (19 << 16)
#define ND_POSE (20 << 16)
#define ND_BONE_ACTIVE (21 << 16)
#define ND_BONE_SELECT (22 << 16)
#define ND_DRAW (23 << 16)
#define ND_MODIFIER (24 << 16)
#define ND_KEYS (25 << 16)
#define ND_CONSTRAINT (26 << 16)
#define ND_PARTICLE (27 << 16)
#define ND_POINTCACHE (28 << 16)
#define ND_PARENT (29 << 16)
#define ND_LOD (30 << 16)
#define ND_DRAW_RENDER_VIEWPORT \
  (31 << 16) /* for camera & sequencer viewport update, also /w NC_SCENE */
#define ND_SHADERFX (32 << 16)
/* For updating motion paths in 3dview. */
#define ND_DRAW_ANIMVIZ (33 << 16)

/* NC_MATERIAL Material */
#define ND_SHADING (30 << 16)
#define ND_SHADING_DRAW (31 << 16)
#define ND_SHADING_LINKS (32 << 16)
#define ND_SHADING_PREVIEW (33 << 16)

/* NC_LAMP Light */
#define ND_LIGHTING (40 << 16)
#define ND_LIGHTING_DRAW (41 << 16)

/* NC_WORLD World */
#define ND_WORLD_DRAW (45 << 16)

/* NC_TEXT Text */
#define ND_CURSOR (50 << 16)
#define ND_DISPLAY (51 << 16)

/* NC_ANIMATION Animato */
#define ND_KEYFRAME (70 << 16)
#define ND_KEYFRAME_PROP (71 << 16)
#define ND_ANIMCHAN (72 << 16)
#define ND_NLA (73 << 16)
#define ND_NLA_ACTCHANGE (74 << 16)
#define ND_FCURVES_ORDER (75 << 16)
#define ND_NLA_ORDER (76 << 16)

/* NC_GPENCIL */
#define ND_GPENCIL_EDITMODE (85 << 16)

/* NC_GEOM Geometry */
/* Mesh, Curve, MetaBall, Armature, etc. */
#define ND_SELECT (90 << 16)
#define ND_DATA (91 << 16)
#define ND_VERTEX_GROUP (92 << 16)

/* NC_NODE Nodes */

/* NC_SPACE */
#define ND_SPACE_CONSOLE (1 << 16)     /* general redraw */
#define ND_SPACE_INFO_REPORT (2 << 16) /* update for reports, could specify type */
#define ND_SPACE_INFO (3 << 16)
#define ND_SPACE_IMAGE (4 << 16)
#define ND_SPACE_FILE_PARAMS (5 << 16)
#define ND_SPACE_FILE_LIST (6 << 16)
#define ND_SPACE_ASSET_PARAMS (7 << 16)
#define ND_SPACE_NODE (8 << 16)
#define ND_SPACE_OUTLINER (9 << 16)
#define ND_SPACE_VIEW3D (10 << 16)
#define ND_SPACE_PROPERTIES (11 << 16)
#define ND_SPACE_TEXT (12 << 16)
#define ND_SPACE_TIME (13 << 16)
#define ND_SPACE_GRAPH (14 << 16)
#define ND_SPACE_DOPESHEET (15 << 16)
#define ND_SPACE_NLA (16 << 16)
#define ND_SPACE_SEQUENCER (17 << 16)
#define ND_SPACE_NODE_VIEW (18 << 16)
/* Sent to a new editor type after it's replaced an old one. */
#define ND_SPACE_CHANGED (19 << 16)
#define ND_SPACE_CLIP (20 << 16)
#define ND_SPACE_FILE_PREVIEW (21 << 16)
#define ND_SPACE_SPREADSHEET (22 << 16)

/* NC_ASSET */
/* Denotes that the AssetList is done reading some previews. NOT that the preview generation of
 * assets is done. */
#define ND_ASSET_LIST (1 << 16)
#define ND_ASSET_LIST_PREVIEW (2 << 16)
#define ND_ASSET_LIST_READING (3 << 16)
/* Catalog data changed, requiring a redraw of catalog UIs. Note that this doesn't denote a
 * reloading of asset libraries & their catalogs should happen. That only happens on explicit user
 * action. */
#define ND_ASSET_CATALOGS (4 << 16)

/* subtype, 256 entries too */
#define NOTE_SUBTYPE 0x0000FF00

/* subtype scene mode */
#define NS_MODE_OBJECT (1 << 8)

#define NS_EDITMODE_MESH (2 << 8)
#define NS_EDITMODE_CURVE (3 << 8)
#define NS_EDITMODE_SURFACE (4 << 8)
#define NS_EDITMODE_TEXT (5 << 8)
#define NS_EDITMODE_MBALL (6 << 8)
#define NS_EDITMODE_LATTICE (7 << 8)
#define NS_EDITMODE_ARMATURE (8 << 8)
#define NS_MODE_POSE (9 << 8)
#define NS_MODE_PARTICLE (10 << 8)
#define NS_EDITMODE_CURVES (11 << 8)

/* subtype 3d view editing */
#define NS_VIEW3D_GPU (16 << 8)
#define NS_VIEW3D_SHADING (17 << 8)

/* subtype layer editing */
#define NS_LAYER_COLLECTION (24 << 8)

/* action classification */
#define NOTE_ACTION (0x000000FF)
#define NA_EDITED 1
#define NA_EVALUATED 2
#define NA_ADDED 3
#define NA_REMOVED 4
#define NA_RENAME 5
#define NA_SELECTED 6
#define NA_ACTIVATED 7
#define NA_PAINTING 8
#define NA_JOB_FINISHED 9

/* ************** Gesture Manager data ************** */

/* wmGesture->type */
#define WM_GESTURE_LINES 1
#define WM_GESTURE_RECT 2
#define WM_GESTURE_CROSS_RECT 3
#define WM_GESTURE_LASSO 4
#define WM_GESTURE_CIRCLE 5
#define WM_GESTURE_STRAIGHTLINE 6

/**
 * wmGesture is registered to #wmWindow.gesture, handled by operator callbacks.
 */
typedef struct wmGesture {
  struct wmGesture *next, *prev;
  /** #wmEvent.type */
  int event_type;
  /** #wmEvent.modifier */
  uint8_t event_modifier;
  /** #wmEvent.keymodifier */
  short event_keymodifier;
  /** Gesture type define. */
  int type;
  /** bounds of region to draw gesture within. */
  rcti winrct;
  /** optional, amount of points stored. */
  int points;
  /** optional, maximum amount of points stored. */
  int points_alloc;
  int modal_state;
  /** optional, draw the active side of the straightline gesture. */
  bool draw_active_side;

  /**
   * For modal operators which may be running idle, waiting for an event to activate the gesture.
   * Typically this is set when the user is click-dragging the gesture
   * (box and circle select for eg).
   */
  uint is_active : 1;
  /** Previous value of is-active (use to detect first run & edge cases). */
  uint is_active_prev : 1;
  /** Use for gestures that support both immediate or delayed activation. */
  uint wait_for_input : 1;
  /** Use for gestures that can be moved, like box selection */
  uint move : 1;
  /** For gestures that support snapping, stores if snapping is enabled using the modal keymap
   * toggle. */
  uint use_snap : 1;
  /** For gestures that support flip, stores if flip is enabled using the modal keymap
   * toggle. */
  uint use_flip : 1;

  /**
   * customdata
   * - for border is a #rcti.
   * - for circle is recti, (xmin, ymin) is center, xmax radius.
   * - for lasso is short array.
   * - for straight line is a recti: (xmin,ymin) is start, (xmax, ymax) is end.
   */
  void *customdata;

  /** Free pointer to use for operator allocs (if set, its freed on exit). */
  wmGenericUserData user_data;
} wmGesture;

/* ************** wmEvent ************************ */

typedef enum eWM_EventFlag {
  /**
   * True if the operating system inverted the delta x/y values and resulting
   * `prev_xy` values, for natural scroll direction.
   * For absolute scroll direction, the delta must be negated again.
   */
  WM_EVENT_SCROLL_INVERT = (1 << 0),
  /**
   * Generated by auto-repeat, note that this must only ever be set for keyboard events
   * where `ISKEYBOARD(event->type) == true`.
   *
   * See #KMI_REPEAT_IGNORE for details on how key-map handling uses this.
   */
  WM_EVENT_IS_REPEAT = (1 << 1),
  /**
   * Mouse-move events may have this flag set to force creating a click-drag event
   * even when the threshold has not been met.
   */
  WM_EVENT_FORCE_DRAG_THRESHOLD = (1 << 2),
} eWM_EventFlag;
ENUM_OPERATORS(eWM_EventFlag, WM_EVENT_FORCE_DRAG_THRESHOLD);

typedef struct wmTabletData {
  /** 0=EVT_TABLET_NONE, 1=EVT_TABLET_STYLUS, 2=EVT_TABLET_ERASER. */
  int active;
  /** range 0.0 (not touching) to 1.0 (full pressure). */
  float pressure;
  /** range 0.0 (upright) to 1.0 (tilted fully against the tablet surface). */
  float x_tilt;
  /** as above. */
  float y_tilt;
  /** Interpret mouse motion as absolute as typical for tablets. */
  char is_motion_absolute;
} wmTabletData;

/**
 * Each event should have full modifier state.
 * event comes from event manager and from keymap.
 *
 *
 * Previous State (`prev_*`)
 * =========================
 *
 * Events hold information about the previous event.
 *
 * - Previous values are only set for events types that generate #KM_PRESS.
 *   See: #ISKEYBOARD_OR_BUTTON.
 *
 * - Previous x/y are exceptions: #wmEvent.prev
 *   these are set on mouse motion, see #MOUSEMOVE & track-pad events.
 *
 * - Modal key-map handling sets `prev_val` & `prev_type` to `val` & `type`,
 *   this allows modal keys-maps to check the original values (needed in some cases).
 *
 *
 * Press State (`prev_press_*`)
 * ============================
 *
 * Events hold information about the state when the last #KM_PRESS event was added.
 * This is used for generating #KM_CLICK, #KM_DBL_CLICK & #KM_CLICK_DRAG events.
 * See #wm_handlers_do for the implementation.
 *
 * - Previous values are only set when a #KM_PRESS event is detected.
 *   See: #ISKEYBOARD_OR_BUTTON.
 *
 * - The reason to differentiate between "press" and the previous event state is
 *   the previous event may be set by key-release events. In the case of a single key click
 *   this isn't a problem however releasing other keys such as modifiers prevents click/click-drag
 *   events from being detected, see: T89989.
 *
 * - Mouse-wheel events are excluded even though they generate #KM_PRESS
 *   as clicking and dragging don't make sense for mouse wheel events.
 */
typedef struct wmEvent {
  struct wmEvent *next, *prev;

  /** Event code itself (short, is also in key-map). */
  short type;
  /** Press, release, scroll-value. */
  short val;
  /** Mouse pointer position, screen coord. */
  int xy[2];
  /** Region relative mouse position (name convention before Blender 2.5). */
  int mval[2];
  /**
   * A single UTF8 encoded character.
   * #BLI_str_utf8_size() must _always_ return a valid value,
   * check when assigning so we don't need to check on every access after.
   */
  char utf8_buf[6];

  /** Modifier states: #KM_SHIFT, #KM_CTRL, #KM_ALT & #KM_OSKEY. */
  uint8_t modifier;

  /** The direction (for #KM_CLICK_DRAG events only). */
  int8_t direction;

  /**
   * Raw-key modifier (allow using any key as a modifier).
   * Compatible with values in `type`.
   */
  short keymodifier;

  /** Tablet info, available for mouse move and button events. */
  wmTabletData tablet;

  eWM_EventFlag flag;

  /* Custom data. */

  /** Custom data type, stylus, 6-DOF, see `wm_event_types.h`. */
  short custom;
  short customdata_free;
  /** Ascii, unicode, mouse-coords, angles, vectors, NDOF data, drag-drop info. */
  void *customdata;

  /* Previous State. */

  /** The previous value of `type`. */
  short prev_type;
  /** The previous value of `val`. */
  short prev_val;
  /**
   * The previous value of #wmEvent.xy,
   * Unlike other previous state variables, this is set on any mouse motion.
   * Use `prev_press_*` for the value at time of pressing.
   */
  int prev_xy[2];

  /* Previous Press State (when `val == KM_PRESS`). */

  /** The `type` at the point of the press action. */
  short prev_press_type;
  /**
   * The location when the key is pressed.
   * used to enforce drag threshold & calculate the `direction`.
   */
  int prev_press_xy[2];
  /** The `modifier` at the point of the press action. */
  uint8_t prev_press_modifier;
  /** The `keymodifier` at the point of the press action. */
  short prev_press_keymodifier;
  /**
   * The time when the key is pressed, see #PIL_check_seconds_timer.
   * Used to detect double-click events.
   */
  double prev_press_time;
} wmEvent;

/**
 * Values below are ignored when detecting if the user intentionally moved the cursor.
 * Keep this very small since it's used for selection cycling for eg,
 * where we want intended adjustments to pass this threshold and select new items.
 *
 * Always check for <= this value since it may be zero.
 */
#define WM_EVENT_CURSOR_MOTION_THRESHOLD ((float)U.move_threshold * U.dpi_fac)

/** Motion progress, for modal handlers. */
typedef enum {
  P_NOT_STARTED,
  P_STARTING,    /* <-- */
  P_IN_PROGRESS, /* <-- only these are sent for NDOF motion. */
  P_FINISHING,   /* <-- */
  P_FINISHED,
} wmProgress;

#ifdef WITH_INPUT_NDOF
typedef struct wmNDOFMotionData {
  /* awfully similar to GHOST_TEventNDOFMotionData... */
  /**
   * Each component normally ranges from -1 to +1, but can exceed that.
   * These use blender standard view coordinates,
   * with positive rotations being CCW about the axis.
   */
  /** Translation. */
  float tvec[3];
  /** Rotation.
   * <pre>
   * axis = (rx,ry,rz).normalized.
   * amount = (rx,ry,rz).magnitude [in revolutions, 1.0 = 360 deg]
   * </pre>
   */
  float rvec[3];
  /** Time since previous NDOF Motion event. */
  float dt;
  /** Is this the first event, the last, or one of many in between? */
  wmProgress progress;
} wmNDOFMotionData;
#endif /* WITH_INPUT_NDOF */

#ifdef WITH_XR_OPENXR
/* Similar to GHOST_XrPose. */
typedef struct wmXrPose {
  float position[3];
  /* Blender convention (w, x, y, z) */
  float orientation_quat[4];
} wmXrPose;

typedef struct wmXrActionState {
  union {
    bool state_boolean;
    float state_float;
    float state_vector2f[2];
    wmXrPose state_pose;
  };
  int type; /* eXrActionType */
} wmXrActionState;

typedef struct wmXrActionData {
  /** Action set name. */
  char action_set[64];
  /** Action name. */
  char action[64];
  /** User path. E.g. "/user/hand/left" */
  char user_path[64];
  /** Other user path, for bimanual actions. E.g. "/user/hand/right" */
  char user_path_other[64];
  /** Type. */
  eXrActionType type;
  /** State. Set appropriately based on type. */
  float state[2];
  /** State of the other sub-action path for bimanual actions. */
  float state_other[2];

  /** Input threshold for float/vector2f actions. */
  float float_threshold;

  /** Controller aim pose corresponding to the action's sub-action path. */
  float controller_loc[3];
  float controller_rot[4];
  /** Controller aim pose of the other sub-action path for bimanual actions. */
  float controller_loc_other[3];
  float controller_rot_other[4];

  /** Operator. */
  struct wmOperatorType *ot;
  struct IDProperty *op_properties;

  /** Whether bimanual interaction is occurring. */
  bool bimanual;
} wmXrActionData;
#endif

/** Timer flags. */
typedef enum {
  /** Do not attempt to free custom-data pointer even if non-NULL. */
  WM_TIMER_NO_FREE_CUSTOM_DATA = 1 << 0,
} wmTimerFlags;

typedef struct wmTimer {
  struct wmTimer *next, *prev;

  /** Window this timer is attached to (optional). */
  struct wmWindow *win;

  /** Set by timer user. */
  double timestep;
  /** Set by timer user, goes to event system. */
  int event_type;
  /** Various flags controlling timer options, see below. */
  wmTimerFlags flags;
  /** Set by timer user, to allow custom values. */
  void *customdata;

  /** Total running time in seconds. */
  double duration;
  /** Time since previous step in seconds. */
  double delta;

  /** Internal, last time timer was activated. */
  double ltime;
  /** Internal, next time we want to activate the timer. */
  double ntime;
  /** Internal, when the timer started. */
  double stime;
  /** Internal, put timers to sleep when needed. */
  bool sleep;
} wmTimer;

typedef struct wmOperatorType {
  /** Text for UI, undo. */
  const char *name;
  /** Unique identifier. */
  const char *idname;
  const char *translation_context;
  /** Use for tool-tips and Python docs. */
  const char *description;
  /** Identifier to group operators together. */
  const char *undo_group;

  /**
   * This callback executes the operator without any interactive input,
   * parameters may be provided through operator properties. cannot use
   * any interface code or input device state.
   * See defines below for return values.
   */
  int (*exec)(struct bContext *, struct wmOperator *) ATTR_WARN_UNUSED_RESULT;

  /**
   * This callback executes on a running operator whenever as property
   * is changed. It can correct its own properties or report errors for
   * invalid settings in exceptional cases.
   * Boolean return value, True denotes a change has been made and to redraw.
   */
  bool (*check)(struct bContext *, struct wmOperator *);

  /**
   * For modal temporary operators, initially invoke is called. then
   * any further events are handled in modal. if the operation is
   * canceled due to some external reason, cancel is called
   * See defines below for return values.
   */
  int (*invoke)(struct bContext *,
                struct wmOperator *,
                const struct wmEvent *) ATTR_WARN_UNUSED_RESULT;

  /**
   * Called when a modal operator is canceled (not used often).
   * Internal cleanup can be done here if needed.
   */
  void (*cancel)(struct bContext *, struct wmOperator *);

  /**
   * Modal is used for operators which continuously run, eg:
   * fly mode, knife tool, circle select are all examples of modal operators.
   * Modal operators can handle events which would normally access other operators,
   * they keep running until they don't return `OPERATOR_RUNNING_MODAL`.
   */
  int (*modal)(struct bContext *,
               struct wmOperator *,
               const struct wmEvent *) ATTR_WARN_UNUSED_RESULT;

  /**
   * Verify if the operator can be executed in the current context, note
   * that the operator might still fail to execute even if this return true.
   */
  bool (*poll)(struct bContext *) ATTR_WARN_UNUSED_RESULT;

  /**
   * Use to check if properties should be displayed in auto-generated UI.
   * Use 'check' callback to enforce refreshing.
   */
  bool (*poll_property)(const struct bContext *C,
                        struct wmOperator *op,
                        const PropertyRNA *prop) ATTR_WARN_UNUSED_RESULT;

  /** Optional panel for redo and repeat, auto-generated if not set. */
  void (*ui)(struct bContext *, struct wmOperator *);

  /**
   * Return a different name to use in the user interface, based on property values.
   * The returned string does not need to be freed.
   */
  const char *(*get_name)(struct wmOperatorType *, struct PointerRNA *);

  /**
   * Return a different description to use in the user interface, based on property values.
   * The returned string must be freed by the caller, unless NULL.
   */
  char *(*get_description)(struct bContext *C, struct wmOperatorType *, struct PointerRNA *);

  /** rna for properties */
  struct StructRNA *srna;

  /** previous settings - for initializing on re-use */
  struct IDProperty *last_properties;

  /**
   * Default rna property to use for generic invoke functions.
   * menus, enum search... etc. Example: Enum 'type' for a Delete menu.
   *
   * When assigned a string/number property,
   * immediately edit the value when used in a popup. see: #UI_BUT_ACTIVATE_ON_INIT.
   */
  PropertyRNA *prop;

  /** struct wmOperatorTypeMacro */
  ListBase macro;

  /** pointer to modal keymap, do not free! */
  struct wmKeyMap *modalkeymap;

  /** python needs the operator type as well */
  bool (*pyop_poll)(struct bContext *, struct wmOperatorType *ot) ATTR_WARN_UNUSED_RESULT;

  /** RNA integration */
  ExtensionRNA rna_ext;

  /** Cursor to use when waiting for cursor input, see: #OPTYPE_DEPENDS_ON_CURSOR. */
  int cursor_pending;

  /** Flag last for padding */
  short flag;

} wmOperatorType;

/**
 * Wrapper to reference a #wmOperatorType together with some set properties and other relevant
 * information to invoke the operator in a customizable way.
 */
typedef struct wmOperatorCallParams {
  struct wmOperatorType *optype;
  struct PointerRNA *opptr;
  wmOperatorCallContext opcontext;
} wmOperatorCallParams;

#ifdef WITH_INPUT_IME
/* *********** Input Method Editor (IME) *********** */
/**
 * \note similar to #GHOST_TEventImeData.
 */
typedef struct wmIMEData {
  size_t result_len, composite_len;

  /** utf8 encoding */
  char *str_result;
  /** utf8 encoding */
  char *str_composite;

  /** Cursor position in the IME composition. */
  int cursor_pos;
  /** Beginning of the selection. */
  int sel_start;
  /** End of the selection. */
  int sel_end;

  bool is_ime_composing;
} wmIMEData;
#endif

/* **************** Paint Cursor ******************* */

typedef void (*wmPaintCursorDraw)(struct bContext *C, int, int, void *customdata);

/* *************** Drag and drop *************** */

#define WM_DRAG_ID 0
#define WM_DRAG_ASSET 1
/** The user is dragging multiple assets. This is only supported in few specific cases, proper
 * multi-item support for dragging isn't supported well yet. Therefore this is kept separate from
 * #WM_DRAG_ASSET. */
#define WM_DRAG_ASSET_LIST 2
#define WM_DRAG_RNA 3
#define WM_DRAG_PATH 4
#define WM_DRAG_NAME 5
#define WM_DRAG_VALUE 6
#define WM_DRAG_COLOR 7
#define WM_DRAG_DATASTACK 8
#define WM_DRAG_ASSET_CATALOG 9

typedef enum eWM_DragFlags {
  WM_DRAG_NOP = 0,
  WM_DRAG_FREE_DATA = 1,
} eWM_DragFlags;
ENUM_OPERATORS(eWM_DragFlags, WM_DRAG_FREE_DATA)

/* NOTE: structs need not exported? */

typedef struct wmDragID {
  struct wmDragID *next, *prev;
  struct ID *id;
  struct ID *from_parent;
} wmDragID;

typedef struct wmDragAsset {
  /* NOTE: Can't store the #AssetHandle here, since the #FileDirEntry it wraps may be freed while
   * dragging. So store necessary data here directly. */

  char name[64]; /* MAX_NAME */
  /* Always freed. */
  const char *path;
  int id_type;
  struct AssetMetaData *metadata;
  int import_type; /* eFileAssetImportType */

  /* FIXME: This is temporary evil solution to get scene/view-layer/etc in the copy callback of the
   * #wmDropBox.
   * TODO: Handle link/append in operator called at the end of the drop process, and NOT in its
   * copy callback.
   * */
  struct bContext *evil_C;
} wmDragAsset;

typedef struct wmDragAssetCatalog {
  bUUID drag_catalog_id;
} wmDragAssetCatalog;

/**
 * For some specific cases we support dragging multiple assets (#WM_DRAG_ASSET_LIST). There is no
 * proper support for dragging multiple items in the `wmDrag`/`wmDrop` API yet, so this is really
 * just to enable specific features for assets.
 *
 * This struct basically contains a tagged union to either store a local ID pointer, or information
 * about an externally stored asset.
 */
typedef struct wmDragAssetListItem {
  struct wmDragAssetListItem *next, *prev;

  union {
    struct ID *local_id;
    wmDragAsset *external_info;
  } asset_data;

  bool is_external;
} wmDragAssetListItem;

typedef char *(*WMDropboxTooltipFunc)(struct bContext *,
                                      struct wmDrag *,
                                      const int xy[2],
                                      struct wmDropBox *drop);

typedef struct wmDragActiveDropState {
  /** Informs which dropbox is activated with the drag item.
   * When this value changes, the #draw_activate and #draw_deactivate dropbox callbacks are
   * triggered.
   */
  struct wmDropBox *active_dropbox;

  /** If `active_dropbox` is set, the area it successfully polled in. To restore the context of it
   * as needed. */
  struct ScrArea *area_from;
  /** If `active_dropbox` is set, the region it successfully polled in. To restore the context of
   * it as needed. */
  struct ARegion *region_from;

  /** If `active_dropbox` is set, additional context provided by the active (i.e. hovered) button.
   * Activated before context sensitive operations (polling, drawing, dropping). */
  struct bContextStore *ui_context;

  /** Text to show when a dropbox poll succeeds (so the dropbox itself is available) but the
   * operator poll fails. Typically the message the operator set with
   * CTX_wm_operator_poll_msg_set(). */
  const char *disabled_info;
  bool free_disabled_info;
} wmDragActiveDropState;

typedef struct wmDrag {
  struct wmDrag *next, *prev;

  int icon;
  /** See 'WM_DRAG_' defines above. */
  int type;
  void *poin;
  char path[1024]; /* FILE_MAX */
  double value;

  /** If no icon but imbuf should be drawn around cursor. */
  struct ImBuf *imb;
  float imbuf_scale;

  wmDragActiveDropState drop_state;

  eWM_DragFlags flags;

  /** List of wmDragIDs, all are guaranteed to have the same ID type. */
  ListBase ids;
  /** List of `wmDragAssetListItem`s. */
  ListBase asset_items;
} wmDrag;

/**
 * Drop-boxes are like key-maps, part of the screen/area/region definition.
 * Allocation and free is on startup and exit.
 *
 * The operator is polled and invoked with the current context (#WM_OP_INVOKE_DEFAULT), there is no
 * way to override that (by design, since drop-boxes should act on the exact mouse position).
 * So the drop-boxes are supposed to check the required area and region context in their poll.
 */
typedef struct wmDropBox {
  struct wmDropBox *next, *prev;

  /** Test if the dropbox is active. */
  bool (*poll)(struct bContext *C, struct wmDrag *drag, const wmEvent *event);

  /** Called when the drag action starts. Can be used to prefetch data for previews.
   * \note The dropbox that will be called eventually is not known yet when starting the drag.
   * So this callback is called on every dropbox that is registered in the current screen. */
  void (*on_drag_start)(struct bContext *C, struct wmDrag *drag);

  /** Before exec, this copies drag info to #wmDrop properties. */
  void (*copy)(struct bContext *C, struct wmDrag *drag, struct wmDropBox *drop);

  /**
   * If the operator is canceled (returns `OPERATOR_CANCELLED`), this can be used for cleanup of
   * `copy()` resources.
   */
  void (*cancel)(struct Main *bmain, struct wmDrag *drag, struct wmDropBox *drop);

  /**
   * Override the default cursor overlay drawing function.
   * Can be used to draw text or thumbnails. IE a tooltip for drag and drop.
   * \param xy: Cursor location in window coordinates (#wmEvent.xy compatible).
   */
  void (*draw_droptip)(struct bContext *C,
                       struct wmWindow *win,
                       struct wmDrag *drag,
                       const int xy[2]);

  /**
   * Called with the draw buffer (#GPUViewport) set up for drawing into the region's view.
   * \note Only setups the drawing buffer for drawing in view, not the GPU transform matrices.
   * The callback has to do that itself, with for example #UI_view2d_view_ortho.
   * \param xy: Cursor location in window coordinates (#wmEvent.xy compatible).
   */
  void (*draw_in_view)(struct bContext *C,
                       struct wmWindow *win,
                       struct wmDrag *drag,
                       const int xy[2]);

  /** Called when poll returns true the first time. */
  void (*draw_activate)(struct wmDropBox *drop, struct wmDrag *drag);

  /** Called when poll returns false the first time or when the drag event ends. */
  void (*draw_deactivate)(struct wmDropBox *drop, struct wmDrag *drag);

  /** Custom data for drawing. */
  void *draw_data;

  /** Custom tooltip shown during dragging. */
  WMDropboxTooltipFunc tooltip;

  /**
   * If poll succeeds, operator is called.
   * Not saved in file, so can be pointer.
   */
  wmOperatorType *ot;

  /** Operator properties, assigned to ptr->data and can be written to a file. */
  struct IDProperty *properties;
  /** RNA pointer to access properties. */
  struct PointerRNA *ptr;
} wmDropBox;

/**
 * Struct to store tool-tip timer and possible creation if the time is reached.
 * Allows UI code to call #WM_tooltip_timer_init without each user having to handle the timer.
 */
typedef struct wmTooltipState {
  /** Create tooltip on this event. */
  struct wmTimer *timer;
  /** The area the tooltip is created in. */
  struct ScrArea *area_from;
  /** The region the tooltip is created in. */
  struct ARegion *region_from;
  /** The tooltip region. */
  struct ARegion *region;
  /** Create the tooltip region (assign to 'region'). */
  struct ARegion *(*init)(struct bContext *C,
                          struct ARegion *region,
                          int *pass,
                          double *pass_delay,
                          bool *r_exit_on_event);
  /** Exit on any event, not needed for buttons since their highlight state is used. */
  bool exit_on_event;
  /** Cursor location at the point of tooltip creation. */
  int event_xy[2];
  /** Pass, use when we want multiple tips, count down to zero. */
  int pass;
} wmTooltipState;

/* *************** migrated stuff, clean later? ************** */

typedef struct RecentFile {
  struct RecentFile *next, *prev;
  char *filepath;
} RecentFile;

/* Logging */
struct CLG_LogRef;
/* wm_init_exit.c */

extern struct CLG_LogRef *WM_LOG_OPERATORS;
extern struct CLG_LogRef *WM_LOG_HANDLERS;
extern struct CLG_LogRef *WM_LOG_EVENTS;
extern struct CLG_LogRef *WM_LOG_KEYMAPS;
extern struct CLG_LogRef *WM_LOG_TOOLS;
extern struct CLG_LogRef *WM_LOG_MSGBUS_PUB;
extern struct CLG_LogRef *WM_LOG_MSGBUS_SUB;

#ifdef __cplusplus
}
#endif
