/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscr
 */

#pragma once

#include "DNA_space_types.h"

struct ARegion;
struct AZone;
struct ReportList;
struct bContext;
struct bContextDataResult;
struct bScreen;
struct Main;
struct rcti;
struct ScrArea;
struct ScrAreaMap;
struct ScrEdge;
struct ScrVert;
struct WorkSpaceLayout;
struct wmOperatorType;
struct wmWindow;

/* internal exports only */

enum eScreenDir {
  /** This can mean unset, unknown or invalid. */
  SCREEN_DIR_NONE = -1,
  /** West/Left. */
  SCREEN_DIR_W = 0,
  /** North/Up. */
  SCREEN_DIR_N = 1,
  /** East/Right. */
  SCREEN_DIR_E = 2,
  /** South/Down. */
  SCREEN_DIR_S = 3,
};

#define SCREEN_DIR_IS_VERTICAL(dir) (ELEM(dir, SCREEN_DIR_N, SCREEN_DIR_S))
#define SCREEN_DIR_IS_HORIZONTAL(dir) (ELEM(dir, SCREEN_DIR_W, SCREEN_DIR_E))

enum eScreenAxis {
  /** Horizontal. */
  SCREEN_AXIS_H = 'h',
  /** Vertical. */
  SCREEN_AXIS_V = 'v',
};

enum class AreaDockTarget {
  None,
  Right,  /* Right diagonal quadrant of area. */
  Left,   /* Left diagonal quadrant of area. */
  Top,    /* Top diagonal quadrant of area. */
  Bottom, /* Bottom diagonal quadrant of area. */
  Center, /* Middle portion of area. */
};

#define AZONEFADEIN (5.0f * U.widget_unit)  /* when #AZone is totally visible */
#define AZONEFADEOUT (6.5f * U.widget_unit) /* when we start seeing the #AZone */

/* Edges must be within these to allow joining. */
#define AREAJOINTOLERANCEX (AREAMINX * UI_SCALE_FAC)
#define AREAJOINTOLERANCEY (HEADERY * UI_SCALE_FAC)

/**
 * Expanded interaction influence of area borders.
 */
#define BORDERPADDING (U.border_width * UI_SCALE_FAC + 3.0f * UI_SCALE_FAC)

/**
 * Number of pixels of the area border corner radius.
 */
#define EDITORRADIUS (6.0f * UI_SCALE_FAC)

/* Less expansion needed for global edges. */
#define BORDERPADDING_GLOBAL (3.0f * UI_SCALE_FAC)

#define AREA_CLOSE_FADEOUT 0.15f /* seconds */
#define AREA_DOCK_FADEOUT 0.15f  /* seconds */
#define AREA_DOCK_FADEIN 0.15f   /* seconds */
#define AREA_JOIN_FADEOUT 0.15f  /* seconds */
#define AREA_SPLIT_FADEOUT 0.15f /* seconds */

/* `area.cc` */

/**
 * We swap spaces for full-screen to keep all allocated data area vertices were set.
 */
void ED_area_data_copy(ScrArea *area_dst, ScrArea *area_src, bool do_free);
void ED_area_data_swap(ScrArea *area_dst, ScrArea *area_src);
/* for quick toggle, can skip fades */
void region_toggle_hidden(bContext *C, ARegion *region, bool do_fade);

/* `screen_draw.cc` */

/**
 * Visual indication of the two areas involved in a proposed join.
 *
 * \param sa1: Area from which the resultant originates.
 * \param sa2: Target area that will be replaced.
 */
void screen_draw_join_highlight(
    const wmWindow *win, ScrArea *sa1, ScrArea *sa2, eScreenDir dir, float anim_factor);
void screen_draw_dock_preview(const wmWindow *win,
                              ScrArea *source,
                              ScrArea *target,
                              AreaDockTarget dock_target,
                              float factor,
                              int x,
                              int y,
                              float anim_factor);
void screen_draw_split_preview(ScrArea *area, eScreenAxis dir_axis, float factor);

void screen_draw_move_highlight(const wmWindow *win, bScreen *screen, eScreenAxis dir_axis);

void screen_draw_region_scale_highlight(ARegion *region);

void screen_animate_area_highlight(wmWindow *win,
                                   bScreen *screen,
                                   const rcti *rect,
                                   float inner[4],
                                   float outline[4],
                                   float seconds);

/* `screen_edit.cc` */

/**
 * Empty screen, with 1 dummy area without space-data. Uses window size.
 */
bScreen *screen_add(Main *bmain, const char *name, const rcti *rect);
void screen_data_copy(bScreen *to, bScreen *from);
/**
 * Prepare a newly created screen for initializing it as active screen.
 */
void screen_new_activate_prepare(const wmWindow *win, bScreen *screen_new);
void screen_change_update(bContext *C, wmWindow *win, bScreen *screen);
/**
 * \return the screen to activate.
 * \warning The returned screen may not always equal \a screen_new!
 */
void screen_change_prepare(
    bScreen *screen_old, bScreen *screen_new, Main *bmain, bContext *C, wmWindow *win);
ScrArea *area_split(const wmWindow *win,
                    bScreen *screen,
                    ScrArea *area,
                    eScreenAxis dir_axis,
                    float fac,
                    bool merge);
/**
 * Join any two neighboring areas. Might involve complex changes.
 */
int screen_area_join(
    bContext *C, ReportList *reports, bScreen *screen, ScrArea *sa1, ScrArea *sa2);
/**
 * with `sa_a` as center, `sa_b` is located at: 0=W, 1=N, 2=E, 3=S
 * -1 = not valid check.
 * used with join operator.
 */
eScreenDir area_getorientation(ScrArea *sa_a, ScrArea *sa_b);
/**
 * Get alignment offset of adjacent areas. 'dir' value is like #area_getorientation().
 */
void area_getoffsets(ScrArea *sa_a, ScrArea *sa_b, eScreenDir dir, int *r_offset1, int *r_offset2);
/**
 * Close a screen area, allowing most-aligned neighbor to take its place.
 */
bool screen_area_close(bContext *C, ReportList *reports, bScreen *screen, ScrArea *area);
void screen_area_spacelink_add(const Scene *scene, ScrArea *area, eSpace_Type space_type);
AZone *ED_area_actionzone_find_xy(ScrArea *area, const int xy[2]);

/**
 * \return true if any region polling state changed, and an area re-init is needed.
 */
bool area_regions_poll(bContext *C, const bScreen *screen, ScrArea *area);

/* `screen_geometry.cc` */

int screen_geom_area_height(const ScrArea *area);
int screen_geom_area_width(const ScrArea *area);
ScrVert *screen_geom_vertex_add_ex(ScrAreaMap *area_map, short x, short y);
ScrVert *screen_geom_vertex_add(bScreen *screen, short x, short y);
ScrEdge *screen_geom_edge_add_ex(ScrAreaMap *area_map, ScrVert *v1, ScrVert *v2);
ScrEdge *screen_geom_edge_add(bScreen *screen, ScrVert *v1, ScrVert *v2);
bool screen_geom_edge_is_horizontal(ScrEdge *se);
/**
 * \param bounds_rect: Either window or screen bounds.
 * Used to exclude edges along window/screen edges.
 */
ScrEdge *screen_geom_area_map_find_active_scredge(const ScrAreaMap *area_map,
                                                  const rcti *bounds_rect,
                                                  int mx,
                                                  int my,
                                                  int safety = BORDERPADDING);
/**
 * Need win size to make sure not to include edges along screen edge.
 */
ScrEdge *screen_geom_find_active_scredge(const wmWindow *win,
                                         const bScreen *screen,
                                         int mx,
                                         int my);
/**
 * \brief Main screen-layout calculation function.
 *
 * * Scale areas nicely on window size and DPI changes.
 * * Ensure areas have a minimum height.
 * * Correctly set global areas to their fixed height.
 */
void screen_geom_vertices_scale(const wmWindow *win, bScreen *screen);
/**
 * \return 0 if no split is possible, otherwise the screen-coordinate at which to split.
 */
short screen_geom_find_area_split_point(const ScrArea *area,
                                        const rcti *window_rect,
                                        eScreenAxis dir_axis,
                                        float fac);
/**
 * Select all edges that are directly or indirectly connected to \a edge.
 */
void screen_geom_select_connected_edge(const wmWindow *win, ScrEdge *edge);

/* `screen_context.cc` */

/**
 * Entry point for the screen context.
 */
int ed_screen_context(const bContext *C, const char *member, bContextDataResult *result);

extern "C" const char *screen_context_dir[]; /* doc access */

/* `screendump.cc` */

void SCREEN_OT_screenshot(wmOperatorType *ot);
void SCREEN_OT_screenshot_area(wmOperatorType *ot);

/* `workspace_layout_edit.cc` */

bool workspace_layout_set_poll(const WorkSpaceLayout *layout);
