/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * \name Generic Gizmo Utilities.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct bContext;
struct wmGizmoGroupType;

/** Wrapper function (operator name can't be guessed). */
bool ED_gizmo_poll_or_unlink_delayed_from_operator(const struct bContext *C,
                                                   struct wmGizmoGroupType *gzgt,
                                                   const char *idname);

bool ED_gizmo_poll_or_unlink_delayed_from_tool_ex(const struct bContext *C,
                                                  struct wmGizmoGroupType *gzgt,
                                                  const char *gzgt_idname);

/** Use this as poll function directly for: #wmGizmoGroupType.poll */
bool ED_gizmo_poll_or_unlink_delayed_from_tool(const struct bContext *C,
                                               struct wmGizmoGroupType *gzgt);

#ifdef __cplusplus
}
#endif
