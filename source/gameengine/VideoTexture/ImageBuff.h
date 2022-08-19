/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file ImageBuff.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "Common.h"
#include "ImageBase.h"

struct ImBuf;

/// class for image buffer
class ImageBuff : public ImageBase {
 private:
  struct ImBuf *m_imbuf;  // temporary structure for buffer manipulation
 public:
  /// constructor
  ImageBuff(void) : ImageBase(true), m_imbuf(nullptr)
  {
  }

  /// destructor
  virtual ~ImageBuff(void);

  /// load image from buffer
  void load(unsigned char *img, short width, short height);
  /// clear image with color set on RGB channels and 0xFF on alpha channel
  void clear(short width, short height, unsigned char color);

  /// plot image from extern RGBA buffer to image at position x,y using one of IMB_BlendMode
  void plot(unsigned char *img, short width, short height, short x, short y, short mode);
  /// plot image from other ImageBuf to image at position x,y using one of IMB_BlendMode
  void plot(ImageBuff *img, short x, short y, short mode);

  /// refresh image - do nothing
  virtual void refresh(void)
  {
  }
};
