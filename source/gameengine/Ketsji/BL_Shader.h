
/** \file BL_Shader.h
 *  \ingroup ketsji
 */

#ifndef __BL_SHADER_H__
#define __BL_SHADER_H__

#include "RAS_Shader.h"
#include "RAS_Texture.h" // For RAS_Texture::MaxUnits.
#include "RAS_AttributeArray.h" // For RAS_AttributeArray::AttribList.
#include "RAS_Mesh.h" // For RAS_Mesh::LayersInfo.

#include "EXP_Value.h"

#include "CM_Update.h"

class RAS_MeshSlot;
class RAS_IMaterial;

class BL_Shader : public EXP_Value, public virtual RAS_Shader
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
#ifdef WITH_PYTHON
	PyObject *m_callbacks[CALLBACKS_MAX];
#endif  // WITH_PYTHON

	AttribTypes m_attr;
	CM_UpdateServer<RAS_IMaterial> *m_materialUpdateServer;

	virtual bool LinkProgram();

public:
	BL_Shader(CM_UpdateServer<RAS_IMaterial> *materialUpdateServer);
	virtual ~BL_Shader();

	virtual std::string GetName();
	virtual std::string GetText();

#ifdef WITH_PYTHON
	PyObject *GetCallbacks(CallbacksType type);
	void SetCallbacks(CallbacksType type, PyObject *callbacks);
#endif // WITH_PYTHON

	RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo,
			RAS_Texture *const textures[RAS_Texture::MaxUnits]) const;

	void BindProg();

	/** Update the uniform shader for the current rendered mesh user (= object).
	 * The python callbacks are executed in this function and at the end
	 * RAS_Shader::Update(rasty, mat) is called.
	 */
	void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser);

	// Python interface
#ifdef WITH_PYTHON

	static PyObject *pyattr_get_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	// -----------------------------------
	EXP_PYMETHOD_DOC(BL_Shader, setSource);
	EXP_PYMETHOD_DOC(BL_Shader, setSourceList);
	EXP_PYMETHOD_DOC(BL_Shader, delSource);
	EXP_PYMETHOD_DOC(BL_Shader, getVertexProg);
	EXP_PYMETHOD_DOC(BL_Shader, getFragmentProg);
	EXP_PYMETHOD_DOC(BL_Shader, setNumberOfPasses);
	EXP_PYMETHOD_DOC(BL_Shader, isValid);
	EXP_PYMETHOD_DOC(BL_Shader, validate);

	// -----------------------------------
	EXP_PYMETHOD_DOC(BL_Shader, setUniform4f);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform3f);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform2f);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform1f);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform4i);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform3i);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform2i);
	EXP_PYMETHOD_DOC(BL_Shader, setUniform1i);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformEyef);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformfv);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformiv);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformMatrix4);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformMatrix3);
	EXP_PYMETHOD_DOC(BL_Shader, setUniformDef);
	EXP_PYMETHOD_DOC(BL_Shader, setAttrib);
	EXP_PYMETHOD_DOC(BL_Shader, setSampler);
#endif
};

#endif /* __BL_SHADER_H__ */
