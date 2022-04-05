/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file VideoTexture/Texture.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "DNA_image_types.h"

#include "EXP_Value.h"
#include "Exception.h"
#include "ImageBase.h"

struct ImBuf;
class RAS_Texture;
class RAS_IPolyMaterial;
class KX_Scene;
class KX_GameObject;

// type Texture declaration
class Texture : public EXP_Value {
  Py_Header protected : virtual void DestructFromPython();

 public:
  // texture is using blender material
  bool m_useMatTexture;

  // video texture bind code
  unsigned int m_actTex;
  // original texture bind code
  unsigned int m_orgTex;
  // original image bind code
  unsigned int m_orgImg;
  // original texture saved
  bool m_orgSaved;

  // kernel image buffer, to make sure the image is loaded before we swap the bindcode
  struct ImBuf *m_imgBuf;
  // texture image for game materials
  Image *m_imgTexture;
  // texture for blender materials
  RAS_Texture *m_matTexture;

  KX_Scene *m_scene;
  KX_GameObject *m_gameobj;

  // use mipmapping
  bool m_mipmap;

  // scaled image buffer
  ImBuf *m_scaledImBuf;
  // last refresh
  double m_lastClock;

  // image source
  PyImage *m_source;

  Texture();
  virtual ~Texture();

  virtual std::string GetName();

  void Close();
  void SetSource(PyImage *source);

  static void FreeAllTextures(KX_Scene *scene);

  EXP_PYMETHOD_DOC(Texture, close);
  EXP_PYMETHOD_DOC(Texture, refresh);

  static PyObject *pyattr_get_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_mipmap(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
  static PyObject *pyattr_get_source(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_source(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
  static PyObject *pyattr_get_bindId(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_bindId(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
};

// load texture
void loadTexture(unsigned int texId,
                 unsigned int *texture,
                 short *size,
                 bool mipmap,
                 unsigned int internalFormat);

// get material
RAS_IPolyMaterial *getMaterial(KX_GameObject *gameObj, short matID);

// get material ID
short getMaterialID(PyObject *obj, const char *name);

// Exceptions
extern ExceptionID MaterialNotAvail;
extern ExceptionID TextureNotAvail;
