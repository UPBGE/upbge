#include "RAS_TextShader.h"

RAS_TextShader::RAS_TextShader()
{
}

RAS_TextShader::~RAS_TextShader()
{
}

void RAS_TextShader::Prepare(RAS_Rasterizer *rasty)
{
}

void RAS_TextShader::Activate(RAS_Rasterizer *rasty)
{
}

void RAS_TextShader::Deactivate(RAS_Rasterizer *rasty)
{
}

void RAS_TextShader::ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer)
{
}

void RAS_TextShader::ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans)
{
}

const RAS_AttributeArray::AttribList RAS_TextShader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const
{
	return {};
}

RAS_InstancingBuffer::Attrib RAS_TextShader::GetInstancingAttribs() const
{
	return RAS_InstancingBuffer::DEFAULT_ATTRIBS;
}

RAS_TextShader *RAS_TextShader::GetSingleton()
{
	static RAS_TextShader singleton;
	return &singleton;
}

