#include "RAS_OverrideShader.h"

#include "BLI_utildefines.h"

RAS_OverrideShader::RAS_OverrideShader(GPUShader *shader)
	:m_shader(shader)
{
	BLI_assert(shader);
}

RAS_OverrideShader::RAS_OverrideShader(GPUBuiltinShader type)
	:RAS_OverrideShader(GPU_shader_get_builtin_shader(type))
{
}

RAS_OverrideShader::~RAS_OverrideShader()
{
}

bool RAS_OverrideShader::IsValid() const
{
	return true;
}

void RAS_OverrideShader::Activate()
{
	GPU_shader_bind(m_shader);
}

void RAS_OverrideShader::Desactivate()
{
	GPU_shader_unbind();
}

void RAS_OverrideShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata)
{
}

const RAS_AttributeArray::AttribList RAS_OverrideShader::GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const
{
	return RAS_AttributeArray::AttribList();
}
