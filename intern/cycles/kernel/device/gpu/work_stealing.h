/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

/*
 * Utility functions for work stealing
 */

/* Map global work index to tile, pixel X/Y and sample. */
ccl_device_inline void get_work_pixel(ccl_global const KernelWorkTile *tile,
                                      uint global_work_index,
                                      ccl_private uint *x,
                                      ccl_private uint *y,
                                      ccl_private uint *sample)
{
  uint sample_offset, pixel_offset;

  if (kernel_data.integrator.scrambling_distance < 0.9f) {
    /* Keep threads for the same sample together. */
    uint tile_pixels = tile->w * tile->h;
    sample_offset = global_work_index / tile_pixels;
    pixel_offset = global_work_index - sample_offset * tile_pixels;
  }
  else {
    /* Keeping threads for the same pixel together.
     * Appears to improve performance by a few % on CUDA and OptiX. */
    sample_offset = global_work_index % tile->num_samples;
    pixel_offset = global_work_index / tile->num_samples;
  }

  uint y_offset = pixel_offset / tile->w;
  uint x_offset = pixel_offset - y_offset * tile->w;

  *x = tile->x + x_offset;
  *y = tile->y + y_offset;
  *sample = tile->start_sample + sample_offset;
}

CCL_NAMESPACE_END
