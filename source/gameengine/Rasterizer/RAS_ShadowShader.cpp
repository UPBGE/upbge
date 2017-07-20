#include "RAS_ShadowShader.h"
#include "RAS_SceneLayerData.h"
#include "RAS_MeshUser.h"

extern "C" {
#  include "DRW_render.h"
}

RAS_ShadowShader::RAS_ShadowShader(RAS_SceneLayerData *layerData)
	:RAS_OverrideShader(EEVEE_shadow_shader_get())
{
	m_matLoc = GPU_shader_get_uniform(m_shader, "ShadowModelMatrix");

	const EEVEE_SceneLayerData& sldata = layerData->GetData();
	DRW_shgroup_uniform_block(m_shGroup, "shadow_render_block", sldata.shadow_render_ubo);
}

RAS_ShadowShader::~RAS_ShadowShader()
{
}

void RAS_ShadowShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser)
{
	GPU_shader_uniform_vector(m_shader, m_matLoc, 16, 1, (float *)meshUser->GetMatrix());
}
