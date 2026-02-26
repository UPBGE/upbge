/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "util/simd.h"

CCL_NAMESPACE_BEGIN

TEST(simd, bsf)
{
  EXPECT_EQ(__bsf(uint32_t(1)), 0);
  EXPECT_EQ(__bsf(uint32_t(2)), 1);
  EXPECT_EQ(__bsf(uint32_t(4)), 2);
  EXPECT_EQ(__bsf(uint32_t(0x80000000)), 31);

  EXPECT_EQ(__bsf(uint64_t(1)), 0);
  EXPECT_EQ(__bsf(uint64_t(2)), 1);
  EXPECT_EQ(__bsf(uint64_t(0x8000000000000000ULL)), 63);
}

TEST(simd, bsr)
{
  EXPECT_EQ(__bsr(uint32_t(1)), 0);
  EXPECT_EQ(__bsr(uint32_t(2)), 1);
  EXPECT_EQ(__bsr(uint32_t(3)), 1); /* 11 binary */
  EXPECT_EQ(__bsr(uint32_t(4)), 2);
  EXPECT_EQ(__bsr(uint32_t(0x80000000)), 31);

  EXPECT_EQ(__bsr(uint64_t(1)), 0);
  EXPECT_EQ(__bsr(uint64_t(2)), 1);
  EXPECT_EQ(__bsr(uint64_t(3)), 1);
  EXPECT_EQ(__bsr(uint64_t(0x8000000000000000ULL)), 63);
}

TEST(simd, btc)
{
  EXPECT_EQ(__btc(uint32_t(0), 0), 1);
  EXPECT_EQ(__btc(uint32_t(1), 0), 0);
  EXPECT_EQ(__btc(uint32_t(0), 1), 2);
  EXPECT_EQ(__btc(uint32_t(2), 1), 0);
  EXPECT_EQ(__btc(uint32_t(0), 31), 0x80000000);
  EXPECT_EQ(__btc(uint32_t(0x80000000), 31), 0);
  EXPECT_EQ(__btc(uint32_t(0xFFFFFFFF), 0), 0xFFFFFFFE);

  EXPECT_EQ(__btc(uint64_t(0), 0), 1);
  EXPECT_EQ(__btc(uint64_t(1), 0), 0);
  EXPECT_EQ(__btc(uint64_t(0), 63), 0x8000000000000000ULL);
  EXPECT_EQ(__btc(uint64_t(0x8000000000000000ULL), 63), 0);
}

TEST(simd, bitscan)
{
  EXPECT_EQ(bitscan(uint32_t(1)), 0);
  EXPECT_EQ(bitscan(uint32_t(2)), 1);
  EXPECT_EQ(bitscan(uint32_t(4)), 2);
  EXPECT_EQ(bitscan(uint32_t(0x80000000)), 31);
  /* Test with multiple bits set, should find least significant */
  EXPECT_EQ(bitscan(uint32_t(3)), 0);
  EXPECT_EQ(bitscan(uint32_t(6)), 1);

  EXPECT_EQ(bitscan(uint64_t(1)), 0);
  EXPECT_EQ(bitscan(uint64_t(2)), 1);
  EXPECT_EQ(bitscan(uint64_t(0x8000000000000000ULL)), 63);
  /* Test with multiple bits set */
  EXPECT_EQ(bitscan(uint64_t(3)), 0);
  EXPECT_EQ(bitscan(uint64_t(6)), 1);
}

CCL_NAMESPACE_END
