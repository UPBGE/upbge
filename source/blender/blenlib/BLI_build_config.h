/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Based on Chromium's `build_config.h`, governed by a BSD-style license,
 * with tweaks and extensions needed for the Blender project.
 */

/**
 * Compile-time detection of compiler and hardware platform configuration.
 * There are few categories of the defined symbols this header provides.
 *
 *
 * Operating system detection
 * ==========================
 *
 * An operating system is detected is defined as an `OS_<NAME>` symbols. For example, on Windows
 * the `OS_WINDOWS` is defined to 1, and all the other symbols prefixed with `OS_` are defined to 0
 * (except of the aggregates described above).
 *
 * There are aggregates which allows to access "family" of the operating system:
 *
 * - OS_BSD is defined for 1 for all BSD family of OS (FreeBSD, NextBSD, DragonFly...).
 * - OS_POSIX is defined to 1 if the OS implements POSIX API.
 *
 *
 * Compiler detection
 * ==================
 *
 * The following compilers are detected: CLANG, GCC, MSVC, MINGW32, MINGW64.
 *
 * The COMPILER_<family> for the detected compiler is defined to 1, and all the rest of the
 * compiler defines are set to 0.
 *
 * The aggregate COMPILER_MINGW is defined when the compiler is wither MINGW32 or MINGW64.
 *
 *
 * CPU detection
 * =============
 *
 * The commonly detected CPU capabilities are:
 * - Family: `ARCH_CPU_<FAMILY>_FAMILY`
 * - CPU bitness: `ARCH_CPU_<32|64>_BITS`
 * - Endianness: `ARCH_CPU_<LITTLE|BIG>_ENDIAN`
 *
 * Supported CPU families: X86, S390, PPC, ARM, MIPS.
 */

#pragma once

/**
 * All commonly used symbols (which are checked on a "top" level, from outside of any
 * platform-specific `ifdef` block) are to be explicitly defined to 0 when they are not "active".
 * Such an approach helps catching cases when one is attempted to access build configuration
 * variable without including the header by simply using the `-Wundef` compiler attribute.
 */

/* -------------------------------------------------------------------- */
/** \name A set of macros to use for platform detection.
 * \{ */

#if defined(__native_client__)
/* __native_client__ must be first, so that other OS_ defines are not set. */
#  define OS_NACL 1
/* OS_NACL comes in two sand-boxing technology flavors, SFI or Non-SFI. PNaCl toolchain defines
 * __native_client_nonsfi__ macro in Non-SFI build mode, while it does not in SFI build mode. */
#  if defined(__native_client_nonsfi__)
#    define OS_NACL_NONSFI
#  else
#    define OS_NACL_SFI
#  endif
#elif defined(_AIX)
#  define OS_AIX 1
#elif defined(ANDROID)
#  define OS_ANDROID 1
#elif defined(__APPLE__)
/* Only include TargetConditions after testing ANDROID as some android builds on mac don't have
 * this header available and it's not needed unless the target is really mac/IOS. */
#  include <TargetConditionals.h>
#  define OS_MAC 1
#  if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#    define OS_IOS 1
#  endif
#elif defined(__HAIKU__)
#  define OS_HAIKU 1
#elif defined(__hpux)
#  define OS_HPUX 1
#elif defined(__linux__)
#  define OS_LINUX 1
/* Include a system header to pull in features.h for GLIBC/UCLIBC macros. */
#  include <unistd.h>
#  if defined(__GLIBC__) && !defined(__UCLIBC__)
/* We really are using GLIBC, not UCLIBC pretending to be GLIBC. */
#    define LIBC_GLIBC 1
#  endif
#elif defined(__sgi)
#  define OS_IRIX 1
#elif defined(_WIN32)
#  define OS_WINDOWS 1
#elif defined(__Fuchsia__)
#  define OS_FUCHSIA 1
#elif defined(__DragonFly__)
#  define OS_DRAGONFLYBSD 1
#elif defined(__FreeBSD__)
#  define OS_FREEBSD 1
#elif defined(__NetBSD__)
#  define OS_NETBSD 1
#elif defined(__OpenBSD__)
#  define OS_OPENBSD 1
#elif defined(__sun)
#  define OS_SOLARIS 1
#elif defined(__QNXNTO__)
#  define OS_QNX 1
#elif defined(__asmjs__) || defined(__wasm__)
#  define OS_ASMJS 1
#elif defined(__MVS__)
#  define OS_ZOS 1
#else
#  error Please add support for your platform in BLI_build_config.h
#endif

#if !defined(OS_AIX)
#  define OS_AIX 0
#endif
#if !defined(OS_ASMJS)
#  define OS_ASMJS 0
#endif
#if !defined(OS_NACL)
#  define OS_NACL 0
#endif
#if !defined(OS_NACL_NONSFI)
#  define OS_NACL_NONSFI 0
#endif
#if !defined(OS_NACL_SFI)
#  define OS_NACL_SFI 0
#endif
#if !defined(OS_ANDROID)
#  define OS_ANDROID 0
#endif
#if !defined(OS_MAC)
#  define OS_MAC 0
#endif
#if !defined(OS_IOS)
#  define OS_IOS 0
#endif
#if !defined(OS_HAIKU)
#  define OS_HAIKU 0
#endif
#if !defined(OS_HPUX)
#  define OS_HPUX 0
#endif
#if !defined(OS_IRIX)
#  define OS_IRIX 0
#endif
#if !defined(OS_LINUX)
#  define OS_LINUX 0
#endif
#if !defined(LIBC_GLIBC)
#  define LIBC_GLIBC 0
#endif
#if !defined(OS_WINDOWS)
#  define OS_WINDOWS 0
#endif
#if !defined(OS_FUCHSIA)
#  define OS_FUCHSIA 0
#endif
#if !defined(OS_DRAGONFLYBSD)
#  define OS_DRAGONFLYBSD 0
#endif
#if !defined(OS_FREEBSD)
#  define OS_FREEBSD 0
#endif
#if !defined(OS_NETBSD)
#  define OS_NETBSD 0
#endif
#if !defined(OS_OPENBSD)
#  define OS_OPENBSD 0
#endif
#if !defined(OS_SOLARIS)
#  define OS_SOLARIS 0
#endif
#if !defined(OS_QNX)
#  define OS_QNX 0
#endif
#if !defined(OS_ZOS)
#  define OS_ZOS 0
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name *BSD OS family detection.
 * For access to standard BSD features, use OS_BSD instead of a more specific macro.
 * \{ */

#if OS_DRAGONFLYBSD || OS_FREEBSD || OS_OPENBSD || OS_NETBSD
#  define OS_BSD 1
#else
#  define OS_BSD 0
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name POSIX system detection.
 * For access to standard POSIXish features use OS_POSIX instead of a more specific macro.
 * \{ */

#if OS_AIX || OS_ANDROID || OS_ASMJS || OS_FREEBSD || OS_LINUX || OS_MAC || OS_NACL || \
    OS_NETBSD || OS_OPENBSD || OS_QNX || OS_SOLARIS
#  define OS_POSIX 1
#else
#  define OS_POSIX 0
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compiler detection, including its capabilities.
 * \{ */

#if defined(__clang__)
#  define COMPILER_CLANG 1
#elif defined(__GNUC__)
#  define COMPILER_GCC 1
#  define COMPILER_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#elif defined(_MSC_VER)
#  define COMPILER_MSVC 1
#  define COMPILER_MSVC_VERSION (_MSC_VER)
#elif defined(__MINGW32__)
#  define COMPILER_MINGW32 1
#elif defined(__MINGW64__)
#  define COMPILER_MINGW64 1
#else
#  error Please add support for your compiler in BLI_build_config.h
#endif

#if !defined(COMPILER_CLANG)
#  define COMPILER_CLANG 0
#endif
#if !defined(COMPILER_GCC)
#  define COMPILER_GCC 0
#endif
#if !defined(COMPILER_MSVC)
#  define COMPILER_MSVC 0
#endif
#if !defined(COMPILER_MINGW32)
#  define COMPILER_MINGW32 0
#endif
#if !defined(COMPILER_MINGW64)
#  define COMPILER_MINGW64 0
#endif

/* Compiler is any of MinGW family. */
#if COMPILER_MINGW32 || COMPILER_MINGW64
#  define COMPILER_MINGW 1
#else
#  define COMPILER_MINGW 0
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Processor architecture detection.
 * For more info on what's defined, see:
 *
 *   http://msdn.microsoft.com/en-us/library/b0084kay.aspx
 *   http://www.agner.org/optimize/calling_conventions.pdf
 *
 *   or with GCC, run: `echo | gcc -E -dM -`
 * \{ */

#if defined(_M_X64) || defined(__x86_64__)
#  define ARCH_CPU_X86_FAMILY 1
#  define ARCH_CPU_X86_64 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(_M_IX86) || defined(__i386__)
#  define ARCH_CPU_X86_FAMILY 1
#  define ARCH_CPU_X86 1
#  define ARCH_CPU_32_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__s390x__)
#  define ARCH_CPU_S390_FAMILY 1
#  define ARCH_CPU_S390X 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_BIG_ENDIAN 1
#elif defined(__s390__)
#  define ARCH_CPU_S390_FAMILY 1
#  define ARCH_CPU_S390 1
#  define ARCH_CPU_31_BITS 1
#  define ARCH_CPU_BIG_ENDIAN 1
#elif (defined(__PPC64__) || defined(__PPC__)) && defined(__BIG_ENDIAN__)
#  define ARCH_CPU_PPC64_FAMILY 1
#  define ARCH_CPU_PPC64 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_BIG_ENDIAN 1
#elif defined(__PPC64__)
#  define ARCH_CPU_PPC64_FAMILY 1
#  define ARCH_CPU_PPC64 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__ARMEL__)
#  define ARCH_CPU_ARM_FAMILY 1
#  define ARCH_CPU_ARMEL 1
#  define ARCH_CPU_32_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define ARCH_CPU_ARM_FAMILY 1
#  define ARCH_CPU_ARM64 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__pnacl__) || defined(__asmjs__) || defined(__wasm__)
#  define ARCH_CPU_32_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#elif defined(__MIPSEL__)
#  if defined(__LP64__)
#    define ARCH_CPU_MIPS_FAMILY 1
#    define ARCH_CPU_MIPS64EL 1
#    define ARCH_CPU_64_BITS 1
#    define ARCH_CPU_LITTLE_ENDIAN 1
#  else
#    define ARCH_CPU_MIPS_FAMILY 1
#    define ARCH_CPU_MIPSEL 1
#    define ARCH_CPU_32_BITS 1
#    define ARCH_CPU_LITTLE_ENDIAN 1
#  endif
#elif defined(__MIPSEB__)
#  if defined(__LP64__)
#    define ARCH_CPU_MIPS_FAMILY 1
#    define ARCH_CPU_MIPS64 1
#    define ARCH_CPU_64_BITS 1
#    define ARCH_CPU_BIG_ENDIAN 1
#  else
#    define ARCH_CPU_MIPS_FAMILY 1
#    define ARCH_CPU_MIPS 1
#    define ARCH_CPU_32_BITS 1
#    define ARCH_CPU_BIG_ENDIAN 1
#  endif
#elif defined(__riscv)
#  define ARCH_CPU_RISCV_FAMILY 1
#  if defined(__LP128__)
#    define ARCH_CPU_RISCV128 1
#    define ARCH_CPU_128_BITS 1
#  elif defined(__LP64__)
#    define ARCH_CPU_RISCV64 1
#    define ARCH_CPU_64_BITS 1
#  else
#    define ARCH_CPU_RISCV32 1
#    define ARCH_CPU_32_BITS 1
#  endif
#  if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#    define ARCH_CPU_LITTLE_ENDIAN 1
#  else
#    define ARCH_CPU_BIG_ENDIAN 1
#  endif
#elif defined(__loongarch_lp64)
#  define ARCH_CPU_LOONG_FAMILY 1
#  define ARCH_CPU_LOONG64 1
#  define ARCH_CPU_64_BITS 1
#  define ARCH_CPU_LITTLE_ENDIAN 1
#else
#  error Please add support for your architecture in BLI_build_config.h
#endif

#if !defined(ARCH_CPU_LITTLE_ENDIAN)
#  define ARCH_CPU_LITTLE_ENDIAN 0
#endif
#if !defined(ARCH_CPU_BIG_ENDIAN)
#  define ARCH_CPU_BIG_ENDIAN 0
#endif

#if !defined(ARCH_CPU_31_BITS)
#  define ARCH_CPU_31_BITS 0
#endif
#if !defined(ARCH_CPU_32_BITS)
#  define ARCH_CPU_32_BITS 0
#endif
#if !defined(ARCH_CPU_64_BITS)
#  define ARCH_CPU_64_BITS 0
#endif
#if !defined(ARCH_CPU_128_BITS)
#  define ARCH_CPU_128_BITS 0
#endif

#if !defined(ARCH_CPU_X86_FAMILY)
#  define ARCH_CPU_X86_FAMILY 0
#endif
#if !defined(ARCH_CPU_ARM_FAMILY)
#  define ARCH_CPU_ARM_FAMILY 0
#endif
#if !defined(ARCH_CPU_MIPS_FAMILY)
#  define ARCH_CPU_MIPS_FAMILY 0
#endif
#if !defined(ARCH_CPU_PPC64_FAMILY)
#  define ARCH_CPU_PPC64_FAMILY 0
#endif
#if !defined(ARCH_CPU_S390_FAMILY)
#  define ARCH_CPU_S390_FAMILY 0
#endif
#if !defined(ARCH_CPU_RISCV_FAMILY)
#  define ARCH_CPU_RISCV_FAMILY 0
#endif
#if !defined(ARCH_CPU_LOONG_FAMILY)
#  define ARCH_CPU_LOONG_FAMILY 0
#endif

#if !defined(ARCH_CPU_ARM64)
#  define ARCH_CPU_ARM64 0
#endif
#if !defined(ARCH_CPU_ARMEL)
#  define ARCH_CPU_ARMEL 0
#endif
#if !defined(ARCH_CPU_MIPS)
#  define ARCH_CPU_MIPS 0
#endif
#if !defined(ARCH_CPU_MIPS64)
#  define ARCH_CPU_MIPS64 0
#endif
#if !defined(ARCH_CPU_MIPS64EL)
#  define ARCH_CPU_MIPS64EL 0
#endif
#if !defined(ARCH_CPU_MIPSEL)
#  define ARCH_CPU_MIPSEL 0
#endif
#if !defined(ARCH_CPU_PPC64)
#  define ARCH_CPU_PPC64 0
#endif
#if !defined(ARCH_CPU_S390)
#  define ARCH_CPU_S390 0
#endif
#if !defined(ARCH_CPU_S390X)
#  define ARCH_CPU_S390X 0
#endif
#if !defined(ARCH_CPU_X86)
#  define ARCH_CPU_X86 0
#endif
#if !defined(ARCH_CPU_X86_64)
#  define ARCH_CPU_X86_64 0
#endif
#if !defined(ARCH_CPU_RISCV32)
#  define ARCH_CPU_RISCV32 0
#endif
#if !defined(ARCH_CPU_RISCV64)
#  define ARCH_CPU_RISCV64 0
#endif
#if !defined(ARCH_CPU_RISCV128)
#  define ARCH_CPU_RISCV128 0
#endif
#if !defined(ARCH_CPU_LOONG64)
#  define ARCH_CPU_LOONG64 0
#endif

/** \} */
