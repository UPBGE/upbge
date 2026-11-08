/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Random numbers of Logic Nodes (2026-11-07). A random node's number is a hash, never a generator
 * with state, of:
 * - the game's random seed (Scene > Game > Random Numbers: Fixed (default) or New Each Play;
 *   Set Random Seed while playing),
 * - the node's identity (its tree's and its own name: editing other nodes changes nothing),
 * - a tick count: steps for Logic + Physics Step routes and for On Init (the steps done before
 *   the tree's first tick, frame or step), frames for Frame Update routes, so the numbers of step
 *   logic do not depend on the frame rate (LN_RuntimeTree::RandomTick),
 * - the node's Seed input, and
 * - the tree's stream (`LN_RuntimeTree::GetRandomStream`, one per object and tree, numbered as the
 *   trees start) unless the node's Variation is Same for All.
 * The same inputs give the same number on any thread and in any order, so a wide column run, a
 * column pass and a tree batch agree bit for bit. Every path computes through these functions.
 */

#pragma once

#include <cstddef>
#include <cstdint>

/** Bit 0 of a random expression's identity: Same for All (no stream in the hash). The other bits
 * are the node's name hash. */
constexpr uint32_t LN_RANDOM_SAME_FOR_ALL = 1u;

/** The murmur3 64-bit finalizer: every input bit moves every output bit. */
inline uint64_t LN_RandomMix(uint64_t value)
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33;
  return value;
}

/** FNV-1a of text: the same on every platform and compiler (std::hash is not). */
inline uint64_t LN_RandomHashText(const char *text, const size_t size, uint64_t hash = 0xcbf29ce484222325ull)
{
  for (size_t index = 0; index < size; index++) {
    hash ^= uint64_t(uint8_t(text[index]));
    hash *= 0x100000001b3ull;
  }
  return hash;
}

/** The part of a node's number every object shares: the game's seed, the node and the tick. */
inline uint64_t LN_RandomNodePart(const uint64_t game_seed,
                                  const uint32_t identity,
                                  const uint64_t tick)
{
  return LN_RandomMix(game_seed ^
                      LN_RandomMix((uint64_t(identity & ~LN_RANDOM_SAME_FOR_ALL) << 32) ^
                                   LN_RandomMix(tick)));
}

/** One random number: the node's part, its Seed input, the tree's stream (0 with Same for All)
 * and `salt`, which tells one node's numbers apart (a vector's axes: 1, 2, 3; else 0). */
inline uint64_t LN_RandomBits(const uint64_t node_part,
                              const int32_t seed,
                              const uint64_t stream,
                              const uint32_t salt)
{
  return LN_RandomMix(node_part ^ LN_RandomMix((uint64_t(salt) << 32) ^ uint32_t(seed)) ^ stream);
}

/** [0, 1): the top 24 bits. */
inline float LN_RandomUnitFloat(const uint64_t bits)
{
  return float(bits >> 40) * (1.0f / 16777216.0f);
}

/** [lower, upper] for any two ends, both included, every value equally likely (the whole int32
 * range too: no overflow). */
inline int32_t LN_RandomIntInRange(const uint64_t bits, int32_t a, int32_t b)
{
  const int64_t lower = a < b ? a : b;
  const int64_t upper = a < b ? b : a;
  const uint64_t span = uint64_t(upper - lower) + 1u; /* at most 2^32 */
  return int32_t(lower + int64_t((uint64_t(uint32_t(bits >> 32)) * span) >> 32));
}

/** [a, b) or [b, a) for floats: the two ends in either order. */
inline float LN_RandomFloatInRange(const uint64_t bits, const float a, const float b)
{
  const float lower = a < b ? a : b;
  const float upper = a < b ? b : a;
  return lower + (upper - lower) * LN_RandomUnitFloat(bits);
}

/** True with `probability` (0 never, 1 and more always). */
inline bool LN_RandomChance(const uint64_t bits, const float probability)
{
  return LN_RandomUnitFloat(bits) < probability;
}

/** A place in a list of `size` (> 0) items, every place equally likely. */
inline size_t LN_RandomIndex(const uint64_t bits, const size_t size)
{
  return size_t((uint64_t(uint32_t(bits >> 32)) * uint64_t(size)) >> 32);
}

/** The game's random seed: what every random node's number starts from. Changed only between
 * ticks (game start, Set Random Seed applied after its tick, Load Game), so worker threads read
 * it freely while trees run. */
uint32_t LN_GetRandomGameSeed();
void LN_SetRandomGameSeed(uint32_t seed);

/** Scene > Game > Random Numbers (the same values as GAME_RANDOM_SEED_*). */
enum class LN_RandomSeedMode : uint8_t {
  /** The same seed every play (every scene's default): a node's Seed always gives the same
   * numbers; tests, replays, shared worlds, a bug that must happen again. */
  Fixed = 0,
  /** A new seed from the clock at every game start. */
  NewEachPlay = 1,
};

/** Game start (LA_Launcher): the seed of this play from the start scene's setting.
 * `UPBGE_LN_RANDOM_SEED=<number>` plays every game with that fixed seed, `=new` with a new one
 * (test tools). */
uint32_t LN_StartRandomGameSeed(LN_RandomSeedMode mode, int32_t fixed_seed);
/** Whether this play's seed came from the clock (the play report and the console say it). */
bool LN_RandomGameSeedIsNew();
/** True the first time it is asked after a game start: the console says the play's seed once. */
bool LN_NoteRandomSeedOnce();
/** A new seed from the clock, 0 .. 2^31 - 1 (typed into Fixed, it gives the play again). */
uint32_t LN_NewRandomGameSeed();

/** Set Random Seed: the seed takes effect after the tick it was asked in, for every tree at once
 * (the trees of one tick all see one seed; the last request of a tick wins). */
void LN_RequestRandomGameSeed(uint32_t seed);
/** The managers call it after each tick's flush (and test rigs after theirs). True when a
 * request was applied. */
bool LN_ApplyPendingRandomGameSeed();
/** Tests: no request waiting. */
void LN_ClearPendingRandomGameSeedForTests();
