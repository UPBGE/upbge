#ifndef __RAS_OVERRIDE_SHADER_H__
#define __RAS_OVERRIDE_SHADER_H__

#include "RAS_MaterialShader.h"

#include "GPU_shader.h"

/** \brief Override shader used to draw geometry in case of shadow, wire etc...
 */
class RAS_OverrideShader : public RAS_MaterialShader
{
protected:
	GPUShader *m_shader;

	short m_posLoc;
	short m_mvpLoc;

public:
	RAS_OverrideShader(GPUShader *shader);
	RAS_OverrideShader(GPUBuiltinShader type);
	virtual ~RAS_OverrideShader();

	virtual bool IsValid() const;
	virtual void Activate();
	virtual void Desactivate();
	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata);
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const;
};

#endif  // __RAS_OVERRIDE_SHADER_H__
