/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

#ifndef WIN32
#  include <unistd.h>
#else
#  include <io.h>
#endif
#include "MEM_guardedalloc.h"
#include <string.h>

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_sound_types.h"
#include "DNA_vfont_types.h"
#include "DNA_volume_types.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BKE_main.h"
#include "BKE_packedFile.h"
#include "BKE_report.h"
#include "BKE_sound.h"
#include "BKE_vfont.h"
#include "BKE_volume.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BLO_read_write.h"

int BKE_packedfile_seek(PackedFile *pf, int offset, int whence)
{
  int oldseek = -1, seek = 0;

  if (pf) {
    oldseek = pf->seek;
    switch (whence) {
      case SEEK_CUR:
        seek = oldseek + offset;
        break;
      case SEEK_END:
        seek = pf->size + offset;
        break;
      case SEEK_SET:
        seek = offset;
        break;
      default:
        oldseek = -1;
        break;
    }
    if (seek < 0) {
      seek = 0;
    }
    else if (seek > pf->size) {
      seek = pf->size;
    }
    pf->seek = seek;
  }

  return oldseek;
}

void BKE_packedfile_rewind(PackedFile *pf)
{
  BKE_packedfile_seek(pf, 0, SEEK_SET);
}

int BKE_packedfile_read(PackedFile *pf, void *data, int size)
{
  if ((pf != NULL) && (size >= 0) && (data != NULL)) {
    if (size + pf->seek > pf->size) {
      size = pf->size - pf->seek;
    }

    if (size > 0) {
      memcpy(data, ((char *)pf->data) + pf->seek, size);
    }
    else {
      size = 0;
    }

    pf->seek += size;
  }
  else {
    size = -1;
  }

  return size;
}

int BKE_packedfile_count_all(Main *bmain)
{
  Image *ima;
  VFont *vf;
  bSound *sound;
  Volume *volume;
  int count = 0;

  /* let's check if there are packed files... */
  for (ima = bmain->images.first; ima; ima = ima->id.next) {
    if (BKE_image_has_packedfile(ima)) {
      count++;
    }
  }

  for (vf = bmain->fonts.first; vf; vf = vf->id.next) {
    if (vf->packedfile) {
      count++;
    }
  }

  for (sound = bmain->sounds.first; sound; sound = sound->id.next) {
    if (sound->packedfile) {
      count++;
    }
  }

  for (volume = bmain->volumes.first; volume; volume = volume->id.next) {
    if (volume->packedfile) {
      count++;
    }
  }

  return count;
}

void BKE_packedfile_free(PackedFile *pf)
{
  if (pf) {
    BLI_assert(pf->data != NULL);

    MEM_SAFE_FREE(pf->data);
    MEM_freeN(pf);
  }
  else {
    printf("%s: Trying to free a NULL pointer\n", __func__);
  }
}

PackedFile *BKE_packedfile_duplicate(const PackedFile *pf_src)
{
  BLI_assert(pf_src != NULL);
  BLI_assert(pf_src->data != NULL);

  PackedFile *pf_dst;

  pf_dst = MEM_dupallocN(pf_src);
  pf_dst->data = MEM_dupallocN(pf_src->data);

  return pf_dst;
}

PackedFile *BKE_packedfile_new_from_memory(void *mem, int memlen)
{
  BLI_assert(mem != NULL);

  PackedFile *pf = MEM_callocN(sizeof(*pf), "PackedFile");
  pf->data = mem;
  pf->size = memlen;

  return pf;
}

PackedFile *BKE_packedfile_new(ReportList *reports, const char *filepath, const char *basepath)
{
  PackedFile *pf = NULL;
  int file, filelen;
  char name[FILE_MAX];
  void *data;

  /* render result has no filepath and can be ignored
   * any other files with no name can be ignored too */
  if (filepath[0] == '\0') {
    return pf;
  }

  // XXX waitcursor(1);

  /* convert relative filenames to absolute filenames */

  BLI_strncpy(name, filepath, sizeof(name));
  BLI_path_abs(name, basepath);

  /* open the file
   * and create a PackedFile structure */

  file = BLI_open(name, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    BKE_reportf(reports, RPT_ERROR, "Unable to pack file, source path '%s' not found", name);
  }
  else {
    filelen = BLI_file_descriptor_size(file);

    if (filelen == 0) {
      /* MEM_mallocN complains about MEM_mallocN(0, "bla");
       * we don't care.... */
      data = MEM_mallocN(1, "packFile");
    }
    else {
      data = MEM_mallocN(filelen, "packFile");
    }
    if (read(file, data, filelen) == filelen) {
      pf = BKE_packedfile_new_from_memory(data, filelen);
    }
    else {
      MEM_freeN(data);
    }

    close(file);
  }

  // XXX waitcursor(0);

  return pf;
}

void BKE_packedfile_pack_all(Main *bmain, ReportList *reports, bool verbose)
{
  Image *ima;
  VFont *vfont;
  bSound *sound;
  Volume *volume;
  int tot = 0;

  for (ima = bmain->images.first; ima; ima = ima->id.next) {
    if (BKE_image_has_packedfile(ima) == false && !ID_IS_LINKED(ima)) {
      if (ELEM(ima->source, IMA_SRC_FILE, IMA_SRC_TILED)) {
        BKE_image_packfiles(reports, ima, ID_BLEND_PATH(bmain, &ima->id));
        tot++;
      }
      else if (ELEM(ima->source, IMA_SRC_MOVIE, IMA_SRC_SEQUENCE) && verbose) {
        BKE_reportf(reports,
                    RPT_WARNING,
                    "Image '%s' skipped, packing movies or image sequences not supported",
                    ima->id.name + 2);
      }
    }
  }

  for (vfont = bmain->fonts.first; vfont; vfont = vfont->id.next) {
    if (vfont->packedfile == NULL && !ID_IS_LINKED(vfont) &&
        BKE_vfont_is_builtin(vfont) == false) {
      vfont->packedfile = BKE_packedfile_new(
          reports, vfont->filepath, BKE_main_blendfile_path(bmain));
      tot++;
    }
  }

  for (sound = bmain->sounds.first; sound; sound = sound->id.next) {
    if (sound->packedfile == NULL && !ID_IS_LINKED(sound)) {
      sound->packedfile = BKE_packedfile_new(
          reports, sound->filepath, BKE_main_blendfile_path(bmain));
      tot++;
    }
  }

  for (volume = bmain->volumes.first; volume; volume = volume->id.next) {
    if (volume->packedfile == NULL && !ID_IS_LINKED(volume)) {
      volume->packedfile = BKE_packedfile_new(
          reports, volume->filepath, BKE_main_blendfile_path(bmain));
      tot++;
    }
  }

  if (tot > 0) {
    BKE_reportf(reports, RPT_INFO, "Packed %d file(s)", tot);
  }
  else if (verbose) {
    BKE_report(reports, RPT_INFO, "No new files have been packed");
  }
}

int BKE_packedfile_write_to_file(ReportList *reports,
                                 const char *ref_file_name,
                                 const char *filepath,
                                 PackedFile *pf,
                                 const bool guimode)
{
  int file, number;
  int ret_value = RET_OK;
  bool remove_tmp = false;
  char name[FILE_MAX];
  char tempname[FILE_MAX];
  /*      void *data; */

  if (guimode) {
  }  // XXX  waitcursor(1);

  BLI_strncpy(name, filepath, sizeof(name));
  BLI_path_abs(name, ref_file_name);

  if (BLI_exists(name)) {
    for (number = 1; number <= 999; number++) {
      BLI_snprintf(tempname, sizeof(tempname), "%s.%03d_", name, number);
      if (!BLI_exists(tempname)) {
        if (BLI_copy(name, tempname) == RET_OK) {
          remove_tmp = true;
        }
        break;
      }
    }
  }

  /* make sure the path to the file exists... */
  BLI_make_existing_file(name);

  file = BLI_open(name, O_BINARY + O_WRONLY + O_CREAT + O_TRUNC, 0666);
  if (file == -1) {
    BKE_reportf(reports, RPT_ERROR, "Error creating file '%s'", name);
    ret_value = RET_ERROR;
  }
  else {
    if (write(file, pf->data, pf->size) != pf->size) {
      BKE_reportf(reports, RPT_ERROR, "Error writing file '%s'", name);
      ret_value = RET_ERROR;
    }
    else {
      BKE_reportf(reports, RPT_INFO, "Saved packed file to: %s", name);
    }

    close(file);
  }

  if (remove_tmp) {
    if (ret_value == RET_ERROR) {
      if (BLI_rename(tempname, name) != 0) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Error restoring temp file (check files '%s' '%s')",
                    tempname,
                    name);
      }
    }
    else {
      if (BLI_delete(tempname, false, false) != 0) {
        BKE_reportf(reports, RPT_ERROR, "Error deleting '%s' (ignored)", tempname);
      }
    }
  }

  if (guimode) {
  }  // XXX waitcursor(0);

  return ret_value;
}

enum ePF_FileCompare BKE_packedfile_compare_to_file(const char *ref_file_name,
                                                    const char *filepath,
                                                    PackedFile *pf)
{
  BLI_stat_t st;
  enum ePF_FileCompare ret_val;
  char buf[4096];
  char name[FILE_MAX];

  BLI_strncpy(name, filepath, sizeof(name));
  BLI_path_abs(name, ref_file_name);

  if (BLI_stat(name, &st) == -1) {
    ret_val = PF_CMP_NOFILE;
  }
  else if (st.st_size != pf->size) {
    ret_val = PF_CMP_DIFFERS;
  }
  else {
    /* we'll have to compare the two... */

    const int file = BLI_open(name, O_BINARY | O_RDONLY, 0);
    if (file == -1) {
      ret_val = PF_CMP_NOFILE;
    }
    else {
      ret_val = PF_CMP_EQUAL;

      for (int i = 0; i < pf->size; i += sizeof(buf)) {
        int len = pf->size - i;
        if (len > sizeof(buf)) {
          len = sizeof(buf);
        }

        if (read(file, buf, len) != len) {
          /* read error ... */
          ret_val = PF_CMP_DIFFERS;
          break;
        }

        if (memcmp(buf, ((char *)pf->data) + i, len) != 0) {
          ret_val = PF_CMP_DIFFERS;
          break;
        }
      }

      close(file);
    }
  }

  return ret_val;
}

char *BKE_packedfile_unpack_to_file(ReportList *reports,
                                    const char *ref_file_name,
                                    const char *abs_name,
                                    const char *local_name,
                                    PackedFile *pf,
                                    enum ePF_FileStatus how)
{
  char *newname = NULL;
  const char *temp = NULL;

  if (pf != NULL) {
    switch (how) {
      case PF_KEEP:
        break;
      case PF_REMOVE:
        temp = abs_name;
        break;
      case PF_USE_LOCAL: {
        char temp_abs[FILE_MAX];

        BLI_strncpy(temp_abs, local_name, sizeof(temp_abs));
        BLI_path_abs(temp_abs, ref_file_name);

        /* if file exists use it */
        if (BLI_exists(temp_abs)) {
          temp = local_name;
          break;
        }
        /* else create it */
        ATTR_FALLTHROUGH;
      }
      case PF_WRITE_LOCAL:
        if (BKE_packedfile_write_to_file(reports, ref_file_name, local_name, pf, 1) == RET_OK) {
          temp = local_name;
        }
        break;
      case PF_USE_ORIGINAL: {
        char temp_abs[FILE_MAX];

        BLI_strncpy(temp_abs, abs_name, sizeof(temp_abs));
        BLI_path_abs(temp_abs, ref_file_name);

        /* if file exists use it */
        if (BLI_exists(temp_abs)) {
          BKE_reportf(reports, RPT_INFO, "Use existing file (instead of packed): %s", abs_name);
          temp = abs_name;
          break;
        }
        /* else create it */
        ATTR_FALLTHROUGH;
      }
      case PF_WRITE_ORIGINAL:
        if (BKE_packedfile_write_to_file(reports, ref_file_name, abs_name, pf, 1) == RET_OK) {
          temp = abs_name;
        }
        break;
      default:
        printf("%s: unknown return_value %u\n", __func__, how);
        break;
    }

    if (temp) {
      newname = BLI_strdup(temp);
    }
  }

  return newname;
}

static void unpack_generate_paths(const char *name,
                                  ID *id,
                                  char *r_abspath,
                                  char *r_relpath,
                                  size_t abspathlen,
                                  size_t relpathlen)
{
  const short id_type = GS(id->name);
  char tempname[FILE_MAX];
  char tempdir[FILE_MAXDIR];

  BLI_split_dirfile(name, tempdir, tempname, sizeof(tempdir), sizeof(tempname));

  if (tempname[0] == '\0') {
    /* NOTE: we generally do not have any real way to re-create extension out of data. */
    const size_t len = BLI_strncpy_rlen(tempname, id->name + 2, sizeof(tempname));
    printf("%s\n", tempname);

    /* For images ensure that the temporary filename contains tile number information as well as
     * a file extension based on the file magic. */
    if (id_type == ID_IM) {
      Image *ima = (Image *)id;
      ImagePackedFile *imapf = ima->packedfiles.last;
      if (imapf != NULL && imapf->packedfile != NULL) {
        const PackedFile *pf = imapf->packedfile;
        enum eImbFileType ftype = IMB_ispic_type_from_memory((const uchar *)pf->data, pf->size);
        if (ima->source == IMA_SRC_TILED) {
          char tile_number[6];
          BLI_snprintf(tile_number, sizeof(tile_number), ".%d", imapf->tile_number);
          BLI_strncpy(tempname + len, tile_number, sizeof(tempname) - len);
        }
        if (ftype != IMB_FTYPE_NONE) {
          const int imtype = BKE_ftype_to_imtype(ftype, NULL);
          BKE_image_path_ensure_ext_from_imtype(tempname, imtype);
        }
      }
    }

    BLI_filename_make_safe(tempname);
    printf("%s\n", tempname);
  }

  if (tempdir[0] == '\0') {
    /* Fallback to relative dir. */
    BLI_strncpy(tempdir, "//", sizeof(tempdir));
  }

  switch (id_type) {
    case ID_VF:
      BLI_snprintf(r_relpath, relpathlen, "//fonts/%s", tempname);
      break;
    case ID_SO:
      BLI_snprintf(r_relpath, relpathlen, "//sounds/%s", tempname);
      break;
    case ID_IM:
      BLI_snprintf(r_relpath, relpathlen, "//textures/%s", tempname);
      break;
    case ID_VO:
      BLI_snprintf(r_relpath, relpathlen, "//volumes/%s", tempname);
      break;
    default:
      break;
  }

  {
    size_t len = BLI_strncpy_rlen(r_abspath, tempdir, abspathlen);
    BLI_strncpy(r_abspath + len, tempname, abspathlen - len);
  }
}

char *BKE_packedfile_unpack(Main *bmain,
                            ReportList *reports,
                            ID *id,
                            const char *orig_file_path,
                            PackedFile *pf,
                            enum ePF_FileStatus how)
{
  char localname[FILE_MAX], absname[FILE_MAX];
  char *new_name = NULL;

  if (id != NULL) {
    unpack_generate_paths(
        orig_file_path, id, absname, localname, sizeof(absname), sizeof(localname));
    new_name = BKE_packedfile_unpack_to_file(
        reports, BKE_main_blendfile_path(bmain), absname, localname, pf, how);
  }

  return new_name;
}

int BKE_packedfile_unpack_vfont(Main *bmain,
                                ReportList *reports,
                                VFont *vfont,
                                enum ePF_FileStatus how)
{
  int ret_value = RET_ERROR;
  if (vfont) {
    char *new_file_path = BKE_packedfile_unpack(
        bmain, reports, (ID *)vfont, vfont->filepath, vfont->packedfile, how);

    if (new_file_path != NULL) {
      ret_value = RET_OK;
      BKE_packedfile_free(vfont->packedfile);
      vfont->packedfile = NULL;
      BLI_strncpy(vfont->filepath, new_file_path, sizeof(vfont->filepath));
      MEM_freeN(new_file_path);
    }
  }

  return ret_value;
}

int BKE_packedfile_unpack_sound(Main *bmain,
                                ReportList *reports,
                                bSound *sound,
                                enum ePF_FileStatus how)
{
  int ret_value = RET_ERROR;

  if (sound != NULL) {
    char *new_file_path = BKE_packedfile_unpack(
        bmain, reports, (ID *)sound, sound->filepath, sound->packedfile, how);
    if (new_file_path != NULL) {
      BLI_strncpy(sound->filepath, new_file_path, sizeof(sound->filepath));
      MEM_freeN(new_file_path);

      BKE_packedfile_free(sound->packedfile);
      sound->packedfile = NULL;

      BKE_sound_load(bmain, sound);

      ret_value = RET_OK;
    }
  }

  return ret_value;
}

int BKE_packedfile_unpack_image(Main *bmain,
                                ReportList *reports,
                                Image *ima,
                                enum ePF_FileStatus how)
{
  int ret_value = RET_ERROR;

  if (ima != NULL) {
    while (ima->packedfiles.last) {
      ImagePackedFile *imapf = ima->packedfiles.last;
      char *new_file_path = BKE_packedfile_unpack(
          bmain, reports, (ID *)ima, imapf->filepath, imapf->packedfile, how);

      if (new_file_path != NULL) {
        ImageView *iv;

        ret_value = ret_value == RET_ERROR ? RET_ERROR : RET_OK;
        BKE_packedfile_free(imapf->packedfile);
        imapf->packedfile = NULL;

        /* update the new corresponding view filepath */
        iv = BLI_findstring(&ima->views, imapf->filepath, offsetof(ImageView, filepath));
        if (iv) {
          BLI_strncpy(iv->filepath, new_file_path, sizeof(imapf->filepath));
        }

        /* keep the new name in the image for non-pack specific reasons */
        if (how != PF_REMOVE) {
          BLI_strncpy(ima->filepath, new_file_path, sizeof(imapf->filepath));
          if (ima->source == IMA_SRC_TILED) {
            /* Ensure that the Image filepath is kept in a tokenized format. */
            char *filename = (char *)BLI_path_basename(ima->filepath);
            BKE_image_ensure_tile_token(filename);
          }
        }
        MEM_freeN(new_file_path);
      }
      else {
        ret_value = RET_ERROR;
      }

      BLI_remlink(&ima->packedfiles, imapf);
      MEM_freeN(imapf);
    }
  }

  if (ret_value == RET_OK) {
    BKE_image_signal(bmain, ima, NULL, IMA_SIGNAL_RELOAD);
  }

  return ret_value;
}

int BKE_packedfile_unpack_volume(Main *bmain,
                                 ReportList *reports,
                                 Volume *volume,
                                 enum ePF_FileStatus how)
{
  int ret_value = RET_ERROR;

  if (volume != NULL) {
    char *new_file_path = BKE_packedfile_unpack(
        bmain, reports, (ID *)volume, volume->filepath, volume->packedfile, how);
    if (new_file_path != NULL) {
      BLI_strncpy(volume->filepath, new_file_path, sizeof(volume->filepath));
      MEM_freeN(new_file_path);

      BKE_packedfile_free(volume->packedfile);
      volume->packedfile = NULL;

      BKE_volume_unload(volume);

      ret_value = RET_OK;
    }
  }

  return ret_value;
}

int BKE_packedfile_unpack_all_libraries(Main *bmain, ReportList *reports)
{
  Library *lib;
  char *newname;
  int ret_value = RET_ERROR;

  for (lib = bmain->libraries.first; lib; lib = lib->id.next) {
    if (lib->packedfile && lib->filepath[0]) {

      newname = BKE_packedfile_unpack_to_file(reports,
                                              BKE_main_blendfile_path(bmain),
                                              lib->filepath_abs,
                                              lib->filepath_abs,
                                              lib->packedfile,
                                              PF_WRITE_ORIGINAL);
      if (newname != NULL) {
        ret_value = RET_OK;

        printf("Unpacked .blend library: %s\n", newname);

        BKE_packedfile_free(lib->packedfile);
        lib->packedfile = NULL;

        MEM_freeN(newname);
      }
    }
  }

  return ret_value;
}

void BKE_packedfile_pack_all_libraries(Main *bmain, ReportList *reports)
{
  Library *lib;

  /* Test for relativeness. */
  for (lib = bmain->libraries.first; lib; lib = lib->id.next) {
    if (!BLI_path_is_rel(lib->filepath)) {
      break;
    }
  }

  if (lib) {
    BKE_reportf(reports, RPT_ERROR, "Cannot pack absolute file: '%s'", lib->filepath);
    return;
  }

  for (lib = bmain->libraries.first; lib; lib = lib->id.next) {
    if (lib->packedfile == NULL) {
      lib->packedfile = BKE_packedfile_new(reports, lib->filepath, BKE_main_blendfile_path(bmain));
    }
  }
}

void BKE_packedfile_unpack_all(Main *bmain, ReportList *reports, enum ePF_FileStatus how)
{
  Image *ima;
  VFont *vf;
  bSound *sound;
  Volume *volume;

  for (ima = bmain->images.first; ima; ima = ima->id.next) {
    if (BKE_image_has_packedfile(ima)) {
      BKE_packedfile_unpack_image(bmain, reports, ima, how);
    }
  }

  for (vf = bmain->fonts.first; vf; vf = vf->id.next) {
    if (vf->packedfile) {
      BKE_packedfile_unpack_vfont(bmain, reports, vf, how);
    }
  }

  for (sound = bmain->sounds.first; sound; sound = sound->id.next) {
    if (sound->packedfile) {
      BKE_packedfile_unpack_sound(bmain, reports, sound, how);
    }
  }

  for (volume = bmain->volumes.first; volume; volume = volume->id.next) {
    if (volume->packedfile) {
      BKE_packedfile_unpack_volume(bmain, reports, volume, how);
    }
  }
}

bool BKE_packedfile_id_check(const ID *id)
{
  switch (GS(id->name)) {
    case ID_IM: {
      const Image *ima = (const Image *)id;
      return BKE_image_has_packedfile(ima);
    }
    case ID_VF: {
      const VFont *vf = (const VFont *)id;
      return vf->packedfile != NULL;
    }
    case ID_SO: {
      const bSound *snd = (const bSound *)id;
      return snd->packedfile != NULL;
    }
    case ID_VO: {
      const Volume *volume = (const Volume *)id;
      return volume->packedfile != NULL;
    }
    case ID_LI: {
      const Library *li = (const Library *)id;
      return li->packedfile != NULL;
    }
    default:
      break;
  }
  return false;
}

void BKE_packedfile_id_unpack(Main *bmain, ID *id, ReportList *reports, enum ePF_FileStatus how)
{
  switch (GS(id->name)) {
    case ID_IM: {
      Image *ima = (Image *)id;
      if (BKE_image_has_packedfile(ima)) {
        BKE_packedfile_unpack_image(bmain, reports, ima, how);
      }
      break;
    }
    case ID_VF: {
      VFont *vf = (VFont *)id;
      if (vf->packedfile) {
        BKE_packedfile_unpack_vfont(bmain, reports, vf, how);
      }
      break;
    }
    case ID_SO: {
      bSound *snd = (bSound *)id;
      if (snd->packedfile) {
        BKE_packedfile_unpack_sound(bmain, reports, snd, how);
      }
      break;
    }
    case ID_VO: {
      Volume *volume = (Volume *)id;
      if (volume->packedfile) {
        BKE_packedfile_unpack_volume(bmain, reports, volume, how);
      }
      break;
    }
    case ID_LI: {
      Library *li = (Library *)id;
      BKE_reportf(reports, RPT_ERROR, "Cannot unpack individual Library file, '%s'", li->filepath);
      break;
    }
    default:
      break;
  }
}

void BKE_packedfile_blend_write(BlendWriter *writer, PackedFile *pf)
{
  if (pf == NULL) {
    return;
  }
  BLO_write_struct(writer, PackedFile, pf);
  BLO_write_raw(writer, pf->size, pf->data);
}

void BKE_packedfile_blend_read(BlendDataReader *reader, PackedFile **pf_p)
{
  BLO_read_packed_address(reader, pf_p);
  PackedFile *pf = *pf_p;
  if (pf == NULL) {
    return;
  }

  BLO_read_packed_address(reader, &pf->data);
  if (pf->data == NULL) {
    /* We cannot allow a PackedFile with a NULL data field,
     * the whole code assumes this is not possible. See T70315. */
    printf("%s: NULL packedfile data, cleaning up...\n", __func__);
    MEM_SAFE_FREE(pf);
  }
}
