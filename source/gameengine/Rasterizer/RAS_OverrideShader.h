#ifndef __RAS_OVERRIDE_SHADER_H__
#define __RAS_OVERRIDE_SHADER_H__

#include "RAS_MaterialShader.h"

#include "GPU_shader.h"

struct DRWShadingGroup;
class RAS_Rasterizer;

/** \brief Override shader used to draw geometry in case of shadow, wire etc...
 */
class RAS_OverrideShader : public RAS_MaterialShader
{
protected:
	GPUShader *m_shader;
	DRWShadingGroup *m_shGroup;

	short m_posLoc;
	short m_mvpLoc;

public:
	RAS_OverrideShader(GPUShader *shader);
	RAS_OverrideShader(GPUBuiltinShader type);
	virtual ~RAS_OverrideShader();

	virtual bool IsValid() const;
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate();
	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser);
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const;
};

#endif  // __RAS_OVERRIDE_SHADER_H__
