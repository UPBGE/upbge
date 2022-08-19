/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup imbuf
 * \brief Function declarations for filter.c
 */

#pragma once

struct ImBuf;

void imb_filterx(struct ImBuf *ibuf);

void IMB_premultiply_rect(unsigned int *rect, char planes, int w, int h);
void IMB_premultiply_rect_float(float *rect_float, int channels, int w, int h);

void IMB_unpremultiply_rect(unsigned int *rect, char planes, int w, int h);
void IMB_unpremultiply_rect_float(float *rect_float, int channels, int w, int h);

/**
 * Result in ibuf2, scaling should be done correctly.
 */
void imb_onehalf_no_alloc(struct ImBuf *ibuf2, struct ImBuf *ibuf1);
