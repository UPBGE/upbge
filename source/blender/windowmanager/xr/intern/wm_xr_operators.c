/*
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
 */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR Operators
 *
 * Collection of XR-related operators.
 */

#include "BLI_listbase.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "GHOST_Types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_xr_intern.h"

/* -------------------------------------------------------------------- */
/** \name XR Session Toggle
 *
 * Toggles an XR session, creating an XR context if necessary.
 * \{ */

static void wm_xr_session_update_screen(Main *bmain, const wmXrData *xr_data)
{
  const bool session_exists = WM_xr_session_exists(xr_data);

  for (bScreen *screen = bmain->screens.first; screen; screen = screen->id.next) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      LISTBASE_FOREACH (SpaceLink *, slink, &area->spacedata) {
        if (slink->spacetype == SPACE_VIEW3D) {
          View3D *v3d = (View3D *)slink;

          if (v3d->flag & V3D_XR_SESSION_MIRROR) {
            ED_view3d_xr_mirror_update(area, v3d, session_exists);
          }

          if (session_exists) {
            wmWindowManager *wm = bmain->wm.first;
            const Scene *scene = WM_windows_scene_get_from_screen(wm, screen);

            ED_view3d_xr_shading_update(wm, v3d, scene);
          }
          /* Ensure no 3D View is tagged as session root. */
          else {
            v3d->runtime.flag &= ~V3D_RUNTIME_XR_SESSION_ROOT;
          }
        }
      }
    }
  }

  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);
}

static void wm_xr_session_update_screen_on_exit_cb(const wmXrData *xr_data)
{
  /* Just use G_MAIN here, storing main isn't reliable enough on file read or exit. */
  wm_xr_session_update_screen(G_MAIN, xr_data);
}

static int wm_xr_session_toggle_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  View3D *v3d = CTX_wm_view3d(C);

  /* Lazy-create xr context - tries to dynlink to the runtime, reading active_runtime.json. */
  if (wm_xr_init(wm) == false) {
    return OPERATOR_CANCELLED;
  }

  v3d->runtime.flag |= V3D_RUNTIME_XR_SESSION_ROOT;
  wm_xr_session_toggle(C, wm, win, wm_xr_session_update_screen_on_exit_cb);
  wm_xr_session_update_screen(bmain, &wm->xr);

  WM_event_add_notifier(C, NC_WM | ND_XR_DATA_CHANGED, NULL);

  return OPERATOR_FINISHED;
}

static void WM_OT_xr_session_toggle(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Toggle VR Session";
  ot->idname = "WM_OT_xr_session_toggle";
  ot->description =
      "Open a view for use with virtual reality headsets, or close it if already "
      "opened";

  /* callbacks */
  ot->exec = wm_xr_session_toggle_exec;
  ot->poll = ED_operator_view3d_active;

  /* XXX INTERNAL just to hide it from the search menu by default, an Add-on will expose it in the
   * UI instead. Not meant as a permanent solution. */
  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Registration
 * \{ */

void wm_xr_operatortypes_register(void)
{
  WM_operatortype_append(WM_OT_xr_session_toggle);
}

/** \} */
