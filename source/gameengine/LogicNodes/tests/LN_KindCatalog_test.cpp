/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests of the kind catalog (lean runtime plan, phase A2): it lists every opcode, expression kind
 * and query kind once, under the name the enum gives it, and what it says about batching is what
 * the tree batch lists really do. A new kind added to an enum without a row fails here. */

#include "testing/testing.h"

#include <array>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "LN_KindCatalog.h"
#include "LN_TreeBatch.h"
#include "LN_Types.h"

namespace {

struct EnumSource {
  LN_KindDomain domain;
  const char *enum_name;
};

constexpr EnumSource kEnums[] = {{LN_KindDomain::Op, "LN_OpCode"},
                                 {LN_KindDomain::Bool, "LN_BoolExpressionKind"},
                                 {LN_KindDomain::Int, "LN_IntExpressionKind"},
                                 {LN_KindDomain::Float, "LN_FloatExpressionKind"},
                                 {LN_KindDomain::String, "LN_StringExpressionKind"},
                                 {LN_KindDomain::Vector, "LN_VectorExpressionKind"},
                                 {LN_KindDomain::Color, "LN_ColorExpressionKind"},
                                 {LN_KindDomain::Value, "LN_ValueExpressionKind"},
                                 {LN_KindDomain::Query, "LN_QueryExpressionKind"}};

/** The enumerators of an enum of LN_Types.h, in order, without the Count sentinel. */
std::vector<std::string> EnumeratorsOf(const std::string &source, const std::string &enum_name)
{
  const std::regex enum_start("enum class " + enum_name + " : uint8_t \\{");
  std::smatch start;
  if (!std::regex_search(source, start, enum_start)) {
    return {};
  }
  const size_t begin = size_t(start.position(0) + start.length(0));
  const size_t end = source.find("\n};", begin);
  std::string body = source.substr(begin, end - begin);
  body = std::regex_replace(body, std::regex("/\\*[^*]*(\\*+[^/*][^*]*)*\\*+/"), "");
  body = std::regex_replace(body, std::regex("//[^\n]*"), "");
  std::vector<std::string> names;
  std::stringstream stream(body);
  std::string item;
  while (std::getline(stream, item, ',')) {
    std::smatch name;
    if (std::regex_search(item, name, std::regex("([A-Za-z_][A-Za-z0-9_]*)\\s*(=[^,]*)?\\s*$"))) {
      names.push_back(name[1]);
    }
  }
  if (!names.empty() && names.back() == "Count") {
    names.pop_back();
  }
  return names;
}

std::string TypesHeader()
{
  std::ifstream file(std::string(LOGIC_NODES_TEST_SOURCE_DIR) +
                     "/source/gameengine/LogicNodes/LN_Types.h");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

/* The table has a row for every enumerator, in order, named after it: add a kind to an enum and
 * this fails until the catalog (and its generator's data) has it, so no kind goes unclassified. */
TEST(LN_KindCatalog, EveryKindOfTheEnumsIsListedOnce)
{
  const std::string source = TypesHeader();
  ASSERT_FALSE(source.empty()) << "LN_Types.h was not found under LOGIC_NODES_TEST_SOURCE_DIR";
  size_t total = 0;
  for (const EnumSource &entry : kEnums) {
    SCOPED_TRACE(entry.enum_name);
    const std::vector<std::string> names = EnumeratorsOf(source, entry.enum_name);
    ASSERT_FALSE(names.empty());
    for (size_t value = 0; value < names.size(); value++) {
      const LN_KindInfo *info = LN_FindKind(entry.domain, uint16_t(value));
      ASSERT_NE(info, nullptr) << names[value] << " (" << value << ") has no row";
      EXPECT_EQ(std::string(info->name),
                std::string(LN_KindDomainName(entry.domain)) + "::" + names[value]);
      EXPECT_EQ(info->domain, entry.domain);
      EXPECT_EQ(info->value, value);
    }
    EXPECT_EQ(LN_FindKind(entry.domain, uint16_t(names.size())), nullptr)
        << "a row past the last enumerator";
    total += names.size();
  }
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(count, total) << "the table has rows that no enumerator has, or twice";
  /* 619 before plan phase A12 deleted the 13 dead kinds; 609 since Random Value's Boolean, Set
   * Random Seed and its Seed output (2026-11-07); 610 since Bool::ObjectGamePropertyExists
   * (2026-11-08); 611 since Op::EditGameProperty (the list and dictionary nodes' Game Property
   * mode, 2026-11-08). */
  EXPECT_EQ(count, 611u);
  /* Each (domain, value) pair appears once. */
  std::set<std::pair<int, int>> seen;
  std::set<std::string> names_seen;
  for (size_t index = 0; index < count; index++) {
    EXPECT_TRUE(seen.insert({int(rows[index].domain), int(rows[index].value)}).second);
    EXPECT_TRUE(names_seen.insert(rows[index].name).second) << rows[index].name;
  }
}

TEST(LN_KindCatalog, EveryKindHasAFamilyAndTheClassesAgree)
{
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  std::array<size_t, 28> per_family = {};
  size_t dead = 0;
  for (size_t index = 0; index < count; index++) {
    const LN_KindInfo &info = rows[index];
    SCOPED_TRACE(info.name);
    ASSERT_GE(info.family, 1);
    ASSERT_LE(info.family, 27);
    per_family[info.family]++;
    EXPECT_STRNE(LN_FamilyName(info.family), "F??");
    /* A pure kind is nothing else; an expression is pure, context dependent, mutable or
     * stateful (an op is none of the expression classes). */
    if ((info.classes & LN_KIND_PURE) != 0) {
      EXPECT_EQ(info.classes & ~LN_KIND_USES_FALLBACK, uint8_t(LN_KIND_PURE));
    }
    if (info.domain == LN_KindDomain::Op) {
      EXPECT_EQ(info.classes & LN_KIND_PURE, 0);
    }
    else {
      EXPECT_NE(info.classes, 0);
    }
    dead += info.lean == LN_LeanStatus::Dead ? 1 : 0;
    EXPECT_EQ(info.block, LN_BlockStatus::None) << "no block form exists yet";
  }
  for (size_t family = 1; family <= 27; family++) {
    EXPECT_GT(per_family[family], 0u) << LN_FamilyName(uint8_t(family));
  }
  EXPECT_EQ(dead, 0u); /* the 13 kinds the compiler never produced were deleted in plan phase A12
                        * (the 10 snapshot transform vectors, Value SnapshotGameProperty and
                        * Select, Bool Timer) */
  EXPECT_STREQ(LN_KindName(LN_KindDomain::Float, uint16_t(LN_FloatExpressionKind::Add)), "Float::Add");
  EXPECT_STREQ(LN_KindName(LN_KindDomain::Float, 60000), "Float::?60000");
  /* The kinds the plan's caching rule names as never cacheable are not pure. */
  for (const char *name : {"Float::UnscaledDeltaTime", "Int::LoopIndex", "Value::LoopCurrentValue",
                           "Float::StepTimePassed", "Bool::InstructionExecuted"})
  {
    bool found = false;
    for (size_t index = 0; index < count; index++) {
      if (std::string(rows[index].name) == name) {
        found = true;
        EXPECT_EQ(rows[index].classes & LN_KIND_PURE, 0) << name;
      }
    }
    EXPECT_TRUE(found) << name;
  }
}

/* What the catalog says about tree batches is what LN_TreeBatch.cpp's refusal functions say: a
 * kind marked Yes is accepted and a kind marked No is refused, judged by the kind alone; the
 * kinds marked Conditional (a move of the owner only ...) depend on more than the kind. */
TEST(LN_KindCatalog, TheBatchFlagsAreWhatTheTreeBatchRefusalsSay)
{
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  size_t yes = 0, no = 0, conditional = 0;
  for (size_t index = 0; index < count; index++) {
    const LN_KindInfo &info = rows[index];
    const char *refusal = LN_BatchKindRefusalForTests(uint8_t(info.domain), info.value);
    switch (info.batch) {
      case LN_BatchAllowance::Yes:
        yes++;
        EXPECT_EQ(refusal, nullptr) << info.name << " is marked allowed but refused: " << refusal;
        break;
      case LN_BatchAllowance::No:
        no++;
        EXPECT_NE(refusal, nullptr) << info.name << " is marked refused but accepted";
        break;
      case LN_BatchAllowance::Conditional:
        conditional++;
        break;
    }
  }
  EXPECT_GT(yes, 100u);
  EXPECT_GT(no, 100u);
  EXPECT_EQ(conditional, 24u); /* 22 ops, Get Distance and Get Object Attribute */
  EXPECT_EQ(yes + no + conditional, count);
}

}  // namespace
