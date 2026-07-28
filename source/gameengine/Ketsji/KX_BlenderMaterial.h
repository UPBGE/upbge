
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

namespace blender { struct Material; }
struct GPUMaterial;

class KX_BlenderMaterial : public EXP_Value, public RAS_IPolyMaterial {
  Py_Header

      public : KX_BlenderMaterial(RAS_Rasterizer *rasty,
                                  KX_Scene *scene,
                                  blender::Material *mat,
                                  const std::string &name);

  virtual ~KX_BlenderMaterial();

  virtual const std::string GetTextureName() const;
  virtual blender::Material *GetBlenderMaterial() const;
  virtual blender::Scene *GetBlenderScene() const;
  virtual SCA_IScene *GetScene() const;

  void ReplaceScene(KX_Scene *scene);

  // Stuff for cvalue related things.
  virtual std::string GetName();

#ifdef WITH_PYTHON

  static PyObject *pyattr_get_materialIndex(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_textures(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON

  virtual void OnConstruction();

  static void EndFrame(RAS_Rasterizer *rasty);

 private:
  blender::bNodeTree *m_nodetree;

  blender::Material *m_material;
  RAS_Rasterizer *m_rasterizer;
  KX_Scene *m_scene;
  bool m_constructed;  // if false, don't clean on exit

  void InitTextures();
};

#ifdef WITH_PYTHON
bool ConvertPythonToMaterial(PyObject *value,
                             KX_BlenderMaterial **material,
                             bool py_none_ok,
                             const char *error_prefix);
#endif  // WITH_PYTHON
