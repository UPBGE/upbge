/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/Exception.h
 *  \ingroup bgevideotex
 */

#pragma once

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

#include "Common.h"

#define CHCKHRSLTV(fnc, val, err) \
  { \
    HRESULT macroHRslt = (fnc); \
    if (macroHRslt != val) \
      throw Exception(err, macroHRslt, __FILE__, __LINE__); \
  }

#define THRWEXCP(err, hRslt) throw Exception(err, hRslt, __FILE__, __LINE__)

#if defined WIN32

#  define CHCKHRSLT(fnc, err) \
    { \
      HRESULT macroHRslt = (fnc); \
      if (FAILED(macroHRslt)) \
        throw Exception(err, macroHRslt, __FILE__, __LINE__); \
    }

#else

#  define CHCKHRSLT(fnc, err) CHCKHRSLTV(fnc, S_OK, err)

#endif

// forward declarations
class ExceptionID;
class Exception;

// exception identificators
extern ExceptionID ErrGeneral, ErrNotFound;

// result type
typedef long RESULT;

// class ExceptionID for exception identification
class ExceptionID {
 public:
  // constructor a destructor
  ExceptionID(void)
  {
  }
  ~ExceptionID(void)
  {
  }

 private:
  // not allowed
  ExceptionID(const ExceptionID &obj) throw()
  {
  }
  ExceptionID &operator=(const ExceptionID &obj) throw()
  {
    return *this;
  }
};

// class ExpDesc for exception description
class ExpDesc {
 public:
  // constructor a destructor
  ExpDesc(ExceptionID &exp, const char *desc, RESULT hres = S_OK);
  ~ExpDesc(void);

  // comparision function
  // returns 0, if exception identification don't match at all
  // returns 1, if only exception identification is matching
  // returns 2, if both exception identification and result are matching
  int isExp(ExceptionID *exp, RESULT hres = S_OK) throw()
  {
    // check exception identification
    if (&m_expID == exp) {
      // check result value
      if (m_hRslt == hres)
        return 2;
      // only identification match
      if (m_hRslt == S_OK)
        return 1;
    }
    // no match
    return 0;
  }

  // get exception description
  void loadDesc(std::string &desc) throw()
  {
    desc = m_description;
  }

  void registerDesc(void)
  {
    if (std::find(m_expDescs.begin(), m_expDescs.end(), this) == m_expDescs.end())
      m_expDescs.push_back(this);
  }
  // list of exception descriptions
  static std::vector<ExpDesc *> m_expDescs;

 private:
  // exception ID
  ExceptionID &m_expID;
  // result
  RESULT m_hRslt;
  // description
  const char *m_description;

  // not allowed
  ExpDesc(const ExpDesc &obj) : m_expID(ErrNotFound)
  {
  }
  ExpDesc &operator=(const ExpDesc &obj)
  {
    return *this;
  }
};

// class Exception
class Exception : public std::exception {
 public:
  // constructor
  Exception();
  // destructor
  virtual ~Exception() throw();
  // copy constructor
  Exception(const Exception &xpt);
  // assignment operator
  Exception &operator=(const Exception &xpt);
  // get exception description
  virtual const char *what(void);

  // debug version of constructor
  Exception(ExceptionID &expID, RESULT rslt, const char *fil, int lin);
  // set source file and line of exception
  void setFileLine(const char *fil, int lin);

  // get description in string
  std::string &getDesc(void) throw()
  {
    return m_desc;
  }

  // report exception
  virtual void report(void);

  // get exception id
  ExceptionID *getID(void) throw()
  {
    return m_expID;
  }

  /// last exception description
  static std::string m_lastError;

  /// log file name
  static const char *m_logFile;

 protected:
  // exception identification
  ExceptionID *m_expID;
  // RESULT code
  RESULT m_hRslt;

  // exception description
  std::string m_desc;

  // set exception description
  virtual void setXptDesc(void);

  // copy exception
  void copy(const Exception &xpt);

  // file name where exception was thrown
  std::string m_fileName;
  // line number in file
  int m_line;
};

extern ExpDesc MaterialNotAvailDesc;
extern ExpDesc TextureNotAvailDesc;
extern ExpDesc ImageSizesNotMatchDesc;
extern ExpDesc ImageHasExportsDesc;
extern ExpDesc InvalidColorChannelDesc;
extern ExpDesc InvalidImageModeDesc;
extern ExpDesc SceneInvalidDesc;
extern ExpDesc CameraInvalidDesc;
extern ExpDesc ObserverInvalidDesc;
extern ExpDesc FrameBufferInvalidDesc;
extern ExpDesc MirrorInvalidDesc;
extern ExpDesc MirrorSizeInvalidDesc;
extern ExpDesc MirrorNormalInvalidDesc;
extern ExpDesc MirrorHorizontalDesc;
extern ExpDesc MirrorTooSmallDesc;
extern ExpDesc SourceVideoEmptyDesc;
extern ExpDesc SourceVideoCreationDesc;
extern ExpDesc DeckLinkBadDisplayModeDesc;
extern ExpDesc DeckLinkBadPixelFormatDesc;
extern ExpDesc AutoDetectionNotAvailDesc;
extern ExpDesc DeckLinkOpenCardDesc;
extern ExpDesc DeckLinkBadFormatDesc;
extern ExpDesc DeckLinkInternalErrorDesc;
extern ExpDesc SourceVideoOnlyCaptureDesc;
extern ExpDesc VideoDeckLinkBadFormatDesc;
extern ExpDesc VideoDeckLinkOpenCardDesc;
extern ExpDesc VideoDeckLinkDvpInternalErrorDesc;
extern ExpDesc VideoDeckLinkPinMemoryErrorDesc;

extern ExceptionID InvalidImageMode;

void registerAllExceptions(void);
