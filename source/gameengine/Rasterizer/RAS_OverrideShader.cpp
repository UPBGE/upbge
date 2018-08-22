#include "RAS_OverrideShader.h"

#include "GPU_shader.h"
#include "GPU_material.h"

static const GPUBuiltinShader builtinTable[] = {
	GPU_SHADER_BLACK, // RAS_OVERRIDE_SHADER_BLACK
	GPU_SHADER_BLACK_INSTANCING, // RAS_OVERRIDE_SHADER_BLACK_INSTANCING
	GPU_SHADER_VSM_STORE, // RAS_OVERRIDE_SHADER_SHADOW_VARIANCE
	GPU_SHADER_VSM_STORE_INSTANCING // RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING
};

RAS_OverrideShader::RAS_OverrideShader(Type type)
	:m_shader(GPU_shader_get_builtin_shader(builtinTable[type]))
{
}

RAS_OverrideShader::~RAS_OverrideShader()
{
}

void RAS_OverrideShader::Activate(RAS_Rasterizer *rasty)
{
	rasty->SetAlphaBlend(GPU_BLEND_SOLID);
}

void RAS_OverrideShader::Deactivate(RAS_Rasterizer *rasty)
{
}

void RAS_OverrideShader::Prepare(RAS_Rasterizer *rasty)
{
}

void RAS_OverrideShader::ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer)
{
	GPU_shader_bind_instancing_attrib(m_shader, (void *)buffer->GetMatrixOffset(), (void *)buffer->GetPositionOffset());
}

void RAS_OverrideShader::ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans)
{
}

const RAS_AttributeArray::AttribList RAS_OverrideShader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const
{
	return {};
}

RAS_InstancingBuffer::Attrib RAS_OverrideShader::GetInstancingAttribs() const
{
	return RAS_InstancingBuffer::DEFAULT_ATTRIBS;
}

static RAS_OverrideShader *overrideShaders[RAS_OverrideShader::RAS_OVERRIDE_SHADER_MAX];

void RAS_OverrideShader::InitShaders()
{
	for (unsigned short i = 0; i < RAS_OVERRIDE_SHADER_MAX; ++i) {
		overrideShaders[i] = new RAS_OverrideShader((Type)i);
	}
}

void RAS_OverrideShader::DeinitShaders()
{
	for (unsigned short i = 0; i < RAS_OVERRIDE_SHADER_MAX; ++i) {
		delete overrideShaders[i];
	}
}

RAS_OverrideShader *RAS_OverrideShader::GetShader(RAS_OverrideShader::Type type)
{
	return overrideShaders[type];
}

