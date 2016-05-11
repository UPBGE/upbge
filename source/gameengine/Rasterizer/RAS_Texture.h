
/** \file RAS_Texture.h
 *  \ingroup ketsji
 */

#ifndef __RAS_TEXTURE_H__
#define __RAS_TEXTURE_H__

#include "STR_String.h"

struct MTex;
struct Image;

class RAS_Texture
{
protected:
	int m_bindCode;
	STR_String m_name;

public:
	RAS_Texture();
	virtual ~RAS_Texture();

	virtual bool Ok() = 0;

	virtual MTex *GetMTex() = 0;
	virtual Image *GetImage() = 0;
	STR_String& GetName();

	virtual unsigned int GetTextureType() const = 0;

	/// Return GL_TEXTURE_2D
	static int GetCubeMapTextureType();
	/// Return GL_TEXTURE_CUBE_MAP
	static int GetTexture2DType();

	// Check if bindcode is an existing texture
	static bool CheckBindCode(int bindcode);

	enum {MaxUnits = 8};

	virtual void ActivateTexture(int unit) = 0;
	virtual void DisableTexture() = 0;
	unsigned int swapTexture(unsigned int bindcode);
};

#endif // __RAS_TEXTURE_H__
