/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Joseph Eagar <joeedh@gmail.com>. */

/** \file
 * \ingroup imbcineon
 *
 * Cineon image file format library routines.
 */

#pragma once

#include "logImageCore.h"

#include <stdlib.h>

int logimage_fseek(LogImageFile *logFile, intptr_t offset, int origin);
int logimage_fwrite(void *buffer, size_t size, unsigned int count, LogImageFile *logFile);
int logimage_fread(void *buffer, size_t size, unsigned int count, LogImageFile *logFile);
int logimage_read_uchar(unsigned char *x, LogImageFile *logFile);
int logimage_read_ushort(unsigned short *x, LogImageFile *logFile);
int logimage_read_uint(unsigned int *x, LogImageFile *logFile);
