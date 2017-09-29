#include "RAS_OverrideShader.h"
#include "RAS_MeshUser.h"

#include "BLI_utildefines.h"

extern "C" {
#  include "DRW_render.h"
}

RAS_OverrideShader::RAS_OverrideShader(GPUShader *shader)
	:m_shader(shader)
{
	BLI_assert(shader);

	m_posLoc = GPU_shader_get_attribute(m_shader, "pos");
	m_mvpLoc = GPU_shader_get_uniform(m_shader, "ModelViewProjectionMatrix");

	m_shGroup = DRW_shgroup_create(m_shader, nullptr);
}

RAS_OverrideShader::RAS_OverrideShader(GPUBuiltinShader type)
	:RAS_OverrideShader(GPU_shader_get_builtin_shader(type))
{
}

RAS_OverrideShader::~RAS_OverrideShader()
{
	if (m_shGroup) {
		DRW_shgroup_free(m_shGroup);
	}
}

bool RAS_OverrideShader::IsValid(RAS_Rasterizer::DrawType drawtype) const
{
	return true;
}

void RAS_OverrideShader::Activate(RAS_Rasterizer *rasty)
{
	DRW_bind_shader_shgroup(m_shGroup);
}

void RAS_OverrideShader::Desactivate()
{
}

void RAS_OverrideShader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser)
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
