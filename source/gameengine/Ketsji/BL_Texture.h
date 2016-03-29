
/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

struct MTex;
struct EnvMap;
class BL_Material;

class BL_Texture
{
private:
	unsigned int m_bindcode; // Bound texture unit data
	unsigned int m_type; // enum TEXTURE_2D | CUBE_MAP
	unsigned int m_envState; // cache textureEnv
	MTex *m_mtex;
	static unsigned int m_disableState; // speed up disabling calls

public:
	BL_Texture();
	~BL_Texture();

	bool Ok();

	unsigned int GetTextureType() const;
	void DeleteTex();

	void Init(MTex *mtex, bool cubemap, bool mipmap);

	static void ActivateFirst();
	static void DisableAllTextures();
	static void ActivateUnit(int unit);
	static int GetMaxUnits();
	static void SplitEnvMap(EnvMap *map);

	void ActivateTexture(int unit);
	void SetMapping(int mode);
	void DisableUnit(int unit);
	void setTexEnv(int unit, BL_Material *mat, bool modulate = false);
	unsigned int swapTexture(unsigned int bindcode)
	{
		// swap texture codes
		unsigned int tmp = m_bindcode;
		m_bindcode = bindcode;
		// return original texture code
		return tmp;
	}

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_Texture")
#endif
};

#endif // __BL_TEXTURE_H__
