#include "RAS_ShadowShader.h"

extern "C" {
#  include "eevee_private.h"
}

RAS_ShadowShader::RAS_ShadowShader()
	:RAS_OverrideShader(EEVEE_shadow_shader_get())
{
}

RAS_ShadowShader::~RAS_ShadowShader()
{
}

void RAS_ShadowShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata)
{
	
}
