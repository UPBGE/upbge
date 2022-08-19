/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * Random number generator, contains persistent state and sample count logic.
 */

#pragma once

#include "BKE_colortools.h"
#include "BLI_system.h"
#include "BLI_vector.hh"
#include "DNA_scene_types.h"
#include "DRW_render.h"

#include "eevee_shader_shared.hh"

namespace blender::eevee {

class Instance;

class Sampling {
 private:
  Instance &inst_;

  /* Number of samples in the first ring of jittered depth of field. */
  static constexpr uint64_t dof_web_density_ = 6;
  /* High number of sample for viewport infinite rendering. */
  static constexpr uint64_t infinite_sample_count_ = 0xFFFFFFu;
  /* During interactive rendering, loop over the first few samples. */
  static constexpr uint64_t interactive_sample_max_ = 8;

  /** 0 based current sample. Might not increase sequentially in viewport. */
  uint64_t sample_ = 0;
  /** Target sample count. */
  uint64_t sample_count_ = 64;
  /** Number of ring in the web pattern of the jittered Depth of Field. */
  uint64_t dof_ring_count_ = 0;
  /** Number of samples in the web pattern of the jittered Depth of Field. */
  uint64_t dof_sample_count_ = 1;
  /** Motion blur steps. */
  uint64_t motion_blur_steps_ = 1;
  /** Increases if the view and the scene is static. Does increase sequentially. */
  int64_t viewport_sample_ = 0;
  /** Tag to reset sampling for the next sample. */
  bool reset_ = false;
  /**
   * Switch between interactive and static accumulation.
   * In interactive mode, image stability is prioritized over quality.
   */
  bool interactive_mode_ = false;
  /**
   * Sample count after which we use the static accumulation.
   * Interactive sampling from sample 0 to (interactive_mode_threshold - 1).
   * Accumulation sampling from sample interactive_mode_threshold to sample_count_.
   */
  static constexpr int interactive_mode_threshold = 3;

  SamplingDataBuf data_;

 public:
  Sampling(Instance &inst) : inst_(inst){};
  ~Sampling(){};

  void init(const Scene *scene);
  void end_sync();
  void step();

  /* Viewport Only: Function to call to notify something in the scene changed.
   * This will reset accumulation. Do not call after end_sync() or during sample rendering. */
  void reset()
  {
    reset_ = true;
  }

  /* Viewport Only: true if an update happened in the scene and accumulation needs reset. */
  bool is_reset() const
  {
    return reset_;
  }

  void bind_resources(DRWShadingGroup *grp)
  {
    DRW_shgroup_storage_block_ref(grp, "sampling_buf", &data_);
  }

  /* Returns a pseudo random number in [0..1] range. Each dimension are de-correlated. */
  float rng_get(eSamplingDimension dimension) const
  {
    return data_.dimensions[dimension];
  }

  /* Returns a pseudo random number in [0..1] range. Each dimension are de-correlated. */
  float2 rng_2d_get(eSamplingDimension starting_dimension) const
  {
    return *reinterpret_cast<const float2 *>(&data_.dimensions[starting_dimension]);
  }

  /* Returns a pseudo random number in [0..1] range. Each dimension are de-correlated. */
  float3 rng_3d_get(eSamplingDimension starting_dimension) const
  {
    return *reinterpret_cast<const float3 *>(&data_.dimensions[starting_dimension]);
  }

  /* Returns true if rendering has finished. */
  bool finished() const
  {
    return (sample_ >= sample_count_);
  }

  /* Returns true if viewport smoothing and sampling has finished. */
  bool finished_viewport() const
  {
    return (viewport_sample_ >= sample_count_) && !interactive_mode_;
  }

  /* Returns true if viewport renderer is in interactive mode and should use TAA. */
  bool interactive_mode() const
  {
    return interactive_mode_;
  }

  uint64_t sample_count() const
  {
    return sample_count_;
  }

  /* Return true if we are starting a new motion blur step. We need to run sync again since
   * depsgraph was updated by MotionBlur::step(). */
  bool do_render_sync() const
  {
    return ((sample_ % (sample_count_ / motion_blur_steps_)) == 0);
  }

  /**
   * Special ball distribution:
   * Point are distributed in a way that when they are orthogonally
   * projected into any plane, the resulting distribution is (close to)
   * a uniform disc distribution.
   * \a rand is 3 random float in the [0..1] range.
   * Returns point in a ball of radius 1 and centered on the origin.
   */
  static float3 sample_ball(const float3 &rand);

  /**
   * Uniform disc distribution.
   * \a rand is 2 random float in the [0..1] range.
   * Returns point in a disk of radius 1 and centered on the origin.
   */
  static float2 sample_disk(const float2 &rand);

  /**
   * Uniform disc distribution using Fibonacci spiral sampling.
   * \a rand is 2 random float in the [0..1] range.
   * Returns point in a disk of radius 1 and centered on the origin.
   */
  static float2 sample_spiral(const float2 &rand);

  /**
   * Special RNG for depth of field.
   * Returns \a radius and \a theta angle offset to apply to the web sampling pattern.
   */
  void dof_disk_sample_get(float *r_radius, float *r_theta) const;

  /**
   * Returns sample count inside the jittered depth of field web pattern.
   */
  uint64_t dof_ring_count_get() const
  {
    return dof_ring_count_;
  }

  /**
   * Returns sample count inside the jittered depth of field web pattern.
   */
  uint64_t dof_sample_count_get() const
  {
    return dof_sample_count_;
  }

  /* Cumulative Distribution Function Utils. */
  static void cdf_from_curvemapping(const CurveMapping &curve, Vector<float> &cdf);
  static void cdf_invert(Vector<float> &cdf, Vector<float> &inverted_cdf);
};

}  // namespace blender::eevee
