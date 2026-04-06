/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Film present — final blit from the HDR combined texture to the viewport
 * default framebuffer.
 *
 * Applies linear exposure before output. The display transform (OCIO) is
 * handled by Blender's compositor layer after this pass, so we output
 * linear scene-referred values.
 *
 * When FSR3 is active the input is already at display resolution (FSR wrote
 * there). When FSR3 is OFF the input is the post-processed render_res buffer.
 * Either way this shader is a straight blit — no upscaling logic here.
 */

/* Bound by eevee_game_film_present ShaderCreateInfo */
uniform sampler2D combined_tx;
uniform float     exposure;

out vec4 fragColor;

void main()
{
  /* gl_FragCoord.xy is in window pixels [0, viewport_size).
   * Divide by textureSize to get [0, 1) UVs regardless of resolution mismatch. */
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(combined_tx, 0));

  vec4 color = texture(combined_tx, uv);

  /* Linear exposure: multiply scene-linear radiance by the exposure scalar.
   * exposure = 2^(EV100 stops) as computed by the camera/scene settings.
   * This matches the EEVEE film pass exposure application. */
  color.rgb *= exposure;

  /* Alpha is set to 1 so the viewport compositor treats this as fully opaque.
   * The game viewport does not composite over a background — it owns the full
   * framebuffer. */
  fragColor = vec4(color.rgb, 1.0);
}
