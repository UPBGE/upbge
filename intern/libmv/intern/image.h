/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

#ifndef LIBMV_IMAGE_H_
#define LIBMV_IMAGE_H_

#ifdef __cplusplus
#  include "libmv/image/image.h"
void libmv_byteBufferToFloatImage(const unsigned char* buffer,
                                  int width,
                                  int height,
                                  int channels,
                                  libmv::FloatImage* image);

void libmv_floatBufferToFloatImage(const float* buffer,
                                   int width,
                                   int height,
                                   int channels,
                                   libmv::FloatImage* image);

void libmv_floatImageToFloatBuffer(const libmv::FloatImage& image,
                                   float* buffer);

void libmv_floatImageToByteBuffer(const libmv::FloatImage& image,
                                  unsigned char* buffer);

bool libmv_saveImage(const libmv::FloatImage& image,
                     const char* prefix,
                     int x0,
                     int y0);
#endif  // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libmv_FloatImage {
  float* buffer;
  int width;
  int height;
  int channels;
} libmv_FloatImage;

void libmv_floatImageDestroy(libmv_FloatImage* image);

void libmv_samplePlanarPatchFloat(const float* image,
                                  int width,
                                  int height,
                                  int channels,
                                  const double* xs,
                                  const double* ys,
                                  int num_samples_x,
                                  int num_samples_y,
                                  const float* mask,
                                  float* patch,
                                  double* warped_position_x,
                                  double* warped_position_y);

void libmv_samplePlanarPatchByte(const unsigned char* image,
                                 int width,
                                 int height,
                                 int channels,
                                 const double* xs,
                                 const double* ys,
                                 int num_samples_x,
                                 int num_samples_y,
                                 const float* mask,
                                 unsigned char* patch,
                                 double* warped_position_x,
                                 double* warped_position_y);

#ifdef __cplusplus
}
#endif

#endif  // LIBMV_IMAGE_H_
