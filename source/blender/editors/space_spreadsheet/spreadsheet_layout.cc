/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iomanip>
#include <sstream>

#include <fmt/format.h>

#include "BLF_api.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_instances.hh"

#include "spreadsheet_column_values.hh"
#include "spreadsheet_data_source_geometry.hh"
#include "spreadsheet_layout.hh"

#include "DNA_meshdata_types.h"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

namespace blender::ed::spreadsheet {

class SpreadsheetLayoutDrawer : public SpreadsheetDrawer {
 private:
  const SpreadsheetLayout &spreadsheet_layout_;

 public:
  SpreadsheetLayoutDrawer(const SpreadsheetLayout &spreadsheet_layout)
      : spreadsheet_layout_(spreadsheet_layout)
  {
    tot_columns = spreadsheet_layout.columns.size();
    tot_rows = spreadsheet_layout.row_indices.size();
    left_column_width = spreadsheet_layout.index_column_width;
  }

  void draw_top_row_cell(int column_index, const CellDrawParams &params) const final
  {
    const StringRefNull name = spreadsheet_layout_.columns[column_index].values->name();
    uiBut *but = uiDefIconTextBut(params.block,
                                  ButType::Label,
                                  0,
                                  ICON_NONE,
                                  name,
                                  params.xmin,
                                  params.ymin,
                                  params.width,
                                  params.height,
                                  nullptr,
                                  std::nullopt);
    UI_but_func_tooltip_set(
        but,
        [](bContext * /*C*/, void *arg, blender::StringRef /*tip*/) {
          return *static_cast<std::string *>(arg);
        },
        MEM_new<std::string>(__func__, name),
        [](void *arg) { MEM_delete(static_cast<std::string *>(arg)); });
    /* Center-align column headers. */
    UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
    UI_but_drawflag_disable(but, UI_BUT_TEXT_RIGHT);
  }

  void draw_left_column_cell(int row_index, const CellDrawParams &params) const final
  {
    const int real_index = spreadsheet_layout_.row_indices[row_index];
    std::string index_str = std::to_string(real_index);
    uiBut *but = uiDefIconTextBut(params.block,
                                  ButType::Label,
                                  0,
                                  ICON_NONE,
                                  index_str,
                                  params.xmin,
                                  params.ymin,
                                  params.width,
                                  params.height,
                                  nullptr,
                                  std::nullopt);
    /* Right-align indices. */
    UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
  }

  void draw_content_cell(int row_index, int column_index, const CellDrawParams &params) const final
  {
    const int real_index = spreadsheet_layout_.row_indices[row_index];
    const ColumnValues &column = *spreadsheet_layout_.columns[column_index].values;
    if (real_index > column.size()) {
      return;
    }

    const GVArray &data = column.data();

    if (data.type().is<int>()) {
      const int value = data.get<int>(real_index);
      const std::string value_str = std::to_string(value);
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin,
                                    params.ymin,
                                    params.width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            return fmt::format("{}", *((int *)argN));
          },
          MEM_dupallocN<int>(__func__, value),
          MEM_freeN);
      /* Right-align Integers. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    }
    if (data.type().is<int8_t>()) {
      const int8_t value = data.get<int8_t>(real_index);
      const std::string value_str = std::to_string(value);
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin,
                                    params.ymin,
                                    params.width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      /* Right-align Integers. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    }
    else if (data.type().is<short2>()) {
      const int2 value = int2(data.get<short2>(real_index));
      this->draw_int_vector(params, Span(&value.x, 2));
    }
    else if (data.type().is<int2>()) {
      const int2 value = data.get<int2>(real_index);
      this->draw_int_vector(params, Span(&value.x, 2));
    }
    else if (data.type().is<float>()) {
      const float value = data.get<float>(real_index);
      std::stringstream ss;
      ss << std::fixed << std::setprecision(3) << value;
      const std::string value_str = ss.str();
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin,
                                    params.ymin,
                                    params.width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            return fmt::format("{:f}", *((float *)argN));
          },
          MEM_dupallocN<float>(__func__, value),
          MEM_freeN);
      /* Right-align Floats. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    }
    else if (data.type().is<bool>()) {
      const bool value = data.get<bool>(real_index);
      const int icon = value ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    icon,
                                    "",
                                    params.xmin,
                                    params.ymin,
                                    params.width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      UI_but_drawflag_disable(but, UI_BUT_ICON_LEFT);
    }
    else if (data.type().is<float2>()) {
      const float2 value = data.get<float2>(real_index);
      this->draw_float_vector(params, Span(&value.x, 2));
    }
    else if (data.type().is<float3>()) {
      const float3 value = data.get<float3>(real_index);
      this->draw_float_vector(params, Span(&value.x, 3));
    }
    else if (data.type().is<ColorGeometry4f>()) {
      const ColorGeometry4f value = data.get<ColorGeometry4f>(real_index);
      this->draw_float_vector(params, Span(&value.r, 4));
    }
    else if (data.type().is<ColorGeometry4b>()) {
      const ColorGeometry4b value = data.get<ColorGeometry4b>(real_index);
      this->draw_byte_color(params, value);
    }
    else if (data.type().is<math::Quaternion>()) {
      const float4 value = float4(data.get<math::Quaternion>(real_index));
      this->draw_float_vector(params, Span(&value.x, 4));
    }
    else if (data.type().is<float4x4>()) {
      this->draw_float4x4(params, data.get<float4x4>(real_index));
    }
    else if (data.type().is<bke::InstanceReference>()) {
      const bke::InstanceReference value = data.get<bke::InstanceReference>(real_index);
      const StringRefNull name = value.name().is_empty() ? IFACE_("(Geometry)") : value.name();
      const int icon = get_instance_reference_icon(value);
      uiDefIconTextBut(params.block,
                       ButType::Label,
                       0,
                       icon,
                       name.c_str(),
                       params.xmin,
                       params.ymin,
                       params.width,
                       params.height,
                       nullptr,
                       std::nullopt);
    }
    else if (data.type().is<std::string>()) {
      uiDefIconTextBut(params.block,
                       ButType::Label,
                       0,
                       ICON_NONE,
                       data.get<std::string>(real_index),
                       params.xmin,
                       params.ymin,
                       params.width,
                       params.height,
                       nullptr,
                       std::nullopt);
    }
    else if (data.type().is<MStringProperty>()) {
      MStringProperty *prop = MEM_callocN<MStringProperty>(__func__);
      data.get_to_uninitialized(real_index, prop);
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    StringRef(prop->s, prop->s_len),
                                    params.xmin,
                                    params.ymin,
                                    params.width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);

      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            const MStringProperty &prop = *static_cast<MStringProperty *>(argN);
            return std::string(StringRef(prop.s, prop.s_len));
          },
          prop,
          MEM_freeN);
    }
  }

  void draw_float_vector(const CellDrawParams &params, const Span<float> values) const
  {
    BLI_assert(!values.is_empty());
    const float segment_width = float(params.width) / values.size();
    for (const int i : values.index_range()) {
      std::stringstream ss;
      const float value = values[i];
      ss << " " << std::fixed << std::setprecision(3) << value;
      const std::string value_str = ss.str();
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin + i * segment_width,
                                    params.ymin,
                                    segment_width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);

      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            return fmt::format("{:f}", *((float *)argN));
          },
          MEM_dupallocN<float>(__func__, value),
          MEM_freeN);
      /* Right-align Floats. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    }
  }

  void draw_int_vector(const CellDrawParams &params, const Span<int> values) const
  {
    BLI_assert(!values.is_empty());
    const float segment_width = float(params.width) / values.size();
    for (const int i : values.index_range()) {
      std::stringstream ss;
      const int value = values[i];
      ss << " " << value;
      const std::string value_str = ss.str();
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin + i * segment_width,
                                    params.ymin,
                                    segment_width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            return fmt::format("{}", *((int *)argN));
          },
          MEM_dupallocN<int>(__func__, value),
          MEM_freeN);
      /* Right-align Floats. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    }
  }

  void draw_byte_color(const CellDrawParams &params, const ColorGeometry4b color) const
  {
    const ColorGeometry4f float_color = color.decode();
    Span<float> values(&float_color.r, 4);
    const float segment_width = float(params.width) / values.size();
    for (const int i : values.index_range()) {
      std::stringstream ss;
      const float value = values[i];
      ss << " " << std::fixed << std::setprecision(3) << value;
      const std::string value_str = ss.str();
      uiBut *but = uiDefIconTextBut(params.block,
                                    ButType::Label,
                                    0,
                                    ICON_NONE,
                                    value_str,
                                    params.xmin + i * segment_width,
                                    params.ymin,
                                    segment_width,
                                    params.height,
                                    nullptr,
                                    std::nullopt);
      /* Right-align Floats. */
      UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
      UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);

      /* Tooltip showing raw byte values. Encode values in pointer to avoid memory allocation. */
      UI_but_func_tooltip_set(
          but,
          [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
            const uint32_t uint_color = POINTER_AS_UINT(argN);
            ColorGeometry4b color = *(ColorGeometry4b *)&uint_color;
            return fmt::format(fmt::runtime(TIP_("Byte Color (sRGB encoded):\n{}  {}  {}  {}")),
                               color.r,
                               color.g,
                               color.b,
                               color.a);
          },
          POINTER_FROM_UINT(*(uint32_t *)&color),
          nullptr);
    }
  }

  void draw_float4x4(const CellDrawParams &params, const float4x4 &value) const
  {
    uiBut *but = uiDefIconTextBut(params.block,
                                  ButType::Label,
                                  0,
                                  ICON_NONE,
                                  "...",
                                  params.xmin,
                                  params.ymin,
                                  params.width,
                                  params.height,
                                  nullptr,
                                  std::nullopt);
    /* Center alignment. */
    UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
    UI_but_func_tooltip_set(
        but,
        [](bContext * /*C*/, void *argN, const StringRef /*tip*/) {
          /* Transpose to be able to print row by row. */
          const float4x4 value = math::transpose(*static_cast<const float4x4 *>(argN));
          std::stringstream ss;
          ss << value[0] << ",\n";
          ss << value[1] << ",\n";
          ss << value[2] << ",\n";
          ss << value[3];
          return ss.str();
        },
        MEM_dupallocN<float4x4>(__func__, value),
        MEM_freeN);
  }

  int column_width(int column_index) const final
  {
    return spreadsheet_layout_.columns[column_index].width;
  }
};

template<typename T>
static float estimate_max_column_width(const float min_width,
                                       const int fontid,
                                       const std::optional<int64_t> max_sample_size,
                                       const VArray<T> &data,
                                       FunctionRef<std::string(const T &)> to_string)
{
  if (const std::optional<T> value = data.get_if_single()) {
    const std::string str = to_string(*value);
    return std::max(min_width, BLF_width(fontid, str.c_str(), str.size()));
  }
  const int sample_size = max_sample_size.value_or(data.size());
  float width = min_width;
  for (const int i : data.index_range().take_front(sample_size)) {
    const std::string str = to_string(data[i]);
    const float value_width = BLF_width(fontid, str.c_str(), str.size());
    width = std::max(width, value_width);
  }
  return width;
}

float ColumnValues::fit_column_values_width_px(const std::optional<int64_t> &max_sample_size) const
{
  const int fontid = BLF_default();
  BLF_size(fontid, UI_DEFAULT_TEXT_POINTS * UI_SCALE_FAC);

  auto get_min_width = [&](const float min_width) {
    return max_sample_size.has_value() ? min_width : 0.0f;
  };

  const eSpreadsheetColumnValueType column_type = this->type();
  switch (column_type) {
    case SPREADSHEET_VALUE_TYPE_BOOL: {
      return 2.0f * SPREADSHEET_WIDTH_UNIT;
    }
    case SPREADSHEET_VALUE_TYPE_FLOAT4X4: {
      return 2.0f * SPREADSHEET_WIDTH_UNIT;
    }
    case SPREADSHEET_VALUE_TYPE_INT8: {
      return estimate_max_column_width<int8_t>(
          get_min_width(3 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<int8_t>(),
          [](const int value) { return fmt::format("{}", value); });
    }
    case SPREADSHEET_VALUE_TYPE_INT32: {
      return estimate_max_column_width<int>(
          get_min_width(3 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<int>(),
          [](const int value) { return fmt::format("{}", value); });
    }
    case SPREADSHEET_VALUE_TYPE_FLOAT: {
      return estimate_max_column_width<float>(
          get_min_width(3 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<float>(),
          [](const float value) { return fmt::format("{:.3f}", value); });
    }
    case SPREADSHEET_VALUE_TYPE_INT32_2D: {
      return estimate_max_column_width<int2>(
          get_min_width(3 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<int2>(),
          [](const int2 value) { return fmt::format("{}  {}", value.x, value.y); });
    }
    case SPREADSHEET_VALUE_TYPE_FLOAT2: {
      return estimate_max_column_width<float2>(
          get_min_width(6 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<float2>(),
          [](const float2 value) { return fmt::format("{:.3f}  {:.3f}", value.x, value.y); });
    }
    case SPREADSHEET_VALUE_TYPE_FLOAT3: {
      return estimate_max_column_width<float3>(
          get_min_width(9 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<float3>(),
          [](const float3 value) {
            return fmt::format("{:.3f}  {:.3f}  {:.3f}", value.x, value.y, value.z);
          });
    }
    case SPREADSHEET_VALUE_TYPE_COLOR: {
      return estimate_max_column_width<ColorGeometry4f>(
          get_min_width(12 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<ColorGeometry4f>(),
          [](const ColorGeometry4f value) {
            return fmt::format(
                "{:.3f}  {:.3f}  {:.3f}  {:.3f}", value.r, value.g, value.b, value.a);
          });
    }
    case SPREADSHEET_VALUE_TYPE_BYTE_COLOR: {
      return estimate_max_column_width<ColorGeometry4b>(
          get_min_width(12 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<ColorGeometry4b>(),
          [](const ColorGeometry4b value) {
            return fmt::format("{}  {}  {}  {}", value.r, value.g, value.b, value.a);
          });
    }
    case SPREADSHEET_VALUE_TYPE_QUATERNION: {
      return estimate_max_column_width<math::Quaternion>(
          get_min_width(12 * SPREADSHEET_WIDTH_UNIT),
          fontid,
          max_sample_size,
          data_.typed<math::Quaternion>(),
          [](const math::Quaternion value) {
            return fmt::format(
                "{:.3f}  {:.3f}  {:.3f}  {:.3f}", value.x, value.y, value.z, value.w);
          });
    }
    case SPREADSHEET_VALUE_TYPE_INSTANCES: {
      return UI_ICON_SIZE + 0.5f * UI_UNIT_X +
             estimate_max_column_width<bke::InstanceReference>(
                 get_min_width(8 * SPREADSHEET_WIDTH_UNIT),
                 fontid,
                 max_sample_size,
                 data_.typed<bke::InstanceReference>(),
                 [](const bke::InstanceReference &value) {
                   const StringRef name = value.name().is_empty() ? IFACE_("(Geometry)") :
                                                                    value.name();
                   return name;
                 });
    }
    case SPREADSHEET_VALUE_TYPE_STRING: {
      if (data_.type().is<std::string>()) {
        return estimate_max_column_width<std::string>(get_min_width(SPREADSHEET_WIDTH_UNIT),
                                                      fontid,
                                                      max_sample_size,
                                                      data_.typed<std::string>(),
                                                      [](const StringRef value) { return value; });
      }
      if (data_.type().is<MStringProperty>()) {
        return estimate_max_column_width<MStringProperty>(
            get_min_width(SPREADSHEET_WIDTH_UNIT),
            fontid,
            max_sample_size,
            data_.typed<MStringProperty>(),
            [](const MStringProperty &value) { return StringRef(value.s, value.s_len); });
      }
      break;
    }
    case SPREADSHEET_VALUE_TYPE_UNKNOWN: {
      break;
    }
  }
  return 2.0f * SPREADSHEET_WIDTH_UNIT;
}

float ColumnValues::fit_column_width_px(const std::optional<int64_t> &max_sample_size) const
{
  const float padding_px = 0.5 * SPREADSHEET_WIDTH_UNIT;
  const float min_width_px = SPREADSHEET_WIDTH_UNIT;

  const float data_width_px = this->fit_column_values_width_px(max_sample_size);

  const int fontid = BLF_default();
  BLF_size(fontid, UI_DEFAULT_TEXT_POINTS * UI_SCALE_FAC);
  const float name_width_px = BLF_width(fontid, name_.data(), name_.size());

  const float width_px = std::max(min_width_px,
                                  padding_px + std::max(data_width_px, name_width_px));
  return width_px;
}

std::unique_ptr<SpreadsheetDrawer> spreadsheet_drawer_from_layout(
    const SpreadsheetLayout &spreadsheet_layout)
{
  return std::make_unique<SpreadsheetLayoutDrawer>(spreadsheet_layout);
}

}  // namespace blender::ed::spreadsheet
