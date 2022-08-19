#pragma BLENDER_REQUIRE(gpu_shader_common_color_utils.glsl)

void mix_blend(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = mix(col1, col2, fac);
  outcol.a = col1.a;
}

void mix_add(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = mix(col1, col1 + col2, fac);
  outcol.a = col1.a;
}

void mix_mult(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = mix(col1, col1 * col2, fac);
  outcol.a = col1.a;
}

void mix_screen(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = vec4(1.0) - (vec4(facm) + fac * (vec4(1.0) - col2)) * (vec4(1.0) - col1);
  outcol.a = col1.a;
}

void mix_overlay(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = col1;

  if (outcol.r < 0.5) {
    outcol.r *= facm + 2.0 * fac * col2.r;
  }
  else {
    outcol.r = 1.0 - (facm + 2.0 * fac * (1.0 - col2.r)) * (1.0 - outcol.r);
  }

  if (outcol.g < 0.5) {
    outcol.g *= facm + 2.0 * fac * col2.g;
  }
  else {
    outcol.g = 1.0 - (facm + 2.0 * fac * (1.0 - col2.g)) * (1.0 - outcol.g);
  }

  if (outcol.b < 0.5) {
    outcol.b *= facm + 2.0 * fac * col2.b;
  }
  else {
    outcol.b = 1.0 - (facm + 2.0 * fac * (1.0 - col2.b)) * (1.0 - outcol.b);
  }
}

void mix_sub(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = mix(col1, col1 - col2, fac);
  outcol.a = col1.a;
}

void mix_div(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = vec4(vec3(0.0), col1.a);

  if (col2.r != 0.0) {
    outcol.r = facm * col1.r + fac * col1.r / col2.r;
  }
  if (col2.g != 0.0) {
    outcol.g = facm * col1.g + fac * col1.g / col2.g;
  }
  if (col2.b != 0.0) {
    outcol.b = facm * col1.b + fac * col1.b / col2.b;
  }
}

/* A variant of mix_div that fallback to the first color upon zero division. */
void mix_div_fallback(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = col1;

  if (col2.r != 0.0) {
    outcol.r = facm * outcol.r + fac * outcol.r / col2.r;
  }
  if (col2.g != 0.0) {
    outcol.g = facm * outcol.g + fac * outcol.g / col2.g;
  }
  if (col2.b != 0.0) {
    outcol.b = facm * outcol.b + fac * outcol.b / col2.b;
  }
}

void mix_diff(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = mix(col1, abs(col1 - col2), fac);
  outcol.a = col1.a;
}

void mix_dark(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol.rgb = mix(col1.rgb, min(col1.rgb, col2.rgb), fac);
  outcol.a = col1.a;
}

void mix_light(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol.rgb = mix(col1.rgb, max(col1.rgb, col2.rgb), fac);
  outcol.a = col1.a;
}

void mix_dodge(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = col1;

  if (outcol.r != 0.0) {
    float tmp = 1.0 - fac * col2.r;
    if (tmp <= 0.0) {
      outcol.r = 1.0;
    }
    else if ((tmp = outcol.r / tmp) > 1.0) {
      outcol.r = 1.0;
    }
    else {
      outcol.r = tmp;
    }
  }
  if (outcol.g != 0.0) {
    float tmp = 1.0 - fac * col2.g;
    if (tmp <= 0.0) {
      outcol.g = 1.0;
    }
    else if ((tmp = outcol.g / tmp) > 1.0) {
      outcol.g = 1.0;
    }
    else {
      outcol.g = tmp;
    }
  }
  if (outcol.b != 0.0) {
    float tmp = 1.0 - fac * col2.b;
    if (tmp <= 0.0) {
      outcol.b = 1.0;
    }
    else if ((tmp = outcol.b / tmp) > 1.0) {
      outcol.b = 1.0;
    }
    else {
      outcol.b = tmp;
    }
  }
}

void mix_burn(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float tmp, facm = 1.0 - fac;

  outcol = col1;

  tmp = facm + fac * col2.r;
  if (tmp <= 0.0) {
    outcol.r = 0.0;
  }
  else if ((tmp = (1.0 - (1.0 - outcol.r) / tmp)) < 0.0) {
    outcol.r = 0.0;
  }
  else if (tmp > 1.0) {
    outcol.r = 1.0;
  }
  else {
    outcol.r = tmp;
  }

  tmp = facm + fac * col2.g;
  if (tmp <= 0.0) {
    outcol.g = 0.0;
  }
  else if ((tmp = (1.0 - (1.0 - outcol.g) / tmp)) < 0.0) {
    outcol.g = 0.0;
  }
  else if (tmp > 1.0) {
    outcol.g = 1.0;
  }
  else {
    outcol.g = tmp;
  }

  tmp = facm + fac * col2.b;
  if (tmp <= 0.0) {
    outcol.b = 0.0;
  }
  else if ((tmp = (1.0 - (1.0 - outcol.b) / tmp)) < 0.0) {
    outcol.b = 0.0;
  }
  else if (tmp > 1.0) {
    outcol.b = 1.0;
  }
  else {
    outcol.b = tmp;
  }
}

void mix_hue(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = col1;

  vec4 hsv, hsv2, tmp;
  rgb_to_hsv(col2, hsv2);

  if (hsv2.y != 0.0) {
    rgb_to_hsv(outcol, hsv);
    hsv.x = hsv2.x;
    hsv_to_rgb(hsv, tmp);

    outcol = mix(outcol, tmp, fac);
    outcol.a = col1.a;
  }
}

void mix_sat(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = col1;

  vec4 hsv, hsv2;
  rgb_to_hsv(outcol, hsv);

  if (hsv.y != 0.0) {
    rgb_to_hsv(col2, hsv2);

    hsv.y = facm * hsv.y + fac * hsv2.y;
    hsv_to_rgb(hsv, outcol);
  }
}

void mix_val(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  vec4 hsv, hsv2;
  rgb_to_hsv(col1, hsv);
  rgb_to_hsv(col2, hsv2);

  hsv.z = facm * hsv.z + fac * hsv2.z;
  hsv_to_rgb(hsv, outcol);
}

void mix_color(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  outcol = col1;

  vec4 hsv, hsv2, tmp;
  rgb_to_hsv(col2, hsv2);

  if (hsv2.y != 0.0) {
    rgb_to_hsv(outcol, hsv);
    hsv.x = hsv2.x;
    hsv.y = hsv2.y;
    hsv_to_rgb(hsv, tmp);

    outcol = mix(outcol, tmp, fac);
    outcol.a = col1.a;
  }
}

void mix_soft(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  float facm = 1.0 - fac;

  vec4 one = vec4(1.0);
  vec4 scr = one - (one - col2) * (one - col1);
  outcol = facm * col1 + fac * ((one - col1) * col2 * col1 + col1 * scr);
  outcol.a = col1.a;
}

void mix_linear(float fac, vec4 col1, vec4 col2, out vec4 outcol)
{
  outcol = col1 + fac * (2.0 * (col2 - vec4(0.5)));
  outcol.a = col1.a;
}

void clamp_color(vec4 vec, const vec4 min, const vec4 max, out vec4 out_vec)
{
  out_vec = clamp(vec, min, max);
}

void multiply_by_alpha(float factor, vec4 color, out float result)
{
  result = factor * color.a;
}
