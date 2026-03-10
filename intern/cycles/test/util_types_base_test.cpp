/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "util/types_base.h"

CCL_NAMESPACE_BEGIN

TEST(types_base, divide_up)
{
  EXPECT_EQ(divide_up(0, 1), 0);
  EXPECT_EQ(divide_up(1, 1), 1);
  EXPECT_EQ(divide_up(2, 1), 2);

  EXPECT_EQ(divide_up(0, 2), 0);
  EXPECT_EQ(divide_up(1, 2), 1);
  EXPECT_EQ(divide_up(2, 2), 1);
  EXPECT_EQ(divide_up(3, 2), 2);
  EXPECT_EQ(divide_up(4, 2), 2);

  EXPECT_EQ(divide_up(10, 3), 4);
  EXPECT_EQ(divide_up(11, 3), 4);
  EXPECT_EQ(divide_up(12, 3), 4);

  EXPECT_EQ(divide_up(1234567, 100), 12346);
}

TEST(types_base, divide_up_by_shift)
{
  EXPECT_EQ(divide_up_by_shift(0, 0), 0);
  EXPECT_EQ(divide_up_by_shift(1, 0), 1);

  EXPECT_EQ(divide_up_by_shift(0, 1), 0);
  EXPECT_EQ(divide_up_by_shift(1, 1), 1);
  EXPECT_EQ(divide_up_by_shift(2, 1), 1);
  EXPECT_EQ(divide_up_by_shift(3, 1), 2);

  /* 1 << 2 = 4 */
  EXPECT_EQ(divide_up_by_shift(0, 2), 0);
  EXPECT_EQ(divide_up_by_shift(1, 2), 1);
  EXPECT_EQ(divide_up_by_shift(4, 2), 1);
  EXPECT_EQ(divide_up_by_shift(5, 2), 2);
  EXPECT_EQ(divide_up_by_shift(8, 2), 2);

  /* 1 << 10 = 1024 */
  EXPECT_EQ(divide_up_by_shift(10240, 10), 10);
  EXPECT_EQ(divide_up_by_shift(10241, 10), 11);
}

TEST(types_base, align_up)
{
  EXPECT_EQ(align_up(0, 1), 0);
  EXPECT_EQ(align_up(1, 1), 1);
  EXPECT_EQ(align_up(2, 1), 2);

  EXPECT_EQ(align_up(0, 4), 0);
  EXPECT_EQ(align_up(1, 4), 4);
  EXPECT_EQ(align_up(2, 4), 4);
  EXPECT_EQ(align_up(3, 4), 4);
  EXPECT_EQ(align_up(4, 4), 4);
  EXPECT_EQ(align_up(5, 4), 8);

  EXPECT_EQ(align_up(0, 16), 0);
  EXPECT_EQ(align_up(1, 16), 16);
  EXPECT_EQ(align_up(15, 16), 16);
  EXPECT_EQ(align_up(16, 16), 16);
  EXPECT_EQ(align_up(17, 16), 32);

  EXPECT_EQ(align_up(123456, 1024), 123904);
}

TEST(types_base, round_up)
{
  EXPECT_EQ(round_up(0, 1), 0);
  EXPECT_EQ(round_up(1, 1), 1);
  EXPECT_EQ(round_up(2, 1), 2);

  EXPECT_EQ(round_up(0, 5), 0);
  EXPECT_EQ(round_up(1, 5), 5);
  EXPECT_EQ(round_up(4, 5), 5);
  EXPECT_EQ(round_up(5, 5), 5);
  EXPECT_EQ(round_up(6, 5), 10);

  EXPECT_EQ(round_up(10, 3), 12);
  EXPECT_EQ(round_up(11, 3), 12);
  EXPECT_EQ(round_up(12, 3), 12);

  EXPECT_EQ(round_up(1000000, 7), 1000006);
}

TEST(types_base, round_down)
{
  EXPECT_EQ(round_down(0, 1), 0);
  EXPECT_EQ(round_down(1, 1), 1);
  EXPECT_EQ(round_down(2, 1), 2);

  EXPECT_EQ(round_down(0, 5), 0);
  EXPECT_EQ(round_down(1, 5), 0);
  EXPECT_EQ(round_down(4, 5), 0);
  EXPECT_EQ(round_down(5, 5), 5);
  EXPECT_EQ(round_down(6, 5), 5);

  EXPECT_EQ(round_down(10, 3), 9);
  EXPECT_EQ(round_down(11, 3), 9);
  EXPECT_EQ(round_down(12, 3), 12);

  EXPECT_EQ(round_down(1000000, 7), 999999);
}

TEST(types_base, is_power_of_two)
{
  EXPECT_TRUE(is_power_of_two(0));
  EXPECT_TRUE(is_power_of_two(1));
  EXPECT_TRUE(is_power_of_two(2));
  EXPECT_FALSE(is_power_of_two(3));
  EXPECT_TRUE(is_power_of_two(4));
  EXPECT_FALSE(is_power_of_two(5));
  EXPECT_FALSE(is_power_of_two(6));
  EXPECT_FALSE(is_power_of_two(7));
  EXPECT_TRUE(is_power_of_two(8));

  EXPECT_TRUE(is_power_of_two(1024));
  EXPECT_FALSE(is_power_of_two(1023));
  EXPECT_FALSE(is_power_of_two(1025));

  EXPECT_TRUE(is_power_of_two(1ULL << 60));
  EXPECT_FALSE(is_power_of_two((1ULL << 60) + 1));
}

CCL_NAMESPACE_END
