#include "RAS_OverrideShader.h"
#include "RAS_MeshUser.h"

#include "BLI_utildefines.h"

RAS_OverrideShader::RAS_OverrideShader(GPUShader *shader)
	:m_shader(shader)
{
	BLI_assert(shader);
	m_posLoc = GPU_shader_get_attribute(m_shader, "pos");
	m_mvpLoc = GPU_shader_get_uniform(m_shader, "ModelViewProjectionMatrix");
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

void RAS_OverrideShader::Activate(EEVEE_SceneLayerData *sldata)
{
	GPU_shader_bind(m_shader);
}

void RAS_OverrideShader::Desactivate()
{
	GPU_shader_unbind();
}

void RAS_OverrideShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser, EEVEE_SceneLayerData *sldata)
{
	const MT_Matrix4x4 mat = rasty->GetPersMatrix() * MT_Matrix4x4(meshUser->GetMatrix());
	float mvp[16];
	mat.getValue(mvp);

	GPU_shader_uniform_vector(m_shader, m_mvpLoc, 16, 1, (float *)mvp);
}

const RAS_AttributeArray::AttribList RAS_OverrideShader::GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const
{
	return {{m_posLoc, RAS_AttributeArray::RAS_ATTRIB_POS, 0}};
}
