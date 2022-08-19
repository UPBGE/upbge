/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

#ifdef __cplusplus
extern "C" {
#endif

struct BlendFileData;
struct BlendFileReadParams;
struct BlendFileReadReport;
struct ID;
struct Main;
struct MemFile;
struct ReportList;
struct UserDef;
struct bContext;

/**
 * Shared setup function that makes the data from `bfd` into the current blend file,
 * replacing the contents of #G.main.
 * This uses the bfd #BKE_blendfile_read and similarly named functions.
 *
 * This is done in a separate step so the caller may perform actions after it is known the file
 * loaded correctly but before the file replaces the existing blend file contents.
 */
void BKE_blendfile_read_setup_ex(struct bContext *C,
                                 struct BlendFileData *bfd,
                                 const struct BlendFileReadParams *params,
                                 struct BlendFileReadReport *reports,
                                 /* Extra args. */
                                 bool startup_update_defaults,
                                 const char *startup_app_template);

void BKE_blendfile_read_setup(struct bContext *C,
                              struct BlendFileData *bfd,
                              const struct BlendFileReadParams *params,
                              struct BlendFileReadReport *reports);

/**
 * \return Blend file data, this must be passed to #BKE_blendfile_read_setup when non-NULL.
 */
struct BlendFileData *BKE_blendfile_read(const char *filepath,
                                         const struct BlendFileReadParams *params,
                                         struct BlendFileReadReport *reports);

/**
 * \return Blend file data, this must be passed to #BKE_blendfile_read_setup when non-NULL.
 */
struct BlendFileData *BKE_blendfile_read_from_memory(const void *filebuf,
                                                     int filelength,
                                                     const struct BlendFileReadParams *params,
                                                     struct ReportList *reports);

/**
 * \return Blend file data, this must be passed to #BKE_blendfile_read_setup when non-NULL.
 * \note `memfile` is the undo buffer.
 */
struct BlendFileData *BKE_blendfile_read_from_memfile(struct Main *bmain,
                                                      struct MemFile *memfile,
                                                      const struct BlendFileReadParams *params,
                                                      struct ReportList *reports);
/**
 * Utility to make a file 'empty' used for startup to optionally give an empty file.
 * Handy for tests.
 */
void BKE_blendfile_read_make_empty(struct bContext *C);

/**
 * Only read the #UserDef from a .blend.
 */
struct UserDef *BKE_blendfile_userdef_read(const char *filepath, struct ReportList *reports);
struct UserDef *BKE_blendfile_userdef_read_from_memory(const void *filebuf,
                                                       int filelength,
                                                       struct ReportList *reports);
struct UserDef *BKE_blendfile_userdef_from_defaults(void);

/**
 * Only write the #UserDef in a `.blend`.
 * \return success.
 */
bool BKE_blendfile_userdef_write(const char *filepath, struct ReportList *reports);
/**
 * Only write the #UserDef in a `.blend`, merging with the existing blend file.
 * \return success.
 *
 * \note In the future we should re-evaluate user preferences,
 * possibly splitting out system/hardware specific preferences.
 */
bool BKE_blendfile_userdef_write_app_template(const char *filepath, struct ReportList *reports);

bool BKE_blendfile_userdef_write_all(struct ReportList *reports);

struct WorkspaceConfigFileData *BKE_blendfile_workspace_config_read(const char *filepath,
                                                                    const void *filebuf,
                                                                    int filelength,
                                                                    struct ReportList *reports);
bool BKE_blendfile_workspace_config_write(struct Main *bmain,
                                          const char *filepath,
                                          struct ReportList *reports);
void BKE_blendfile_workspace_config_data_free(struct WorkspaceConfigFileData *workspace_config);

/* Partial blend file writing. */

void BKE_blendfile_write_partial_tag_ID(struct ID *id, bool set);
void BKE_blendfile_write_partial_begin(struct Main *bmain_src);
/**
 * \param remap_mode: Choose the kind of path remapping or none #eBLO_WritePathRemap.
 * \return Success.
 */
bool BKE_blendfile_write_partial(struct Main *bmain_src,
                                 const char *filepath,
                                 int write_flags,
                                 int remap_mode,
                                 struct ReportList *reports);
void BKE_blendfile_write_partial_end(struct Main *bmain_src);

#ifdef __cplusplus
}
#endif
