/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2017-2022 Blender Foundation */

#ifndef __UTIL_RECT_H__
#define __UTIL_RECT_H__

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Rectangles are represented as a int4 containing the coordinates of the lower-left and
 * upper-right corners in the order (x0, y0, x1, y1). */

ccl_device_inline int4 rect_from_shape(int x0, int y0, int w, int h)
{
  return make_int4(x0, y0, x0 + w, y0 + h);
}

ccl_device_inline int4 rect_expand(int4 rect, int d)
{
  return make_int4(rect.x - d, rect.y - d, rect.z + d, rect.w + d);
}

/* Returns the intersection of two rects. */
ccl_device_inline int4 rect_clip(int4 a, int4 b)
{
  return make_int4(max(a.x, b.x), max(a.y, b.y), min(a.z, b.z), min(a.w, b.w));
}

ccl_device_inline bool rect_is_valid(int4 rect)
{
  return (rect.z > rect.x) && (rect.w > rect.y);
}

/* Returns the local row-major index of the pixel inside the rect. */
ccl_device_inline int coord_to_local_index(int4 rect, int x, int y)
{
  int w = rect.z - rect.x;
  return (y - rect.y) * w + (x - rect.x);
}

/* Finds the coordinates of a pixel given by its row-major index in the rect,
 * and returns whether the pixel is inside it. */
ccl_device_inline bool local_index_to_coord(int4 rect,
                                            int idx,
                                            ccl_private int *x,
                                            ccl_private int *y)
{
  int w = rect.z - rect.x;
  *x = (idx % w) + rect.x;
  *y = (idx / w) + rect.y;
  return (*y < rect.w);
}

ccl_device_inline int rect_size(int4 rect)
{
  return (rect.z - rect.x) * (rect.w - rect.y);
}

CCL_NAMESPACE_END

#endif /* __UTIL_RECT_H__ */
