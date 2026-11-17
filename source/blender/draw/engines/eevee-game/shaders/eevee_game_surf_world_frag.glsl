/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * World radiance for the lighting capture: EEVEE's world surface without the
 * probe based background blur and the render pass outputs. Drawn into the
 * faces of a cube map, one full-screen triangle per face view.
 */

#include "infos/eevee_game_infos.hh"
#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_world)
FRAGMENT_SHADER_CREATE_INFO(eevee_game_surf_world)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_colorspace_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

float4 closure_to_rgba(Closure /*cl*/)
{
  return float4(0.0f);
}

void main()
{
  init_globals();
  /* View position is passed to keep accuracy. */
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load(WorldPoint{0});

  nodetree_surface(0.0f);

  g_holdout = saturate(g_holdout);
  out_background = float4(colorspace_safe_color(g_emission) * (1.0f - g_holdout), 1.0f);
}
