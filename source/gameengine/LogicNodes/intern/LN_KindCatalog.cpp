/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "LN_KindCatalog.h"

#include <array>
#include <string>
#include <vector>

#include "LN_Types.h"

namespace {

const LN_KindInfo kCatalog[] = {
#include "LN_KindCatalogData.inc"
};

constexpr size_t kCatalogSize = sizeof(kCatalog) / sizeof(kCatalog[0]);

const char *const kFamilyNames[] = {
    "F??",
    "F01 Constants, scalar math, compare, logic",
    "F02 Vector / rotation / color / matrix math",
    "F03 Strings",
    "F04 Generic values, lists, dicts (LN_Value)",
    "F05 Game properties (own and other objects)",
    "F06 Tree properties and global properties",
    "F07 Object references and object state reads",
    "F08 Transforms and object look writes",
    "F09 Physics bodies, character, vehicle",
    "F10 Physics constraints and constraint-break events",
    "F11 Queries: distance and average",
    "F12 Queries: rays, shape casts, mouse/camera rays",
    "F13 Collision sensors",
    "F14 Flow and latent (branch, gates, timers, tween, loops, motion)",
    "F15 Time and game control",
    "F16 Events",
    "F17 Sound",
    "F18 Animation and armature",
    "F19 Materials, node trees, modifiers, mesh, lights",
    "F20 UI and menus",
    "F21 Inventory and items",
    "F22 Save/load, variables, files",
    "F23 Scene, object lifecycle, parenting, collections, tree control",
    "F24 Input, input map, players and look",
    "F25 Debug draw and print",
    "F26 Camera, window, split screen, screen projection",
    "F27 Navigation and curves",
};

/** The catalog's row of each value, per domain (built once; the enums are small). */
struct Index {
  std::array<std::vector<const LN_KindInfo *>, size_t(LN_KindDomain::Count)> by_value;
  Index()
  {
    for (const LN_KindInfo &info : kCatalog) {
      std::vector<const LN_KindInfo *> &rows = by_value[size_t(info.domain)];
      if (rows.size() <= info.value) {
        rows.resize(size_t(info.value) + 1, nullptr);
      }
      rows[info.value] = &info;
    }
  }
};

const Index &index()
{
  static const Index instance;
  return instance;
}

}  // namespace

const LN_KindInfo *LN_KindCatalog(size_t *r_count)
{
  if (r_count != nullptr) {
    *r_count = kCatalogSize;
  }
  return kCatalog;
}

const LN_KindInfo *LN_FindKind(const LN_KindDomain domain, const uint16_t value)
{
  if (domain >= LN_KindDomain::Count) {
    return nullptr;
  }
  const std::vector<const LN_KindInfo *> &rows = index().by_value[size_t(domain)];
  return value < rows.size() ? rows[value] : nullptr;
}

const char *LN_KindName(const LN_KindDomain domain, const uint16_t value)
{
  if (const LN_KindInfo *info = LN_FindKind(domain, value)) {
    return info->name;
  }
  /* Not listed: a name that says what is missing (copied once per distinct unknown value). */
  static thread_local std::string unknown;
  unknown = std::string(LN_KindDomainName(domain)) + "::?" + std::to_string(value);
  return unknown.c_str();
}

const char *LN_FamilyName(const uint8_t family)
{
  return family < sizeof(kFamilyNames) / sizeof(kFamilyNames[0]) ? kFamilyNames[family] :
                                                                    kFamilyNames[0];
}

const char *LN_KindDomainName(const LN_KindDomain domain)
{
  switch (domain) {
    case LN_KindDomain::Op:
      return "Op";
    case LN_KindDomain::Bool:
      return "Bool";
    case LN_KindDomain::Int:
      return "Int";
    case LN_KindDomain::Float:
      return "Float";
    case LN_KindDomain::String:
      return "String";
    case LN_KindDomain::Vector:
      return "Vector";
    case LN_KindDomain::Color:
      return "Color";
    case LN_KindDomain::Value:
      return "Value";
    case LN_KindDomain::Query:
      return "Query";
    case LN_KindDomain::Count:
      break;
  }
  return "?";
}
