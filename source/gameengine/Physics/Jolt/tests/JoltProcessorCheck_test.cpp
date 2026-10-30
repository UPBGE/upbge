/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "JoltProcessorCheck.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* CPUID bits (Intel's numbering). */
constexpr uint32_t sse41 = 1u << 19;
constexpr uint32_t sse42 = 1u << 20;
constexpr uint32_t popcnt = 1u << 23;
constexpr uint32_t osxsave = 1u << 27;
constexpr uint32_t avx = 1u << 28;
constexpr uint32_t fma = 1u << 12;
constexpr uint32_t f16c = 1u << 29;
constexpr uint32_t bmi1 = 1u << 3;
constexpr uint32_t avx2 = 1u << 5;
constexpr uint32_t bmi2 = 1u << 8;
constexpr uint32_t lzcnt = 1u << 5;
constexpr uint32_t avx512 = (1u << 16) | (1u << 17) | (1u << 31);

/** Intel Core 4th generation (Haswell, 2013): everything this build of Jolt can use but AVX-512. */
JoltProcessorFeatures Haswell()
{
  JoltProcessorFeatures features;
  features.basic_ecx = sse41 | sse42 | popcnt | osxsave | avx | fma | f16c;
  features.extended_ebx = bmi1 | avx2 | bmi2;
  features.amd_ecx = lzcnt;
  features.xcr0 = 0x7;
  return features;
}

std::vector<std::string> Missing(const JoltProcessorFeatures &features)
{
  const char *names[16];
  const int count = JoltMissingInstructions(features, names, 16);
  return std::vector<std::string>(names, names + std::min(count, 16));
}

/** The instructions this build needs that are in `lacking`, in the check's order. */
std::vector<std::string> NeededOf(const std::vector<std::string> &lacking)
{
  const char *names[16];
  const int count = JoltRequiredInstructions(names, 16);
  std::vector<std::string> needed;
  for (int i = 0; i < std::min(count, 16); i++) {
    if (std::find(lacking.begin(), lacking.end(), names[i]) != lacking.end()) {
      needed.push_back(names[i]);
    }
  }
  return needed;
}

TEST(JoltProcessorCheck, ThisBuildNeedsWhatJoltIsBuiltWith)
{
  const char *names[16];
  const int count = JoltRequiredInstructions(names, 16);
  const std::vector<std::string> required(names, names + std::min(count, 16));
  const auto needs = [&](const char *name) {
    return std::find(required.begin(), required.end(), name) != required.end();
  };
#ifdef JPH_USE_AVX2
  EXPECT_TRUE(needs("AVX2"));
  EXPECT_TRUE(needs("AVX"));
  /* Jolt's AVX2 flags bring these along (-mbmi -mpopcnt -mlzcnt -mf16c). */
  EXPECT_TRUE(needs("BMI1"));
  EXPECT_TRUE(needs("LZCNT"));
  EXPECT_TRUE(needs("F16C"));
  EXPECT_TRUE(needs("POPCNT"));
#endif
#ifdef JPH_USE_FMADD
  EXPECT_TRUE(needs("FMA"));
#endif
#ifndef JPH_USE_AVX512
  EXPECT_FALSE(needs("AVX-512"));
#endif
}

TEST(JoltProcessorCheck, ThisComputerRunsThisBuild)
{
  /* Otherwise the test program would have stopped before main(). */
  EXPECT_TRUE(Missing(JoltReadProcessorFeatures()).empty());
}

TEST(JoltProcessorCheck, Haswell)
{
  EXPECT_TRUE(Missing(Haswell()).empty());
}

TEST(JoltProcessorCheck, SandyBridgeHasAvxButNotAvx2)
{
  /* Intel Core 2nd generation (2011). */
  JoltProcessorFeatures features;
  features.basic_ecx = sse41 | sse42 | popcnt | osxsave | avx;
  features.xcr0 = 0x7;
  EXPECT_EQ(Missing(features), NeededOf({"AVX2", "FMA", "F16C", "BMI1", "LZCNT"}));
}

TEST(JoltProcessorCheck, PentiumWithoutAvx)
{
  /* Pentium and Celeron models sold until about 2020: SSE4.2, no AVX at all. */
  JoltProcessorFeatures features;
  features.basic_ecx = sse41 | sse42 | popcnt | osxsave;
  features.xcr0 = 0x3;
  EXPECT_EQ(Missing(features), NeededOf({"AVX", "AVX2", "FMA", "F16C", "BMI1", "LZCNT"}));
}

TEST(JoltProcessorCheck, AmdPiledriverLacksOnlyAvx2)
{
  /* AMD FX and A-series (2012-2015): FMA and BMI1, no AVX2. */
  JoltProcessorFeatures features = Haswell();
  features.extended_ebx = bmi1;
  EXPECT_EQ(Missing(features), NeededOf({"AVX2"}));
}

TEST(JoltProcessorCheck, AvxTurnedOffByTheSystem)
{
  /* The processor has AVX but the operating system doesn't save its registers (a virtual machine
   * set up so, an old system): every instruction using them would stop the program. */
  JoltProcessorFeatures saves_sse_only = Haswell();
  saves_sse_only.xcr0 = 0x3;
  const std::vector<std::string> avx_family = NeededOf({"AVX", "AVX2", "FMA", "F16C"});
  EXPECT_EQ(Missing(saves_sse_only), avx_family);

  JoltProcessorFeatures no_xgetbv = Haswell();
  no_xgetbv.basic_ecx &= ~osxsave;
  EXPECT_EQ(Missing(no_xgetbv), avx_family);
}

TEST(JoltProcessorCheck, EachNeededInstructionIsChecked)
{
  /* Every feature bit taken away on its own is reported, if this build uses it. */
  struct Bit {
    const char *name;
    uint32_t JoltProcessorFeatures::*bits;
    uint32_t mask;
  };
  const Bit bits[] = {
      {"SSE4.1", &JoltProcessorFeatures::basic_ecx, sse41},
      {"SSE4.2", &JoltProcessorFeatures::basic_ecx, sse42},
      {"POPCNT", &JoltProcessorFeatures::basic_ecx, popcnt},
      {"AVX", &JoltProcessorFeatures::basic_ecx, avx},
      {"FMA", &JoltProcessorFeatures::basic_ecx, fma},
      {"F16C", &JoltProcessorFeatures::basic_ecx, f16c},
      {"BMI1", &JoltProcessorFeatures::extended_ebx, bmi1},
      {"AVX2", &JoltProcessorFeatures::extended_ebx, avx2},
      {"LZCNT", &JoltProcessorFeatures::amd_ecx, lzcnt},
  };
  for (const Bit &bit : bits) {
    JoltProcessorFeatures features = Haswell();
    features.extended_ebx |= avx512;
    features.xcr0 = 0xe7;
    features.*bit.bits &= ~bit.mask;
    EXPECT_EQ(Missing(features), NeededOf({bit.name})) << bit.name;
  }
}

TEST(JoltProcessorCheck, CountsBeyondTheGivenRoom)
{
  const char *names[2] = {nullptr, nullptr};
  const JoltProcessorFeatures nothing;
  const int needed = JoltRequiredInstructions(nullptr, 0);
  EXPECT_EQ(JoltMissingInstructions(nothing, names, 1), needed);
  if (needed > 0) {
    EXPECT_NE(names[0], nullptr);
  }
  EXPECT_EQ(names[1], nullptr);
}

}  // namespace
