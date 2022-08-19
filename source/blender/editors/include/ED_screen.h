/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view2d_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "DNA_object_enums.h"

#include "WM_types.h"

#include "BLI_compiler_attrs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ARegion;
struct Depsgraph;
struct IDProperty;
struct Main;
struct MenuType;
struct Scene;
struct SpaceLink;
struct WorkSpace;
struct WorkSpaceInstanceHook;
struct bContext;
struct bScreen;
struct rcti;
struct uiBlock;
struct uiLayout;
struct wmKeyConfig;
struct wmMsgSubscribeKey;
struct wmMsgSubscribeValue;
struct wmNotifier;
struct wmOperatorType;
struct wmRegionListenerParams;
struct wmRegionMessageSubscribeParams;
struct wmSpaceTypeListenerParams;
struct wmWindow;
struct wmWindowManager;

/* regions */
/** Only exported for WM. */
void ED_region_do_listen(struct wmRegionListenerParams *params);
/** Only exported for WM. */
void ED_region_do_layout(struct bContext *C, struct ARegion *region);
/** Only exported for WM. */
void ED_region_do_draw(struct bContext *C, struct ARegion *region);
void ED_region_exit(struct bContext *C, struct ARegion *region);
/**
 * Utility to exit and free an area-region. Screen level regions (menus/popups) need to be treated
 * slightly differently, see #ui_region_temp_remove().
 */
void ED_region_remove(struct bContext *C, struct ScrArea *area, struct ARegion *region);
void ED_region_pixelspace(const struct ARegion *region);
/**
 * Call to move a popup window (keep OpenGL context free!)
 */
void ED_region_update_rect(struct ARegion *region);
/**
 * Externally called for floating regions like menus.
 */
void ED_region_floating_init(struct ARegion *region);
void ED_region_tag_redraw(struct ARegion *region);
void ED_region_tag_redraw_partial(struct ARegion *region, const struct rcti *rct, bool rebuild);
void ED_region_tag_redraw_cursor(struct ARegion *region);
void ED_region_tag_redraw_no_rebuild(struct ARegion *region);
void ED_region_tag_refresh_ui(struct ARegion *region);
/**
 * Tag editor overlays to be redrawn. If in doubt about which parts need to be redrawn (partial
 * clipping rectangle set), redraw everything.
 */
void ED_region_tag_redraw_editor_overlays(struct ARegion *region);

/**
 * Set the temporary update flag for property search.
 */
void ED_region_search_filter_update(const struct ScrArea *area, struct ARegion *region);
/**
 * Returns the search string if the space type and region type support property search.
 */
const char *ED_area_region_search_filter_get(const struct ScrArea *area,
                                             const struct ARegion *region);

void ED_region_panels_init(struct wmWindowManager *wm, struct ARegion *region);
void ED_region_panels_ex(const struct bContext *C, struct ARegion *region, const char *contexts[]);
void ED_region_panels(const struct bContext *C, struct ARegion *region);
/**
 * \param contexts: A NULL terminated array of context strings to match against.
 * Matching against any of these strings will draw the panel.
 * Can be NULL to skip context checks.
 */
void ED_region_panels_layout_ex(const struct bContext *C,
                                struct ARegion *region,
                                struct ListBase *paneltypes,
                                const char *contexts[],
                                const char *category_override);
/**
 * Build the same panel list as #ED_region_panels_layout_ex and checks whether any
 * of the panels contain a search result based on the area / region's search filter.
 */
bool ED_region_property_search(const struct bContext *C,
                               struct ARegion *region,
                               struct ListBase *paneltypes,
                               const char *contexts[],
                               const char *category_override);

void ED_region_panels_layout(const struct bContext *C, struct ARegion *region);
void ED_region_panels_draw(const struct bContext *C, struct ARegion *region);

void ED_region_header_init(struct ARegion *region);
void ED_region_header(const struct bContext *C, struct ARegion *region);
void ED_region_header_layout(const struct bContext *C, struct ARegion *region);
void ED_region_header_draw(const struct bContext *C, struct ARegion *region);

void ED_region_cursor_set(struct wmWindow *win, struct ScrArea *area, struct ARegion *region);
/**
 * Exported to all editors, uses fading default.
 */
void ED_region_toggle_hidden(struct bContext *C, struct ARegion *region);
/**
 * For use after changing visibility of regions.
 */
void ED_region_visibility_change_update(struct bContext *C,
                                        struct ScrArea *area,
                                        struct ARegion *region);
/* screen_ops.c */

/**
 * \note Assumes that \a region itself is not a split version from previous region.
 */
void ED_region_visibility_change_update_animated(struct bContext *C,
                                                 struct ScrArea *area,
                                                 struct ARegion *region);

void ED_region_info_draw(struct ARegion *region,
                         const char *text,
                         float fill_color[4],
                         bool full_redraw);
void ED_region_info_draw_multiline(ARegion *region,
                                   const char *text_array[],
                                   float fill_color[4],
                                   bool full_redraw);
void ED_region_image_metadata_panel_draw(struct ImBuf *ibuf, struct uiLayout *layout);
void ED_region_grid_draw(struct ARegion *region, float zoomx, float zoomy, float x0, float y0);
float ED_region_blend_alpha(struct ARegion *region);
void ED_region_visible_rect_calc(struct ARegion *region, struct rcti *rect);
const rcti *ED_region_visible_rect(ARegion *region);
/**
 * Overlapping regions only in the following restricted cases.
 */
bool ED_region_is_overlap(int spacetype, int regiontype);

int ED_region_snap_size_test(const struct ARegion *region);
bool ED_region_snap_size_apply(struct ARegion *region, int snap_flag);

/* message_bus callbacks */
void ED_region_do_msg_notify_tag_redraw(struct bContext *C,
                                        struct wmMsgSubscribeKey *msg_key,
                                        struct wmMsgSubscribeValue *msg_val);
void ED_area_do_msg_notify_tag_refresh(struct bContext *C,
                                       struct wmMsgSubscribeKey *msg_key,
                                       struct wmMsgSubscribeValue *msg_val);

/**
 * Follow #ARegionType.message_subscribe.
 */
void ED_area_do_mgs_subscribe_for_tool_header(const struct wmRegionMessageSubscribeParams *params);
void ED_area_do_mgs_subscribe_for_tool_ui(const struct wmRegionMessageSubscribeParams *params);

/* message bus */

/**
 * Generate subscriptions for this region.
 */
void ED_region_message_subscribe(struct wmRegionMessageSubscribeParams *params);

/* spaces */

/**
 * \note Keymap definitions are registered only once per WM initialize,
 * usually on file read, using the keymap the actual areas/regions add the handlers.
 * \note Called in wm.c. */
void ED_spacetypes_keymap(struct wmKeyConfig *keyconf);
/**
 * Returns offset for next button in header.
 */
int ED_area_header_switchbutton(const struct bContext *C, struct uiBlock *block, int yco);

/* areas */
/**
 * Called in screen_refresh, or screens_init, also area size changes.
 */
void ED_area_init(struct wmWindowManager *wm, struct wmWindow *win, struct ScrArea *area);
void ED_area_exit(struct bContext *C, struct ScrArea *area);
int ED_screen_area_active(const struct bContext *C);
void ED_screen_global_areas_refresh(struct wmWindow *win);
void ED_screen_global_areas_sync(struct wmWindow *win);
/** Only exported for WM. */
void ED_area_do_listen(struct wmSpaceTypeListenerParams *params);
void ED_area_tag_redraw(ScrArea *area);
void ED_area_tag_redraw_no_rebuild(ScrArea *area);
void ED_area_tag_redraw_regiontype(ScrArea *area, int type);
void ED_area_tag_refresh(ScrArea *area);
/**
 * Only exported for WM.
 */
void ED_area_do_refresh(struct bContext *C, ScrArea *area);
struct AZone *ED_area_azones_update(ScrArea *area, const int mouse_xy[2]);
/**
 * Use NULL to disable it.
 */
void ED_area_status_text(ScrArea *area, const char *str);
/**
 * \param skip_region_exit: Skip calling area exit callback. Set for opening temp spaces.
 */
void ED_area_newspace(struct bContext *C, ScrArea *area, int type, bool skip_region_exit);
void ED_area_prevspace(struct bContext *C, ScrArea *area);
void ED_area_swapspace(struct bContext *C, ScrArea *sa1, ScrArea *sa2);
int ED_area_headersize(void);
int ED_area_footersize(void);
/**
 * \return the final height of a global \a area, accounting for DPI.
 */
int ED_area_global_size_y(const ScrArea *area);
int ED_area_global_min_size_y(const ScrArea *area);
int ED_area_global_max_size_y(const ScrArea *area);
bool ED_area_is_global(const ScrArea *area);
/**
 * For now we just assume all global areas are made up out of horizontal bars
 * with the same size. A fixed size could be stored in ARegion instead if needed.
 *
 * \return the DPI aware height of a single bar/region in global areas.
 */
int ED_region_global_size_y(void);
void ED_area_update_region_sizes(struct wmWindowManager *wm,
                                 struct wmWindow *win,
                                 struct ScrArea *area);
bool ED_area_has_shared_border(struct ScrArea *a, struct ScrArea *b);
ScrArea *ED_area_offscreen_create(struct wmWindow *win, eSpace_Type space_type);
void ED_area_offscreen_free(struct wmWindowManager *wm,
                            struct wmWindow *win,
                            struct ScrArea *area);

/**
 * Search all screens, even non-active or overlapping (multiple windows), return the most-likely
 * area of interest. xy is relative to active window, like all similar functions.
 */
ScrArea *ED_area_find_under_cursor(const struct bContext *C, int spacetype, const int xy[2]);

ScrArea *ED_screen_areas_iter_first(const struct wmWindow *win, const bScreen *screen);
ScrArea *ED_screen_areas_iter_next(const bScreen *screen, const ScrArea *area);
/**
 * Iterate over all areas visible in the screen (screen as in everything
 * visible in the window, not just bScreen).
 * \note Skips global areas with flag GLOBAL_AREA_IS_HIDDEN.
 */
#define ED_screen_areas_iter(win, screen, area_name) \
  for (ScrArea *area_name = ED_screen_areas_iter_first(win, screen); area_name != NULL; \
       area_name = ED_screen_areas_iter_next(screen, area_name))
#define ED_screen_verts_iter(win, screen, vert_name) \
  for (ScrVert *vert_name = (win)->global_areas.vertbase.first ? \
                                (win)->global_areas.vertbase.first : \
                                (screen)->vertbase.first; \
       vert_name != NULL; \
       vert_name = (vert_name == (win)->global_areas.vertbase.last) ? (screen)->vertbase.first : \
                                                                      vert_name->next)

/* screens */

/**
 * File read, set all screens, ....
 */
void ED_screens_init(struct Main *bmain, struct wmWindowManager *wm);
/**
 * Only for edge lines between areas.
 */
void ED_screen_draw_edges(struct wmWindow *win);

/**
 * Make this screen usable.
 * for file read and first use, for scaling window, area moves.
 */
void ED_screen_refresh(struct wmWindowManager *wm, struct wmWindow *win);
void ED_screen_ensure_updated(struct wmWindowManager *wm,
                              struct wmWindow *win,
                              struct bScreen *screen);
void ED_screen_do_listen(struct bContext *C, struct wmNotifier *note);
/**
 * \brief Change the active screen.
 *
 * Operator call, WM + Window + screen already existed before
 *
 * \warning Do NOT call in area/region queues!
 * \returns if screen changing was successful.
 */
bool ED_screen_change(struct bContext *C, struct bScreen *screen);
void ED_screen_scene_change(struct bContext *C,
                            struct wmWindow *win,
                            struct Scene *scene,
                            bool refresh_toolsystem);
/**
 * Called in wm_event_system.c. sets state vars in screen, cursors.
 * event type is mouse move.
 */
void ED_screen_set_active_region(struct bContext *C, struct wmWindow *win, const int xy[2]);
void ED_screen_exit(struct bContext *C, struct wmWindow *window, struct bScreen *screen);
/**
 * redraws: uses defines from `stime->redraws`
 * \param enable: 1 - forward on, -1 - backwards on, 0 - off.
 */
void ED_screen_animation_timer(struct bContext *C, int redraws, int sync, int enable);
void ED_screen_animation_timer_update(struct bScreen *screen, int redraws);
void ED_screen_restore_temp_type(struct bContext *C, ScrArea *area);
ScrArea *ED_screen_full_newspace(struct bContext *C, ScrArea *area, int type);
/**
 * \a was_prev_temp for the case previous space was a temporary full-screen as well
 */
void ED_screen_full_prevspace(struct bContext *C, ScrArea *area);
/**
 * Restore a screen / area back to default operation, after temp full-screen modes.
 */
void ED_screen_full_restore(struct bContext *C, ScrArea *area);
/**
 * Create a new temporary screen with a maximized, empty area.
 * This can be closed with #ED_screen_state_toggle().
 *
 * Use this to just create a new maximized screen/area, rather than maximizing an existing one.
 * Otherwise, maximize with #ED_screen_state_toggle().
 */
bScreen *ED_screen_state_maximized_create(struct bContext *C);
/**
 * This function toggles: if area is maximized/full then the parent will be restored.
 *
 * Use #ED_screen_state_maximized_create() if you do not want the toggle behavior when changing to
 * a maximized area. I.e. if you just want to open a new maximized screen/area, not maximize a
 * specific area. In the former case, space data of the maximized and non-maximized area should be
 * independent, in the latter it should be the same.
 *
 * \warning \a area may be freed.
 */
struct ScrArea *ED_screen_state_toggle(struct bContext *C,
                                       struct wmWindow *win,
                                       struct ScrArea *area,
                                       short state);
/**
 * Wrapper to open a temporary space either as fullscreen space, or as separate window, as defined
 * by \a display_type.
 *
 * \param title: Title to set for the window, if a window is spawned.
 * \param x, y: Position of the window, if a window is spawned.
 * \param sizex, sizey: Dimensions of the window, if a window is spawned.
 */
ScrArea *ED_screen_temp_space_open(struct bContext *C,
                                   const char *title,
                                   int x,
                                   int y,
                                   int sizex,
                                   int sizey,
                                   eSpace_Type space_type,
                                   int display_type,
                                   bool dialog);
void ED_screens_header_tools_menu_create(struct bContext *C, struct uiLayout *layout, void *arg);
void ED_screens_footer_tools_menu_create(struct bContext *C, struct uiLayout *layout, void *arg);
void ED_screens_navigation_bar_tools_menu_create(struct bContext *C,
                                                 struct uiLayout *layout,
                                                 void *arg);
/**
 * \return true if any active area requires to see in 3D.
 */
bool ED_screen_stereo3d_required(const struct bScreen *screen, const struct Scene *scene);
Scene *ED_screen_scene_find(const struct bScreen *screen, const struct wmWindowManager *wm);
/**
 * Find the scene displayed in \a screen.
 * \note Assumes \a screen to be visible/active!
 */
Scene *ED_screen_scene_find_with_window(const struct bScreen *screen,
                                        const struct wmWindowManager *wm,
                                        struct wmWindow **r_window);
ScrArea *ED_screen_area_find_with_spacedata(const bScreen *screen,
                                            const struct SpaceLink *sl,
                                            bool only_visible);
struct wmWindow *ED_screen_window_find(const struct bScreen *screen,
                                       const struct wmWindowManager *wm);
/**
 * Render the preview for a screen layout in \a screen.
 */
void ED_screen_preview_render(const struct bScreen *screen,
                              int size_x,
                              int size_y,
                              unsigned int *r_rect) ATTR_NONNULL();

/* workspaces */

struct WorkSpace *ED_workspace_add(struct Main *bmain, const char *name) ATTR_NONNULL();
/**
 * \brief Change the active workspace.
 *
 * Operator call, WM + Window + screen already existed before
 * Pretty similar to #ED_screen_change since changing workspace also changes screen.
 *
 * \warning Do NOT call in area/region queues!
 * \returns if workspace changing was successful.
 */
bool ED_workspace_change(struct WorkSpace *workspace_new,
                         struct bContext *C,
                         struct wmWindowManager *wm,
                         struct wmWindow *win) ATTR_NONNULL();
/**
 * Duplicate a workspace including its layouts. Does not activate the workspace, but
 * it stores the screen-layout to be activated (BKE_workspace_temp_layout_store)
 */
struct WorkSpace *ED_workspace_duplicate(struct WorkSpace *workspace_old,
                                         struct Main *bmain,
                                         struct wmWindow *win);
/**
 * \return if succeeded.
 */
bool ED_workspace_delete(struct WorkSpace *workspace,
                         struct Main *bmain,
                         struct bContext *C,
                         struct wmWindowManager *wm) ATTR_NONNULL();
/**
 * Some editor data may need to be synced with scene data (3D View camera and layers).
 * This function ensures data is synced for editors in active layout of \a workspace.
 */
void ED_workspace_scene_data_sync(struct WorkSpaceInstanceHook *hook, Scene *scene) ATTR_NONNULL();
/**
 * Make sure there is a non-full-screen layout to switch to that isn't used yet by an other window.
 * Needed for workspace or screen switching to ensure valid screens.
 *
 * \param layout_fallback_base: As last resort, this layout is duplicated and returned.
 */
struct WorkSpaceLayout *ED_workspace_screen_change_ensure_unused_layout(
    struct Main *bmain,
    struct WorkSpace *workspace,
    struct WorkSpaceLayout *layout_new,
    const struct WorkSpaceLayout *layout_fallback_base,
    struct wmWindow *win) ATTR_NONNULL();
/**
 * Empty screen, with 1 dummy area without space-data. Uses window size.
 */
struct WorkSpaceLayout *ED_workspace_layout_add(struct Main *bmain,
                                                struct WorkSpace *workspace,
                                                struct wmWindow *win,
                                                const char *name) ATTR_NONNULL();
struct WorkSpaceLayout *ED_workspace_layout_duplicate(struct Main *bmain,
                                                      struct WorkSpace *workspace,
                                                      const struct WorkSpaceLayout *layout_old,
                                                      struct wmWindow *win) ATTR_NONNULL();
/**
 * \warning Only call outside of area/region loops!
 * \return true if succeeded.
 */
bool ED_workspace_layout_delete(struct WorkSpace *workspace,
                                struct WorkSpaceLayout *layout_old,
                                struct bContext *C) ATTR_NONNULL();
bool ED_workspace_layout_cycle(struct WorkSpace *workspace, short direction, struct bContext *C)
    ATTR_NONNULL();

void ED_workspace_status_text(struct bContext *C, const char *str);

/* anim */
/**
 * Results in fully updated anim system.
 */
void ED_update_for_newframe(struct Main *bmain, struct Depsgraph *depsgraph);

/**
 * Update frame rate info for viewport drawing.
 */
void ED_refresh_viewport_fps(struct bContext *C);
/**
 * Toggle operator.
 */
int ED_screen_animation_play(struct bContext *C, int sync, int mode);
/**
 * Find window that owns the animation timer.
 */
bScreen *ED_screen_animation_playing(const struct wmWindowManager *wm);
bScreen *ED_screen_animation_no_scrub(const struct wmWindowManager *wm);

/* screen keymaps */
/* called in spacetypes.c */
void ED_operatortypes_screen(void);
/* called in spacetypes.c */
void ED_keymap_screen(struct wmKeyConfig *keyconf);
/**
 * Workspace key-maps.
 */
void ED_operatortypes_workspace(void);

/* operators; context poll callbacks */

bool ED_operator_screenactive(struct bContext *C);
bool ED_operator_screenactive_nobackground(struct bContext *C);
/**
 * When mouse is over area-edge.
 */
bool ED_operator_screen_mainwinactive(struct bContext *C);
bool ED_operator_areaactive(struct bContext *C);
bool ED_operator_regionactive(struct bContext *C);

bool ED_operator_scene(struct bContext *C);
bool ED_operator_scene_editable(struct bContext *C);
bool ED_operator_objectmode(struct bContext *C);
/**
 * Same as #ED_operator_objectmode() but additionally sets a "disabled hint". That is, a message
 * to be displayed to the user explaining why the operator can't be used in current context.
 */
bool ED_operator_objectmode_poll_msg(struct bContext *C);

bool ED_operator_view3d_active(struct bContext *C);
bool ED_operator_region_view3d_active(struct bContext *C);
/**
 * Generic for any view2d which uses anim_ops.
 */
bool ED_operator_animview_active(struct bContext *C);
bool ED_operator_outliner_active(struct bContext *C);
bool ED_operator_outliner_active_no_editobject(struct bContext *C);
/**
 * \note Will return true for file spaces in either file or asset browsing mode! See
 * #ED_operator_file_browsing_active() (file browsing only) and
 * #ED_operator_asset_browsing_active() (asset browsing only).
 */
bool ED_operator_file_active(struct bContext *C);
/**
 * \note Will only return true if the file space is in file browsing mode, not asset browsing! See
 * #ED_operator_file_active() (file or asset browsing) and
 * #ED_operator_asset_browsing_active() (asset browsing only).
 */
bool ED_operator_file_browsing_active(struct bContext *C);
bool ED_operator_asset_browsing_active(struct bContext *C);
bool ED_operator_spreadsheet_active(struct bContext *C);
bool ED_operator_action_active(struct bContext *C);
bool ED_operator_buttons_active(struct bContext *C);
bool ED_operator_node_active(struct bContext *C);
bool ED_operator_node_editable(struct bContext *C);
bool ED_operator_graphedit_active(struct bContext *C);
bool ED_operator_sequencer_active(struct bContext *C);
bool ED_operator_sequencer_active_editable(struct bContext *C);
bool ED_operator_image_active(struct bContext *C);
bool ED_operator_nla_active(struct bContext *C);
bool ED_operator_info_active(struct bContext *C);
bool ED_operator_logic_active(struct bContext *C);
bool ED_operator_console_active(struct bContext *C);

bool ED_operator_object_active(struct bContext *C);
bool ED_operator_object_active_editable_ex(struct bContext *C, const Object *ob);
bool ED_operator_object_active_editable(struct bContext *C);
/**
 * Object must be editable and fully local (i.e. not an override).
 */
bool ED_operator_object_active_local_editable_ex(struct bContext *C, const Object *ob);
bool ED_operator_object_active_local_editable(struct bContext *C);
bool ED_operator_object_active_editable_mesh(struct bContext *C);
bool ED_operator_object_active_editable_font(struct bContext *C);
bool ED_operator_editable_mesh(struct bContext *C);
bool ED_operator_editmesh(struct bContext *C);
bool ED_operator_editmesh_view3d(struct bContext *C);
bool ED_operator_editmesh_region_view3d(struct bContext *C);
bool ED_operator_editmesh_auto_smooth(struct bContext *C);
bool ED_operator_editarmature(struct bContext *C);
bool ED_operator_editcurve(struct bContext *C);
bool ED_operator_editcurve_3d(struct bContext *C);
bool ED_operator_editsurf(struct bContext *C);
bool ED_operator_editsurfcurve(struct bContext *C);
bool ED_operator_editsurfcurve_region_view3d(struct bContext *C);
bool ED_operator_editfont(struct bContext *C);
bool ED_operator_editlattice(struct bContext *C);
bool ED_operator_editmball(struct bContext *C);
/**
 * Wrapper for #ED_space_image_show_uvedit.
 */
bool ED_operator_uvedit(struct bContext *C);
bool ED_operator_uvedit_space_image(struct bContext *C);
bool ED_operator_uvmap(struct bContext *C);
bool ED_operator_posemode_exclusive(struct bContext *C);
/**
 * Object must be editable, fully local (i.e. not an override), and exclusively in Pose mode.
 */
bool ED_operator_object_active_local_editable_posemode_exclusive(struct bContext *C);
/**
 * Allows for pinned pose objects to be used in the object buttons
 * and the non-active pose object to be used in the 3D view.
 */
bool ED_operator_posemode_context(struct bContext *C);
bool ED_operator_posemode(struct bContext *C);
bool ED_operator_posemode_local(struct bContext *C);
bool ED_operator_camera_poll(struct bContext *C);

/* screen_user_menu.c */

bUserMenu **ED_screen_user_menus_find(const struct bContext *C, uint *r_len);
struct bUserMenu *ED_screen_user_menu_ensure(struct bContext *C);

struct bUserMenuItem_Op *ED_screen_user_menu_item_find_operator(struct ListBase *lb,
                                                                const struct wmOperatorType *ot,
                                                                struct IDProperty *prop,
                                                                wmOperatorCallContext opcontext);
struct bUserMenuItem_Menu *ED_screen_user_menu_item_find_menu(struct ListBase *lb,
                                                              const struct MenuType *mt);
struct bUserMenuItem_Prop *ED_screen_user_menu_item_find_prop(struct ListBase *lb,
                                                              const char *context_data_path,
                                                              const char *prop_id,
                                                              int prop_index);

void ED_screen_user_menu_item_add_operator(struct ListBase *lb,
                                           const char *ui_name,
                                           const struct wmOperatorType *ot,
                                           const struct IDProperty *prop,
                                           wmOperatorCallContext opcontext);
void ED_screen_user_menu_item_add_menu(struct ListBase *lb,
                                       const char *ui_name,
                                       const struct MenuType *mt);
void ED_screen_user_menu_item_add_prop(ListBase *lb,
                                       const char *ui_name,
                                       const char *context_data_path,
                                       const char *prop_id,
                                       int prop_index);

void ED_screen_user_menu_item_remove(struct ListBase *lb, struct bUserMenuItem *umi);
void ED_screen_user_menu_register(void);

/* Cache display helpers */

void ED_region_cache_draw_background(struct ARegion *region);
void ED_region_cache_draw_curfra_label(int framenr, float x, float y);
void ED_region_cache_draw_cached_segments(
    struct ARegion *region, int num_segments, const int *points, int sfra, int efra);

/* area_utils.c */

/**
 * Callback for #ARegionType.message_subscribe
 */
void ED_region_generic_tools_region_message_subscribe(
    const struct wmRegionMessageSubscribeParams *params);
/**
 * Callback for #ARegionType.snap_size
 */
int ED_region_generic_tools_region_snap_size(const struct ARegion *region, int size, int axis);

/* area_query.c */

bool ED_region_overlap_isect_x(const ARegion *region, int event_x);
bool ED_region_overlap_isect_y(const ARegion *region, int event_y);
bool ED_region_overlap_isect_xy(const ARegion *region, const int event_xy[2]);
bool ED_region_overlap_isect_any_xy(const ScrArea *area, const int event_xy[2]);
bool ED_region_overlap_isect_x_with_margin(const ARegion *region, int event_x, int margin);
bool ED_region_overlap_isect_y_with_margin(const ARegion *region, int event_y, int margin);
bool ED_region_overlap_isect_xy_with_margin(const ARegion *region,
                                            const int event_xy[2],
                                            int margin);

bool ED_region_panel_category_gutter_calc_rect(const ARegion *region, rcti *r_region_gutter);
bool ED_region_panel_category_gutter_isect_xy(const ARegion *region, const int event_xy[2]);

/**
 * \note This may return true for multiple overlapping regions.
 * If it matters, check overlapped regions first (#ARegion.overlap).
 */
bool ED_region_contains_xy(const struct ARegion *region, const int event_xy[2]);
/**
 * Similar to #BKE_area_find_region_xy() but when \a event_xy intersects an overlapping region,
 * this returns the region that is visually under the cursor. E.g. when over the
 * transparent part of the region, it returns the region underneath.
 *
 * The overlapping region is determined using the #ED_region_contains_xy() query.
 */
ARegion *ED_area_find_region_xy_visual(const ScrArea *area, int regiontype, const int event_xy[2]);

/* interface_region_hud.c */

struct ARegionType *ED_area_type_hud(int space_type);
void ED_area_type_hud_clear(struct wmWindowManager *wm, ScrArea *area_keep);
void ED_area_type_hud_ensure(struct bContext *C, struct ScrArea *area);

/**
 * Default key-maps, bit-flags (matches order of evaluation).
 */
enum {
  ED_KEYMAP_UI = (1 << 1),
  ED_KEYMAP_GIZMO = (1 << 2),
  ED_KEYMAP_TOOL = (1 << 3),
  ED_KEYMAP_VIEW2D = (1 << 4),
  ED_KEYMAP_ANIMATION = (1 << 6),
  ED_KEYMAP_FRAMES = (1 << 7),
  ED_KEYMAP_HEADER = (1 << 8),
  ED_KEYMAP_FOOTER = (1 << 9),
  ED_KEYMAP_GPENCIL = (1 << 10),
  ED_KEYMAP_NAVBAR = (1 << 11),
};

/** #SCREEN_OT_space_context_cycle direction. */
typedef enum eScreenCycle {
  SPACE_CONTEXT_CYCLE_PREV,
  SPACE_CONTEXT_CYCLE_NEXT,
} eScreenCycle;

/* UPBGE */
void ED_screen_refresh_blenderplayer(struct wmWindow *win);
/**************************/

#ifdef __cplusplus
}
#endif
