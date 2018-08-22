
/** \file BL_Material.h
 *  \ingroup ketsji
 */

#ifndef __KX_BLENDERMATERIAL_H__
#define __KX_BLENDERMATERIAL_H__

#include "RAS_IMaterial.h"
#include "BL_Texture.h"
#include "BL_Resource.h"

#include "EXP_Value.h"

class SCA_IScene;
class KX_Scene;
class KX_MaterialShader;
class BL_MaterialShader;
struct Material;

#ifdef USE_MATHUTILS
void BL_Material_Mathutils_Callback_Init(void);
#endif

class BL_Material : public EXP_Value, public BL_Resource, public RAS_IMaterial
{
	Py_Header

public:
	BL_Material(Material *mat, const std::string& name, KX_Scene *scene);
	virtual ~BL_Material();

	bool GetUserBlend() const;
	const RAS_Rasterizer::BlendFunc (&GetBlendFunc() const)[2];

	Material *GetBlenderMaterial() const;

	virtual RAS_IMaterialShader *GetShader(RAS_Rasterizer::DrawType drawingMode) const;
	virtual const std::string GetTextureName() const;
	virtual SCA_IScene *GetScene() const;
	virtual void ReloadMaterial();
	virtual void Prepare();

	void InitTextures();
	void UpdateTextures();
	void ApplyTextures();

	void ReplaceScene(KX_Scene *scene);

	static void EndFrame(RAS_Rasterizer *rasty);

	// for ipos
	virtual void UpdateIPO(const mt::vec4 &rgba, const mt::vec3 &specrgb, float hard, float spec, float ref,
						   float emit, float ambient, float alpha, float specalpha);

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

	EXP_PYMETHOD_DOC(BL_Material, getShader);
	EXP_PYMETHOD_DOC(BL_Material, getTextureBindcode);

	EXP_PYMETHOD_DOC(BL_Material, setBlending);

#endif  // WITH_PYTHON

private:
	KX_Scene *m_scene;
	Material *m_material;

	std::unique_ptr<KX_MaterialShader> m_customShader;
	std::unique_ptr<BL_MaterialShader> m_blenderShader;

	int m_alphaBlend;
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
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value, BL_Material **material, bool py_none_ok, const char *error_prefix);
#endif  // WITH_PYTHON

#endif
