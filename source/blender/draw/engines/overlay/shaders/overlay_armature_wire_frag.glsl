/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_armature_info.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_armature_wire)

#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

void main()
{
  lineOutput = pack_line_data(gl_FragCoord.xy, edgeStart, edgePos);
  fragColor = float4(finalColor.rgb, finalColor.a * alpha);
  select_id_output(select_id);
}
