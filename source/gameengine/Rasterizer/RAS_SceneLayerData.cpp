#include "RAS_SceneLayerData.h"

extern "C" {
#  include "DRW_render.h"
}

RAS_SceneLayerData::RAS_SceneLayerData(EEVEE_SceneLayerData& data)
	:m_data(data)
{
	m_shadowStoreGroup = DRW_shgroup_create(EEVEE_shadow_store_shader_get(), nullptr);
	DRW_shgroup_uniform_buffer(m_shadowStoreGroup, "shadowCube", &m_data.shadow_cube_target);//shadow_color_cube_target);
	DRW_shgroup_uniform_block(m_shadowStoreGroup, "shadow_render_block", m_data.shadow_render_ubo);
}

RAS_SceneLayerData::~RAS_SceneLayerData()
{
	if (m_shadowStoreGroup) {
		DRW_shgroup_free(m_shadowStoreGroup);
	}
}

const EEVEE_SceneLayerData& RAS_SceneLayerData::GetData() const
{
	return m_data;
}

EEVEE_Light& RAS_SceneLayerData::GetLight(unsigned short id)
{
	return m_data.lamps->light_data[id];
}

EEVEE_ShadowCube& RAS_SceneLayerData::GetShadowCube(unsigned short id)
{
	return m_data.lamps->shadow_cube_data[id];
}

EEVEE_ShadowRender& RAS_SceneLayerData::GetShadowRender()
{
	return m_data.lamps->shadow_render_data;
}

void RAS_SceneLayerData::FlushLightData(unsigned short lightCount)
{
	EEVEE_LampsInfo *linfo = m_data.lamps;

	linfo->num_light = lightCount;
	DRW_uniformbuffer_update(m_data.light_ubo, &linfo->light_data);
	DRW_uniformbuffer_update(m_data.shadow_ubo, &linfo->shadow_cube_data);
}

void RAS_SceneLayerData::PrepareShadowRender()
{
	static float clear_color[4] = {FLT_MAX, FLT_MAX, FLT_MAX, 0.0f};
	EEVEE_LampsInfo *linfo = m_data.lamps;

	DRW_uniformbuffer_update(m_data.shadow_render_ubo, &linfo->shadow_render_data);
	//DRW_framebuffer_bind(m_data.shadow_cube_target_fb);
	DRW_framebuffer_clear(true, true, false, clear_color, 1.0f);
}

void RAS_SceneLayerData::PrepareShadowStore()
{
	//DRW_framebuffer_bind(m_data.shadow_cube_fb);
	DRW_bind_shader_shgroup(m_shadowStoreGroup);
}

