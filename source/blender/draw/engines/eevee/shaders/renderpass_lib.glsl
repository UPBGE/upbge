#define EEVEE_AOV_HASH_COLOR_TYPE_MASK 1u

/* ---------------------------------------------------------------------- */
/** \name Resources
 * \{ */

layout(std140) uniform renderpass_block
{
  bool renderPassDiffuse;
  bool renderPassDiffuseLight;
  bool renderPassGlossy;
  bool renderPassGlossyLight;
  bool renderPassEmit;
  bool renderPassSSSColor;
  bool renderPassEnvironment;
  bool renderPassAOV;
  uint renderPassAOVActive;
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Functions
 * \{ */

vec3 render_pass_diffuse_mask(vec3 diffuse_light)
{
  return renderPassDiffuse ? (renderPassDiffuseLight ? diffuse_light : vec3(1.0)) : vec3(0.0);
}

vec3 render_pass_glossy_mask(vec3 specular_light)
{
  return renderPassGlossy ? (renderPassGlossyLight ? specular_light : vec3(1.0)) : vec3(0.0);
}

vec3 render_pass_emission_mask(vec3 emission_light)
{
  return renderPassEmit ? emission_light : vec3(0.0);
}

bool render_pass_aov_is_color()
{
  return (renderPassAOVActive & EEVEE_AOV_HASH_COLOR_TYPE_MASK) != 0u;
}

uint render_pass_aov_hash()
{
  return renderPassAOVActive & ~EEVEE_AOV_HASH_COLOR_TYPE_MASK;
}

/** \} */
