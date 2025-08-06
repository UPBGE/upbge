/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "BLI_math_quaternion_types.hh"
#include "BLI_string_utf8.h"

#include "DNA_curve_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_duplilist.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "DRW_render.hh"

#include "UI_resources.hh"

#include "draw_manager_text.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Displays geometry node viewer output.
 * Values are displayed as text on top of the active object.
 */
class AttributeTexts : Overlay {
 public:
  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = !res.is_selection() && state.show_attribute_viewer_text();
  }

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final
  {
    if (!enabled_) {
      return;
    }

    const Object &object = *ob_ref.object;
    const bool is_preview = ob_ref.preview_base_geometry() != nullptr;
    if (!is_preview) {
      return;
    }

    DRWTextStore *dt = state.dt;
    const float4x4 &object_to_world = object.object_to_world();

    if (ob_ref.preview_instance_index() >= 0) {
      const bke::Instances *instances = ob_ref.preview_base_geometry()->get_instances();
      if (instances->attributes().contains(".viewer")) {
        add_instance_attributes_to_text_cache(
            dt, instances->attributes(), object_to_world, ob_ref.preview_instance_index());

        return;
      }
    }

    switch (object.type) {
      case OB_MESH: {
        const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
        add_attributes_to_text_cache(dt, mesh.attributes(), object_to_world);
        break;
      }
      case OB_POINTCLOUD: {
        const PointCloud &pointcloud = DRW_object_get_data_for_drawing<PointCloud>(object);
        add_attributes_to_text_cache(dt, pointcloud.attributes(), object_to_world);
        break;
      }
      case OB_CURVES_LEGACY: {
        const Curve &curve = DRW_object_get_data_for_drawing<Curve>(object);
        if (curve.curve_eval) {
          const bke::CurvesGeometry &curves = curve.curve_eval->geometry.wrap();
          add_attributes_to_text_cache(dt, curves.attributes(), object_to_world);
        }
        break;
      }
      case OB_CURVES: {
        const Curves &curves_id = DRW_object_get_data_for_drawing<Curves>(object);
        const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
        add_attributes_to_text_cache(dt, curves.attributes(), object_to_world);
        break;
      }
    }
  }

 private:
  void add_attributes_to_text_cache(DRWTextStore *dt,
                                    bke::AttributeAccessor attribute_accessor,
                                    const float4x4 &object_to_world)
  {
    if (!attribute_accessor.contains(".viewer")) {
      return;
    }

    const bke::GAttributeReader attribute = attribute_accessor.lookup(".viewer");
    const VArraySpan<float3> positions = *attribute_accessor.lookup<float3>("position",
                                                                            attribute.domain);

    add_values_to_text_cache(dt, attribute.varray, positions, object_to_world);
  }

  void add_instance_attributes_to_text_cache(DRWTextStore *dt,
                                             bke::AttributeAccessor attribute_accessor,
                                             const float4x4 &object_to_world,
                                             int instance_index)
  {
    /* Data from instances are read as a single value from a given index. The data is converted
     * back to an array so one function can handle both instance and object data. */
    const GVArray attribute = attribute_accessor.lookup(".viewer").varray.slice(
        IndexRange(instance_index, 1));

    add_values_to_text_cache(dt, attribute, {float3(0, 0, 0)}, object_to_world);
  }

  static void add_text_to_cache(DRWTextStore *dt,
                                const float3 &position,
                                const StringRef text,
                                const uchar4 &color)
  {
    DRW_text_cache_add(dt,
                       position,
                       text.data(),
                       text.size(),
                       0,
                       0,
                       DRW_TEXT_CACHE_GLOBALSPACE,
                       color,
                       true,
                       true);
  }

  static void add_lines_to_cache(DRWTextStore *dt,
                                 const float3 &position,
                                 const Span<StringRef> lines,
                                 const uchar4 &color)
  {
    for (const int i : lines.index_range()) {
      const StringRef line = lines[i];
      DRW_text_cache_add(dt,
                         position,
                         line.data(),
                         line.size(),
                         0,
                         -i * 12.0f * UI_SCALE_FAC,
                         DRW_TEXT_CACHE_GLOBALSPACE,
                         color,
                         true,
                         true);
    }
  }

  void add_values_to_text_cache(DRWTextStore *dt,
                                const GVArray &values,
                                const Span<float3> positions,
                                const float4x4 &object_to_world)
  {
    uchar col[4];
    UI_GetThemeColor4ubv(TH_TEXT_HI, col);

    bke::attribute_math::convert_to_static_type(values.type(), [&](auto dummy) {
      using T = decltype(dummy);
      const VArray<T> &values_typed = values.typed<T>();
      for (const int i : values.index_range()) {
        const float3 position = math::transform_point(object_to_world, positions[i]);
        const T &value = values_typed[i];

        if constexpr (std::is_same_v<T, bool>) {
          char numstr[64];
          const size_t numstr_len = STRNCPY_UTF8_RLEN(numstr, value ? "True" : "False");
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, int8_t>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(numstr, "%d", int(value));
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, int>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(numstr, "%d", value);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, int2>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(numstr, "(%d, %d)", value.x, value.y);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, float>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(numstr, "%g", value);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, float2>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(numstr, "(%g, %g)", value.x, value.y);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, float3>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(
              numstr, "(%g, %g, %g)", value.x, value.y, value.z);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, ColorGeometry4b>) {
          const ColorGeometry4f color = value.decode();
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(
              numstr, "(%.3f, %.3f, %.3f, %.3f)", color.r, color.g, color.b, color.a);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, ColorGeometry4f>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(
              numstr, "(%.3f, %.3f, %.3f, %.3f)", value.r, value.g, value.b, value.a);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, math::Quaternion>) {
          char numstr[64];
          const size_t numstr_len = SNPRINTF_UTF8_RLEN(
              numstr, "(%.3f, %.3f, %.3f, %.3f)", value.w, value.x, value.y, value.z);
          add_text_to_cache(dt, position, StringRef(numstr, numstr_len), col);
        }
        else if constexpr (std::is_same_v<T, float4x4>) {
          float3 location;
          math::EulerXYZ rotation;
          float3 scale;
          math::to_loc_rot_scale_safe<true>(value, location, rotation, scale);

          char location_str[64];
          const size_t location_str_len = SNPRINTF_UTF8_RLEN(
              location_str, "Location: %.3f, %.3f, %.3f", location.x, location.y, location.z);
          char rotation_str[64];
          const size_t rotation_str_len = SNPRINTF_UTF8_RLEN(rotation_str,
                                                             "Rotation: %.3f°, %.3f°, %.3f°",
                                                             rotation.x().degree(),
                                                             rotation.y().degree(),
                                                             rotation.z().degree());
          char scale_str[64];
          const size_t scale_str_len = SNPRINTF_UTF8_RLEN(
              scale_str, "Scale: %.3f, %.3f, %.3f", scale.x, scale.y, scale.z);
          add_lines_to_cache(dt,
                             position,
                             {StringRef(location_str, location_str_len),
                              StringRef(rotation_str, rotation_str_len),
                              StringRef(scale_str, scale_str_len)},
                             col);
        }
        else {
          BLI_assert_unreachable();
        }
      }
    });
  }
};

}  // namespace blender::draw::overlay
