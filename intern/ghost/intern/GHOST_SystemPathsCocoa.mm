/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2010 Blender Foundation. All rights reserved. */

#import <Foundation/Foundation.h>

#include "GHOST_Debug.h"
#include "GHOST_SystemPathsCocoa.h"

#pragma mark initialization/finalization

GHOST_SystemPathsCocoa::GHOST_SystemPathsCocoa()
{
}

GHOST_SystemPathsCocoa::~GHOST_SystemPathsCocoa()
{
}

#pragma mark Base directories retrieval

static const char *GetApplicationSupportDir(const char *versionstr,
                                            const NSSearchPathDomainMask mask,
                                            char *tempPath,
                                            const std::size_t len_tempPath)
{
  @autoreleasepool {
    const NSArray *const paths = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, mask, YES);

    if ([paths count] == 0) {
      return NULL;
    }
    const NSString *const basePath = [paths objectAtIndex:0];

    snprintf(tempPath,
             len_tempPath,
             "%s/UPBGE/%s",
             [basePath cStringUsingEncoding:NSASCIIStringEncoding],
             versionstr);
  }
  return tempPath;
}

const char *GHOST_SystemPathsCocoa::getSystemDir(int, const char *versionstr) const
{
  static char tempPath[512] = "";
  return GetApplicationSupportDir(versionstr, NSLocalDomainMask, tempPath, sizeof(tempPath));
}

const char *GHOST_SystemPathsCocoa::getUserDir(int, const char *versionstr) const
{
  static char tempPath[512] = "";
  return GetApplicationSupportDir(versionstr, NSUserDomainMask, tempPath, sizeof(tempPath));
}

const char *GHOST_SystemPathsCocoa::getUserSpecialDir(GHOST_TUserSpecialDirTypes type) const
{
  static char tempPath[512] = "";
  @autoreleasepool {
    NSSearchPathDirectory ns_directory;

    switch (type) {
      case GHOST_kUserSpecialDirDesktop:
        ns_directory = NSDesktopDirectory;
        break;
      case GHOST_kUserSpecialDirDocuments:
        ns_directory = NSDocumentDirectory;
        break;
      case GHOST_kUserSpecialDirDownloads:
        ns_directory = NSDownloadsDirectory;
        break;
      case GHOST_kUserSpecialDirMusic:
        ns_directory = NSMusicDirectory;
        break;
      case GHOST_kUserSpecialDirPictures:
        ns_directory = NSPicturesDirectory;
        break;
      case GHOST_kUserSpecialDirVideos:
        ns_directory = NSMoviesDirectory;
        break;
      case GHOST_kUserSpecialDirCaches:
        ns_directory = NSCachesDirectory;
        break;
      default:
        GHOST_ASSERT(
            false,
            "GHOST_SystemPathsCocoa::getUserSpecialDir(): Invalid enum value for type parameter");
        return NULL;
    }

    const NSArray *const paths = NSSearchPathForDirectoriesInDomains(
        ns_directory, NSUserDomainMask, YES);
    if ([paths count] == 0) {
      return NULL;
    }
    const NSString *const basePath = [paths objectAtIndex:0];

    strncpy(tempPath, [basePath cStringUsingEncoding:NSASCIIStringEncoding], sizeof(tempPath));
  }
  return tempPath;
}

const char *GHOST_SystemPathsCocoa::getBinaryDir() const
{
  static char tempPath[512] = "";

  @autoreleasepool {
    const NSString *const basePath = [[NSBundle mainBundle] bundlePath];

    if (basePath == nil) {
      return NULL;
    }

    strcpy(tempPath, [basePath cStringUsingEncoding:NSASCIIStringEncoding]);
  }
  return tempPath;
}

void GHOST_SystemPathsCocoa::addToSystemRecentFiles(const char *filename) const
{
  /* TODO: implement for macOS */
}
