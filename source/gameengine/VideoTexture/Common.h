/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file VideoTexture/Common.h
 *  \ingroup bgevideotex
 */

#if defined WIN32
#  define WINDOWS_LEAN_AND_MEAN
#endif

#ifndef _HRESULT_DEFINED
#  define _HRESULT_DEFINED
#  define HRESULT long
#endif

#ifndef DWORD
#  define DWORD unsigned long
#endif

#ifndef S_OK
#  define S_OK ((HRESULT)0L)
#endif

#ifndef BYTE
#  define BYTE unsigned char
#endif

#ifndef WIN32
#  define Sleep(time) sleep(time)
#endif

#ifndef FAILED
#  define FAILED(Status) ((HRESULT)(Status) < 0)
#endif

#include <iostream>
