/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include "testing/testing.h"

#include "util/time.h"

CCL_NAMESPACE_BEGIN

TEST(time_human_readable_to_seconds, Empty)
{
  EXPECT_EQ(time_human_readable_to_seconds(""), 0.0);
  EXPECT_EQ(time_human_readable_from_seconds(0.0), "00:00.00");
}

TEST(time_human_readable_to_seconds, Fraction)
{
  EXPECT_NEAR(time_human_readable_to_seconds(".1"), 0.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds(".10"), 0.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(0.1), "00:00.10");
}

TEST(time_human_readable_to_seconds, Seconds)
{
  EXPECT_NEAR(time_human_readable_to_seconds("2.1"), 2.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("02.10"), 2.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(2.1), "00:02.10");

  EXPECT_NEAR(time_human_readable_to_seconds("12.1"), 12.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("12.10"), 12.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(12.1), "00:12.10");
}

TEST(time_human_readable_to_seconds, MinutesSeconds)
{
  EXPECT_NEAR(time_human_readable_to_seconds("3:2.1"), 182.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("03:02.10"), 182.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(182.1), "03:02.10");

  EXPECT_NEAR(time_human_readable_to_seconds("34:12.1"), 2052.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("34:12.10"), 2052.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(2052.1), "34:12.10");
}

TEST(time_human_readable_to_seconds, HoursMinutesSeconds)
{
  EXPECT_NEAR(time_human_readable_to_seconds("4:3:2.1"), 14582.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("04:03:02.10"), 14582.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(14582.1), "04:03:02.10");

  EXPECT_NEAR(time_human_readable_to_seconds("56:34:12.1"), 203652.1, 1e-8f);
  EXPECT_NEAR(time_human_readable_to_seconds("56:34:12.10"), 203652.1, 1e-8f);
  EXPECT_EQ(time_human_readable_from_seconds(203652.1), "56:34:12.10");
}

CCL_NAMESPACE_END
