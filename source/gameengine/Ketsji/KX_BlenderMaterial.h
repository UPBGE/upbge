
/** \file KX_BlenderMaterial.h
 *  \ingroup ketsji
 */

#pragma once

#include "BL_Texture.h"
#include "EXP_Value.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"
#include "RAS_IPolygonMaterial.h"

class SCA_IScene;
class KX_Scene;
class RAS_Rasterizer;
class KX_MaterialShader;
struct Material;
struct GPUMaterial;

class KX_BlenderMaterial : public EXP_Value, public RAS_IPolyMaterial {
  Py_Header

      public : KX_BlenderMaterial(RAS_Rasterizer *rasty,
                                  KX_Scene *scene,
                                  Material *mat,
                                  const std::string &name,
                                  GameSettings *game,
                                  int lightlayer,
                                  bool converting_during_runtime);

  virtual ~KX_BlenderMaterial();

  virtual RAS_MaterialShader *GetShader() const;
  virtual const std::string GetTextureName() const;
  virtual Material *GetBlenderMaterial() const;
  virtual bool UsesLighting() const;
  virtual Scene *GetBlenderScene() const;
  virtual SCA_IScene *GetScene() const;
  virtual void ReleaseMaterial();

  unsigned int *GetBlendFunc()
  {
    return m_blendFunc;
  }

  void ReplaceScene(KX_Scene *scene);

  // Stuff for cvalue related things.
  virtual std::string GetName();

#ifdef WITH_PYTHON

  static PyObject *pyattr_get_materialIndex(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_textures(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);

  EXP_PYMETHOD_DOC(KX_BlenderMaterial, getShader);
  EXP_PYMETHOD_DOC(KX_BlenderMaterial, setBlending);

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
  bool m_constructed;  // if false, don't clean on exit
  int m_lightLayer;

  void InitTextures();

  void SetShaderData(RAS_Rasterizer *ras);

  // cleanup stuff
  void OnExit();
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value,
                             KX_BlenderMaterial **material,
                             bool py_none_ok,
                             const char *error_prefix);
#endif  // WITH_PYTHON
