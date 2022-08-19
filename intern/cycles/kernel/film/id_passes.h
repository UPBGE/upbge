/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2018-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

/* Element of ID pass stored in the render buffers.
 * It is `float2` semantically, but it must be unaligned since the offset of ID passes in the
 * render buffers might not meet expected by compiler alignment. */
typedef struct IDPassBufferElement {
  float x;
  float y;
} IDPassBufferElement;

ccl_device_inline void kernel_write_id_slots(ccl_global float *buffer,
                                             int num_slots,
                                             float id,
                                             float weight)
{
  kernel_assert(id != ID_NONE);
  if (weight == 0.0f) {
    return;
  }

  for (int slot = 0; slot < num_slots; slot++) {
    ccl_global IDPassBufferElement *id_buffer = (ccl_global IDPassBufferElement *)buffer;
#ifdef __ATOMIC_PASS_WRITE__
    /* If the loop reaches an empty slot, the ID isn't in any slot yet - so add it! */
    if (id_buffer[slot].x == ID_NONE) {
      /* Use an atomic to claim this slot.
       * If a different thread got here first, try again from this slot on. */
      float old_id = atomic_compare_and_swap_float(buffer + slot * 2, ID_NONE, id);
      if (old_id != ID_NONE && old_id != id) {
        continue;
      }
      atomic_add_and_fetch_float(buffer + slot * 2 + 1, weight);
      break;
    }
    /* If there already is a slot for that ID, add the weight.
     * If no slot was found, add it to the last. */
    else if (id_buffer[slot].x == id || slot == num_slots - 1) {
      atomic_add_and_fetch_float(buffer + slot * 2 + 1, weight);
      break;
    }
#else  /* __ATOMIC_PASS_WRITE__ */
    /* If the loop reaches an empty slot, the ID isn't in any slot yet - so add it! */
    if (id_buffer[slot].x == ID_NONE) {
      id_buffer[slot].x = id;
      id_buffer[slot].y = weight;
      break;
    }
    /* If there already is a slot for that ID, add the weight.
     * If no slot was found, add it to the last. */
    else if (id_buffer[slot].x == id || slot == num_slots - 1) {
      id_buffer[slot].y += weight;
      break;
    }
#endif /* __ATOMIC_PASS_WRITE__ */
  }
}

ccl_device_inline void kernel_sort_id_slots(ccl_global float *buffer, int num_slots)
{
  ccl_global IDPassBufferElement *id_buffer = (ccl_global IDPassBufferElement *)buffer;
  for (int slot = 1; slot < num_slots; ++slot) {
    if (id_buffer[slot].x == ID_NONE) {
      return;
    }
    /* Since we're dealing with a tiny number of elements, insertion sort should be fine. */
    int i = slot;
    while (i > 0 && id_buffer[i].y > id_buffer[i - 1].y) {
      const IDPassBufferElement swap = id_buffer[i];
      id_buffer[i] = id_buffer[i - 1];
      id_buffer[i - 1] = swap;
      --i;
    }
  }
}

/* post-sorting for Cryptomatte */
ccl_device_inline void kernel_cryptomatte_post(KernelGlobals kg,
                                               ccl_global float *render_buffer,
                                               int pixel_index)
{
  const int pass_stride = kernel_data.film.pass_stride;
  const uint64_t render_buffer_offset = (uint64_t)pixel_index * pass_stride;
  ccl_global float *cryptomatte_buffer = render_buffer + render_buffer_offset +
                                         kernel_data.film.pass_cryptomatte;
  kernel_sort_id_slots(cryptomatte_buffer, 2 * kernel_data.film.cryptomatte_depth);
}

CCL_NAMESPACE_END
