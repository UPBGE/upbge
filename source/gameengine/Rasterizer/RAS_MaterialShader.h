#pragma once

#include "RAS_MeshObject.h"  // For RAS_MeshObject::LayersInfo.

class KX_GameObject;
class RAS_Rasterizer;

class RAS_MaterialShader {
 public:
  RAS_MaterialShader() = default;
  virtual ~RAS_MaterialShader() = default;

  /// Return true when the shader can be bound.
  virtual bool IsValid() const = 0;
  // Bind the shader and mainly update global uniforms.
  virtual void Activate(RAS_Rasterizer *rasty) = 0;
  /// Unbind the shader.
  virtual void Desactivate() = 0;
  /// Update the shader with mesh user data as model matrix.
  virtual void Update(RAS_Rasterizer *rasty, KX_GameObject *gameobj) = 0;
};
