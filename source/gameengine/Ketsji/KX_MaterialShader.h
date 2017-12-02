#ifndef __KX_MATERIAL_SHADER__
#define __KX_MATERIAL_SHADER__

#include "RAS_MaterialShader.h"

#include <memory>

class BL_Shader;

class KX_MaterialShader : public RAS_MaterialShader
{
private:
	std::unique_ptr<BL_Shader> m_shader;

public:
	KX_MaterialShader();
	virtual ~KX_MaterialShader();

	BL_Shader *GetShader() const;

	virtual bool IsValid(RAS_Rasterizer::DrawType drawtype) const;
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate();
	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser);

};

#endif  // __KX_MATERIAL_SHADER__
