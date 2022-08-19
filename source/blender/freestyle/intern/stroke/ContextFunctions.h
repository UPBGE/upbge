/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Functions related to context queries
 * \brief Interface to access the context related information.
 */

#include "Canvas.h"

#include "../image/GaussianFilter.h"
#include "../image/Image.h"

namespace Freestyle {

//
// Context Functions definitions
//
///////////////////////////////////////////////////////////
/** namespace containing all the Context related functions */
namespace ContextFunctions {

// GetTimeStamp
/** Returns the system time stamp */
unsigned GetTimeStampCF();

// GetCanvasWidth
/** Returns the canvas width */
unsigned GetCanvasWidthCF();

// GetCanvasHeight
/** Returns the canvas height */
unsigned GetCanvasHeightCF();

// GetBorder
/** Returns the border */
BBox<Vec2i> GetBorderCF();

// Load map
/** Loads an image map for further reading */
void LoadMapCF(const char *iFileName,
               const char *iMapName,
               unsigned iNbLevels = 4,
               float iSigma = 1.0f);

// ReadMapPixel
/** Reads a pixel in a user-defined map
 * \return the floating value stored for that pixel
 * \param iMapName:
 *    The name of the map
 * \param level:
 *    The level of the pyramid in which we wish to read the pixel
 * \param x:
 *    The x-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 * \param y:
 *    The y-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 */
float ReadMapPixelCF(const char *iMapName, int level, unsigned x, unsigned y);

// ReadCompleteViewMapPixel
/** Reads a pixel in the complete view map
 * \return the floating value stored for that pixel
 * \param level:
 *    The level of the pyramid in which we wish to read the pixel
 * \param x:
 *    The x-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 * \param y:
 *    The y-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 */
float ReadCompleteViewMapPixelCF(int level, unsigned x, unsigned y);

// ReadOrientedViewMapPixel
/** Reads a pixel in one of the oriented view map images
 * \return the floating value stored for that pixel
 * \param iOrientation:
 *    The number telling which orientation we want to check
 * \param level:
 *    The level of the pyramid in which we wish to read the pixel
 * \param x:
 *    The x-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 * \param y:
 *    The y-coordinate of the pixel we wish to read. The origin is in the lower-left corner.
 */
float ReadDirectionalViewMapPixelCF(int iOrientation, int level, unsigned x, unsigned y);

// DEBUG
FEdge *GetSelectedFEdgeCF();

}  // end of namespace ContextFunctions

} /* namespace Freestyle */
