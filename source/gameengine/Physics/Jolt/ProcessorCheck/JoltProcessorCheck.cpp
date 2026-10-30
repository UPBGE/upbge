/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file JoltProcessorCheck.cpp
 *  \ingroup physjolt
 *
 * Runs before the program's own start-up code, so it uses nothing that start-up code makes: its
 * table is constant data, its message plain C strings.
 */

#include "JoltProcessorCheck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER) && defined(_M_X64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <immintrin.h>
#  include <intrin.h>
#  include <windows.h>
/* This file's start-up code runs with the libraries' (after the C runtime's), before the
 * program's own: Jolt's constants are made there. */
#  pragma warning(disable : 4073) /* Initializers put in library initialization area. */
#  pragma init_seg(lib)
#elif defined(__x86_64__)
#  include <cpuid.h>
#endif

namespace {

/* The instruction sets Jolt was built with (its JPH_USE_* definitions, given to this library by
 * its CMake file), and the ones they bring along in Jolt's compiler flags (Jolt.cmake). */
#ifdef JPH_USE_AVX512
constexpr bool built_avx512 = true;
#else
constexpr bool built_avx512 = false;
#endif
#ifdef JPH_USE_AVX2
constexpr bool built_avx2 = true;
#else
constexpr bool built_avx2 = false;
#endif
#ifdef JPH_USE_AVX
constexpr bool built_avx = true;
#else
constexpr bool built_avx = false;
#endif
#ifdef JPH_USE_SSE4_2
constexpr bool built_sse42 = true;
#else
constexpr bool built_sse42 = false;
#endif
#ifdef JPH_USE_SSE4_1
constexpr bool built_sse41 = true;
#else
constexpr bool built_sse41 = false;
#endif
#ifdef JPH_USE_LZCNT
constexpr bool built_lzcnt = true;
#else
constexpr bool built_lzcnt = false;
#endif
#ifdef JPH_USE_TZCNT
constexpr bool built_tzcnt = true;
#else
constexpr bool built_tzcnt = false;
#endif
#ifdef JPH_USE_F16C
constexpr bool built_f16c = true;
#else
constexpr bool built_f16c = false;
#endif
#ifdef JPH_USE_FMADD
constexpr bool built_fma = true;
#else
constexpr bool built_fma = false;
#endif

constexpr bool needs_avx2 = built_avx2 || built_avx512;
constexpr bool needs_avx = built_avx || needs_avx2;
constexpr bool needs_sse42 = built_sse42 || needs_avx;
constexpr bool needs_sse41 = built_sse41 || needs_sse42;
/* Jolt's SSE4.2 and AVX flags add -mpopcnt, its AVX2 ones -mbmi, -mlzcnt and -mf16c. */
constexpr bool needs_popcnt = needs_sse42;
constexpr bool needs_bmi1 = built_tzcnt || needs_avx2;
constexpr bool needs_lzcnt = built_lzcnt || needs_avx2;
constexpr bool needs_f16c = built_f16c || needs_avx2;

/** CPUID leaf 1 ECX: the operating system turned on XGETBV (it saves the states it reports). */
constexpr uint32_t osxsave_bit = 1u << 27;
/** XCR0 bits: the SSE and AVX registers; the AVX-512 ones on top. */
constexpr uint64_t avx_state = 0x6;
constexpr uint64_t avx512_state = 0xe6;

struct Instruction {
  const char *name;
  bool needed;
  uint32_t JoltProcessorFeatures::*bits;
  uint32_t mask;
  /** The register states the operating system must save for these instructions (0: none). */
  uint64_t state;
};

constexpr Instruction instructions[] = {
    {"SSE4.1", needs_sse41, &JoltProcessorFeatures::basic_ecx, 1u << 19, 0},
    {"SSE4.2", needs_sse42, &JoltProcessorFeatures::basic_ecx, 1u << 20, 0},
    {"POPCNT", needs_popcnt, &JoltProcessorFeatures::basic_ecx, 1u << 23, 0},
    {"AVX", needs_avx, &JoltProcessorFeatures::basic_ecx, 1u << 28, avx_state},
    {"AVX2", needs_avx2, &JoltProcessorFeatures::extended_ebx, 1u << 5, avx_state},
    {"FMA", built_fma, &JoltProcessorFeatures::basic_ecx, 1u << 12, avx_state},
    {"F16C", needs_f16c, &JoltProcessorFeatures::basic_ecx, 1u << 29, avx_state},
    {"BMI1", needs_bmi1, &JoltProcessorFeatures::extended_ebx, 1u << 3, 0},
    {"LZCNT", needs_lzcnt, &JoltProcessorFeatures::amd_ecx, 1u << 5, 0},
    /* Foundation, DQ and VL, as Jolt's -mavx512f -mavx512dq -mavx512vl. */
    {"AVX-512",
     built_avx512,
     &JoltProcessorFeatures::extended_ebx,
     (1u << 16) | (1u << 17) | (1u << 31),
     avx512_state},
};

#if (defined(_MSC_VER) && defined(_M_X64)) || (!defined(_MSC_VER) && defined(__x86_64__))
#  define JOLT_PROCESSOR_CHECK_X86_64

void cpuid(const uint32_t leaf, const uint32_t subleaf, uint32_t regs[4])
{
#  ifdef _MSC_VER
  int info[4];
  __cpuidex(info, int(leaf), int(subleaf));
  for (int i = 0; i < 4; i++) {
    regs[i] = uint32_t(info[i]);
  }
#  else
  __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#  endif
}

uint64_t xgetbv0()
{
#  ifdef _MSC_VER
  return _xgetbv(0);
#  else
  uint32_t low, high;
  __asm__ __volatile__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
  return (uint64_t(high) << 32) | low;
#  endif
}

/** The processor's own name without the spaces around it ("" when it gives none). */
const char *brand_string()
{
  static char brand[49] = {0};
  uint32_t regs[4];
  cpuid(0x80000000, 0, regs);
  if (regs[0] < 0x80000004) {
    return "";
  }
  for (uint32_t part = 0; part < 3; part++) {
    cpuid(0x80000002 + part, 0, regs);
    memcpy(brand + part * 16, regs, 16);
  }
  for (size_t end = strlen(brand); end > 0 && brand[end - 1] == ' '; end--) {
    brand[end - 1] = '\0';
  }
  const char *name = brand;
  while (*name == ' ') {
    name++;
  }
  return name;
}
#endif

}  // namespace

JoltProcessorFeatures JoltReadProcessorFeatures()
{
  JoltProcessorFeatures features;
#ifdef JOLT_PROCESSOR_CHECK_X86_64
  uint32_t regs[4];
  cpuid(0, 0, regs);
  const uint32_t max_leaf = regs[0];
  if (max_leaf >= 1) {
    cpuid(1, 0, regs);
    features.basic_ecx = regs[2];
  }
  if (max_leaf >= 7) {
    cpuid(7, 0, regs);
    features.extended_ebx = regs[1];
  }
  cpuid(0x80000000, 0, regs);
  if (regs[0] >= 0x80000001) {
    cpuid(0x80000001, 0, regs);
    features.amd_ecx = regs[2];
  }
  if (features.basic_ecx & osxsave_bit) {
    features.xcr0 = xgetbv0();
  }
#endif
  return features;
}

int JoltMissingInstructions(const JoltProcessorFeatures &features,
                            const char **missing,
                            const int capacity)
{
  int count = 0;
  for (const Instruction &instruction : instructions) {
    if (!instruction.needed) {
      continue;
    }
    const bool has_bits = (features.*instruction.bits & instruction.mask) == instruction.mask;
    const bool has_state = instruction.state == 0 ||
                           ((features.basic_ecx & osxsave_bit) &&
                            (features.xcr0 & instruction.state) == instruction.state);
    if (has_bits && has_state) {
      continue;
    }
    if (count < capacity) {
      missing[count] = instruction.name;
    }
    count++;
  }
  return count;
}

int JoltRequiredInstructions(const char **names, const int capacity)
{
  int count = 0;
  for (const Instruction &instruction : instructions) {
    if (instruction.needed) {
      if (count < capacity) {
        names[count] = instruction.name;
      }
      count++;
    }
  }
  return count;
}

void JoltStopOnUnsupportedProcessor()
{
#ifdef JOLT_PROCESSOR_CHECK_X86_64
  constexpr int capacity = int(sizeof(instructions) / sizeof(instructions[0]));
  const char *missing[capacity];
  const int count = JoltMissingInstructions(JoltReadProcessorFeatures(), missing, capacity);
  if (count == 0) {
    return;
  }

  char list[128] = "";
  size_t used = 0;
  for (int i = 0; i < count && used < sizeof(list); i++) {
    used += size_t(snprintf(list + used, sizeof(list) - used, i ? ", %s" : "%s", missing[i]));
  }
  const char *brand = brand_string();
  char message[512];
  snprintf(message,
           sizeof(message),
           "This processor%s%s%s lacks instructions this build of UPBGE needs for its physics "
           "engine: %s.%s",
           *brand ? " (" : "",
           brand,
           *brand ? ")" : "",
           list,
           needs_avx2 ? "\n\nIt needs an Intel Core processor from 2013 on (4th generation or "
                        "newer) or an AMD processor from 2015 on (every Ryzen included). Many "
                        "Pentium and Celeron processors lack these instructions too." :
                        "");
#  ifdef _MSC_VER
  MessageBoxA(nullptr, message, "UPBGE: unsupported processor", MB_OK | MB_ICONERROR);
#  else
  fprintf(stderr, "UPBGE: unsupported processor\n%s\n", message);
  fflush(stderr);
#  endif
  /* Nothing else ran yet that would need ending: no destructors, no exit handlers. */
  std::_Exit(1);
#endif
}

/* Before main(): before Jolt's constants are made with the instructions checked for. */
#if defined(_MSC_VER) && defined(_M_X64)
namespace {
struct ProcessorGate {
  ProcessorGate()
  {
    JoltStopOnUnsupportedProcessor();
  }
};
const ProcessorGate processor_gate;
}  // namespace
#elif defined(__x86_64__) && defined(__ELF__)
/* 101 is the first priority programs may use; constructors without one (Jolt's) run after. */
__attribute__((constructor(101))) static void jolt_processor_gate()
{
  JoltStopOnUnsupportedProcessor();
}
#endif
