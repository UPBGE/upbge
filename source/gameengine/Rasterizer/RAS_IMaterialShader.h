#ifndef __RAS_IMATERIAL_SHADER_H__
#define __RAS_IMATERIAL_SHADER_H__

#include "RAS_RenderNode.h"
#include "RAS_AttributeArray.h"
#include "RAS_Mesh.h"
#include "RAS_InstancingBuffer.h"

class RAS_IMaterial;

class RAS_IMaterialShader
{
public:
	/// Enumeration of different mode processing the geometry of the mesh.
	enum GeomType
	{
		GEOM_NORMAL,
		GEOM_INSTANCING,
		GEOM_MAX
	};

protected:
	RAS_ShaderNodeData m_nodeData;
	GeomType m_geomMode;

private:
	RAS_ShaderDownwardNode m_downwardNode;
	RAS_ShaderUpwardNode m_upwardNode;

	void BindNode(const RAS_ShaderNodeTuple& tuple);
	void UnbindNode(const RAS_ShaderNodeTuple& tuple);

public:
	RAS_IMaterialShader();
	~RAS_IMaterialShader();

	RAS_ShaderDownwardNode& GetDownwardNode();
	RAS_ShaderUpwardNode& GetUpwardNode();
	/// Return the geometry mode used.
	GeomType GetGeomMode() const;

	/// Prepare the shader data for rendering.
	virtual void Prepare(RAS_Rasterizer *rasty) = 0;
	/// Bind the shader.
	virtual void Activate(RAS_Rasterizer *rasty) = 0;
	/// Unbind the shader.
	virtual void Deactivate(RAS_Rasterizer *rasty) = 0;
	/// Setup vertex attributes for rendering using geometry instancing.
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer) = 0;
	/// Setup per mesh user (object) uniforms.
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans) = 0;
	/// Generate a list of vertex attributes used by the shader.
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const = 0;
	/// Return attributes category used for instancing, this value tell what attributes must be updated.
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const = 0;
};

#endif // __RAS_IMATERIAL_SHADER_H__
