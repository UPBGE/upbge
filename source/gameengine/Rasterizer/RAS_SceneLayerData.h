#ifndef __RAS_SCENE_LAYER_DATA_H__
#define __RAS_SCENE_LAYER_DATA_H__

extern "C" {
#  include "eevee_private.h"
}

class RAS_SceneLayerData
{
private:
	EEVEE_SceneLayerData m_data;
	DRWShadingGroup *m_shadowStoreGroup;

public:
	RAS_SceneLayerData(EEVEE_SceneLayerData& data);
	~RAS_SceneLayerData();

	/// Get direct eevee scene layer data, used only for shader creation from eevee and DRW functions.
	const EEVEE_SceneLayerData& GetData() const;

	EEVEE_Light& GetLight(unsigned short id);
	EEVEE_ShadowCube& GetShadowCube(unsigned short id);
	EEVEE_ShadowRender& GetShadowRender();

	void FlushLightData(unsigned short lightCount);

	void PrepareShadowRender();
	void PrepareShadowStore(int shadowid);
};

#endif  // __RAS_SCENE_LAYER_DATA_H__
