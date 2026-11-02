/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The lean runtime's reader (plan phase A16): what the kind functions and the op handlers read
 * their inputs through when a tree runs on the lean VM. In the generic form it works an
 * expression out by calling its kind function with itself as the reader: no per-tick memo, no
 * profiling hook, no taint (the memo never changes a result, so this equals the reference). Later
 * phases put a per-pass value cache and native kernels behind the same calls. */

#include <string>

#include "LN_RuntimeTree.h"

struct LN_LeanReader {
  LN_RuntimeTree &tree;
  const LN_TickContext &context;

  LN_LeanReader(LN_RuntimeTree &tree, const LN_TickContext &context) : tree(tree), context(context)
  {
  }

  bool Bool(const uint32_t index, const bool fallback)
  {
    return tree.LeanBool(*this, index, fallback);
  }
  int32_t Int(const uint32_t index, const int32_t fallback)
  {
    return tree.LeanInt(*this, index, fallback);
  }
  float Float(const uint32_t index, const float fallback)
  {
    return tree.LeanFloat(*this, index, fallback);
  }
  std::string String(const uint32_t index, const std::string &fallback)
  {
    return tree.LeanString(*this, index, fallback);
  }
  MT_Vector3 Vector(const uint32_t index, const MT_Vector3 &fallback)
  {
    return tree.LeanVector(*this, index, fallback);
  }
  LN_Rotation Rotation(const uint32_t index, const LN_Rotation &fallback)
  {
    return tree.LeanRotation(*this, index, fallback);
  }
  MT_Vector4 Color(const uint32_t index, const MT_Vector4 &fallback)
  {
    return tree.LeanColor(*this, index, fallback);
  }
  LN_Value Value(const uint32_t index, const LN_Value &fallback)
  {
    return tree.LeanValue(*this, index, fallback);
  }
  const LN_QueryResult &Query(const uint32_t index)
  {
    return tree.QueryEntry(*this, index);
  }
};
