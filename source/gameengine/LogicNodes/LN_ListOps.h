/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Insert's place and List Operation's Min, Max, Sum and Average (2026-11-17), once for the
 * runtime kinds, the flush's game property edit and the tests.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "LN_Types.h"

/** List Operation's statistics (Float::ListStatistic / Int::ListStatisticIndex int_value). */
enum class LN_ListStatistic : int32_t { Min = 0, Max = 1, Sum = 2, Average = 3 };

/** Where Insert puts its value in a list of `size` items: `index` (below 0 counts from the end,
 * -1 before the last item), clamped to [0, size] (as Python's list.insert). */
inline size_t LN_ListInsertPlace(const int32_t index, const size_t size)
{
  int64_t place = index < 0 ? int64_t(size) + int64_t(index) : int64_t(index);
  place = place < 0 ? 0 : (place > int64_t(size) ? int64_t(size) : place);
  return size_t(place);
}

/** An item as a number: a whole or decimal number that is a number (not NaN). */
inline bool LN_ListItemNumber(const LN_Value &item, double &r_number)
{
  if (!item.exists) {
    return false;
  }
  if (item.type == LN_ValueType::Int) {
    r_number = double(item.int_value);
    return true;
  }
  if (item.type == LN_ValueType::Float && !std::isnan(item.float_value)) {
    r_number = double(item.float_value);
    return true;
  }
  return false;
}

/** Min, Max, Sum or Average of the numbers in `items` (others skipped; none: 0), and in
 * `r_index` where the first smallest or largest is (-1 without numbers; Sum and Average: -1). */
inline float LN_ListStatisticOf(const std::vector<LN_Value> &items,
                                const LN_ListStatistic statistic,
                                int32_t &r_index)
{
  r_index = -1;
  double best = 0.0;
  double sum = 0.0;
  int64_t count = 0;
  for (size_t place = 0; place < items.size(); place++) {
    double number = 0.0;
    if (!LN_ListItemNumber(items[place], number)) {
      continue;
    }
    if (count == 0 || (statistic == LN_ListStatistic::Min && number < best) ||
        (statistic == LN_ListStatistic::Max && number > best))
    {
      best = number;
      if (statistic == LN_ListStatistic::Min || statistic == LN_ListStatistic::Max) {
        r_index = int32_t(place);
      }
    }
    sum += number;
    count++;
  }
  if (count == 0) {
    return 0.0f;
  }
  switch (statistic) {
    case LN_ListStatistic::Sum:
      return float(sum);
    case LN_ListStatistic::Average:
      return float(sum / double(count));
    default:
      return float(best);
  }
}
