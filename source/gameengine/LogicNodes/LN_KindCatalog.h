/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The kind catalog (lean runtime plan, phase A2): one table of every opcode, expression kind and
 * query kind of the Logic Nodes runtime (606), with its family, what kind of value or action it
 * is, whether a tree batch may hold it, and where it stands on the way to the lean runtime.
 *
 * Why it exists: the plan's caches, native ports and blocks may treat a kind in a special way
 * only when this table says it is safe, and a test fails when a kind is added to an enum without a
 * row here (rule 21 of the plan). The rows are in LN_KindCatalogData.inc, generated once by
 * TESTS/lean_plan/make_kind_catalog.py and then kept by hand. */

#include <cstddef>
#include <cstdint>

enum class LN_KindDomain : uint8_t {
  Op = 0,
  Bool,
  Int,
  Float,
  String,
  Vector,
  Color,
  Value,
  Query,
  Count
};

/** What kind of thing a kind is, as bits (a kind can be several). Only PURE alone is safe for a
 * cache that lives for a tick: the kind reads nothing but its inputs and the tick's snapshot. */
enum LN_KindClass : uint8_t {
  /** Same inputs, same result, for the whole tick. */
  LN_KIND_PURE = 1,
  /** Reads the pass's scope or time that a step gate changes (loop index, delta time ...), or an
   * event or UI payload: a result is good only where it was made. */
  LN_KIND_CONTEXT = 2,
  /** Reads state that earlier work of the same tick can change (tree properties, runtime state,
   * the scene, a file, global properties). */
  LN_KIND_MUTABLE_READ = 4,
  /** Keeps state between ticks. */
  LN_KIND_STATEFUL = 8,
  /** Reading it applies the pending moves of what it reads (Get Distance, the rays ...). */
  LN_KIND_APPLIES_PENDING_MOVES = 16,
  /** Only the main thread may evaluate it. */
  LN_KIND_MAIN_THREAD = 32,
  /** Gives the use site's fallback when it cannot work out a value (the fallback audit of plan
   * phase A14, `lean_plan/fallback_audit.md`): the answer depends on who asks, so it is never
   * kept per expression. A PURE kind may carry this bit; the memo skips it. */
  LN_KIND_USES_FALLBACK = 64,
};

/** Whether a tree batch may hold a program using the kind (LN_TreeBatch.cpp's refusal
 * functions). Conditional: it depends on more than the kind (a move of the owner only, the ends of
 * Get Distance ...). */
enum class LN_BatchAllowance : uint8_t { No = 0, Yes, Conditional };

/** Where the kind stands on the way to the lean runtime: Old (only the old interpreter), Generic
 * (the lean program calls the kind function), Native (a typed op or kernel), Dead (the compiler
 * never builds it; to be deleted). */
enum class LN_LeanStatus : uint8_t { Old = 0, Generic, Native, Dead };

/** The same for blocks (part B). */
enum class LN_BlockStatus : uint8_t { None = 0, Planned, Done };

struct LN_KindInfo {
  LN_KindDomain domain;
  /** The enumerator's value in its enum (LN_OpCode, LN_BoolExpressionKind ...). */
  uint16_t value;
  /** "Domain::Name", e.g. "Float::Add", "Op::SetGameProperty". */
  const char *name;
  /** 1..27 for F01..F27. */
  uint8_t family;
  uint8_t classes;
  LN_BatchAllowance batch;
  LN_LeanStatus lean;
  LN_BlockStatus block;
};

/** All kinds, in the order of the domains and enums. */
const LN_KindInfo *LN_KindCatalog(size_t *r_count);
const LN_KindInfo *LN_FindKind(LN_KindDomain domain, uint16_t value);
/** "Domain::Name" of a kind, or "Domain::?N" for a value the catalog does not list. */
const char *LN_KindName(LN_KindDomain domain, uint16_t value);
/** "F05 Game properties (own and other objects)" for 5; "F??" outside 1..27. */
const char *LN_FamilyName(uint8_t family);
const char *LN_KindDomainName(LN_KindDomain domain);
