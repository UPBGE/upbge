/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * String Operation's Substring, Length, Find, Split and Format Time (2026-11-17), once for the
 * runtime kinds and the compiler's folding. Texts are UTF-8: positions and lengths count
 * characters (code points), never bytes, so "Über" is 4 long and its second character is "b".
 * A byte that is no UTF-8 continuation byte starts a character (broken text never stops them).
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/** Whether byte `c` starts a character (is no UTF-8 continuation byte 10xxxxxx). */
inline bool LN_TextStartsCharacter(const unsigned char c)
{
  return (c & 0xC0u) != 0x80u;
}

/** How many characters `text` has. */
inline int32_t LN_TextLength(const std::string &text)
{
  int32_t count = 0;
  for (const char c : text) {
    count += LN_TextStartsCharacter(static_cast<unsigned char>(c)) ? 1 : 0;
  }
  return count;
}

/** The byte where character `index` starts (`index` at or past the end: the text's size). */
inline size_t LN_TextByteOfCharacter(const std::string &text, const int32_t index)
{
  if (index <= 0) {
    return 0;
  }
  int32_t seen = 0;
  for (size_t byte = 0; byte < text.size(); byte++) {
    if (LN_TextStartsCharacter(static_cast<unsigned char>(text[byte]))) {
      if (seen == index) {
        return byte;
      }
      seen++;
    }
  }
  return text.size();
}

/** The part [r_first, r_last) of `count` items that Substring and List Operation's Slice take:
 * `length` items from `start` (0 the first; a negative start counts from the end, -1 the last; a
 * negative length takes the rest), clamped to the items (2026-11-17). */
inline void LN_ClampSlice(const int64_t count,
                          const int32_t start,
                          const int32_t length,
                          int64_t &r_first,
                          int64_t &r_last)
{
  int64_t first = start < 0 ? count + int64_t(start) : int64_t(start);
  first = first < 0 ? 0 : (first > count ? count : first);
  int64_t last = length < 0 ? count : first + int64_t(length);
  last = last > count ? count : last;
  r_first = first;
  r_last = last < first ? first : last;
}

/** Substring: `length` characters from character `start` (LN_ClampSlice). */
inline std::string LN_TextSubstring(const std::string &text,
                                    const int32_t start,
                                    const int32_t length)
{
  int64_t first = 0, last = 0;
  LN_ClampSlice(LN_TextLength(text), start, length, first, last);
  if (last <= first) {
    return std::string();
  }
  const size_t from = LN_TextByteOfCharacter(text, int32_t(first));
  const size_t to = LN_TextByteOfCharacter(text, int32_t(last));
  return text.substr(from, to - from);
}

/** Find: the character where `search` first appears in `text`, -1 when it does not (an empty
 * search is found at 0). */
inline int32_t LN_TextFind(const std::string &text, const std::string &search)
{
  const size_t byte = text.find(search);
  if (byte == std::string::npos) {
    return -1;
  }
  int32_t index = 0;
  for (size_t i = 0; i < byte; i++) {
    index += LN_TextStartsCharacter(static_cast<unsigned char>(text[i])) ? 1 : 0;
  }
  return index;
}

inline bool LN_TextIsSpace(const char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/** Split: the parts of `text` between each `separator`, in order (two separators side by side
 * give an empty part; "" gives one empty part). An empty separator splits at runs of spaces,
 * tabs and new lines and keeps no empty part ("" gives none). */
inline std::vector<std::string> LN_TextSplit(const std::string &text, const std::string &separator)
{
  std::vector<std::string> parts;
  if (separator.empty()) {
    size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && LN_TextIsSpace(text[i])) {
        i++;
      }
      const size_t begin = i;
      while (i < text.size() && !LN_TextIsSpace(text[i])) {
        i++;
      }
      if (i > begin) {
        parts.push_back(text.substr(begin, i - begin));
      }
    }
    return parts;
  }
  size_t begin = 0;
  while (true) {
    const size_t at = text.find(separator, begin);
    if (at == std::string::npos) {
      parts.push_back(text.substr(begin));
      return parts;
    }
    parts.push_back(text.substr(begin, at - begin));
    begin = at + separator.size();
  }
}

/** Format Time's formats (String Operation's custom2). */
enum class LN_TimeFormat : int32_t {
  /** 1:15, 75:00 (minutes as many as there are). */
  MinutesSeconds = 0,
  /** 01:15. */
  MinutesSecondsTwoDigits = 1,
  /** 1:01:15. */
  HoursMinutesSeconds = 2,
  /** 1:15.25 (hundredths). */
  MinutesSecondsHundredths = 3,
};

/** Format Time: `seconds` as a clock. Shown units (seconds, or hundredths for M:SS.cc) are cut
 * down, or rounded up with `round_up` (a countdown shows 0:01 until it reaches 0); a hair of
 * float noise (1e-6 of a unit) never moves a value to the unit below or above. Below zero: a
 * minus sign; not a number or infinite: 0. */
inline std::string LN_FormatTime(const float seconds, const int32_t format, const bool round_up)
{
  double value = std::isfinite(seconds) ? double(seconds) : 0.0;
  const bool negative = value < 0.0;
  value = std::fabs(value);
  const bool hundredths = format == int32_t(LN_TimeFormat::MinutesSecondsHundredths);
  const double scale = hundredths ? 100.0 : 1.0;
  double units = round_up ? std::ceil(value * scale - 1.0e-6) :
                            std::floor(value * scale + 1.0e-6);
  units = units < 0.0 ? 0.0 : (units > 1.0e15 ? 1.0e15 : units);
  const unsigned long long whole = (unsigned long long)units;
  const unsigned long long total = hundredths ? whole / 100ull : whole;
  char buffer[64];
  switch (format) {
    case int32_t(LN_TimeFormat::MinutesSecondsTwoDigits):
      std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu", total / 60ull, total % 60ull);
      break;
    case int32_t(LN_TimeFormat::HoursMinutesSeconds):
      std::snprintf(buffer,
                    sizeof(buffer),
                    "%llu:%02llu:%02llu",
                    total / 3600ull,
                    (total / 60ull) % 60ull,
                    total % 60ull);
      break;
    case int32_t(LN_TimeFormat::MinutesSecondsHundredths):
      std::snprintf(buffer,
                    sizeof(buffer),
                    "%llu:%02llu.%02llu",
                    total / 60ull,
                    total % 60ull,
                    whole % 100ull);
      break;
    default:
      std::snprintf(buffer, sizeof(buffer), "%llu:%02llu", total / 60ull, total % 60ull);
      break;
  }
  return (negative && whole != 0ull ? std::string("-") : std::string()) + buffer;
}
