/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_serialize.hh"

#include "json.hpp"

namespace blender::io::serialize {

const StringValue *Value::as_string_value() const
{
  if (type_ != eValueType::String) {
    return nullptr;
  }
  return static_cast<const StringValue *>(this);
}

const IntValue *Value::as_int_value() const
{
  if (type_ != eValueType::Int) {
    return nullptr;
  }
  return static_cast<const IntValue *>(this);
}

const DoubleValue *Value::as_double_value() const
{
  if (type_ != eValueType::Double) {
    return nullptr;
  }
  return static_cast<const DoubleValue *>(this);
}

const BooleanValue *Value::as_boolean_value() const
{
  if (type_ != eValueType::Boolean) {
    return nullptr;
  }
  return static_cast<const BooleanValue *>(this);
}

const ArrayValue *Value::as_array_value() const
{
  if (type_ != eValueType::Array) {
    return nullptr;
  }
  return static_cast<const ArrayValue *>(this);
}

const DictionaryValue *Value::as_dictionary_value() const
{
  if (type_ != eValueType::Dictionary) {
    return nullptr;
  }
  return static_cast<const DictionaryValue *>(this);
}

static void convert_to_json(nlohmann::ordered_json &j, const Value &value);
static void convert_to_json(nlohmann::ordered_json &j, const ArrayValue &value)
{
  const ArrayValue::Items &items = value.elements();
  /* Create a json array to store the elements. If this isn't done and items is empty it would
   * return use a null value, in stead of an empty array. */
  j = "[]"_json;
  for (const ArrayValue::Item &item_value : items) {
    nlohmann::ordered_json json_item;
    convert_to_json(json_item, *item_value);
    j.push_back(json_item);
  }
}

static void convert_to_json(nlohmann::ordered_json &j, const DictionaryValue &value)
{
  const DictionaryValue::Items &attributes = value.elements();
  /* Create a json object to store the attributes. If this isn't done and attributes is empty it
   * would return use a null value, in stead of an empty object. */
  j = "{}"_json;
  for (const DictionaryValue::Item &attribute : attributes) {
    nlohmann::ordered_json json_item;
    convert_to_json(json_item, *attribute.second);
    j[attribute.first] = json_item;
  }
}

static void convert_to_json(nlohmann::ordered_json &j, const Value &value)
{
  switch (value.type()) {
    case eValueType::String: {
      j = value.as_string_value()->value();
      break;
    }

    case eValueType::Int: {
      j = value.as_int_value()->value();
      break;
    }

    case eValueType::Array: {
      const ArrayValue &array = *value.as_array_value();
      convert_to_json(j, array);
      break;
    }

    case eValueType::Dictionary: {
      const DictionaryValue &object = *value.as_dictionary_value();
      convert_to_json(j, object);
      break;
    }

    case eValueType::Null: {
      j = nullptr;
      break;
    }

    case eValueType::Boolean: {
      j = value.as_boolean_value()->value();
      break;
    }

    case eValueType::Double: {
      j = value.as_double_value()->value();
    }
  }
}

static std::unique_ptr<Value> convert_from_json(const nlohmann::ordered_json &j);
static std::unique_ptr<ArrayValue> convert_from_json_to_array(const nlohmann::ordered_json &j)
{
  std::unique_ptr<ArrayValue> array = std::make_unique<ArrayValue>();
  ArrayValue::Items &elements = array->elements();
  for (auto element : j.items()) {
    nlohmann::ordered_json element_json = element.value();
    std::unique_ptr<Value> value = convert_from_json(element_json);
    elements.append_as(value.release());
  }
  return array;
}

static std::unique_ptr<DictionaryValue> convert_from_json_to_object(
    const nlohmann::ordered_json &j)
{
  std::unique_ptr<DictionaryValue> object = std::make_unique<DictionaryValue>();
  DictionaryValue::Items &elements = object->elements();
  for (auto element : j.items()) {
    std::string key = element.key();
    nlohmann::ordered_json element_json = element.value();
    std::unique_ptr<Value> value = convert_from_json(element_json);
    elements.append_as(std::pair(key, value.release()));
  }
  return object;
}

static std::unique_ptr<Value> convert_from_json(const nlohmann::ordered_json &j)
{
  switch (j.type()) {
    case nlohmann::json::value_t::array: {
      return convert_from_json_to_array(j);
    }

    case nlohmann::json::value_t::object: {
      return convert_from_json_to_object(j);
    }

    case nlohmann::json::value_t::string: {
      std::string value = j;
      return std::make_unique<StringValue>(value);
    }

    case nlohmann::json::value_t::null: {
      return std::make_unique<NullValue>();
    }

    case nlohmann::json::value_t::boolean: {
      return std::make_unique<BooleanValue>(j);
    }
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned: {
      return std::make_unique<IntValue>(j);
    }

    case nlohmann::json::value_t::number_float: {
      return std::make_unique<DoubleValue>(j);
    }

    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
      /*
       * Binary data isn't supported.
       * Discarded is an internal type of nlohmann.
       *
       * Assert in case we need to parse them.
       */
      BLI_assert_unreachable();
      return std::make_unique<NullValue>();
  }

  BLI_assert_unreachable();
  return std::make_unique<NullValue>();
}

void JsonFormatter::serialize(std::ostream &os, const Value &value)
{
  nlohmann::ordered_json j;
  convert_to_json(j, value);
  if (indentation_len) {
    os << j.dump(indentation_len);
  }
  else {
    os << j.dump();
  }
}

std::unique_ptr<Value> JsonFormatter::deserialize(std::istream &is)
{
  nlohmann::ordered_json j;
  is >> j;
  return convert_from_json(j);
}

}  // namespace blender::io::serialize
