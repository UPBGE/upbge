#ifndef __RAS_SHADOW_SHADER_H__
#define __RAS_SHADOW_SHADER_H__

#include "RAS_OverrideShader.h"

class RAS_ShadowShader : public RAS_OverrideShader
{
public:
	RAS_ShadowShader();
	virtual ~RAS_ShadowShader();

	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata);
};

#endif  // __RAS_SHADOW_SHADER_H__
