#ifndef __RAS_DISPLAY_ARRAY_LAYOUT_H__
#define __RAS_DISPLAY_ARRAY_LAYOUT_H__

#include "RAS_Texture.h" // For RAS_Texture::MaxUnits

struct RAS_DisplayArrayLayout
{
	intptr_t position;
	intptr_t normal;
	intptr_t tangent;
	intptr_t uvs[RAS_Texture::MaxUnits];
	intptr_t colors[RAS_Texture::MaxUnits];
	intptr_t size;
};

#endif  // __RAS_DISPLAY_ARRAY_LAYOUT_H__
