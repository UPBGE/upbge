/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#pragma once

#include "DNA_windowmanager_types.h"

/* Operator types should be in exposed header. */

void OBJECT_OT_lineart_bake_strokes(struct wmOperatorType *ot);
void OBJECT_OT_lineart_bake_strokes_all(struct wmOperatorType *ot);
void OBJECT_OT_lineart_clear(struct wmOperatorType *ot);
void OBJECT_OT_lineart_clear_all(struct wmOperatorType *ot);

void WM_operatortypes_lineart(void);

struct LineartCache;

void MOD_lineart_clear_cache(struct LineartCache **lc);
