#include "KX_MaterialShader.h"

#include "BL_Shader.h"

KX_MaterialShader::KX_MaterialShader() : m_shader(new BL_Shader())
{
}

KX_MaterialShader::~KX_MaterialShader()
{
}

BL_Shader *KX_MaterialShader::GetShader() const
{
  return m_shader.get();
}

bool KX_MaterialShader::IsValid() const
{
  return m_shader->Ok();
}

void KX_MaterialShader::Activate(RAS_Rasterizer *rasty)
{
  m_shader->SetProg(true);
  m_shader->ApplyShader();
}

void KX_MaterialShader::Desactivate()
{
  m_shader->SetProg(false);
}

void KX_MaterialShader::Update(RAS_Rasterizer *rasty, KX_GameObject *gameobj)
{
  m_shader->Update(rasty, gameobj);
}
