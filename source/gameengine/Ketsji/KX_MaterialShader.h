#ifndef __KX_CUSTOM_MATERIAL_SHADER_H__
#define __KX_CUSTOM_MATERIAL_SHADER_H__

#include "RAS_IMaterialShader.h"
#include "KX_Shader.h"

class BL_Material;

/** \brief material shader using a custom shader.
 */
class KX_MaterialShader : public RAS_IMaterialShader, public KX_Shader
{
	Py_Header

public:
	enum CallbacksType {
		CALLBACKS_BIND = 0,
		CALLBACKS_OBJECT,
		CALLBACKS_MAX
	};

	enum AttribTypes {
		SHD_NONE = 0,
		SHD_TANGENT = 1
	};

private:
	BL_Material *m_material;
	bool m_useLightings;
	AttribTypes m_attr;
	int m_alphaBlend;

#ifdef WITH_PYTHON
	PyObject *m_callbacks[CALLBACKS_MAX];
#endif  // WITH_PYTHON

	virtual bool LinkProgram();

public:
	KX_MaterialShader(BL_Material *material, bool useLightings, int alphaBlend);
	virtual ~KX_MaterialShader();

	bool Ok() const;
	bool GetError() const;

#ifdef WITH_PYTHON
	PyObject *GetCallbacks(CallbacksType type);
	void SetCallbacks(CallbacksType type, PyObject *callbacks);
#endif // WITH_PYTHON

	/// \section Material shader interface.

	virtual void Prepare(RAS_Rasterizer *rasty);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Deactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer);
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const;

#ifdef WITH_PYTHON
	static PyObject *pyattr_get_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	EXP_PYMETHOD_DOC(KX_MaterialShader, setAttrib);
#endif  // WITH_PYTHON
};

#endif  // __KX_CUSTOM_MATERIAL_SHADER_H__
