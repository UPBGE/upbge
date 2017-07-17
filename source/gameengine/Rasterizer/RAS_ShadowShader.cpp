#include "RAS_ShadowShader.h"
#include "RAS_MeshUser.h"

extern "C" {
#  include "GPU_uniformbuffer.h"
#  include "eevee_private.h"
}

RAS_ShadowShader::RAS_ShadowShader()
	:RAS_OverrideShader(EEVEE_shadow_shader_get())
{
	m_matLoc = GPU_shader_get_uniform(m_shader, "ShadowModelMatrix");
	m_srdLoc = GPU_shader_get_uniform_block(m_shader, "shadow_render_block");
}

RAS_ShadowShader::~RAS_ShadowShader()
{
}

void RAS_ShadowShader::Activate(EEVEE_SceneLayerData* sldata)
{
	RAS_OverrideShader::Activate(sldata);

	GPUUniformBuffer *ubo = sldata->shadow_render_ubo;
	GPU_uniformbuffer_bind(ubo, 0);
	GPU_shader_uniform_buffer(m_shader, m_srdLoc, ubo);
}

void RAS_ShadowShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata)
{
	GPU_shader_uniform_vector(m_shader, m_matLoc, 16, 1, (float *)meshUser->GetMatrix());
}
