/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup ketsji
 *
 * Runtime-only transform-parenting contracts shared by Ketsji and C++ Logic Nodes.
 */

#pragma once

#include <cstdint>
#include <string>

enum class KX_ParentType : uint8_t {
  Object = 0,
  Bone = 1,
};

struct KX_ParentOptions {
  KX_ParentType type = KX_ParentType::Object;
  bool compound = false;
  std::string bone_name;
};

enum class KX_ParentResult : uint8_t {
  Success = 0,
  InvalidRequest,
  DifferentScene,
  ParentingLoop,
  InvalidTransform,
  ParentIsNotArmature,
  BoneNotFound,
  JoltRequired,
  UnsupportedPhysicsBody,
  CompoundUnavailable,
  PhysicsTransitionFailed,
};

const char *KX_ParentResultMessage(KX_ParentResult result);
