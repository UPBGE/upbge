/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bli
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_dynlib.h"

struct DynamicLibrary {
  void *handle;
};

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN
#  include "utf_winfunc.h"
#  include "utfconv.h"
#  include <windows.h>

DynamicLibrary *BLI_dynlib_open(const char *name)
{
  DynamicLibrary *lib;
  void *handle;

  UTF16_ENCODE(name);
  handle = LoadLibraryW(name_16);
  UTF16_UN_ENCODE(name);

  if (!handle) {
    return NULL;
  }

  lib = MEM_callocN(sizeof(*lib), "Dynamic Library");
  lib->handle = handle;

  return lib;
}

void *BLI_dynlib_find_symbol(DynamicLibrary *lib, const char *symname)
{
  return GetProcAddress(lib->handle, symname);
}

char *BLI_dynlib_get_error_as_string(DynamicLibrary *lib)
{
  int err;

  /* if lib is NULL reset the last error code */
  err = GetLastError();
  if (!lib) {
    SetLastError(ERROR_SUCCESS);
  }

  if (err) {
    static char buf[1024];

    if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL,
                      err,
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      buf,
                      sizeof(buf),
                      NULL)) {
      return buf;
    }
  }

  return NULL;
}

void BLI_dynlib_close(DynamicLibrary *lib)
{
  FreeLibrary(lib->handle);
  MEM_freeN(lib);
}

#else /* Unix */

#  include <dlfcn.h>

DynamicLibrary *BLI_dynlib_open(const char *name)
{
  DynamicLibrary *lib;
  void *handle = dlopen(name, RTLD_LAZY);

  if (!handle) {
    return NULL;
  }

  lib = MEM_callocN(sizeof(*lib), "Dynamic Library");
  lib->handle = handle;

  return lib;
}

void *BLI_dynlib_find_symbol(DynamicLibrary *lib, const char *symname)
{
  return dlsym(lib->handle, symname);
}

char *BLI_dynlib_get_error_as_string(DynamicLibrary *lib)
{
  (void)lib; /* unused */
  return dlerror();
}

void BLI_dynlib_close(DynamicLibrary *lib)
{
  dlclose(lib->handle);
  MEM_freeN(lib);
}

#endif
