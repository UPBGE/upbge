/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_screen.h"

#include "BLT_translation.h"

#include "spreadsheet_dataset_draw.hh"
#include "spreadsheet_intern.hh"

namespace blender::ed::spreadsheet {

void spreadsheet_data_set_region_panels_register(ARegionType &region_type)
{
  PanelType *panel_type = MEM_cnew<PanelType>(__func__);
  strcpy(panel_type->idname, "SPREADSHEET_PT_data_set");
  strcpy(panel_type->label, N_("Data Set"));
  strcpy(panel_type->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  panel_type->flag = PANEL_TYPE_NO_HEADER;
  panel_type->draw = spreadsheet_data_set_panel_draw;
  BLI_addtail(&region_type.paneltypes, panel_type);
}

}  // namespace blender::ed::spreadsheet
