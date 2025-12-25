/* Utility helpers for GPU modifier setup retry logic */
#pragma once

#include "BLI_sys_types.h"

/* Advance attempt counter and return:
 * - true if caller should continue attempting setup (i.e. either not pending or allowed to proceed)
 * - false if caller should return early (either first deferred attempt or exceeded attempts)
 * This centralizes the common pattern across modifiers. */
static inline bool draw_gpu_modifier_setup_retry(bool &pending_gpu_setup, int &gpu_setup_attempts)
{
  const int MAX_ATTEMPTS = 3;
  if (pending_gpu_setup) {
    if (gpu_setup_attempts == 0) {
      gpu_setup_attempts = 1;
      return false; /* first deferred attempt, ask caller to return */
    }
    if (gpu_setup_attempts >= MAX_ATTEMPTS) {
      pending_gpu_setup = false;
      gpu_setup_attempts = 0;
      return false; /* give up after max attempts */
    }
    /* Increment and allow to continue attempting setup this frame. */
    gpu_setup_attempts++;
  }

  /* GPU Setup Successfull, reset variables */
  pending_gpu_setup = false;
  gpu_setup_attempts = 0;
  return true;
}
