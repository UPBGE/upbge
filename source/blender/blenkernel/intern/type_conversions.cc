/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_type_conversions.hh"

#include "DNA_meshdata_types.h"

#include "FN_multi_function_builder.hh"

#include "BLI_color.hh"
#include "BLI_math_vector.hh"

namespace blender::bke {

using fn::MFDataType;

template<typename From, typename To, To (*ConversionF)(const From &)>
static void add_implicit_conversion(DataTypeConversions &conversions)
{
  static const CPPType &from_type = CPPType::get<From>();
  static const CPPType &to_type = CPPType::get<To>();
  static const std::string conversion_name = from_type.name() + " to " + to_type.name();

  static fn::CustomMF_SI_SO<From, To> multi_function{
      conversion_name.c_str(),
      /* Use lambda instead of passing #ConversionF directly, because otherwise the compiler won't
       * inline the function. */
      [](const From &a) { return ConversionF(a); },
      fn::CustomMF_presets::AllSpanOrSingle()};
  static auto convert_single_to_initialized = [](const void *src, void *dst) {
    *(To *)dst = ConversionF(*(const From *)src);
  };
  static auto convert_single_to_uninitialized = [](const void *src, void *dst) {
    new (dst) To(ConversionF(*(const From *)src));
  };
  conversions.add(fn::MFDataType::ForSingle<From>(),
                  fn::MFDataType::ForSingle<To>(),
                  multi_function,
                  convert_single_to_initialized,
                  convert_single_to_uninitialized);
}

static float2 float_to_float2(const float &a)
{
  return float2(a);
}
static float3 float_to_float3(const float &a)
{
  return float3(a);
}
static int32_t float_to_int(const float &a)
{
  return (int32_t)a;
}
static bool float_to_bool(const float &a)
{
  return a > 0.0f;
}
static int8_t float_to_int8(const float &a)
{
  return std::clamp(
      a, float(std::numeric_limits<int8_t>::min()), float(std::numeric_limits<int8_t>::max()));
}
static ColorGeometry4f float_to_color(const float &a)
{
  return ColorGeometry4f(a, a, a, 1.0f);
}
static ColorGeometry4b float_to_byte_color(const float &a)
{
  return float_to_color(a).encode();
}

static float3 float2_to_float3(const float2 &a)
{
  return float3(a.x, a.y, 0.0f);
}
static float float2_to_float(const float2 &a)
{
  return (a.x + a.y) / 2.0f;
}
static int float2_to_int(const float2 &a)
{
  return (int32_t)((a.x + a.y) / 2.0f);
}
static bool float2_to_bool(const float2 &a)
{
  return !math::is_zero(a);
}
static int8_t float2_to_int8(const float2 &a)
{
  return float_to_int8((a.x + a.y) / 2.0f);
}
static ColorGeometry4f float2_to_color(const float2 &a)
{
  return ColorGeometry4f(a.x, a.y, 0.0f, 1.0f);
}
static ColorGeometry4b float2_to_byte_color(const float2 &a)
{
  return float2_to_color(a).encode();
}

static bool float3_to_bool(const float3 &a)
{
  return !math::is_zero(a);
}
static int8_t float3_to_int8(const float3 &a)
{
  return float_to_int8((a.x + a.y + a.z) / 3.0f);
}
static float float3_to_float(const float3 &a)
{
  return (a.x + a.y + a.z) / 3.0f;
}
static int float3_to_int(const float3 &a)
{
  return (int)((a.x + a.y + a.z) / 3.0f);
}
static float2 float3_to_float2(const float3 &a)
{
  return float2(a);
}
static ColorGeometry4f float3_to_color(const float3 &a)
{
  return ColorGeometry4f(a.x, a.y, a.z, 1.0f);
}
static ColorGeometry4b float3_to_byte_color(const float3 &a)
{
  return float3_to_color(a).encode();
}

static bool int_to_bool(const int32_t &a)
{
  return a > 0;
}
static int8_t int_to_int8(const int32_t &a)
{
  return std::clamp(
      a, int(std::numeric_limits<int8_t>::min()), int(std::numeric_limits<int8_t>::max()));
}
static float int_to_float(const int32_t &a)
{
  return (float)a;
}
static float2 int_to_float2(const int32_t &a)
{
  return float2((float)a);
}
static float3 int_to_float3(const int32_t &a)
{
  return float3((float)a);
}
static ColorGeometry4f int_to_color(const int32_t &a)
{
  return ColorGeometry4f((float)a, (float)a, (float)a, 1.0f);
}
static ColorGeometry4b int_to_byte_color(const int32_t &a)
{
  return int_to_color(a).encode();
}

static bool int8_to_bool(const int8_t &a)
{
  return a > 0;
}
static int int8_to_int(const int8_t &a)
{
  return static_cast<int>(a);
}
static float int8_to_float(const int8_t &a)
{
  return (float)a;
}
static float2 int8_to_float2(const int8_t &a)
{
  return float2((float)a);
}
static float3 int8_to_float3(const int8_t &a)
{
  return float3((float)a);
}
static ColorGeometry4f int8_to_color(const int8_t &a)
{
  return ColorGeometry4f((float)a, (float)a, (float)a, 1.0f);
}
static ColorGeometry4b int8_to_byte_color(const int8_t &a)
{
  return int8_to_color(a).encode();
}

static float bool_to_float(const bool &a)
{
  return (bool)a;
}
static int8_t bool_to_int8(const bool &a)
{
  return static_cast<int8_t>(a);
}
static int32_t bool_to_int(const bool &a)
{
  return (int32_t)a;
}
static float2 bool_to_float2(const bool &a)
{
  return (a) ? float2(1.0f) : float2(0.0f);
}
static float3 bool_to_float3(const bool &a)
{
  return (a) ? float3(1.0f) : float3(0.0f);
}
static ColorGeometry4f bool_to_color(const bool &a)
{
  return (a) ? ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f) : ColorGeometry4f(0.0f, 0.0f, 0.0f, 1.0f);
}
static ColorGeometry4b bool_to_byte_color(const bool &a)
{
  return bool_to_color(a).encode();
}

static bool color_to_bool(const ColorGeometry4f &a)
{
  return rgb_to_grayscale(a) > 0.0f;
}
static float color_to_float(const ColorGeometry4f &a)
{
  return rgb_to_grayscale(a);
}
static int32_t color_to_int(const ColorGeometry4f &a)
{
  return (int)rgb_to_grayscale(a);
}
static int8_t color_to_int8(const ColorGeometry4f &a)
{
  return int_to_int8(color_to_int(a));
}
static float2 color_to_float2(const ColorGeometry4f &a)
{
  return float2(a.r, a.g);
}
static float3 color_to_float3(const ColorGeometry4f &a)
{
  return float3(a.r, a.g, a.b);
}
static ColorGeometry4b color_to_byte_color(const ColorGeometry4f &a)
{
  return a.encode();
}

static bool byte_color_to_bool(const ColorGeometry4b &a)
{
  return a.r > 0 || a.g > 0 || a.b > 0;
}
static float byte_color_to_float(const ColorGeometry4b &a)
{
  return color_to_float(a.decode());
}
static int32_t byte_color_to_int(const ColorGeometry4b &a)
{
  return color_to_int(a.decode());
}
static int8_t byte_color_to_int8(const ColorGeometry4b &a)
{
  return color_to_int8(a.decode());
}
static float2 byte_color_to_float2(const ColorGeometry4b &a)
{
  return color_to_float2(a.decode());
}
static float3 byte_color_to_float3(const ColorGeometry4b &a)
{
  return color_to_float3(a.decode());
}
static ColorGeometry4f byte_color_to_color(const ColorGeometry4b &a)
{
  return a.decode();
}

static DataTypeConversions create_implicit_conversions()
{
  DataTypeConversions conversions;

  add_implicit_conversion<float, float2, float_to_float2>(conversions);
  add_implicit_conversion<float, float3, float_to_float3>(conversions);
  add_implicit_conversion<float, int32_t, float_to_int>(conversions);
  add_implicit_conversion<float, bool, float_to_bool>(conversions);
  add_implicit_conversion<float, int8_t, float_to_int8>(conversions);
  add_implicit_conversion<float, ColorGeometry4f, float_to_color>(conversions);
  add_implicit_conversion<float, ColorGeometry4b, float_to_byte_color>(conversions);

  add_implicit_conversion<float2, float3, float2_to_float3>(conversions);
  add_implicit_conversion<float2, float, float2_to_float>(conversions);
  add_implicit_conversion<float2, int32_t, float2_to_int>(conversions);
  add_implicit_conversion<float2, bool, float2_to_bool>(conversions);
  add_implicit_conversion<float2, int8_t, float2_to_int8>(conversions);
  add_implicit_conversion<float2, ColorGeometry4f, float2_to_color>(conversions);
  add_implicit_conversion<float2, ColorGeometry4b, float2_to_byte_color>(conversions);

  add_implicit_conversion<float3, bool, float3_to_bool>(conversions);
  add_implicit_conversion<float3, int8_t, float3_to_int8>(conversions);
  add_implicit_conversion<float3, float, float3_to_float>(conversions);
  add_implicit_conversion<float3, int32_t, float3_to_int>(conversions);
  add_implicit_conversion<float3, float2, float3_to_float2>(conversions);
  add_implicit_conversion<float3, ColorGeometry4f, float3_to_color>(conversions);
  add_implicit_conversion<float3, ColorGeometry4b, float3_to_byte_color>(conversions);

  add_implicit_conversion<int32_t, bool, int_to_bool>(conversions);
  add_implicit_conversion<int32_t, int8_t, int_to_int8>(conversions);
  add_implicit_conversion<int32_t, float, int_to_float>(conversions);
  add_implicit_conversion<int32_t, float2, int_to_float2>(conversions);
  add_implicit_conversion<int32_t, float3, int_to_float3>(conversions);
  add_implicit_conversion<int32_t, ColorGeometry4f, int_to_color>(conversions);
  add_implicit_conversion<int32_t, ColorGeometry4b, int_to_byte_color>(conversions);

  add_implicit_conversion<int8_t, bool, int8_to_bool>(conversions);
  add_implicit_conversion<int8_t, int32_t, int8_to_int>(conversions);
  add_implicit_conversion<int8_t, float, int8_to_float>(conversions);
  add_implicit_conversion<int8_t, float2, int8_to_float2>(conversions);
  add_implicit_conversion<int8_t, float3, int8_to_float3>(conversions);
  add_implicit_conversion<int8_t, ColorGeometry4f, int8_to_color>(conversions);
  add_implicit_conversion<int8_t, ColorGeometry4b, int8_to_byte_color>(conversions);

  add_implicit_conversion<bool, float, bool_to_float>(conversions);
  add_implicit_conversion<bool, int8_t, bool_to_int8>(conversions);
  add_implicit_conversion<bool, int32_t, bool_to_int>(conversions);
  add_implicit_conversion<bool, float2, bool_to_float2>(conversions);
  add_implicit_conversion<bool, float3, bool_to_float3>(conversions);
  add_implicit_conversion<bool, ColorGeometry4f, bool_to_color>(conversions);
  add_implicit_conversion<bool, ColorGeometry4b, bool_to_byte_color>(conversions);

  add_implicit_conversion<ColorGeometry4f, bool, color_to_bool>(conversions);
  add_implicit_conversion<ColorGeometry4f, int8_t, color_to_int8>(conversions);
  add_implicit_conversion<ColorGeometry4f, float, color_to_float>(conversions);
  add_implicit_conversion<ColorGeometry4f, int32_t, color_to_int>(conversions);
  add_implicit_conversion<ColorGeometry4f, float2, color_to_float2>(conversions);
  add_implicit_conversion<ColorGeometry4f, float3, color_to_float3>(conversions);
  add_implicit_conversion<ColorGeometry4f, ColorGeometry4b, color_to_byte_color>(conversions);

  add_implicit_conversion<ColorGeometry4b, bool, byte_color_to_bool>(conversions);
  add_implicit_conversion<ColorGeometry4b, int8_t, byte_color_to_int8>(conversions);
  add_implicit_conversion<ColorGeometry4b, float, byte_color_to_float>(conversions);
  add_implicit_conversion<ColorGeometry4b, int32_t, byte_color_to_int>(conversions);
  add_implicit_conversion<ColorGeometry4b, float2, byte_color_to_float2>(conversions);
  add_implicit_conversion<ColorGeometry4b, float3, byte_color_to_float3>(conversions);
  add_implicit_conversion<ColorGeometry4b, ColorGeometry4f, byte_color_to_color>(conversions);

  return conversions;
}

const DataTypeConversions &get_implicit_type_conversions()
{
  static const DataTypeConversions conversions = create_implicit_conversions();
  return conversions;
}

void DataTypeConversions::convert_to_uninitialized(const CPPType &from_type,
                                                   const CPPType &to_type,
                                                   const void *from_value,
                                                   void *to_value) const
{
  if (from_type == to_type) {
    from_type.copy_construct(from_value, to_value);
    return;
  }

  const ConversionFunctions *functions = this->get_conversion_functions(
      MFDataType::ForSingle(from_type), MFDataType::ForSingle(to_type));
  BLI_assert(functions != nullptr);

  functions->convert_single_to_uninitialized(from_value, to_value);
}

static void call_convert_to_uninitialized_fn(const GVArray &from,
                                             const fn::MultiFunction &fn,
                                             const IndexMask mask,
                                             GMutableSpan to)
{
  fn::MFParamsBuilder params{fn, from.size()};
  params.add_readonly_single_input(from);
  params.add_uninitialized_single_output(to);
  fn::MFContextBuilder context;
  fn.call_auto(mask, params, context);
}

static void call_convert_to_uninitialized_fn(const GVArray &from,
                                             const fn::MultiFunction &fn,
                                             GMutableSpan to)
{
  call_convert_to_uninitialized_fn(from, fn, IndexMask(from.size()), to);
}

void DataTypeConversions::convert_to_initialized_n(GSpan from_span, GMutableSpan to_span) const
{
  const CPPType &from_type = from_span.type();
  const CPPType &to_type = to_span.type();

  BLI_assert(from_span.size() == to_span.size());
  BLI_assert(this->is_convertible(from_type, to_type));

  const fn::MultiFunction *fn = this->get_conversion_multi_function(
      MFDataType::ForSingle(from_type), MFDataType::ForSingle(to_type));

  to_type.destruct_n(to_span.data(), to_span.size());
  call_convert_to_uninitialized_fn(GVArray::ForSpan(from_span), *fn, to_span);
}

class GVArray_For_ConvertedGVArray : public GVArrayImpl {
 private:
  GVArray varray_;
  const CPPType &from_type_;
  ConversionFunctions old_to_new_conversions_;

 public:
  GVArray_For_ConvertedGVArray(GVArray varray,
                               const CPPType &to_type,
                               const DataTypeConversions &conversions)
      : GVArrayImpl(to_type, varray.size()), varray_(std::move(varray)), from_type_(varray_.type())
  {
    old_to_new_conversions_ = *conversions.get_conversion_functions(from_type_, to_type);
  }

 private:
  void get(const int64_t index, void *r_value) const override
  {
    BUFFER_FOR_CPP_TYPE_VALUE(from_type_, buffer);
    varray_.get(index, buffer);
    old_to_new_conversions_.convert_single_to_initialized(buffer, r_value);
    from_type_.destruct(buffer);
  }

  void get_to_uninitialized(const int64_t index, void *r_value) const override
  {
    BUFFER_FOR_CPP_TYPE_VALUE(from_type_, buffer);
    varray_.get(index, buffer);
    old_to_new_conversions_.convert_single_to_uninitialized(buffer, r_value);
    from_type_.destruct(buffer);
  }

  void materialize(const IndexMask mask, void *dst) const override
  {
    type_->destruct_n(dst, mask.min_array_size());
    this->materialize_to_uninitialized(mask, dst);
  }

  void materialize_to_uninitialized(const IndexMask mask, void *dst) const override
  {
    call_convert_to_uninitialized_fn(varray_,
                                     *old_to_new_conversions_.multi_function,
                                     mask,
                                     {this->type(), dst, mask.min_array_size()});
  }
};

class GVMutableArray_For_ConvertedGVMutableArray : public GVMutableArrayImpl {
 private:
  GVMutableArray varray_;
  const CPPType &from_type_;
  ConversionFunctions old_to_new_conversions_;
  ConversionFunctions new_to_old_conversions_;

 public:
  GVMutableArray_For_ConvertedGVMutableArray(GVMutableArray varray,
                                             const CPPType &to_type,
                                             const DataTypeConversions &conversions)
      : GVMutableArrayImpl(to_type, varray.size()),
        varray_(std::move(varray)),
        from_type_(varray_.type())
  {
    old_to_new_conversions_ = *conversions.get_conversion_functions(from_type_, to_type);
    new_to_old_conversions_ = *conversions.get_conversion_functions(to_type, from_type_);
  }

 private:
  void get(const int64_t index, void *r_value) const override
  {
    BUFFER_FOR_CPP_TYPE_VALUE(from_type_, buffer);
    varray_.get(index, buffer);
    old_to_new_conversions_.convert_single_to_initialized(buffer, r_value);
    from_type_.destruct(buffer);
  }

  void get_to_uninitialized(const int64_t index, void *r_value) const override
  {
    BUFFER_FOR_CPP_TYPE_VALUE(from_type_, buffer);
    varray_.get(index, buffer);
    old_to_new_conversions_.convert_single_to_uninitialized(buffer, r_value);
    from_type_.destruct(buffer);
  }

  void set_by_move(const int64_t index, void *value) override
  {
    BUFFER_FOR_CPP_TYPE_VALUE(from_type_, buffer);
    new_to_old_conversions_.convert_single_to_uninitialized(value, buffer);
    varray_.set_by_relocate(index, buffer);
  }

  void materialize(const IndexMask mask, void *dst) const override
  {
    type_->destruct_n(dst, mask.min_array_size());
    this->materialize_to_uninitialized(mask, dst);
  }

  void materialize_to_uninitialized(const IndexMask mask, void *dst) const override
  {
    call_convert_to_uninitialized_fn(varray_,
                                     *old_to_new_conversions_.multi_function,
                                     mask,
                                     {this->type(), dst, mask.min_array_size()});
  }
};

GVArray DataTypeConversions::try_convert(GVArray varray, const CPPType &to_type) const
{
  const CPPType &from_type = varray.type();
  if (from_type == to_type) {
    return varray;
  }
  if (!this->is_convertible(from_type, to_type)) {
    return {};
  }
  return GVArray::For<GVArray_For_ConvertedGVArray>(std::move(varray), to_type, *this);
}

GVMutableArray DataTypeConversions::try_convert(GVMutableArray varray,
                                                const CPPType &to_type) const
{
  const CPPType &from_type = varray.type();
  if (from_type == to_type) {
    return varray;
  }
  if (!this->is_convertible(from_type, to_type)) {
    return {};
  }
  return GVMutableArray::For<GVMutableArray_For_ConvertedGVMutableArray>(
      std::move(varray), to_type, *this);
}

fn::GField DataTypeConversions::try_convert(fn::GField field, const CPPType &to_type) const
{
  const CPPType &from_type = field.cpp_type();
  if (from_type == to_type) {
    return field;
  }
  if (!this->is_convertible(from_type, to_type)) {
    return {};
  }
  const fn::MultiFunction &fn = *this->get_conversion_multi_function(
      fn::MFDataType::ForSingle(from_type), fn::MFDataType::ForSingle(to_type));
  return {std::make_shared<fn::FieldOperation>(fn, Vector<fn::GField>{std::move(field)})};
}

}  // namespace blender::bke
