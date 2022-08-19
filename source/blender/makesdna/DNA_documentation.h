/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup DNA
 * \page makesdna makesdna
 *
 * \section aboutdna About the DNA module
 *
 * The DNA module holds all type definitions that are serialized in a
 * blender file. There is an executable that scans all files, looking
 * for struct-s to serialize (hence sdna: Struct \ref DNA). From this
 * information, it builds a file with numbers that encode the format,
 * the names of variables, and the place to look for them.
 *
 * \section dnaissues Known issues with DNA
 *
 * - Function pointers:
 *
 *   Because of historical reasons, some function pointers were
 *   untyped. The parser/dna generator has been modified to explicitly
 *   handle these special cases. Most pointers have been given proper
 *   proto's by now. DNA_space_types.h::Spacefile::returnfunc may still
 *   be badly defined. The reason for this is that it is called with
 *   different types of arguments. It takes a char* at this moment...
 *
 * - Ignoring structs:
 *
 *   Sometimes we need to define structs in DNA which aren't written
 *   to disk, and can be excluded from blend file DNA string.
 *   in this case, add two '#' chars directly before the struct. eg.
 *
 *   \code{.c}
 *   #
 *   #
 *   typedef struct MyStruct {
 *       int value;
 *   } MyStruct;
 *   \endcode
 *
 *   Ignored structs can only be referred to from non-ignored structs
 *   when referred to as a pointer (where they're usually allocated
 *   and cleared in `readfile.c`).
 *
 * - %Path to the header files
 *
 *   Also because of historical reasons, there is a path prefix to the
 *   headers that need to be scanned. This is the BASE_HEADER
 *   define. If you change the file-layout for DNA, you will probably
 *   have to change this (Not very flexible, but it is hardly ever
 *   changed. Sorry.).
 *
 * \section dnadependencies Dependencies
 *
 * DNA has no external dependencies (except for a few system
 * includes).
 *
 * \section dnanote NOTE
 *
 * PLEASE READ INSTRUCTIONS ABOUT ADDING VARIABLES IN 'DNA' STRUCTS IN
 *
 * intern/dna_genfile.c
 * (ton)
 */

/* This file has intentionally no definitions or implementation. */
