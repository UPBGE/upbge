/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <cerrno>
#include <cstdlib>

#include "BLI_path_utils.hh" /* For assertions. */

#include "IMB_colormanagement.hh"
#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "CLG_log.h"

static CLG_LogRef LOG = {"image.write"};

bool IMB_save_image(ImBuf *ibuf, const char *filepath, const int flags)
{
  errno = 0;

  BLI_assert(!BLI_path_is_rel(filepath));

  if (ibuf == nullptr) {
    return false;
  }
  ibuf->flags = flags;

  const ImFileType *type = IMB_file_type_from_ibuf(ibuf);
  if (type == nullptr || type->save == nullptr) {
    CLOG_ERROR(&LOG, "Couldn't save image to \"%s\"", filepath);
    return false;
  }

  /* If writing byte image from float buffer, create a byte buffer for writing.
   *
   * For color managed image writing, IMB_colormanagement_imbuf_for_write should
   * have already created this byte buffer. This is a basic fallback for other
   * cases where we do not have a specific desired output colorspace. */
  if (!(type->flag & IM_FTYPE_FLOAT)) {
    if (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data) {
      ibuf->byte_buffer.colorspace = colormanage_colorspace_get_roled(COLOR_ROLE_DEFAULT_BYTE);
      IMB_byte_from_float(ibuf);
    }
  }

  return type->save(ibuf, filepath, flags);
}
