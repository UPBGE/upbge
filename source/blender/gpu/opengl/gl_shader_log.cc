/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gl_shader.hh"

#include "GPU_platform.h"

namespace blender::gpu {

char *GLLogParser::parse_line(char *log_line, GPULogItem &log_item)
{
  /* Skip ERROR: or WARNING:. */
  log_line = skip_severity_prefix(log_line, log_item);
  log_line = skip_separators(log_line, "(: ");

  /* Parse error line & char numbers. */
  if (at_number(log_line)) {
    char *error_line_number_end;
    log_item.cursor.row = parse_number(log_line, &error_line_number_end);
    /* Try to fetch the error character (not always available). */
    if (at_any(error_line_number_end, "(:") && at_number(&error_line_number_end[1])) {
      log_item.cursor.column = parse_number(error_line_number_end + 1, &log_line);
    }
    else {
      log_line = error_line_number_end;
    }
    /* There can be a 3rd number (case of mesa driver). */
    if (at_any(log_line, "(:") && at_number(&log_line[1])) {
      log_item.cursor.source = log_item.cursor.row;
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = parse_number(log_line + 1, &error_line_number_end);
      log_line = error_line_number_end;
    }
  }

  if ((log_item.cursor.row != -1) && (log_item.cursor.column != -1)) {
    if (GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_ANY, GPU_DRIVER_OFFICIAL) ||
        GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_MAC, GPU_DRIVER_OFFICIAL) ||
        GPU_type_matches(GPU_DEVICE_APPLE, GPU_OS_MAC, GPU_DRIVER_OFFICIAL)) {
      /* 0:line */
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = -1;
    }
    else if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_OFFICIAL) &&
             /* WORKAROUND(@fclem): Both Mesa and AMDGPU-PRO are reported as official. */
             StringRefNull(GPU_platform_version()).find(" Mesa ") == -1) {
      /* source:row */
      log_item.cursor.source = log_item.cursor.row;
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = -1;
      log_item.source_base_row = true;
    }
    else {
      /* line:char */
    }
  }

  log_line = skip_separators(log_line, ":) ");

  /* Skip to message. Avoid redundant info. */
  log_line = skip_severity_keyword(log_line, log_item);
  log_line = skip_separators(log_line, ":) ");

  return log_line;
}

char *GLLogParser::skip_severity_prefix(char *log_line, GPULogItem &log_item)
{
  return skip_severity(log_line, log_item, "ERROR", "WARNING");
}

char *GLLogParser::skip_severity_keyword(char *log_line, GPULogItem &log_item)
{
  return skip_severity(log_line, log_item, "error", "warning");
}

}  // namespace blender::gpu
