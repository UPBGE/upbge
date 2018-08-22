#ifndef __RAS_OVERRIDE_SHADER_H__
#define __RAS_OVERRIDE_SHADER_H__

#include "RAS_IMaterialShader.h"

struct GPUShader;

class RAS_OverrideShader : public RAS_IMaterialShader
{
public:
	enum Type
	{
		RAS_OVERRIDE_SHADER_BLACK = 0,
		RAS_OVERRIDE_SHADER_BLACK_INSTANCING,
		RAS_OVERRIDE_SHADER_SHADOW_VARIANCE,
		RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING,
		RAS_OVERRIDE_SHADER_MAX
	};

private:
	GPUShader *m_shader;

	RAS_OverrideShader(Type type);
	virtual ~RAS_OverrideShader();

public:
	virtual void Prepare(RAS_Rasterizer *rasty);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Deactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer);
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);

	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const;

	static void InitShaders();
	static void DeinitShaders();
	static RAS_OverrideShader *GetShader(RAS_OverrideShader::Type type);
};

#endif  // __RAS_OVERRIDE_SHADER_H__
