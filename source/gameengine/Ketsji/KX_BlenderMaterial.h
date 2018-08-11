
/** \file KX_BlenderMaterial.h
 *  \ingroup ketsji
 */

#ifndef __KX_BLENDERMATERIAL_H__
#define __KX_BLENDERMATERIAL_H__

#include "RAS_IPolygonMaterial.h"
#include "BL_Texture.h"

#include "EXP_Value.h"

class SCA_IScene;
class KX_Scene;
class BL_BlenderShader;
class BL_Shader;
struct Material;

#ifdef USE_MATHUTILS
void KX_BlenderMaterial_Mathutils_Callback_Init(void);
#endif

class KX_BlenderMaterial : public EXP_Value, public RAS_IPolyMaterial
{
	Py_Header

public:
	KX_BlenderMaterial(Material *mat, const std::string& name, KX_Scene *scene);

	virtual ~KX_BlenderMaterial();

	virtual void Prepare(RAS_Rasterizer *rasty);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride);
	virtual void ActivateMeshSlot(RAS_MeshSlot *ms, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);

	void UpdateTextures();
	void ApplyTextures();
	void ActivateShaders(RAS_Rasterizer *rasty);

	void ActivateBlenderShaders(RAS_Rasterizer *rasty);

	const RAS_Rasterizer::BlendFunc *GetBlendFunc() const;
	virtual bool UseInstancing() const;
	virtual const std::string GetTextureName() const;
	virtual Material *GetBlenderMaterial() const;
	virtual bool UsesLighting() const;
	virtual void GetRGBAColor(unsigned char *rgba) const;
	virtual Scene *GetBlenderScene() const;
	virtual SCA_IScene *GetScene() const;
	virtual void ReloadMaterial();

	void ReplaceScene(KX_Scene *scene);
	void InitShader();

	static void EndFrame(RAS_Rasterizer *rasty);

	// for ipos
	virtual void UpdateIPO(const mt::vec4 &rgba, const mt::vec3 &specrgb, float hard, float spec, float ref,
						   float emit, float ambient, float alpha, float specalpha);

	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;

	// Stuff for cvalue related things.
	virtual std::string GetName();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_shader(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_materialIndex(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_blending(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_textures(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_blending(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_hardness(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_intensity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_emit(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_ambient(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ambient(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_alpha(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	unsigned int py_get_textures_size();
	PyObject *py_get_textures_item(unsigned int index);
	std::string py_get_textures_item_name(unsigned int index);

	EXP_PYMETHOD_DOC(KX_BlenderMaterial, getShader);
	EXP_PYMETHOD_DOC(KX_BlenderMaterial, getTextureBindcode);

	EXP_PYMETHOD_DOC(KX_BlenderMaterial, setBlending);

#endif  // WITH_PYTHON

private:
	Material *m_material;
	BL_Shader *m_shader;
	BL_BlenderShader *m_blenderShader;
	KX_Scene *m_scene;
	bool m_userDefBlend;
	RAS_Rasterizer::BlendFunc m_blendFunc[2];

	struct {
		float r, g, b, a;
		float specr, specg, specb;
		float spec;
		float ref;
		float hardness;
		float emit;
		float ambient;
		float specularalpha;
	} m_savedData;

	void InitTextures();

	void ActivateGLMaterials(RAS_Rasterizer *rasty) const;

	void SetBlenderShaderData(RAS_Rasterizer *ras);
	void SetShaderData(RAS_Rasterizer *ras);
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix);
#endif  // WITH_PYTHON

#endif
