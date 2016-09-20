
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

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

class SCA_IScene;
class KX_Scene;
class BL_BlenderShader;
class BL_Shader;
struct Material;
struct MTFace;

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
			GameSettings *game,
			MTFace *mtface,
			int lightlayer);

	virtual ~KX_BlenderMaterial();

	virtual void Activate(RAS_IRasterizer *rasty);
	virtual void Desactivate(RAS_IRasterizer *rasty);
	virtual void ActivateInstancing(RAS_IRasterizer *rasty, void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride);
	virtual void DesactivateInstancing();
	virtual void ActivateMeshSlot(RAS_MeshSlot *ms, RAS_IRasterizer *rasty);

	void ActivateShaders(RAS_IRasterizer *rasty);

	void ActivateBlenderShaders(RAS_IRasterizer *rasty);

	virtual bool UseInstancing() const;
	virtual const STR_String& GetTextureName() const;
	virtual Material *GetBlenderMaterial() const;
	virtual Image *GetBlenderImage() const;
	virtual MTexPoly *GetMTexPoly() const;
	virtual bool UsesLighting(RAS_IRasterizer *rasty) const;
	virtual void GetRGBAColor(unsigned char *rgba) const;
	virtual Scene *GetBlenderScene() const;
	virtual void ReleaseMaterial();

	unsigned int *GetBlendFunc()
	{
		return m_blendFunc;
	}
	// for ipos
	virtual void UpdateIPO(MT_Vector4 rgba, MT_Vector3 specrgb, MT_Scalar hard, MT_Scalar spec, MT_Scalar ref,
						   MT_Scalar emit, MT_Scalar ambient, MT_Scalar alpha, MT_Scalar specalpha);

	virtual const RAS_IRasterizer::AttribLayerList GetAttribLayers(const STR_String uvsname[RAS_Texture::MaxUnits]) const;

	virtual void Replace_IScene(SCA_IScene *val);

	// Stuff for cvalue related things.
	virtual STR_String &GetName();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_shader(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_materialIndex(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_blending(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_textures(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_blending(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_hardness(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_intensity(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_diffuse_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_emit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_ambient(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ambient(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_specular_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_specular_alpha(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	KX_PYMETHOD_DOC(KX_BlenderMaterial, getShader);
	KX_PYMETHOD_DOC(KX_BlenderMaterial, getTextureBindcode);

	KX_PYMETHOD_DOC(KX_BlenderMaterial, setBlending);

#endif  // WITH_PYTHON

	// pre calculate to avoid pops/lag at startup
	virtual void OnConstruction();

	static void EndFrame(RAS_IRasterizer *rasty);

private:
	Material *m_material;
	MTexPoly *m_mtexPoly;
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

	void ActivateGLMaterials(RAS_IRasterizer *rasty) const;
	void ActivateTexGen(RAS_IRasterizer *ras) const;

	void SetBlenderShaderData(RAS_IRasterizer *ras);
	void SetShaderData(RAS_IRasterizer *ras);

	// cleanup stuff
	void OnExit();
};

#endif
