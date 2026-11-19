/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* String Operation's Substring, Length, Find, Split and Format Time (2026-11-17, LN_TextOps.h):
 * the rules against answers written here (characters, not bytes: "Über" is 4 long), then every
 * operation as a node reading game properties, in every mode and in a wide run. */

#include "testing/testing.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "LN_SemRig.h"
#include "LN_TextOps.h"

using namespace ln_test;
using namespace ln_test::sem;

TEST(LN_TextOps, LengthAndSubstringCountCharacters)
{
  EXPECT_EQ(LN_TextLength(""), 0);
  EXPECT_EQ(LN_TextLength("Hello"), 5);
  EXPECT_EQ(LN_TextLength("Über"), 4);
  EXPECT_EQ(LN_TextLength("日本語"), 3);
  EXPECT_EQ(LN_TextLength("a😀b"), 3);

  EXPECT_EQ(LN_TextSubstring("Hello", 0, 2), "He");
  EXPECT_EQ(LN_TextSubstring("Hello", 1, 3), "ell");
  EXPECT_EQ(LN_TextSubstring("Hello", 3, 99), "lo");
  EXPECT_EQ(LN_TextSubstring("Hello", 2, -1), "llo") << "a negative Length takes the rest";
  EXPECT_EQ(LN_TextSubstring("Hello", -2, 2), "lo") << "a negative Start counts from the end";
  EXPECT_EQ(LN_TextSubstring("Hello", -1, -1), "o");
  EXPECT_EQ(LN_TextSubstring("Hello", -9, 2), "He") << "before the start: from the start";
  EXPECT_EQ(LN_TextSubstring("Hello", 7, 3), "") << "past the end: nothing";
  EXPECT_EQ(LN_TextSubstring("Hello", 2, 0), "") << "Length 0 takes nothing (typewriter at 0)";
  EXPECT_EQ(LN_TextSubstring("Über", 0, 2), "Üb");
  EXPECT_EQ(LN_TextSubstring("日本語", 1, 1), "本");
  EXPECT_EQ(LN_TextSubstring("a😀b", 1, 1), "😀");
  EXPECT_EQ(LN_TextSubstring("Hello", std::numeric_limits<int32_t>::max(), 5), "");
  EXPECT_EQ(LN_TextSubstring("Hello", 1, std::numeric_limits<int32_t>::max()), "ello");
  EXPECT_EQ(LN_TextSubstring("Hello", std::numeric_limits<int32_t>::min(), 1), "H");
}

TEST(LN_TextOps, FindGivesTheCharacterOrMinusOne)
{
  EXPECT_EQ(LN_TextFind("Hello", "l"), 2);
  EXPECT_EQ(LN_TextFind("Hello", "lo"), 3);
  EXPECT_EQ(LN_TextFind("Hello", "x"), -1);
  EXPECT_EQ(LN_TextFind("Hello", ""), 0);
  EXPECT_EQ(LN_TextFind("", "a"), -1);
  EXPECT_EQ(LN_TextFind("Über alles", "alles"), 5) << "counted in characters, Ü is one";
  EXPECT_EQ(LN_TextFind("日本語", "語"), 2);
  EXPECT_EQ(LN_TextFind("hello", "H"), -1) << "capitals count";
}

TEST(LN_TextOps, SplitAsPythonDoes)
{
  using V = std::vector<std::string>;
  EXPECT_EQ(LN_TextSplit("sword,shield,potion", ","), (V{"sword", "shield", "potion"}));
  EXPECT_EQ(LN_TextSplit("a,,b", ","), (V{"a", "", "b"})) << "an empty part between separators";
  EXPECT_EQ(LN_TextSplit(",a,", ","), (V{"", "a", ""}));
  EXPECT_EQ(LN_TextSplit("", ","), (V{""}));
  EXPECT_EQ(LN_TextSplit("no separator", ","), (V{"no separator"}));
  EXPECT_EQ(LN_TextSplit("a--b--c", "--"), (V{"a", "b", "c"}));
  EXPECT_EQ(LN_TextSplit("  one two\tthree\n", ""), (V{"one", "two", "three"}))
      << "empty: at runs of spaces, tabs and new lines";
  EXPECT_EQ(LN_TextSplit("   ", ""), V{});
  EXPECT_EQ(LN_TextSplit("", ""), V{});
  EXPECT_EQ(LN_TextSplit("Ü;日本", ";"), (V{"Ü", "日本"}));
}

TEST(LN_TextOps, FormatTimeShowsAClock)
{
  constexpr int32_t mss = 0, mmss = 1, hmmss = 2, msscc = 3;
  EXPECT_EQ(LN_FormatTime(0.0f, mss, false), "0:00");
  EXPECT_EQ(LN_FormatTime(75.0f, mss, false), "1:15");
  EXPECT_EQ(LN_FormatTime(75.9f, mss, false), "1:15") << "cut down";
  EXPECT_EQ(LN_FormatTime(75.1f, mss, true), "1:16") << "Round Up";
  EXPECT_EQ(LN_FormatTime(0.01f, mss, true), "0:01") << "a countdown shows 0:01 until 0";
  EXPECT_EQ(LN_FormatTime(0.0f, mss, true), "0:00");
  EXPECT_EQ(LN_FormatTime(4500.0f, mss, false), "75:00") << "minutes as many as there are";
  EXPECT_EQ(LN_FormatTime(75.0f, mmss, false), "01:15");
  EXPECT_EQ(LN_FormatTime(5.0f, mmss, false), "00:05");
  EXPECT_EQ(LN_FormatTime(3675.0f, hmmss, false), "1:01:15");
  EXPECT_EQ(LN_FormatTime(59.0f, hmmss, false), "0:00:59");
  EXPECT_EQ(LN_FormatTime(75.25f, msscc, false), "1:15.25");
  EXPECT_EQ(LN_FormatTime(0.29f, msscc, false), "0:00.29") << "float noise does not cut .29";
  EXPECT_EQ(LN_FormatTime(1.1f, msscc, false), "0:01.10");
  EXPECT_EQ(LN_FormatTime(59.999f, msscc, false), "0:59.99");
  EXPECT_EQ(LN_FormatTime(59.999f, msscc, true), "1:00.00");
  EXPECT_EQ(LN_FormatTime(-75.0f, mss, false), "-1:15") << "overtime";
  EXPECT_EQ(LN_FormatTime(-0.4f, mss, false), "0:00") << "never -0:00";
  EXPECT_EQ(LN_FormatTime(std::numeric_limits<float>::quiet_NaN(), mss, false), "0:00");
  EXPECT_EQ(LN_FormatTime(std::numeric_limits<float>::infinity(), mss, false), "0:00")
      << "infinite: as no number";
  EXPECT_EQ(LN_FormatTime(1.0e9f, hmmss, false), "277777:46:40") << "a huge time (a float holds "
                                                                    "1e9 exactly), no wrap";
  EXPECT_EQ(LN_FormatTime(75.0f, 99, false), "1:15") << "an unknown format: M:SS";
}

namespace {

std::shared_ptr<LN_Program> TextOpsProgram()
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const auto read = [&](const char *idname, const char *property) -> GenNode & {
    GenNode &node = b.Add(idname);
    b.String(node, "Property", property);
    return node;
  };
  GenNode &text = read("LogicNativeGetGamePropertyString", "text");
  GenNode &search = read("LogicNativeGetGamePropertyString", "search");
  GenNode &start = read("LogicNativeGetGamePropertyInt", "start");
  GenNode &length = read("LogicNativeGetGamePropertyInt", "length");
  GenNode &separator = read("LogicNativeGetGamePropertyString", "separator");
  GenNode &seconds = read("LogicNativeGetGamePropertyFloat", "seconds");
  GenNode &round_up = read("LogicNativeGetGamePropertyBool", "round_up");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *idname, const char *property, GenNode &from, const char *pin) {
    GenNode &set = b.Add(idname);
    b.String(set, "Property", property);
    b.Link(from, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  GenNode &substring = b.Add("LogicNativeStringOperation", 10);
  b.Link(text, "Value", substring, "String");
  b.Link(start, "Value", substring, "Start");
  b.Link(length, "Value", substring, "Length");
  write("LogicNativeSetGamePropertyString", "sub", substring, "String");
  GenNode &count = b.Add("LogicNativeStringOperation", 11);
  b.Link(text, "Value", count, "String");
  write("LogicNativeSetGamePropertyInt", "len", count, "Length");
  GenNode &find = b.Add("LogicNativeStringOperation", 12);
  b.Link(text, "Value", find, "String");
  b.Link(search, "Value", find, "Substring");
  write("LogicNativeSetGamePropertyInt", "at", find, "Index");
  GenNode &split = b.Add("LogicNativeStringOperation", 13);
  b.Link(text, "Value", split, "String");
  b.Link(separator, "Value", split, "Separator");
  GenNode &parts = b.Add("LogicNativeListLength");
  b.Link(split, "List", parts, "List");
  write("LogicNativeSetGamePropertyInt", "parts", parts, "Length");
  for (int k = 0; k < 3; k++) {
    GenNode &item = b.Add("LogicNativeListGetItem");
    b.Link(split, "List", item, "List");
    b.Int(item, "Index", k);
    write("LogicNativeSetGamePropertyString", ("part" + std::to_string(k)).c_str(), item, "Value");
  }
  for (int16_t format = 0; format < 4; format++) {
    GenNode &clock = b.Add("LogicNativeStringOperation", 14, format);
    b.Link(seconds, "Value", clock, "Seconds");
    b.Link(round_up, "Value", clock, "Round Up");
    write("LogicNativeSetGamePropertyString",
          ("clock" + std::to_string(format)).c_str(),
          clock,
          "String");
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

struct TextCase {
  const char *text, *search, *separator;
  int32_t start, length;
  float seconds;
  bool round_up;
};

const TextCase kTextCases[] = {
    {"sword,shield,potion", "shield", ",", 6, 6, 75.25f, false},
    {"Über alles", "alles", " ", 0, 2, 0.01f, true},
    {"日本語", "語", "", -1, -1, 3675.0f, false},
    {"", "a", ",", 3, 2, -75.0f, false},
    {"a,,b", "", ",", -9, 2, 59.999f, true},
    {"one two", "x", "", 2, 0, 0.29f, false},
};

}  // namespace

/* The nodes give the rules' answers, in every mode, and a wide run gives the column passes'. */
TEST(LN_TextOps, StringOperationNodesGiveTheRulesAnswers)
{
  const std::shared_ptr<LN_Program> program = TextOpsProgram();
  ASSERT_NE(program, nullptr);
  const uint32_t owners = uint32_t(std::size(kTextCases));
  const auto run = [&](SemRig &rig) {
    for (uint32_t index = 0; index < owners; index++) {
      const TextCase &c = kTextCases[index];
      KX_GameObject &object = rig.Object(index);
      rig.SetString(object, "text", c.text);
      rig.SetString(object, "search", c.search);
      rig.SetString(object, "separator", c.separator);
      rig.SetInt(object, "start", c.start);
      rig.SetInt(object, "length", c.length);
      rig.SetFloat(object, "seconds", c.seconds);
      rig.SetBool(object, "round_up", c.round_up);
      for (const char *name : {"sub", "part0", "part1", "part2", "clock0", "clock1", "clock2",
                               "clock3"})
      {
        rig.SetString(object, name, "unset");
      }
      for (const char *name : {"len", "at", "parts"}) {
        rig.SetInt(object, name, -99);
      }
    }
    rig.Step();
    rig.Step();
    std::vector<std::string> values;
    for (uint32_t index = 0; index < owners; index++) {
      KX_GameObject &object = rig.Object(index);
      for (const char *name : {"sub", "part0", "part1", "part2", "clock0", "clock1", "clock2",
                               "clock3"})
      {
        values.push_back(rig.Text(object, name));
      }
      for (const char *name : {"len", "at", "parts"}) {
        values.push_back(std::to_string(int(rig.Float(object, name))));
      }
    }
    return values;
  };
  std::vector<std::string> reference;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::vector<std::string> got = run(rig);
    for (uint32_t index = 0; index < owners; index++) {
      const TextCase &c = kTextCases[index];
      SCOPED_TRACE(c.text);
      const std::string *v = &got[index * 11];
      const std::vector<std::string> parts = LN_TextSplit(c.text, c.separator);
      EXPECT_EQ(v[0], LN_TextSubstring(c.text, c.start, c.length));
      for (int k = 0; k < 3; k++) {
        /* (A missing item reads as nothing: the empty text.) */
        EXPECT_EQ(v[1 + k], k < int(parts.size()) ? parts[k] : std::string()) << "part " << k;
      }
      for (int format = 0; format < 4; format++) {
        EXPECT_EQ(v[4 + format], LN_FormatTime(c.seconds, format, c.round_up))
            << "format " << format;
      }
      EXPECT_EQ(v[8], std::to_string(LN_TextLength(c.text)));
      EXPECT_EQ(v[9], std::to_string(LN_TextFind(c.text, c.search)));
      EXPECT_EQ(v[10], std::to_string(parts.size()));
    }
    if (reference.empty()) {
      reference = got;
    }
    EXPECT_EQ(got, reference);
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(run(wide), reference) << "a wide run";
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the objects ran wide: " << wide.LastColumnRefusal();
  }
}
