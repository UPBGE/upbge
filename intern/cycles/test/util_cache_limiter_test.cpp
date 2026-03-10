/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "util/cache_limiter.h"

CCL_NAMESPACE_BEGIN

struct TestResource {
  int value;
  TestResource(int v) : value(v) {}
};

TEST(CacheLimiter, Basic)
{
  CacheLimiter<TestResource> limiter(2);
  CacheHandle<TestResource> handle1;

  /* Create a handle. */
  {
    auto guard = handle1.acquire(limiter, []() { return std::make_unique<TestResource>(1); });
    EXPECT_EQ(guard.get()->value, 1);
  }

  /* Verify handle is not created again. */
  {
    bool created = false;
    auto guard = handle1.acquire(limiter, [&created]() {
      created = true;
      return std::make_unique<TestResource>(2);
    });
    EXPECT_EQ(guard.get()->value, 1);
    EXPECT_FALSE(created);
  }
}

TEST(CacheLimiter, Eviction)
{
  CacheLimiter<TestResource> limiter(1);
  CacheHandle<TestResource> handle1;
  CacheHandle<TestResource> handle2;

  /* Create two handles. */
  {
    auto guard = handle1.acquire(limiter, []() { return std::make_unique<TestResource>(1); });
    EXPECT_EQ(guard.get()->value, 1);
  }

  {
    bool created = false;
    auto guard = handle2.acquire(limiter, [&created]() {
      created = true;
      return std::make_unique<TestResource>(2);
    });
    EXPECT_EQ(guard.get()->value, 2);
    EXPECT_TRUE(created);
  }

  /* Verify first handle got evicted and is created again. */
  {
    bool created = false;
    auto guard = handle1.acquire(limiter, [&created]() {
      created = true;
      return std::make_unique<TestResource>(1);
    });
    EXPECT_EQ(guard.get()->value, 1);
    EXPECT_TRUE(created);
  }
}

TEST(CacheLimiter, NoEvictionWhenUsed)
{
  CacheLimiter<TestResource> limiter(1);
  CacheHandle<TestResource> handle1;
  CacheHandle<TestResource> handle2;

  /* Acquire and hold two guards at the same time. */
  auto guard1 = handle1.acquire(limiter, []() { return std::make_unique<TestResource>(1); });

  {
    bool created = false;
    auto guard2 = handle2.acquire(limiter, [&created]() {
      created = true;
      return std::make_unique<TestResource>(2);
    });
    EXPECT_EQ(guard2.get()->value, 2);
    EXPECT_TRUE(created);
  }

  /* Check first handle is still valid. */
  {
    EXPECT_EQ(guard1.get()->value, 1);
  }
}

CCL_NAMESPACE_END
