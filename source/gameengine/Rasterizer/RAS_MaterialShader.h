#ifndef __RAS_MATERIAL_SHADER_H__
#define __RAS_MATERIAL_SHADER_H__

#include "RAS_AttributeArray.h" // For RAS_AttributeArray::AttribList.
#include "RAS_MeshObject.h" // For RAS_MeshObject::LayersInfo.

class RAS_MeshUser;
class RAS_Rasterizer;

class RAS_MaterialShader
{
public:
	RAS_MaterialShader() = default;
	virtual ~RAS_MaterialShader() = default;

	/// Return true when the shader can be bound.
	virtual bool IsValid() const = 0;
	// Bind the shader and mainly update global uniforms.
	virtual void Activate(RAS_Rasterizer *rasty) = 0;
	/// Unbind the shader.
	virtual void Desactivate() = 0;
	/// Update the shader with mesh user data as model matrix.
	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser) = 0;
	/** Return a map of the corresponding attribut layer for a given attribut index.
	 * \param layers The list of the mesh layers used to link with uv and color material attributes.
	 * \return The map of attributes layers.
	 */
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const = 0;
};

#endif  // __RAS_MATERIAL_SHADER_H__
