#ifndef __RAS_TEXT_SHADER_H__
#define __RAS_TEXT_SHADER_H__

#include "RAS_IMaterialShader.h"

struct GPUShader;

class RAS_TextShader : public RAS_IMaterialShader
{
public:
	RAS_TextShader();
	virtual ~RAS_TextShader();

	virtual void Prepare(RAS_Rasterizer *rasty);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Deactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer);
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);

	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const;

	static RAS_TextShader *GetSingleton();
};

#endif  // __RAS_TEXT_SHADER_H__
