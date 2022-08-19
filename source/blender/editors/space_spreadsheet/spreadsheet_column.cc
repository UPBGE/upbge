/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_color.hh"
#include "BLI_cpp_type.hh"
#include "BLI_hash.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BKE_geometry_set.hh"

#include "spreadsheet_column.hh"
#include "spreadsheet_column_values.hh"

namespace blender::ed::spreadsheet {

eSpreadsheetColumnValueType cpp_type_to_column_type(const CPPType &type)
{
  if (type.is<bool>()) {
    return SPREADSHEET_VALUE_TYPE_BOOL;
  }
  if (type.is<int8_t>()) {
    return SPREADSHEET_VALUE_TYPE_INT8;
  }
  if (type.is<int>()) {
    return SPREADSHEET_VALUE_TYPE_INT32;
  }
  if (type.is<float>()) {
    return SPREADSHEET_VALUE_TYPE_FLOAT;
  }
  if (type.is<float2>()) {
    return SPREADSHEET_VALUE_TYPE_FLOAT2;
  }
  if (type.is<float3>()) {
    return SPREADSHEET_VALUE_TYPE_FLOAT3;
  }
  if (type.is<ColorGeometry4f>()) {
    return SPREADSHEET_VALUE_TYPE_COLOR;
  }
  if (type.is<std::string>()) {
    return SPREADSHEET_VALUE_TYPE_STRING;
  }
  if (type.is<InstanceReference>()) {
    return SPREADSHEET_VALUE_TYPE_INSTANCES;
  }
  if (type.is<ColorGeometry4b>()) {
    return SPREADSHEET_VALUE_TYPE_BYTE_COLOR;
  }

  return SPREADSHEET_VALUE_TYPE_UNKNOWN;
}

SpreadsheetColumnID *spreadsheet_column_id_new()
{
  SpreadsheetColumnID *column_id = MEM_cnew<SpreadsheetColumnID>(__func__);
  return column_id;
}

SpreadsheetColumnID *spreadsheet_column_id_copy(const SpreadsheetColumnID *src_column_id)
{
  SpreadsheetColumnID *new_column_id = spreadsheet_column_id_new();
  new_column_id->name = BLI_strdup(src_column_id->name);
  return new_column_id;
}

void spreadsheet_column_id_free(SpreadsheetColumnID *column_id)
{
  if (column_id->name != nullptr) {
    MEM_freeN(column_id->name);
  }
  MEM_freeN(column_id);
}

SpreadsheetColumn *spreadsheet_column_new(SpreadsheetColumnID *column_id)
{
  SpreadsheetColumn *column = MEM_cnew<SpreadsheetColumn>(__func__);
  column->id = column_id;
  return column;
}

void spreadsheet_column_assign_runtime_data(SpreadsheetColumn *column,
                                            const eSpreadsheetColumnValueType data_type,
                                            const StringRefNull display_name)
{
  column->data_type = data_type;
  MEM_SAFE_FREE(column->display_name);
  column->display_name = BLI_strdup(display_name.c_str());
}

SpreadsheetColumn *spreadsheet_column_copy(const SpreadsheetColumn *src_column)
{
  SpreadsheetColumnID *new_column_id = spreadsheet_column_id_copy(src_column->id);
  SpreadsheetColumn *new_column = spreadsheet_column_new(new_column_id);
  if (src_column->display_name != nullptr) {
    new_column->display_name = BLI_strdup(src_column->display_name);
  }
  return new_column;
}

void spreadsheet_column_free(SpreadsheetColumn *column)
{
  spreadsheet_column_id_free(column->id);
  MEM_SAFE_FREE(column->display_name);
  MEM_freeN(column);
}

}  // namespace blender::ed::spreadsheet
