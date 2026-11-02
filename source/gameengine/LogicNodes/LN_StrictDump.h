/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * Canonical text of Logic Nodes values for the strict dumps of the lean runtime plan (phase A2).
 *
 * "Bit for bit the same" has to be checkable in unit tests: every number that is a float is
 * written as its bit pattern (so -0.0, NaN payloads and 1-ulp differences show), every field of
 * a value or a command is written whether it is set or not, and pointers become stable names
 * (an object's name with the order it first appeared in the dump), never addresses. Two dumps of
 * two runs are equal exactly when the runs recorded the same.
 *
 * Used by LN_CommandBuffer::DumpRecordedForTests, LN_RuntimeTree::DumpStateForTests and the
 * tests; no game code calls it. */

#include <cstdint>
#include <string>
#include <unordered_map>

#include "LN_Types.h"

class KX_GameObject;
class LN_Program;
class LN_RuntimeTree;

/** Names pointers for one dump: "name#n" for an object (n = its order of first appearance, so two
 * objects with one name stay apart), "tree#n" for a runtime tree, "collection:name" for a
 * collection. A null pointer is "-". */
class LN_DumpNames {
 public:
  std::string Object(KX_GameObject *object);
  std::string Tree(const LN_RuntimeTree *tree);
  std::string Collection(const void *collection);

 private:
  std::unordered_map<const void *, uint32_t> m_ordinals;
};

/** 0x%08x of the float's bits. */
std::string LN_DumpFloatBits(float value);
/** 0x%016llx of the double's bits. */
std::string LN_DumpDoubleBits(double value);
std::string LN_DumpQuoted(const std::string &text);
std::string LN_DumpVector3(const MT_Vector3 &value);
std::string LN_DumpVector4(const MT_Vector4 &value);
std::string LN_DumpMatrix3(const MT_Matrix3x3 &value);
/** The quaternion (x y z w) of the rotation. */
std::string LN_DumpRotation(const LN_Rotation &value);
/** kind, slot, generation, shared domain and debug name. */
std::string LN_DumpRuntimeRef(const LN_RuntimeRef &value);
/** Every field of the value, lists and dictionaries recursively (a dictionary in key order).
 * With `resolver`, an object reference also names the object it resolves to. */
std::string LN_DumpValue(const LN_Value &value,
                         LN_DumpNames &names,
                         const LN_RuntimeTree *resolver = nullptr);

/** A compiled program as text, for reading and for golden files (LN_ProgramDump.cpp): its
 * identity, step gates, property references, the instructions of each event with their guards and
 * flags and the exec blocks built from them, and the expression tables. Kinds by name (the kind
 * catalog), floats as bit patterns, a field only when it is not the struct's default. Two
 * compiles of one tree give one text; a compiler change that moves a node shows as a diff. */
std::string LN_DumpProgramText(const LN_Program &program);

