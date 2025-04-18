/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Based on :
 * McGuire and Bavoil, Weighted Blended Order-Independent Transparency, Journal of
 * Computer Graphics Techniques (JCGT), vol. 2, no. 2, 122–141, 2013
 */

#include "infos/workbench_transparent_resolve_info.hh"

FRAGMENT_SHADER_CREATE_INFO(workbench_transparent_resolve)

void main()
{
  /* Revealage is actually stored in transparentAccum alpha channel.
   * This is a workaround to older hardware not having separate blend equation per render target.
   */
  float4 trans_accum = texture(transparentAccum, screen_uv);
  float trans_weight = texture(transparentRevealage, screen_uv).r;
  float trans_reveal = trans_accum.a;

  /* Listing 4 */
  fragColor.rgb = trans_accum.rgb / clamp(trans_weight, 1e-4f, 5e4f);
  fragColor.a = 1.0f - trans_reveal;
}
