
/** \file BL_Material.h
 *  \ingroup ketsji
 */

#ifndef __BL_MATERIAL_H__
#define __BL_MATERIAL_H__

#include "STR_String.h"
#include "MT_Vector2.h"
#include "DNA_meshdata_types.h"

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

// --
struct MTex;
struct Material;
struct Image;
struct MTFace;
struct MTex;
struct Material;
struct EnvMap;
// --

/** max units
 * this will default to users available units
 * to build with more available, just increment this value
 * although the more you add the slower the search time will be.
 * we will go for eight, which should be enough
 */
#define MAXTEX			8	//match in RAS_TexVert & RAS_OpenGLRasterizer

// base material struct
class BL_Material
{
public:
	// -----------------------------------
	BL_Material();
	void Initialize();

	unsigned int ras_mode;

	STR_String matname;

	float matcolor[4];
	float speccolor[3];
	short alphablend, pad;

	float hard, spec_f;
	float alpha, emit, ref;
	float amb, specalpha;

	STR_String uvsName[MAXTEX];

	Material*			material;
	MTexPoly			mtexpoly; /* copy of the derived meshes tface */
	Image*				img[MAXTEX];

	unsigned int rgb[4];

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_Material")
#endif
};

// BL_Material::ras_mode
enum BL_ras_mode
{
	// POLY_VIS=1,
	COLLIDER=2,
	ZSORT=4,
	ALPHA=8,
	// TRIANGLE=16,
	USE_LIGHT=32,
	WIRE=64,
	CAST_SHADOW=128,
	TEX=256,
	TWOSIDED=512,
	ONLY_SHADOW=1024,
};

// BL_Material::BL_Mapping::projplane
enum BL_MappingProj
{
	PROJN=0,
	PROJX,
	PROJY,
	PROJZ
};


#endif


