/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edinterface
 */

#include "BLI_vector.hh"

#include "BKE_screen.h"

#include "RNA_access.h"

#include "ED_screen.h"

#include "UI_interface.h"
#include "UI_interface.hh"
#include "UI_resources.h"

#include "WM_api.h"

namespace blender::ui {

void context_path_add_generic(Vector<ContextPathItem> &path,
                              StructRNA &rna_type,
                              void *ptr,
                              const BIFIconID icon_override)
{
  /* Add the null check here to make calling functions less verbose. */
  if (!ptr) {
    return;
  }

  PointerRNA rna_ptr;
  RNA_pointer_create(nullptr, &rna_type, ptr, &rna_ptr);
  char name[128];
  RNA_struct_name_get_alloc(&rna_ptr, name, sizeof(name), nullptr);

  /* Use a blank icon by default to check whether to retrieve it automatically from the type. */
  const BIFIconID icon = icon_override == ICON_NONE ?
                             static_cast<BIFIconID>(RNA_struct_ui_icon(rna_ptr.type)) :
                             icon_override;

  path.append({name, static_cast<int>(icon)});
}

/* -------------------------------------------------------------------- */
/** \name Breadcrumb Template
 * \{ */

void template_breadcrumbs(uiLayout &layout, Span<ContextPathItem> context_path)
{
  uiLayout *row = uiLayoutRow(&layout, true);
  uiLayoutSetAlignment(&layout, UI_LAYOUT_ALIGN_LEFT);

  for (const int i : context_path.index_range()) {
    uiLayout *sub_row = uiLayoutRow(row, true);
    uiLayoutSetAlignment(sub_row, UI_LAYOUT_ALIGN_LEFT);

    if (i > 0) {
      uiItemL(sub_row, "", ICON_RIGHTARROW_THIN);
    }
    uiItemL(sub_row, context_path[i].name.c_str(), context_path[i].icon);
  }
}

/** \} */

}  // namespace blender::ui
