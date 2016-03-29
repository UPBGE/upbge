
/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

#include <iostream>

struct MTex;
struct EnvMap;
struct GPUTexture;
class BL_Material;

class BL_Texture
{
private:
	unsigned int m_type; // enum TEXTURE_2D | CUBE_MAP
	MTex *m_mtex;
	GPUTexture *m_gputex;

	struct {
		unsigned int bindcode;
	} m_savedData;

public:
	BL_Texture();
	~BL_Texture();

	bool Ok();

	unsigned int GetTextureType() const;

	void Init(MTex *mtex, bool cubemap, bool mipmap);

	static void ActivateFirst();
	static void DisableAllTextures();
	static int GetMaxUnits();

	void ActivateTexture(int unit);
	unsigned int swapTexture(unsigned int bindcode);

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_Texture")
#endif
};

#endif // __BL_TEXTURE_H__
