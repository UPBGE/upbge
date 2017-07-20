#ifndef __RAS_SHADOW_SHADER_H__
#define __RAS_SHADOW_SHADER_H__

#include "RAS_OverrideShader.h"

class RAS_SceneLayerData;

class RAS_ShadowShader : public RAS_OverrideShader
{
private:
	short m_matLoc;

public:
	RAS_ShadowShader(RAS_SceneLayerData *layerData);
	virtual ~RAS_ShadowShader();

	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser);
};

#endif  // __RAS_SHADOW_SHADER_H__
