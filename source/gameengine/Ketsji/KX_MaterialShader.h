#pragma once

#include <memory>

#include "RAS_MaterialShader.h"

class BL_Shader;
class KX_GameObject;

class KX_MaterialShader : public RAS_MaterialShader {
 private:
  std::unique_ptr<BL_Shader> m_shader;

 public:
  KX_MaterialShader();
  virtual ~KX_MaterialShader();

  BL_Shader *GetShader() const;

  virtual bool IsValid() const;
  virtual void Activate(RAS_Rasterizer *rasty);
  virtual void Desactivate();
  virtual void Update(RAS_Rasterizer *rasty, KX_GameObject *gameobj);
};
