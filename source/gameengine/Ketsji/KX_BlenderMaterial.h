
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
class RAS_Rasterizer;
class KX_MaterialShader;
struct Material;
struct GPUMaterial;

class KX_BlenderMaterial : public CValue, public RAS_IPolyMaterial
{
	Py_Header

public:
	KX_BlenderMaterial(
			RAS_Rasterizer *rasty,
			KX_Scene *scene,
			Material *mat,
			const std::string& name,
			GameSettings *game,
			int lightlayer);

	virtual ~KX_BlenderMaterial();

	virtual RAS_MaterialShader *GetShader() const ;
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

	void ReplaceScene(KX_Scene *scene);

	// Stuff for cvalue related things.
	virtual std::string GetName();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_materialIndex(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_textures(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

	KX_PYMETHOD_DOC(KX_BlenderMaterial, getShader);
	KX_PYMETHOD_DOC(KX_BlenderMaterial, setBlending);

#endif  // WITH_PYTHON

	virtual void OnConstruction();

	static void EndFrame(RAS_Rasterizer *rasty);

private:

	GPUMaterial *m_gpuMat;

	Material *m_material;
	std::unique_ptr<KX_MaterialShader> m_shader;
	RAS_Rasterizer *m_rasterizer;
	KX_Scene *m_scene;
	bool m_userDefBlend;
	unsigned int m_blendFunc[2];
	bool m_constructed; // if false, don't clean on exit
	int m_lightLayer;

	void InitTextures();

	void SetShaderData(RAS_Rasterizer *ras);

	// cleanup stuff
	void OnExit();
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix);
#endif  // WITH_PYTHON

#endif
