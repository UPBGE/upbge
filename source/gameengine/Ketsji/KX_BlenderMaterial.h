
/** \file KX_BlenderMaterial.h
 *  \ingroup ketsji
 */

#ifndef __KX_BLENDERMATERIAL_H__
#define __KX_BLENDERMATERIAL_H__

#include "RAS_IPolygonMaterial.h"
#include "BL_Texture.h"

#include "EXP_Value.h"

#include "MT_Vector3.h"
#include "MT_Vector4.h"

class SCA_IScene;
class KX_Scene;
class BL_BlenderShader;
class BL_Shader;
struct Material;

#ifdef USE_MATHUTILS
void KX_BlenderMaterial_Mathutils_Callback_Init(void);
#endif

class KX_BlenderMaterial : public CValue, public RAS_IPolyMaterial
{
	Py_Header

public:
	KX_BlenderMaterial(
			KX_Scene *scene,
			Material *mat,
			const std::string& name,
			int lightlayer);

	virtual ~KX_BlenderMaterial();

	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride);
	virtual void DesactivateInstancing();
	virtual void ActivateMeshSlot(RAS_MeshSlot *ms, RAS_Rasterizer *rasty, const MT_Transform& camtrans);

	void ActivateShaders(RAS_Rasterizer *rasty);

	void ActivateBlenderShaders(RAS_Rasterizer *rasty);

	virtual bool UseInstancing() const;
	virtual const std::string GetTextureName() const;
	virtual Material *GetBlenderMaterial() const;
	virtual bool UsesLighting() const;
	virtual void GetRGBAColor(unsigned char *rgba) const;
	virtual Scene *GetBlenderScene() const;
	virtual SCA_IScene *GetScene() const;
	virtual void ReleaseMaterial();

	unsigned int *GetBlendFunc()
	{
		return m_blendFunc;
	}
	// for ipos
	virtual void UpdateIPO(MT_Vector4 rgba, MT_Vector3 specrgb, MT_Scalar hard, MT_Scalar spec, MT_Scalar ref,
						   MT_Scalar emit, MT_Scalar ambient, MT_Scalar alpha, MT_Scalar specalpha);

	virtual const RAS_Rasterizer::AttribLayerList GetAttribLayers(const RAS_MeshObject::LayersInfo& layersInfo) const;

	void ReplaceScene(KX_Scene *scene);

	// Stuff for cvalue related things.
	virtual std::string GetName();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_shader(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_materialIndex(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_blending(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_textures(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_blending(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_ambient(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ambient(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	KX_PYMETHOD_DOC(KX_BlenderMaterial, getShader);
	KX_PYMETHOD_DOC(KX_BlenderMaterial, getTextureBindcode);

	KX_PYMETHOD_DOC(KX_BlenderMaterial, setBlending);

#endif  // WITH_PYTHON

	// pre calculate to avoid pops/lag at startup
	virtual void OnConstruction();

	static void EndFrame(RAS_Rasterizer *rasty);

private:
	Material *m_material;
	BL_Shader *m_shader;
	BL_BlenderShader *m_blenderShader;
	KX_Scene *m_scene;
	bool m_userDefBlend;
	unsigned int m_blendFunc[2];
	bool m_constructed; // if false, don't clean on exit
	int m_lightLayer;

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

	void SetBlenderGLSLShader();

	void ActivateGLMaterials(RAS_Rasterizer *rasty) const;

	void SetBlenderShaderData(RAS_Rasterizer *ras);
	void SetShaderData(RAS_Rasterizer *ras);

	// cleanup stuff
	void OnExit();
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix);
#endif  // WITH_PYTHON

#endif
