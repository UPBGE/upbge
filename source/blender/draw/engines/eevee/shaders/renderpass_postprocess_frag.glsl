
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_geom_lib.glsl)

#define PASS_POST_UNDEFINED 0
#define PASS_POST_ACCUMULATED_COLOR 1
#define PASS_POST_ACCUMULATED_COLOR_ALPHA 2
#define PASS_POST_ACCUMULATED_LIGHT 3
#define PASS_POST_ACCUMULATED_VALUE 4
#define PASS_POST_DEPTH 5
#define PASS_POST_AO 6
#define PASS_POST_NORMAL 7
#define PASS_POST_TWO_LIGHT_BUFFERS 8
#define PASS_POST_ACCUMULATED_TRANSMITTANCE_COLOR 9

uniform int postProcessType;
uniform int currentSample;

uniform depth2D depthBuffer;
uniform sampler2D inputBuffer;
uniform sampler2D inputSecondLightBuffer;
uniform sampler2D inputColorBuffer;
uniform sampler2D inputTransmittanceBuffer;

out vec4 fragColor;

vec3 safe_divide_even_color(vec3 a, vec3 b)
{
  vec3 result = vec3((b.r != 0.0) ? a.r / b.r : 0.0,
                     (b.g != 0.0) ? a.g / b.g : 0.0,
                     (b.b != 0.0) ? a.b / b.b : 0.0);
  /* try to get gray even if b is zero */
  if (b.r == 0.0) {
    if (b.g == 0.0) {
      result = result.bbb;
    }
    else if (b.b == 0.0) {
      result = result.ggg;
    }
    else {
      result.r = 0.5 * (result.g + result.b);
    }
  }
  else if (b.g == 0.0) {
    if (b.b == 0.0) {
      result = result.rrr;
    }
    else {
      result.g = 0.5 * (result.r + result.b);
    }
  }
  else if (b.b == 0.0) {
    result.b = 0.5 * (result.r + result.g);
  }

  return result;
}

void main()
{
  vec4 color = vec4(0.0, 0.0, 0.0, 1.0);
  ivec2 texel = ivec2(gl_FragCoord.xy);

  if (postProcessType == PASS_POST_DEPTH) {
    float depth = texelFetch(depthBuffer, texel, 0).r;
    if (depth == 1.0f) {
      depth = 1e10;
    }
    else {
      depth = -get_view_z_from_depth(depth);
    }
    color.rgb = vec3(depth);
  }
  else if (postProcessType == PASS_POST_AO) {
    float ao_accum = texelFetch(inputBuffer, texel, 0).r;
    color.rgb = vec3(min(1.0, ao_accum / currentSample));
  }
  else if (postProcessType == PASS_POST_NORMAL) {
    float depth = texelFetch(depthBuffer, texel, 0).r;
    vec2 encoded_normal = texelFetch(inputBuffer, texel, 0).rg;
    /* decode the normals only when they are valid. otherwise the result buffer will be filled
     * with NaN's */
    if (depth != 1.0 && any(notEqual(encoded_normal, vec2(0.0)))) {
      vec3 decoded_normal = normal_decode(texelFetch(inputBuffer, texel, 0).rg, vec3(0.0));
      vec3 world_normal = transform_direction(ViewMatrixInverse, decoded_normal);
      color.rgb = world_normal;
    }
    else {
      color.rgb = vec3(0.0);
    }
  }
  else if (postProcessType == PASS_POST_ACCUMULATED_VALUE) {
    float accumulated_value = texelFetch(inputBuffer, texel, 0).r;
    color.rgb = vec3(accumulated_value / currentSample);
  }
  else if (postProcessType == PASS_POST_ACCUMULATED_COLOR) {
    vec3 accumulated_color = texelFetch(inputBuffer, texel, 0).rgb;
    color.rgb = (accumulated_color / currentSample);
  }
  else if (postProcessType == PASS_POST_ACCUMULATED_COLOR_ALPHA) {
    vec4 accumulated_color = texelFetch(inputBuffer, texel, 0);
    color = (accumulated_color / currentSample);
  }
  else if (postProcessType == PASS_POST_ACCUMULATED_TRANSMITTANCE_COLOR) {
    vec3 accumulated_color = texelFetch(inputBuffer, texel, 0).rgb;
    vec3 transmittance = texelFetch(inputTransmittanceBuffer, texel, 0).rgb;
    color.rgb = (accumulated_color / currentSample) * (transmittance / currentSample);
  }
  else if (postProcessType == PASS_POST_ACCUMULATED_LIGHT) {
    vec3 accumulated_light = texelFetch(inputBuffer, texel, 0).rgb;
    vec3 accumulated_color = texelFetch(inputColorBuffer, texel, 0).rgb;
    color.rgb = safe_divide_even_color(accumulated_light, accumulated_color);
  }
  else if (postProcessType == PASS_POST_TWO_LIGHT_BUFFERS) {
    vec3 accumulated_light = texelFetch(inputBuffer, texel, 0).rgb +
                             texelFetch(inputSecondLightBuffer, texel, 0).rgb;
    vec3 accumulated_color = texelFetch(inputColorBuffer, texel, 0).rgb;
    color.rgb = safe_divide_even_color(accumulated_light, accumulated_color);
  }
  else {
    /* Output error color: Unknown how to post process this pass. */
    color.rgb = vec3(1.0, 0.0, 1.0);
  }

  fragColor = color;
}
