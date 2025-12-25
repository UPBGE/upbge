/* Utility helpers for GPU modifier setup retry logic */
#pragma once

#include "BLI_sys_types.h"

/* Advance attempt counter and return:
 * - true if caller should continue attempting setup (i.e. either not pending or allowed to proceed)
 * - false if caller should return early (either first deferred attempt or exceeded attempts)
 * This centralizes the common pattern across modifiers. */
static inline bool draw_modifier_gpu_setup_retry(bool &pending_gpu_setup, int &gpu_setup_attempts)
{
  const int MAX_ATTEMPTS = 3;

  if (pending_gpu_setup) {
    /* First deferred attempt: mark that we've deferred once and return false
     * so caller exits early. This mirrors the previous behaviour where the
     * first frame after marking pending would skip heavy GPU work. */
    if (gpu_setup_attempts == 0) {
      gpu_setup_attempts = 1;
      return false;
    }

    /* If we already retried enough times, give up and clear pending state. */
    if (gpu_setup_attempts >= MAX_ATTEMPTS) {
      pending_gpu_setup = false;
      gpu_setup_attempts = 0;
      return false;
    }

    /* Otherwise increment attempts and allow the caller to try setup this frame. */
    gpu_setup_attempts++;
    return true;
  }

  /* Not pending: ensure counters are cleared and allow caller to proceed. */
  gpu_setup_attempts = 0;
  return true;
}
