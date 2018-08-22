#include "RAS_IMaterialShader.h"

RAS_IMaterialShader::RAS_IMaterialShader()
	:m_geomMode(GEOM_NORMAL),
	m_downwardNode(this, &m_nodeData, &RAS_IMaterialShader::BindNode, &RAS_IMaterialShader::UnbindNode),
	m_upwardNode(this, &m_nodeData, &RAS_IMaterialShader::BindNode, &RAS_IMaterialShader::UnbindNode)
{
	m_nodeData.m_shader = this;
}

RAS_IMaterialShader::~RAS_IMaterialShader()
{
}

void RAS_IMaterialShader::BindNode(const RAS_ShaderNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	Activate(managerData->m_rasty);
}

void RAS_IMaterialShader::UnbindNode(const RAS_ShaderNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	Deactivate(managerData->m_rasty);
}

RAS_ShaderDownwardNode& RAS_IMaterialShader::GetDownwardNode()
{
	return m_downwardNode;
}

RAS_ShaderUpwardNode& RAS_IMaterialShader::GetUpwardNode()
{
	return m_upwardNode;
}

RAS_IMaterialShader::GeomType RAS_IMaterialShader::GetGeomMode() const
{
	return m_geomMode;
}
