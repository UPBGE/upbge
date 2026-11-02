/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The recursive reader of the expression kinds (lean runtime plan, phase A13): what a kind
 * function (`LN_RuntimeTree::FloatKind` and the others, intern/LN_RuntimeTreeKinds.inl) reads its
 * inputs through. This reader asks the tree's own Resolve functions, which keep the profiling, the
 * register memo and the recursion, so a kind behaves exactly as when the switch was inside them.
 * The lean runtime's reader (phase A16) gives the same calls from slots instead. */

#include <string>

#include "LN_RuntimeTree.h"

struct LN_TreeReader {
  LN_RuntimeTree &tree;
  const LN_TickContext &context;

  LN_TreeReader(LN_RuntimeTree &tree, const LN_TickContext &context) : tree(tree), context(context)
  {
  }

  bool Bool(const uint32_t index, const bool fallback)
  {
    return tree.ResolveBoolExpression(index, fallback, context);
  }
  int32_t Int(const uint32_t index, const int32_t fallback)
  {
    return tree.ResolveIntExpression(index, fallback, context);
  }
  float Float(const uint32_t index, const float fallback)
  {
    return tree.ResolveFloatExpression(index, fallback, context);
  }
  std::string String(const uint32_t index, const std::string &fallback)
  {
    return tree.ResolveStringExpression(index, fallback, context);
  }
  MT_Vector3 Vector(const uint32_t index, const MT_Vector3 &fallback)
  {
    return tree.ResolveVectorExpression(index, fallback, context);
  }
  LN_Rotation Rotation(const uint32_t index, const LN_Rotation &fallback)
  {
    return tree.ResolveRotationExpression(index, fallback, context);
  }
  MT_Vector4 Color(const uint32_t index, const MT_Vector4 &fallback)
  {
    return tree.ResolveColorExpression(index, fallback, context);
  }
  LN_Value Value(const uint32_t index, const LN_Value &fallback)
  {
    return tree.ResolveValueExpression(index, fallback, context);
  }
  const LN_QueryResult &Query(const uint32_t index)
  {
    return tree.ResolveQueryExpression(index, context);
  }
};
