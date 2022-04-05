/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Benoit Bolsee. */

/** \file VideoTexture/DeckLink.h
 *  \ingroup bgevideotex
 */

#pragma once

#ifdef WITH_GAMEENGINE_DECKLINK

#  include "EXP_PyObjectPlus.h"
#  include <structmember.h>

#  include "DNA_image_types.h"

#  include "DeckLinkAPI.h"

#  include "Exception.h"
#  include "ImageBase.h"

// type DeckLink declaration
struct DeckLink {
  PyObject_HEAD

  // last refresh
  double m_lastClock;
  // decklink card to which we output
  IDeckLinkOutput *mDLOutput;
  IDeckLinkKeyer *mKeyer;
  IDeckLinkMutableVideoFrame *mLeftFrame;
  IDeckLinkMutableVideoFrame *mRightFrame;
  bool mUse3D;
  bool mUseKeying;
  bool mUseExtend;
  bool mKeyingSupported;
  bool mHDKeyingSupported;
  uint8_t mKeyingLevel;
  BMDDisplayMode mDisplayMode;
  short mSize[2];
  uint32_t mFrameSize;

  // image source
  PyImage *m_leftEye;
  PyImage *m_rightEye;
};

// DeckLink type description
extern PyTypeObject DeckLinkType;

// helper function
HRESULT decklink_ReadDisplayMode(const char *format, size_t len, BMDDisplayMode *displayMode);
HRESULT decklink_ReadPixelFormat(const char *format, size_t len, BMDPixelFormat *displayMode);

#endif /* WITH_GAMEENGINE_DECKLINK */
