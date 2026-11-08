/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 */

#include "LN_Random.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

#include "CM_Message.h"

namespace {

/* Written only between ticks; relaxed atomics keep helper threads' reads well defined. */
std::atomic<uint32_t> g_game_seed{0};
std::atomic<bool> g_game_seed_new{false};
std::atomic<bool> g_pending{false};
std::atomic<uint32_t> g_pending_seed{0};
std::atomic<bool> g_seed_noted{false};

}  // namespace

uint32_t LN_GetRandomGameSeed()
{
  return g_game_seed.load(std::memory_order_relaxed);
}

void LN_SetRandomGameSeed(const uint32_t seed)
{
  g_game_seed.store(seed, std::memory_order_relaxed);
}

uint32_t LN_NewRandomGameSeed()
{
  /* The device's entropy mixed with the clock: two games started in the same second differ. */
  uint64_t value = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  try {
    std::random_device device;
    value ^= uint64_t(device()) << 32;
    value ^= uint64_t(device());
  }
  catch (...) {
    /* (No entropy device: the clock alone.) */
  }
  value ^= uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
  return uint32_t(LN_RandomMix(value) & 0x7fffffffu);
}

uint32_t LN_StartRandomGameSeed(const LN_RandomSeedMode mode, const int32_t fixed_seed)
{
  g_pending.store(false, std::memory_order_relaxed);
  g_seed_noted.store(false, std::memory_order_relaxed);
  uint32_t seed = uint32_t(fixed_seed);
  bool is_new = mode == LN_RandomSeedMode::NewEachPlay;
  if (const char *text = std::getenv("UPBGE_LN_RANDOM_SEED")) {
    char *end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end != text && *end == '\0') {
      seed = uint32_t(int32_t(value));
      is_new = false;
    }
    else if (std::strcmp(text, "new") == 0) {
      is_new = true;
    }
    else {
      CM_Warning("UPBGE_LN_RANDOM_SEED='" << text
                                          << "' is not a number or 'new': the scene's setting "
                                             "is used");
    }
  }
  if (is_new) {
    seed = LN_NewRandomGameSeed();
  }
  g_game_seed.store(seed, std::memory_order_relaxed);
  g_game_seed_new.store(is_new, std::memory_order_relaxed);
  return seed;
}

bool LN_RandomGameSeedIsNew()
{
  return g_game_seed_new.load(std::memory_order_relaxed);
}

bool LN_NoteRandomSeedOnce()
{
  return !g_seed_noted.exchange(true, std::memory_order_relaxed);
}

void LN_RequestRandomGameSeed(const uint32_t seed)
{
  g_pending_seed.store(seed, std::memory_order_relaxed);
  g_pending.store(true, std::memory_order_relaxed);
}

bool LN_ApplyPendingRandomGameSeed()
{
  if (!g_pending.exchange(false, std::memory_order_relaxed)) {
    return false;
  }
  g_game_seed.store(g_pending_seed.load(std::memory_order_relaxed), std::memory_order_relaxed);
  g_game_seed_new.store(false, std::memory_order_relaxed);
  return true;
}

void LN_ClearPendingRandomGameSeedForTests()
{
  g_pending.store(false, std::memory_order_relaxed);
}
