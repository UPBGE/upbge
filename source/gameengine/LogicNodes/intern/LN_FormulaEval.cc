/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_FormulaEval.cc
 *  \ingroup logicnodes
 */

#include "LN_FormulaEval.hh"

#include <cmath>
#include <cctype>
#include <string>

namespace LN {

namespace {

float Signum(const float value)
{
  return (value > 0.0f) - (value < 0.0f);
}

float Curt(const float value)
{
  if (value > 0.0f) {
    return std::pow(value, 1.0f / 3.0f);
  }
  return -std::pow(-value, 1.0f / 3.0f);
}

bool IsIdentifierChar(const char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

struct EvalContext {
  float a = 0.0f;
  float b = 0.0f;
  const char *cursor = nullptr;
  const char *end = nullptr;
  bool error = false;

  void skip_spaces()
  {
    while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) {
      cursor++;
    }
  }

  bool consume(const char expected)
  {
    skip_spaces();
    if (cursor < end && *cursor == expected) {
      cursor++;
      return true;
    }
    error = true;
    return false;
  }

  float parse_number()
  {
    skip_spaces();
    char *next = nullptr;
    const float value = std::strtof(cursor, &next);
    if (next == cursor) {
      error = true;
      return 0.0f;
    }
    cursor = next;
    return value;
  }

  float parse_identifier()
  {
    skip_spaces();
    if (cursor >= end) {
      error = true;
      return 0.0f;
    }

    const char *start = cursor;
    while (cursor < end && IsIdentifierChar(*cursor)) {
      cursor++;
    }
    const std::string name(start, cursor - start);

    if (name == "a") {
      return a;
    }
    if (name == "b") {
      return b;
    }

    if (name == "pi") {
      return float(M_PI);
    }
    if (name == "e") {
      return float(M_E);
    }

    auto parse_call = [&](auto fn) {
      if (!consume('(')) {
        return 0.0f;
      }
      const float arg = parse_expression();
      if (!consume(')')) {
        return 0.0f;
      }
      return fn(arg);
    };

    auto parse_call2 = [&](auto fn) {
      if (!consume('(')) {
        return 0.0f;
      }
      const float arg0 = parse_expression();
      if (!consume(',')) {
        return 0.0f;
      }
      const float arg1 = parse_expression();
      if (!consume(')')) {
        return 0.0f;
      }
      return fn(arg0, arg1);
    };

    if (name == "abs") {
      return parse_call([](const float x) { return std::fabs(x); });
    }
    if (name == "acos") {
      return parse_call([](const float x) { return std::acos(x); });
    }
    if (name == "asin") {
      return parse_call([](const float x) { return std::asin(x); });
    }
    if (name == "atan") {
      return parse_call([](const float x) { return std::atan(x); });
    }
    if (name == "atan2") {
      return parse_call2([](const float x, const float y) { return std::atan2(x, y); });
    }
    if (name == "acosh") {
      return parse_call([](const float x) { return std::acosh(x); });
    }
    if (name == "asinh") {
      return parse_call([](const float x) { return std::asinh(x); });
    }
    if (name == "atanh") {
      return parse_call([](const float x) { return std::atanh(x); });
    }
    if (name == "ceil") {
      return parse_call([](const float x) { return std::ceil(x); });
    }
    if (name == "cos") {
      return parse_call([](const float x) { return std::cos(x); });
    }
    if (name == "cosh") {
      return parse_call([](const float x) { return std::cosh(x); });
    }
    if (name == "curt") {
      return parse_call(Curt);
    }
    if (name == "degrees") {
      return parse_call([](const float x) { return x * (180.0f / float(M_PI)); });
    }
    if (name == "exp") {
      return parse_call([](const float x) { return std::exp(x); });
    }
    if (name == "floor") {
      return parse_call([](const float x) { return std::floor(x); });
    }
    if (name == "hypot") {
      return parse_call2([](const float x, const float y) { return std::hypot(x, y); });
    }
    if (name == "log") {
      return parse_call([](const float x) { return std::log(x); });
    }
    if (name == "log10") {
      return parse_call([](const float x) { return std::log10(x); });
    }
    if (name == "mod") {
      return parse_call2([](const float x, const float y) { return std::fmod(x, y); });
    }
    if (name == "pow") {
      return parse_call2([](const float x, const float y) { return std::pow(x, y); });
    }
    if (name == "radians") {
      return parse_call([](const float x) { return x * (float(M_PI) / 180.0f); });
    }
    if (name == "sign") {
      return parse_call(Signum);
    }
    if (name == "sin") {
      return parse_call([](const float x) { return std::sin(x); });
    }
    if (name == "sinh") {
      return parse_call([](const float x) { return std::sinh(x); });
    }
    if (name == "sqrt") {
      return parse_call([](const float x) { return std::sqrt(x); });
    }
    if (name == "tan") {
      return parse_call([](const float x) { return std::tan(x); });
    }
    if (name == "tanh") {
      return parse_call([](const float x) { return std::tanh(x); });
    }

    error = true;
    return 0.0f;
  }

  float parse_factor()
  {
    skip_spaces();
    if (cursor >= end) {
      error = true;
      return 0.0f;
    }

    if (*cursor == '(') {
      cursor++;
      const float value = parse_expression();
      if (!consume(')')) {
        return 0.0f;
      }
      return value;
    }

    if (std::isdigit(static_cast<unsigned char>(*cursor)) || *cursor == '.') {
      return parse_number();
    }

    if (*cursor == '+' || *cursor == '-') {
      const bool negate = *cursor == '-';
      cursor++;
      const float value = parse_factor();
      return negate ? -value : value;
    }

    return parse_identifier();
  }

  float parse_term()
  {
    float value = parse_factor();
    while (!error) {
      skip_spaces();
      if (cursor >= end) {
        break;
      }
      const char op = *cursor;
      if (op != '*' && op != '/') {
        break;
      }
      cursor++;
      const float rhs = parse_factor();
      if (op == '*') {
        value *= rhs;
      }
      else {
        value = rhs == 0.0f ? 0.0f : value / rhs;
      }
    }
    return value;
  }

  float parse_expression()
  {
    float value = parse_term();
    while (!error) {
      skip_spaces();
      if (cursor >= end) {
        break;
      }
      const char op = *cursor;
      if (op != '+' && op != '-') {
        break;
      }
      cursor++;
      const float rhs = parse_term();
      value = op == '+' ? value + rhs : value - rhs;
    }
    return value;
  }
};

}  // namespace

float EvaluateFormula(const std::string &formula, const float a, const float b)
{
  if (formula.empty()) {
    return 0.0f;
  }

  EvalContext context;
  context.a = a;
  context.b = b;
  context.cursor = formula.c_str();
  context.end = formula.c_str() + formula.size();

  const float result = context.parse_expression();
  context.skip_spaces();
  if (context.error || context.cursor != context.end) {
    return 0.0f;
  }
  return result;
}

}  // namespace LN
