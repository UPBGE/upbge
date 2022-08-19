/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Interactive walk navigation modal operator
 * (similar to walking around in a first person game).
 *
 * \note Similar logic to `view3d_navigate_fly.c` changes here may apply there too.
 */

/* defines VIEW3D_OT_navigate - walk modal operator */

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_kdopbvh.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_report.h"

#include "BLT_translation.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_transform_snap_object_context.h"

#include "PIL_time.h" /* Smooth-view. */

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_immediate.h"

#include "DEG_depsgraph.h"

#include "view3d_intern.h" /* own include */
#include "view3d_navigate.h"

#ifdef WITH_INPUT_NDOF
//#  define NDOF_WALK_DEBUG
/* is this needed for ndof? - commented so redraw doesn't thrash - campbell */
//#  define NDOF_WALK_DRAW_TOOMUCH
#endif

#define USE_TABLET_SUPPORT

/* -------------------------------------------------------------------- */
/** \name Modal Key-map
 * \{ */

/* NOTE: these defines are saved in keymap files,
 * do not change values but just add new ones */
enum {
  WALK_MODAL_CANCEL = 1,
  WALK_MODAL_CONFIRM,
  WALK_MODAL_DIR_FORWARD,
  WALK_MODAL_DIR_FORWARD_STOP,
  WALK_MODAL_DIR_BACKWARD,
  WALK_MODAL_DIR_BACKWARD_STOP,
  WALK_MODAL_DIR_LEFT,
  WALK_MODAL_DIR_LEFT_STOP,
  WALK_MODAL_DIR_RIGHT,
  WALK_MODAL_DIR_RIGHT_STOP,
  WALK_MODAL_DIR_UP,
  WALK_MODAL_DIR_UP_STOP,
  WALK_MODAL_DIR_DOWN,
  WALK_MODAL_DIR_DOWN_STOP,
  WALK_MODAL_FAST_ENABLE,
  WALK_MODAL_FAST_DISABLE,
  WALK_MODAL_SLOW_ENABLE,
  WALK_MODAL_SLOW_DISABLE,
  WALK_MODAL_JUMP,
  WALK_MODAL_JUMP_STOP,
  WALK_MODAL_TELEPORT,
  WALK_MODAL_GRAVITY_TOGGLE,
  WALK_MODAL_ACCELERATE,
  WALK_MODAL_DECELERATE,
  WALK_MODAL_AXIS_LOCK_Z,
};

enum {
  WALK_BIT_FORWARD = 1 << 0,
  WALK_BIT_BACKWARD = 1 << 1,
  WALK_BIT_LEFT = 1 << 2,
  WALK_BIT_RIGHT = 1 << 3,
  WALK_BIT_UP = 1 << 4,
  WALK_BIT_DOWN = 1 << 5,
};

typedef enum eWalkTeleportState {
  WALK_TELEPORT_STATE_OFF = 0,
  WALK_TELEPORT_STATE_ON,
} eWalkTeleportState;

typedef enum eWalkMethod {
  WALK_MODE_FREE = 0,
  WALK_MODE_GRAVITY,
} eWalkMethod;

typedef enum eWalkGravityState {
  WALK_GRAVITY_STATE_OFF = 0,
  WALK_GRAVITY_STATE_JUMP,
  WALK_GRAVITY_STATE_START,
  WALK_GRAVITY_STATE_ON,
} eWalkGravityState;

/* Relative view axis z axis locking. */
typedef enum eWalkLockState {
  /* Disabled. */
  WALK_AXISLOCK_STATE_OFF = 0,

  /* Moving. */
  WALK_AXISLOCK_STATE_ACTIVE = 2,

  /* Done moving, it cannot be activated again. */
  WALK_AXISLOCK_STATE_DONE = 3,
} eWalkLockState;

void walk_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {WALK_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {WALK_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},

      {WALK_MODAL_DIR_FORWARD, "FORWARD", 0, "Forward", ""},
      {WALK_MODAL_DIR_BACKWARD, "BACKWARD", 0, "Backward", ""},
      {WALK_MODAL_DIR_LEFT, "LEFT", 0, "Left", ""},
      {WALK_MODAL_DIR_RIGHT, "RIGHT", 0, "Right", ""},
      {WALK_MODAL_DIR_UP, "UP", 0, "Up", ""},
      {WALK_MODAL_DIR_DOWN, "DOWN", 0, "Down", ""},

      {WALK_MODAL_DIR_FORWARD_STOP, "FORWARD_STOP", 0, "Stop Move Forward", ""},
      {WALK_MODAL_DIR_BACKWARD_STOP, "BACKWARD_STOP", 0, "Stop Mode Backward", ""},
      {WALK_MODAL_DIR_LEFT_STOP, "LEFT_STOP", 0, "Stop Move Left", ""},
      {WALK_MODAL_DIR_RIGHT_STOP, "RIGHT_STOP", 0, "Stop Mode Right", ""},
      {WALK_MODAL_DIR_UP_STOP, "UP_STOP", 0, "Stop Move Up", ""},
      {WALK_MODAL_DIR_DOWN_STOP, "DOWN_STOP", 0, "Stop Mode Down", ""},

      {WALK_MODAL_TELEPORT, "TELEPORT", 0, "Teleport", "Move forward a few units at once"},

      {WALK_MODAL_ACCELERATE, "ACCELERATE", 0, "Accelerate", ""},
      {WALK_MODAL_DECELERATE, "DECELERATE", 0, "Decelerate", ""},

      {WALK_MODAL_FAST_ENABLE, "FAST_ENABLE", 0, "Fast", "Move faster (walk or fly)"},
      {WALK_MODAL_FAST_DISABLE, "FAST_DISABLE", 0, "Fast (Off)", "Resume regular speed"},

      {WALK_MODAL_SLOW_ENABLE, "SLOW_ENABLE", 0, "Slow", "Move slower (walk or fly)"},
      {WALK_MODAL_SLOW_DISABLE, "SLOW_DISABLE", 0, "Slow (Off)", "Resume regular speed"},

      {WALK_MODAL_JUMP, "JUMP", 0, "Jump", "Jump when in walk mode"},
      {WALK_MODAL_JUMP_STOP, "JUMP_STOP", 0, "Jump (Off)", "Stop pushing jump"},

      {WALK_MODAL_GRAVITY_TOGGLE, "GRAVITY_TOGGLE", 0, "Toggle Gravity", "Toggle gravity effect"},

      {WALK_MODAL_AXIS_LOCK_Z, "AXIS_LOCK_Z", 0, "Z Axis Correction", "Z axis correction"},

      {0, NULL, 0, NULL, NULL},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "View3D Walk Modal");

  /* this function is called for each spacetype, only needs to add map once */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "View3D Walk Modal", modal_items);

  /* assign map to operators */
  WM_modalkeymap_assign(keymap, "VIEW3D_OT_walk");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Walk Structs
 * \{ */

typedef struct WalkTeleport {
  eWalkTeleportState state;
  float duration; /* from user preferences */
  float origin[3];
  float direction[3];
  double initial_time;
  eWalkMethod navigation_mode; /* teleport always set FREE mode on */

} WalkTeleport;

typedef struct WalkInfo {
  /* context stuff */
  RegionView3D *rv3d;
  View3D *v3d;
  ARegion *region;
  struct Depsgraph *depsgraph;
  Scene *scene;

  /** Needed for updating that isn't triggered by input. */
  wmTimer *timer;

  short state;
  bool redraw;

  /**
   * Needed for auto-keyframing, when animation isn't playing, only keyframe on confirmation.
   *
   * Currently we can't cancel this operator usefully while recording on animation playback
   * (this would need to un-key all previous frames).
   */
  bool anim_playing;
  bool need_rotation_keyframe;
  bool need_translation_keyframe;

  /** Previous 2D mouse values. */
  int prev_mval[2];
  /** Initial mouse location. */
  int init_mval[2];

  int moffset[2];

#ifdef WITH_INPUT_NDOF
  /** Latest 3D mouse values. */
  wmNDOFMotionData *ndof;
#endif

  /* Walk state. */
  /** The base speed without run/slow down modifications. */
  float base_speed;
  /** The speed the view is moving per redraw. */
  float speed;
  /** World scale 1.0 default. */
  float grid;

  /* compare between last state */
  /** Time between draws. */
  double time_lastdraw;

  void *draw_handle_pixel;

  /* use for some lag */
  /** Keep the previous value to smooth transitions (use lag). */
  float dvec_prev[3];

  /** Walk/free movement. */
  eWalkMethod navigation_mode;

  /* teleport */
  WalkTeleport teleport;

  /** Look speed factor - user preferences. */
  float mouse_speed;

  /** Speed adjustments. */
  bool is_fast;
  bool is_slow;

  /** Mouse reverse. */
  bool is_reversed;

#ifdef USE_TABLET_SUPPORT
  /** Tablet devices (we can't relocate the cursor). */
  bool is_cursor_absolute;
#endif

  /** Gravity system. */
  eWalkGravityState gravity_state;
  float gravity;

  /** Height to use in walk mode. */
  float view_height;

  /** Counting system to allow movement to continue if a direction (WASD) key is still pressed. */
  int active_directions;

  float speed_jump;
  /** Maximum jump height. */
  float jump_height;
  /** To use for fast/slow speeds. */
  float speed_factor;

  eWalkLockState zlock;
  /** Nicer dynamics. */
  float zlock_momentum;

  struct SnapObjectContext *snap_context;

  struct View3DCameraControl *v3d_camera_control;

} WalkInfo;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Walk Drawing
 * \{ */

/* prototypes */
#ifdef WITH_INPUT_NDOF
static void walkApply_ndof(bContext *C, WalkInfo *walk, bool is_confirm);
#endif /* WITH_INPUT_NDOF */
static int walkApply(bContext *C, struct WalkInfo *walk, bool is_confirm);
static float getVelocityZeroTime(const float gravity, const float velocity);

static void drawWalkPixel(const struct bContext *UNUSED(C), ARegion *region, void *arg)
{
  /* draws an aim/cross in the center */
  WalkInfo *walk = arg;

  const int outter_length = 24;
  const int inner_length = 14;
  int xoff, yoff;
  rctf viewborder;

  if (ED_view3d_cameracontrol_object_get(walk->v3d_camera_control)) {
    ED_view3d_calc_camera_border(
        walk->scene, walk->depsgraph, region, walk->v3d, walk->rv3d, &viewborder, false);
    xoff = viewborder.xmin + BLI_rctf_size_x(&viewborder) * 0.5f;
    yoff = viewborder.ymin + BLI_rctf_size_y(&viewborder) * 0.5f;
  }
  else {
    xoff = walk->region->winx / 2;
    yoff = walk->region->winy / 2;
  }

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

  immUniformThemeColorAlpha(TH_VIEW_OVERLAY, 1.0f);

  immBegin(GPU_PRIM_LINES, 8);

  /* North */
  immVertex2i(pos, xoff, yoff + inner_length);
  immVertex2i(pos, xoff, yoff + outter_length);

  /* East */
  immVertex2i(pos, xoff + inner_length, yoff);
  immVertex2i(pos, xoff + outter_length, yoff);

  /* South */
  immVertex2i(pos, xoff, yoff - inner_length);
  immVertex2i(pos, xoff, yoff - outter_length);

  /* West */
  immVertex2i(pos, xoff - inner_length, yoff);
  immVertex2i(pos, xoff - outter_length, yoff);

  immEnd();
  immUnbindProgram();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Walk Logic
 * \{ */

static void walk_navigation_mode_set(WalkInfo *walk, eWalkMethod mode)
{
  if (mode == WALK_MODE_FREE) {
    walk->navigation_mode = WALK_MODE_FREE;
    walk->gravity_state = WALK_GRAVITY_STATE_OFF;
  }
  else { /* WALK_MODE_GRAVITY */
    walk->navigation_mode = WALK_MODE_GRAVITY;
    walk->gravity_state = WALK_GRAVITY_STATE_START;
  }
}

/**
 * \param r_distance: Distance to the hit point
 */
static bool walk_floor_distance_get(RegionView3D *rv3d,
                                    WalkInfo *walk,
                                    const float dvec[3],
                                    float *r_distance)
{
  const float ray_normal[3] = {0, 0, -1}; /* down */
  float ray_start[3];
  float r_location[3];
  float r_normal_dummy[3];
  float dvec_tmp[3];
  bool ret;

  *r_distance = BVH_RAYCAST_DIST_MAX;

  copy_v3_v3(ray_start, rv3d->viewinv[3]);

  mul_v3_v3fl(dvec_tmp, dvec, walk->grid);
  add_v3_v3(ray_start, dvec_tmp);

  ret = ED_transform_snap_object_project_ray(
      walk->snap_context,
      walk->depsgraph,
      walk->v3d,
      &(const struct SnapObjectParams){
          .snap_target_select = SCE_SNAP_TARGET_ALL,
          /* Avoid having to convert the edit-mesh to a regular mesh. */
          .edit_mode_type = SNAP_GEOM_EDIT,
      },
      ray_start,
      ray_normal,
      r_distance,
      r_location,
      r_normal_dummy);

  /* artificially scale the distance to the scene size */
  *r_distance /= walk->grid;
  return ret;
}

/**
 * \param ray_distance: Distance to the hit point
 * \param r_location: Location of the hit point
 * \param r_normal: Normal of the hit surface, transformed to always face the camera
 */
static bool walk_ray_cast(RegionView3D *rv3d,
                          WalkInfo *walk,
                          float r_location[3],
                          float r_normal[3],
                          float *ray_distance)
{
  float ray_normal[3] = {0, 0, -1}; /* forward */
  float ray_start[3];
  bool ret;

  *ray_distance = BVH_RAYCAST_DIST_MAX;

  copy_v3_v3(ray_start, rv3d->viewinv[3]);

  mul_mat3_m4_v3(rv3d->viewinv, ray_normal);

  normalize_v3(ray_normal);

  ret = ED_transform_snap_object_project_ray(walk->snap_context,
                                             walk->depsgraph,
                                             walk->v3d,
                                             &(const struct SnapObjectParams){
                                                 .snap_target_select = SCE_SNAP_TARGET_ALL,
                                             },
                                             ray_start,
                                             ray_normal,
                                             NULL,
                                             r_location,
                                             r_normal);

  /* dot is positive if both rays are facing the same direction */
  if (dot_v3v3(ray_normal, r_normal) > 0) {
    negate_v3(r_normal);
  }

  /* artificially scale the distance to the scene size */
  *ray_distance /= walk->grid;

  return ret;
}

/* WalkInfo->state */
enum {
  WALK_RUNNING = 0,
  WALK_CANCEL = 1,
  WALK_CONFIRM = 2,
};

/* keep the previous speed until user changes userpreferences */
static float base_speed = -1.0f;
static float userdef_speed = -1.0f;

static bool initWalkInfo(bContext *C, WalkInfo *walk, wmOperator *op, const int mval[2])
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);

  walk->rv3d = CTX_wm_region_view3d(C);
  walk->v3d = CTX_wm_view3d(C);
  walk->region = CTX_wm_region(C);
  walk->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  walk->scene = CTX_data_scene(C);

#ifdef NDOF_WALK_DEBUG
  puts("\n-- walk begin --");
#endif

  /* sanity check: for rare but possible case (if lib-linking the camera fails) */
  if ((walk->rv3d->persp == RV3D_CAMOB) && (walk->v3d->camera == NULL)) {
    walk->rv3d->persp = RV3D_PERSP;
  }

  if (walk->rv3d->persp == RV3D_CAMOB &&
      !BKE_id_is_editable(CTX_data_main(C), &walk->v3d->camera->id)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Cannot navigate a camera from an external library or non-editable override");
    return false;
  }

  if (ED_view3d_offset_lock_check(walk->v3d, walk->rv3d)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot navigate when the view offset is locked");
    return false;
  }

  if (walk->rv3d->persp == RV3D_CAMOB && walk->v3d->camera->constraints.first) {
    BKE_report(op->reports, RPT_ERROR, "Cannot navigate an object with constraints");
    return false;
  }

  walk->state = WALK_RUNNING;

  if (fabsf(U.walk_navigation.walk_speed - userdef_speed) > 0.1f) {
    base_speed = U.walk_navigation.walk_speed;
    userdef_speed = U.walk_navigation.walk_speed;
  }

  walk->speed = 0.0f;
  walk->is_fast = false;
  walk->is_slow = false;
  walk->grid = (walk->scene->unit.system == USER_UNIT_NONE) ?
                   1.0f :
                   1.0f / walk->scene->unit.scale_length;

  /* user preference settings */
  walk->teleport.duration = U.walk_navigation.teleport_time;
  walk->mouse_speed = U.walk_navigation.mouse_speed;

  if (U.walk_navigation.flag & USER_WALK_GRAVITY) {
    walk_navigation_mode_set(walk, WALK_MODE_GRAVITY);
  }
  else {
    walk_navigation_mode_set(walk, WALK_MODE_FREE);
  }

  walk->view_height = U.walk_navigation.view_height;
  walk->jump_height = U.walk_navigation.jump_height;
  walk->speed = U.walk_navigation.walk_speed;
  walk->speed_factor = U.walk_navigation.walk_speed_factor;
  walk->zlock = WALK_AXISLOCK_STATE_OFF;

  walk->gravity_state = WALK_GRAVITY_STATE_OFF;

  if (walk->scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
    walk->gravity = fabsf(walk->scene->physics_settings.gravity[2]);
  }
  else {
    walk->gravity = 9.80668f; /* m/s2 */
  }

  walk->is_reversed = ((U.walk_navigation.flag & USER_WALK_MOUSE_REVERSE) != 0);

#ifdef USE_TABLET_SUPPORT
  walk->is_cursor_absolute = false;
#endif

  walk->active_directions = 0;

#ifdef NDOF_WALK_DRAW_TOOMUCH
  walk->redraw = 1;
#endif
  zero_v3(walk->dvec_prev);

  walk->timer = WM_event_add_timer(CTX_wm_manager(C), win, TIMER, 0.01f);

#ifdef WITH_INPUT_NDOF
  walk->ndof = NULL;
#endif

  walk->anim_playing = ED_screen_animation_playing(wm);
  walk->need_rotation_keyframe = false;
  walk->need_translation_keyframe = false;

  walk->time_lastdraw = PIL_check_seconds_timer();

  walk->draw_handle_pixel = ED_region_draw_cb_activate(
      walk->region->type, drawWalkPixel, walk, REGION_DRAW_POST_PIXEL);

  walk->rv3d->rflag |= RV3D_NAVIGATING;

  walk->snap_context = ED_transform_snap_object_context_create(walk->scene, 0);

  walk->v3d_camera_control = ED_view3d_cameracontrol_acquire(
      walk->depsgraph, walk->scene, walk->v3d, walk->rv3d);

  copy_v2_v2_int(walk->init_mval, mval);
  copy_v2_v2_int(walk->prev_mval, mval);

  WM_cursor_grab_enable(win, 0, true, NULL);

  return 1;
}

static int walkEnd(bContext *C, WalkInfo *walk)
{
  wmWindow *win;
  RegionView3D *rv3d;

  if (walk->state == WALK_RUNNING) {
    return OPERATOR_RUNNING_MODAL;
  }
  if (walk->state == WALK_CONFIRM) {
    /* Needed for auto_keyframe. */
#ifdef WITH_INPUT_NDOF
    if (walk->ndof) {
      walkApply_ndof(C, walk, true);
    }
    else
#endif /* WITH_INPUT_NDOF */
    {
      walkApply(C, walk, true);
    }
  }

#ifdef NDOF_WALK_DEBUG
  puts("\n-- walk end --");
#endif

  win = CTX_wm_window(C);
  rv3d = walk->rv3d;

  WM_event_remove_timer(CTX_wm_manager(C), win, walk->timer);

  ED_region_draw_cb_exit(walk->region->type, walk->draw_handle_pixel);

  ED_transform_snap_object_context_destroy(walk->snap_context);

  ED_view3d_cameracontrol_release(walk->v3d_camera_control, walk->state == WALK_CANCEL);

  rv3d->rflag &= ~RV3D_NAVIGATING;

#ifdef WITH_INPUT_NDOF
  if (walk->ndof) {
    MEM_freeN(walk->ndof);
  }
#endif

  WM_cursor_grab_enable(win, 0, true, NULL);

  if (walk->state == WALK_CONFIRM) {
    MEM_freeN(walk);
    return OPERATOR_FINISHED;
  }

  MEM_freeN(walk);
  return OPERATOR_CANCELLED;
}

static void walkEvent(WalkInfo *walk, const wmEvent *event)
{
  if (event->type == TIMER && event->customdata == walk->timer) {
    walk->redraw = true;
  }
  else if (ISMOUSE_MOTION(event->type)) {

#ifdef USE_TABLET_SUPPORT
    if ((walk->is_cursor_absolute == false) && event->tablet.is_motion_absolute) {
      walk->is_cursor_absolute = true;
    }
#endif /* USE_TABLET_SUPPORT */

    walk->moffset[0] += event->mval[0] - walk->prev_mval[0];
    walk->moffset[1] += event->mval[1] - walk->prev_mval[1];

    copy_v2_v2_int(walk->prev_mval, event->mval);

    if (walk->moffset[0] || walk->moffset[1]) {
      walk->redraw = true;
    }
  }
#ifdef WITH_INPUT_NDOF
  else if (event->type == NDOF_MOTION) {
    /* do these automagically get delivered? yes. */
    // puts("ndof motion detected in walk mode!");
    // static const char *tag_name = "3D mouse position";

    const wmNDOFMotionData *incoming_ndof = event->customdata;
    switch (incoming_ndof->progress) {
      case P_STARTING:
        /* start keeping track of 3D mouse position */
#  ifdef NDOF_WALK_DEBUG
        puts("start keeping track of 3D mouse position");
#  endif
        /* fall-through */
      case P_IN_PROGRESS:
        /* update 3D mouse position */
#  ifdef NDOF_WALK_DEBUG
        putchar('.');
        fflush(stdout);
#  endif
        if (walk->ndof == NULL) {
          // walk->ndof = MEM_mallocN(sizeof(wmNDOFMotionData), tag_name);
          walk->ndof = MEM_dupallocN(incoming_ndof);
          // walk->ndof = malloc(sizeof(wmNDOFMotionData));
        }
        else {
          memcpy(walk->ndof, incoming_ndof, sizeof(wmNDOFMotionData));
        }
        break;
      case P_FINISHING:
        /* stop keeping track of 3D mouse position */
#  ifdef NDOF_WALK_DEBUG
        puts("stop keeping track of 3D mouse position");
#  endif
        if (walk->ndof) {
          MEM_freeN(walk->ndof);
          // free(walk->ndof);
          walk->ndof = NULL;
        }

        /* update the time else the view will jump when 2D mouse/timer resume */
        walk->time_lastdraw = PIL_check_seconds_timer();

        break;
      default:
        break; /* should always be one of the above 3 */
    }
  }
#endif /* WITH_INPUT_NDOF */
  /* handle modal keymap first */
  else if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case WALK_MODAL_CANCEL:
        walk->state = WALK_CANCEL;
        break;
      case WALK_MODAL_CONFIRM:
        walk->state = WALK_CONFIRM;
        break;

      case WALK_MODAL_ACCELERATE:
        base_speed *= 1.0f + (walk->is_slow ? 0.01f : 0.1f);
        break;
      case WALK_MODAL_DECELERATE:
        base_speed /= 1.0f + (walk->is_slow ? 0.01f : 0.1f);
        break;

      /* implement WASD keys */
      case WALK_MODAL_DIR_FORWARD:
        walk->active_directions |= WALK_BIT_FORWARD;
        break;
      case WALK_MODAL_DIR_BACKWARD:
        walk->active_directions |= WALK_BIT_BACKWARD;
        break;
      case WALK_MODAL_DIR_LEFT:
        walk->active_directions |= WALK_BIT_LEFT;
        break;
      case WALK_MODAL_DIR_RIGHT:
        walk->active_directions |= WALK_BIT_RIGHT;
        break;
      case WALK_MODAL_DIR_UP:
        walk->active_directions |= WALK_BIT_UP;
        break;
      case WALK_MODAL_DIR_DOWN:
        walk->active_directions |= WALK_BIT_DOWN;
        break;

      case WALK_MODAL_DIR_FORWARD_STOP:
        walk->active_directions &= ~WALK_BIT_FORWARD;
        break;
      case WALK_MODAL_DIR_BACKWARD_STOP:
        walk->active_directions &= ~WALK_BIT_BACKWARD;
        break;
      case WALK_MODAL_DIR_LEFT_STOP:
        walk->active_directions &= ~WALK_BIT_LEFT;
        break;
      case WALK_MODAL_DIR_RIGHT_STOP:
        walk->active_directions &= ~WALK_BIT_RIGHT;
        break;
      case WALK_MODAL_DIR_UP_STOP:
        walk->active_directions &= ~WALK_BIT_UP;
        break;
      case WALK_MODAL_DIR_DOWN_STOP:
        walk->active_directions &= ~WALK_BIT_DOWN;
        break;

      case WALK_MODAL_FAST_ENABLE:
        walk->is_fast = true;
        break;
      case WALK_MODAL_FAST_DISABLE:
        walk->is_fast = false;
        break;
      case WALK_MODAL_SLOW_ENABLE:
        walk->is_slow = true;
        break;
      case WALK_MODAL_SLOW_DISABLE:
        walk->is_slow = false;
        break;

#define JUMP_SPEED_MIN 1.0f
#define JUMP_TIME_MAX 0.2f /* s */
#define JUMP_SPEED_MAX sqrtf(2.0f * walk->gravity * walk->jump_height)

      case WALK_MODAL_JUMP_STOP:
        if (walk->gravity_state == WALK_GRAVITY_STATE_JUMP) {
          float t;

          /* delta time */
          t = (float)(PIL_check_seconds_timer() - walk->teleport.initial_time);

          /* Reduce the velocity, if JUMP wasn't hold for long enough. */
          t = min_ff(t, JUMP_TIME_MAX);
          walk->speed_jump = JUMP_SPEED_MIN +
                             t * (JUMP_SPEED_MAX - JUMP_SPEED_MIN) / JUMP_TIME_MAX;

          /* when jumping, duration is how long it takes before we start going down */
          walk->teleport.duration = getVelocityZeroTime(walk->gravity, walk->speed_jump);

          /* no more increase of jump speed */
          walk->gravity_state = WALK_GRAVITY_STATE_ON;
        }
        break;
      case WALK_MODAL_JUMP:
        if ((walk->navigation_mode == WALK_MODE_GRAVITY) &&
            (walk->gravity_state == WALK_GRAVITY_STATE_OFF) &&
            (walk->teleport.state == WALK_TELEPORT_STATE_OFF)) {
          /* no need to check for ground,
           * walk->gravity wouldn't be off
           * if we were over a hole */
          walk->gravity_state = WALK_GRAVITY_STATE_JUMP;
          walk->speed_jump = JUMP_SPEED_MAX;

          walk->teleport.initial_time = PIL_check_seconds_timer();
          copy_v3_v3(walk->teleport.origin, walk->rv3d->viewinv[3]);

          /* using previous vec because WASD keys are not called when SPACE is */
          copy_v2_v2(walk->teleport.direction, walk->dvec_prev);

          /* when jumping, duration is how long it takes before we start going down */
          walk->teleport.duration = getVelocityZeroTime(walk->gravity, walk->speed_jump);
        }

        break;

      case WALK_MODAL_TELEPORT: {
        float loc[3], nor[3];
        float distance;
        bool ret = walk_ray_cast(walk->rv3d, walk, loc, nor, &distance);

        /* in case we are teleporting middle way from a jump */
        walk->speed_jump = 0.0f;

        if (ret) {
          WalkTeleport *teleport = &walk->teleport;
          teleport->state = WALK_TELEPORT_STATE_ON;
          teleport->initial_time = PIL_check_seconds_timer();
          teleport->duration = U.walk_navigation.teleport_time;

          teleport->navigation_mode = walk->navigation_mode;
          walk_navigation_mode_set(walk, WALK_MODE_FREE);

          copy_v3_v3(teleport->origin, walk->rv3d->viewinv[3]);

          /* stop the camera from a distance (camera height) */
          normalize_v3_length(nor, walk->view_height);
          add_v3_v3(loc, nor);

          sub_v3_v3v3(teleport->direction, loc, teleport->origin);
        }
        else {
          walk->teleport.state = WALK_TELEPORT_STATE_OFF;
        }
        break;
      }

#undef JUMP_SPEED_MAX
#undef JUMP_TIME_MAX
#undef JUMP_SPEED_MIN

      case WALK_MODAL_GRAVITY_TOGGLE:
        if (walk->navigation_mode == WALK_MODE_GRAVITY) {
          walk_navigation_mode_set(walk, WALK_MODE_FREE);
        }
        else { /* WALK_MODE_FREE */
          walk_navigation_mode_set(walk, WALK_MODE_GRAVITY);
        }
        break;

      case WALK_MODAL_AXIS_LOCK_Z:
        if (walk->zlock != WALK_AXISLOCK_STATE_DONE) {
          walk->zlock = WALK_AXISLOCK_STATE_ACTIVE;
          walk->zlock_momentum = 0.0f;
        }
        break;
    }
  }
}

static void walkMoveCamera(bContext *C,
                           WalkInfo *walk,
                           const bool do_rotate,
                           const bool do_translate,
                           const bool is_confirm)
{
  /* we only consider autokeying on playback or if user confirmed walk on the same frame
   * otherwise we get a keyframe even if the user cancels. */
  const bool use_autokey = is_confirm || walk->anim_playing;
  ED_view3d_cameracontrol_update(
      walk->v3d_camera_control, use_autokey, C, do_rotate, do_translate);
  if (use_autokey) {
    walk->need_rotation_keyframe = false;
    walk->need_translation_keyframe = false;
  }
}

static float getFreeFallDistance(const float gravity, const float time)
{
  return gravity * (time * time) * 0.5f;
}

static float getVelocityZeroTime(const float gravity, const float velocity)
{
  return velocity / gravity;
}

static int walkApply(bContext *C, WalkInfo *walk, bool is_confirm)
{
#define WALK_ROTATE_TABLET_FAC 8.8f             /* Higher is faster, relative to region size. */
#define WALK_ROTATE_CONSTANT_FAC DEG2RAD(0.15f) /* Higher is faster, radians per-pixel. */
#define WALK_TOP_LIMIT DEG2RADF(85.0f)
#define WALK_BOTTOM_LIMIT DEG2RADF(-80.0f)
#define WALK_MOVE_SPEED base_speed
#define WALK_BOOST_FACTOR ((void)0, walk->speed_factor)
#define WALK_ZUP_CORRECT_FAC 0.1f    /* Amount to correct per step. */
#define WALK_ZUP_CORRECT_ACCEL 0.05f /* Increase upright momentum each step. */

  RegionView3D *rv3d = walk->rv3d;
  ARegion *region = walk->region;

  /* 3x3 copy of the view matrix so we can move along the view axis */
  float mat[3][3];
  /* this is the direction that's added to the view offset per redraw */
  float dvec[3] = {0.0f, 0.0f, 0.0f};

  int moffset[2];    /* mouse offset from the views center */
  float tmp_quat[4]; /* used for rotating the view */

#ifdef NDOF_WALK_DEBUG
  {
    static uint iteration = 1;
    printf("walk timer %d\n", iteration++);
  }
#endif

  {
    /* mouse offset from the center */
    copy_v2_v2_int(moffset, walk->moffset);

    /* apply moffset so we can re-accumulate */
    walk->moffset[0] = 0;
    walk->moffset[1] = 0;

    /* revert mouse */
    if (walk->is_reversed) {
      moffset[1] = -moffset[1];
    }

    /* Should we redraw? */
    if ((walk->active_directions) || moffset[0] || moffset[1] ||
        walk->zlock == WALK_AXISLOCK_STATE_ACTIVE ||
        walk->gravity_state != WALK_GRAVITY_STATE_OFF ||
        walk->teleport.state == WALK_TELEPORT_STATE_ON || is_confirm) {
      float dvec_tmp[3];

      /* time how fast it takes for us to redraw,
       * this is so simple scenes don't walk too fast */
      double time_current;
      float time_redraw;
      float time_redraw_clamped;
#ifdef NDOF_WALK_DRAW_TOOMUCH
      walk->redraw = 1;
#endif
      time_current = PIL_check_seconds_timer();
      time_redraw = (float)(time_current - walk->time_lastdraw);

      /* Clamp redraw time to avoid jitter in roll correction. */
      time_redraw_clamped = min_ff(0.05f, time_redraw);

      walk->time_lastdraw = time_current;

      /* base speed in m/s */
      walk->speed = WALK_MOVE_SPEED;

      if (walk->is_fast) {
        walk->speed *= WALK_BOOST_FACTOR;
      }
      else if (walk->is_slow) {
        walk->speed *= 1.0f / WALK_BOOST_FACTOR;
      }

      copy_m3_m4(mat, rv3d->viewinv);

      {
        /* rotate about the X axis- look up/down */
        if (moffset[1]) {
          float upvec[3];
          float angle;
          float y;

          /* relative offset */
          y = (float)moffset[1];

          /* Speed factor. */
#ifdef USE_TABLET_SUPPORT
          if (walk->is_cursor_absolute) {
            y /= region->winy;
            y *= WALK_ROTATE_TABLET_FAC;
          }
          else
#endif
          {
            y *= WALK_ROTATE_CONSTANT_FAC;
          }

          /* user adjustment factor */
          y *= walk->mouse_speed;

          /* clamp the angle limits */
          /* it ranges from 90.0f to -90.0f */
          angle = -asinf(rv3d->viewmat[2][2]);

          if (angle > WALK_TOP_LIMIT && y > 0.0f) {
            y = 0.0f;
          }
          else if (angle < WALK_BOTTOM_LIMIT && y < 0.0f) {
            y = 0.0f;
          }

          copy_v3_fl3(upvec, 1.0f, 0.0f, 0.0f);
          mul_m3_v3(mat, upvec);
          /* Rotate about the relative up vec */
          axis_angle_to_quat(tmp_quat, upvec, -y);
          mul_qt_qtqt(rv3d->viewquat, rv3d->viewquat, tmp_quat);
        }

        /* rotate about the Y axis- look left/right */
        if (moffset[0]) {
          float upvec[3];
          float x;

          /* if we're upside down invert the moffset */
          copy_v3_fl3(upvec, 0.0f, 1.0f, 0.0f);
          mul_m3_v3(mat, upvec);

          if (upvec[2] < 0.0f) {
            moffset[0] = -moffset[0];
          }

          /* relative offset */
          x = (float)moffset[0];

          /* Speed factor. */
#ifdef USE_TABLET_SUPPORT
          if (walk->is_cursor_absolute) {
            x /= region->winx;
            x *= WALK_ROTATE_TABLET_FAC;
          }
          else
#endif
          {
            x *= WALK_ROTATE_CONSTANT_FAC;
          }

          /* user adjustment factor */
          x *= walk->mouse_speed;

          /* Rotate about the relative up vec */
          axis_angle_to_quat_single(tmp_quat, 'Z', x);
          mul_qt_qtqt(rv3d->viewquat, rv3d->viewquat, tmp_quat);
        }

        if (walk->zlock == WALK_AXISLOCK_STATE_ACTIVE) {
          float upvec[3];
          copy_v3_fl3(upvec, 1.0f, 0.0f, 0.0f);
          mul_m3_v3(mat, upvec);

          /* Make sure we have some z rolling. */
          if (fabsf(upvec[2]) > 0.00001f) {
            float roll = upvec[2] * 5.0f;
            /* Rotate the view about this axis. */
            copy_v3_fl3(upvec, 0.0f, 0.0f, 1.0f);
            mul_m3_v3(mat, upvec);
            /* Rotate about the relative up vec. */
            axis_angle_to_quat(tmp_quat,
                               upvec,
                               roll * time_redraw_clamped * walk->zlock_momentum *
                                   WALK_ZUP_CORRECT_FAC);
            mul_qt_qtqt(rv3d->viewquat, rv3d->viewquat, tmp_quat);

            walk->zlock_momentum += WALK_ZUP_CORRECT_ACCEL;
          }
          else {
            /* Lock fixed, don't need to check it ever again. */
            walk->zlock = WALK_AXISLOCK_STATE_DONE;
          }
        }
      }

      /* WASD - 'move' translation code */
      if ((walk->active_directions) && (walk->gravity_state == WALK_GRAVITY_STATE_OFF)) {

        short direction;
        zero_v3(dvec);

        if ((walk->active_directions & WALK_BIT_FORWARD) ||
            (walk->active_directions & WALK_BIT_BACKWARD)) {

          direction = 0;

          if (walk->active_directions & WALK_BIT_FORWARD) {
            direction += 1;
          }

          if (walk->active_directions & WALK_BIT_BACKWARD) {
            direction -= 1;
          }

          copy_v3_fl3(dvec_tmp, 0.0f, 0.0f, direction);
          mul_m3_v3(mat, dvec_tmp);

          if (walk->navigation_mode == WALK_MODE_GRAVITY) {
            dvec_tmp[2] = 0.0f;
          }

          add_v3_v3(dvec, dvec_tmp);
        }

        if ((walk->active_directions & WALK_BIT_LEFT) ||
            (walk->active_directions & WALK_BIT_RIGHT)) {

          direction = 0;

          if (walk->active_directions & WALK_BIT_LEFT) {
            direction += 1;
          }

          if (walk->active_directions & WALK_BIT_RIGHT) {
            direction -= 1;
          }

          dvec_tmp[0] = direction * rv3d->viewinv[0][0];
          dvec_tmp[1] = direction * rv3d->viewinv[0][1];
          dvec_tmp[2] = 0.0f;

          add_v3_v3(dvec, dvec_tmp);
        }

        if ((walk->active_directions & WALK_BIT_UP) || (walk->active_directions & WALK_BIT_DOWN)) {

          if (walk->navigation_mode == WALK_MODE_FREE) {

            direction = 0;

            if (walk->active_directions & WALK_BIT_UP) {
              direction -= 1;
            }

            if (walk->active_directions & WALK_BIT_DOWN) {
              direction = 1;
            }

            copy_v3_fl3(dvec_tmp, 0.0f, 0.0f, direction);
            add_v3_v3(dvec, dvec_tmp);
          }
        }

        normalize_v3(dvec);

        /* apply movement */
        mul_v3_fl(dvec, walk->speed * time_redraw);
      }

      /* stick to the floor */
      if (walk->navigation_mode == WALK_MODE_GRAVITY &&
          ELEM(walk->gravity_state, WALK_GRAVITY_STATE_OFF, WALK_GRAVITY_STATE_START)) {

        bool ret;
        float ray_distance;
        float difference = -100.0f;
        float fall_distance;

        ret = walk_floor_distance_get(rv3d, walk, dvec, &ray_distance);

        if (ret) {
          difference = walk->view_height - ray_distance;
        }

        /* the distance we would fall naturally smoothly enough that we
         * can manually drop the object without activating gravity */
        fall_distance = time_redraw * walk->speed * WALK_BOOST_FACTOR;

        if (fabsf(difference) < fall_distance) {
          /* slope/stairs */
          dvec[2] -= difference;

          /* in case we switched from FREE to GRAVITY too close to the ground */
          if (walk->gravity_state == WALK_GRAVITY_STATE_START) {
            walk->gravity_state = WALK_GRAVITY_STATE_OFF;
          }
        }
        else {
          /* hijack the teleport variables */
          walk->teleport.initial_time = PIL_check_seconds_timer();
          walk->gravity_state = WALK_GRAVITY_STATE_ON;
          walk->teleport.duration = 0.0f;

          copy_v3_v3(walk->teleport.origin, walk->rv3d->viewinv[3]);
          copy_v2_v2(walk->teleport.direction, dvec);
        }
      }

      /* Falling or jumping) */
      if (ELEM(walk->gravity_state, WALK_GRAVITY_STATE_ON, WALK_GRAVITY_STATE_JUMP)) {
        float t;
        float z_cur, z_new;
        bool ret;
        float ray_distance, difference = -100.0f;

        /* delta time */
        t = (float)(PIL_check_seconds_timer() - walk->teleport.initial_time);

        /* keep moving if we were moving */
        copy_v2_v2(dvec, walk->teleport.direction);

        z_cur = walk->rv3d->viewinv[3][2];
        z_new = walk->teleport.origin[2] - getFreeFallDistance(walk->gravity, t) * walk->grid;

        /* jump */
        z_new += t * walk->speed_jump * walk->grid;

        /* duration is the jump duration */
        if (t > walk->teleport.duration) {

          /* check to see if we are landing */
          ret = walk_floor_distance_get(rv3d, walk, dvec, &ray_distance);

          if (ret) {
            difference = walk->view_height - ray_distance;
          }

          if (difference > 0.0f) {
            /* quit falling, lands at "view_height" from the floor */
            dvec[2] -= difference;
            walk->gravity_state = WALK_GRAVITY_STATE_OFF;
            walk->speed_jump = 0.0f;
          }
          else {
            /* keep falling */
            dvec[2] = z_cur - z_new;
          }
        }
        else {
          /* keep going up (jump) */
          dvec[2] = z_cur - z_new;
        }
      }

      /* Teleport */
      else if (walk->teleport.state == WALK_TELEPORT_STATE_ON) {
        float t; /* factor */
        float new_loc[3];
        float cur_loc[3];

        /* linear interpolation */
        t = (float)(PIL_check_seconds_timer() - walk->teleport.initial_time);
        t /= walk->teleport.duration;

        /* clamp so we don't go past our limit */
        if (t >= 1.0f) {
          t = 1.0f;
          walk->teleport.state = WALK_TELEPORT_STATE_OFF;
          walk_navigation_mode_set(walk, walk->teleport.navigation_mode);
        }

        mul_v3_v3fl(new_loc, walk->teleport.direction, t);
        add_v3_v3(new_loc, walk->teleport.origin);

        copy_v3_v3(cur_loc, walk->rv3d->viewinv[3]);
        sub_v3_v3v3(dvec, cur_loc, new_loc);
      }

      /* scale the movement to the scene size */
      mul_v3_v3fl(dvec_tmp, dvec, walk->grid);
      add_v3_v3(rv3d->ofs, dvec_tmp);

      if (rv3d->persp == RV3D_CAMOB) {
        walk->need_rotation_keyframe |= (moffset[0] || moffset[1] ||
                                         walk->zlock == WALK_AXISLOCK_STATE_ACTIVE);
        walk->need_translation_keyframe |= (len_squared_v3(dvec_tmp) > FLT_EPSILON);
        walkMoveCamera(
            C, walk, walk->need_rotation_keyframe, walk->need_translation_keyframe, is_confirm);
      }
    }
    else {
      /* we're not redrawing but we need to update the time else the view will jump */
      walk->time_lastdraw = PIL_check_seconds_timer();
    }
    /* end drawing */
    copy_v3_v3(walk->dvec_prev, dvec);
  }

  return OPERATOR_FINISHED;
#undef WALK_ROTATE_TABLET_FAC
#undef WALK_TOP_LIMIT
#undef WALK_BOTTOM_LIMIT
#undef WALK_MOVE_SPEED
#undef WALK_BOOST_FACTOR
}

#ifdef WITH_INPUT_NDOF
static void walkApply_ndof(bContext *C, WalkInfo *walk, bool is_confirm)
{
  Object *lock_ob = ED_view3d_cameracontrol_object_get(walk->v3d_camera_control);
  bool has_translate, has_rotate;

  view3d_ndof_fly(walk->ndof,
                  walk->v3d,
                  walk->rv3d,
                  walk->is_slow,
                  lock_ob ? lock_ob->protectflag : 0,
                  &has_translate,
                  &has_rotate);

  if (has_translate || has_rotate) {
    walk->redraw = true;

    if (walk->rv3d->persp == RV3D_CAMOB) {
      walk->need_rotation_keyframe |= has_rotate;
      walk->need_translation_keyframe |= has_translate;
      walkMoveCamera(
          C, walk, walk->need_rotation_keyframe, walk->need_translation_keyframe, is_confirm);
    }
  }
}
#endif /* WITH_INPUT_NDOF */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Walk Operator
 * \{ */

static int walk_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  WalkInfo *walk;

  if (RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ANY_TRANSFORM) {
    return OPERATOR_CANCELLED;
  }

  walk = MEM_callocN(sizeof(WalkInfo), "NavigationWalkOperation");

  op->customdata = walk;

  if (initWalkInfo(C, walk, op, event->mval) == false) {
    MEM_freeN(op->customdata);
    return OPERATOR_CANCELLED;
  }

  walkEvent(walk, event);

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static void walk_cancel(bContext *C, wmOperator *op)
{
  WalkInfo *walk = op->customdata;

  walk->state = WALK_CANCEL;
  walkEnd(C, walk);
  op->customdata = NULL;
}

static int walk_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  int exit_code;
  bool do_draw = false;
  WalkInfo *walk = op->customdata;
  View3D *v3d = walk->v3d;
  RegionView3D *rv3d = walk->rv3d;
  Object *walk_object = ED_view3d_cameracontrol_object_get(walk->v3d_camera_control);

  walk->redraw = false;

  walkEvent(walk, event);

#ifdef WITH_INPUT_NDOF
  if (walk->ndof) { /* 3D mouse overrules [2D mouse + timer] */
    if (event->type == NDOF_MOTION) {
      walkApply_ndof(C, walk, false);
    }
  }
  else
#endif /* WITH_INPUT_NDOF */
      if (event->type == TIMER && event->customdata == walk->timer) {
    walkApply(C, walk, false);
  }

  do_draw |= walk->redraw;

  exit_code = walkEnd(C, walk);

  if (exit_code != OPERATOR_RUNNING_MODAL) {
    do_draw = true;
  }
  if (exit_code == OPERATOR_FINISHED) {
    ED_view3d_camera_lock_undo_push(op->type->name, v3d, rv3d, C);
  }

  if (do_draw) {
    if (rv3d->persp == RV3D_CAMOB) {
      WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, walk_object);
    }

    /* too frequent, commented with NDOF_WALK_DRAW_TOOMUCH for now */
    // puts("redraw!");
    ED_region_tag_redraw(CTX_wm_region(C));
  }
  return exit_code;
}

void VIEW3D_OT_walk(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Walk Navigation";
  ot->description = "Interactively walk around the scene";
  ot->idname = "VIEW3D_OT_walk";

  /* api callbacks */
  ot->invoke = walk_invoke;
  ot->cancel = walk_cancel;
  ot->modal = walk_modal;
  ot->poll = ED_operator_region_view3d_active;

  /* flags */
  /* NOTE: #OPTYPE_BLOCKING isn't used because this needs to grab & hide the cursor.
   * where as blocking confines the cursor to the window bounds, even when hidden. */
  ot->flag = 0;
}

/** \} */
