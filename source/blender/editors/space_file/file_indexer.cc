/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edfile
 *
 * This file implements the default file browser indexer and has some helper function to work with
 * `FileIndexerEntries`.
 */
#include "file_indexer.h"

#include "MEM_guardedalloc.h"

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

namespace blender::ed::file::indexer {

static eFileIndexerResult read_index(const char *UNUSED(file_name),
                                     FileIndexerEntries *UNUSED(entries),
                                     int *UNUSED(r_read_entries_len),
                                     void *UNUSED(user_data))
{
  return FILE_INDEXER_NEEDS_UPDATE;
}

static void update_index(const char *UNUSED(file_name),
                         FileIndexerEntries *UNUSED(entries),
                         void *UNUSED(user_data))
{
}

constexpr FileIndexerType default_indexer()
{
  FileIndexerType indexer = {nullptr};
  indexer.read_index = read_index;
  indexer.update_index = update_index;
  return indexer;
}

static FileIndexerEntry *file_indexer_entry_create_from_datablock_info(
    const BLODataBlockInfo *datablock_info, const int idcode)
{
  FileIndexerEntry *entry = static_cast<FileIndexerEntry *>(
      MEM_mallocN(sizeof(FileIndexerEntry), __func__));
  entry->datablock_info = *datablock_info;
  entry->idcode = idcode;
  return entry;
}

}  // namespace blender::ed::file::indexer

extern "C" {

void ED_file_indexer_entries_extend_from_datablock_infos(
    FileIndexerEntries *indexer_entries,
    const LinkNode * /* BLODataBlockInfo */ datablock_infos,
    const int idcode)
{
  for (const LinkNode *ln = datablock_infos; ln; ln = ln->next) {
    const BLODataBlockInfo *datablock_info = static_cast<const BLODataBlockInfo *>(ln->link);
    FileIndexerEntry *file_indexer_entry =
        blender::ed::file::indexer::file_indexer_entry_create_from_datablock_info(datablock_info,
                                                                                  idcode);
    BLI_linklist_prepend(&indexer_entries->entries, file_indexer_entry);
  }
}

static void ED_file_indexer_entry_free(void *indexer_entry)
{
  MEM_freeN(indexer_entry);
}

void ED_file_indexer_entries_clear(FileIndexerEntries *indexer_entries)
{
  BLI_linklist_free(indexer_entries->entries, ED_file_indexer_entry_free);
  indexer_entries->entries = nullptr;
}

const FileIndexerType file_indexer_noop = blender::ed::file::indexer::default_indexer();
}
