/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Date and Time (2026-11-17): the parts of a wall clock reading (seconds since 1 January 1970,
 * UTC), in UTC or the computer's local time. The manager reads the clock once a tick
 * (LN_Manager::WallClockSeconds), so every read of a tick agrees, on any thread.
 */

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>

/** The computer's clock now: seconds since 1 January 1970, UTC. */
inline double LN_ReadWallClockSeconds()
{
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/** Date and Time's whole-number outputs (Int::DateTimePart's int_value & 0xff). */
enum class LN_DateTimePart : int32_t {
  Year = 0,
  Month,
  Day,
  Hour,
  Minute,
  Second,
  /** 0 Monday to 6 Sunday. */
  Weekday,
  /** Days since 1 January 1970 in the chosen zone. */
  DayNumber,
};

struct LN_DateTimeParts {
  int32_t year = 1970;
  int32_t month = 1;
  int32_t day = 1;
  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  int32_t weekday = 3; /* (1 January 1970 was a Thursday) */
  int32_t day_number = 0;
  double day_fraction = 0.0;
};

/** Days from 1 January 1970 to the date (proleptic Gregorian; Howard Hinnant's days_from_civil). */
inline int64_t LN_DaysFromCivil(int64_t year, const int32_t month, const int32_t day)
{
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                             day_of_year;
  return era * 146097 + day_of_era - 719468;
}

/** The parts of `seconds` (since 1970, UTC) in UTC or local time; false (the parts of 1970) for
 * a reading the C library cannot turn into a date. */
inline bool LN_DateTimeFromSeconds(const double seconds, const bool utc, LN_DateTimeParts &r_parts)
{
  r_parts = LN_DateTimeParts();
  if (!std::isfinite(seconds) || std::fabs(seconds) > 1.0e15) {
    return false;
  }
  const double whole = std::floor(seconds);
  const std::time_t time = std::time_t(whole);
  std::tm parts{};
#ifdef _WIN32
  if ((utc ? gmtime_s(&parts, &time) : localtime_s(&parts, &time)) != 0) {
    return false;
  }
#else
  if ((utc ? gmtime_r(&time, &parts) : localtime_r(&time, &parts)) == nullptr) {
    return false;
  }
#endif
  r_parts.year = int32_t(parts.tm_year) + 1900;
  r_parts.month = int32_t(parts.tm_mon) + 1;
  r_parts.day = int32_t(parts.tm_mday);
  r_parts.hour = int32_t(parts.tm_hour);
  r_parts.minute = int32_t(parts.tm_min);
  /* (A leap second, 60, is shown as 59: a minute has 60 seconds in a game.) */
  r_parts.second = parts.tm_sec > 59 ? 59 : int32_t(parts.tm_sec);
  r_parts.weekday = (int32_t(parts.tm_wday) + 6) % 7;
  r_parts.day_number = int32_t(LN_DaysFromCivil(r_parts.year, r_parts.month, r_parts.day));
  const double into_day = double(r_parts.hour) * 3600.0 + double(r_parts.minute) * 60.0 +
                          double(r_parts.second) + (seconds - whole);
  r_parts.day_fraction = into_day / 86400.0;
  if (r_parts.day_fraction >= 1.0) {
    r_parts.day_fraction = std::nextafter(1.0, 0.0);
  }
  return true;
}

/** One output of `parts` (Int::DateTimePart). */
inline int32_t LN_DateTimePartValue(const LN_DateTimeParts &parts, const LN_DateTimePart part)
{
  switch (part) {
    case LN_DateTimePart::Year:
      return parts.year;
    case LN_DateTimePart::Month:
      return parts.month;
    case LN_DateTimePart::Day:
      return parts.day;
    case LN_DateTimePart::Hour:
      return parts.hour;
    case LN_DateTimePart::Minute:
      return parts.minute;
    case LN_DateTimePart::Second:
      return parts.second;
    case LN_DateTimePart::Weekday:
      return parts.weekday;
    case LN_DateTimePart::DayNumber:
      return parts.day_number;
  }
  return 0;
}
