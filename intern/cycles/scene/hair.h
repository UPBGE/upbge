/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#ifndef __HAIR_H__
#define __HAIR_H__

#include "scene/geometry.h"

CCL_NAMESPACE_BEGIN

struct KernelCurveSegment;

class Hair : public Geometry {
 public:
  NODE_DECLARE

  /* Hair Curve */
  struct Curve {
    int first_key;
    int num_keys;

    int num_segments() const
    {
      return num_keys - 1;
    }

    void bounds_grow(const int k,
                     const float3 *curve_keys,
                     const float *curve_radius,
                     BoundBox &bounds) const;
    void bounds_grow(float4 keys[4], BoundBox &bounds) const;
    void bounds_grow(const int k,
                     const float3 *curve_keys,
                     const float *curve_radius,
                     const Transform &aligned_space,
                     BoundBox &bounds) const;

    void motion_keys(const float3 *curve_keys,
                     const float *curve_radius,
                     const float3 *key_steps,
                     size_t num_curve_keys,
                     size_t num_steps,
                     float time,
                     size_t k0,
                     size_t k1,
                     float4 r_keys[2]) const;
    void cardinal_motion_keys(const float3 *curve_keys,
                              const float *curve_radius,
                              const float3 *key_steps,
                              size_t num_curve_keys,
                              size_t num_steps,
                              float time,
                              size_t k0,
                              size_t k1,
                              size_t k2,
                              size_t k3,
                              float4 r_keys[4]) const;

    void keys_for_step(const float3 *curve_keys,
                       const float *curve_radius,
                       const float3 *key_steps,
                       size_t num_curve_keys,
                       size_t num_steps,
                       size_t step,
                       size_t k0,
                       size_t k1,
                       float4 r_keys[2]) const;
    void cardinal_keys_for_step(const float3 *curve_keys,
                                const float *curve_radius,
                                const float3 *key_steps,
                                size_t num_curve_keys,
                                size_t num_steps,
                                size_t step,
                                size_t k0,
                                size_t k1,
                                size_t k2,
                                size_t k3,
                                float4 r_keys[4]) const;
  };

  NODE_SOCKET_API_ARRAY(array<float3>, curve_keys)
  NODE_SOCKET_API_ARRAY(array<float>, curve_radius)
  NODE_SOCKET_API_ARRAY(array<int>, curve_first_key)
  NODE_SOCKET_API_ARRAY(array<int>, curve_shader)

  /* BVH */
  size_t curve_key_offset;
  size_t curve_segment_offset;
  CurveShapeType curve_shape;

  /* Constructor/Destructor */
  Hair();
  ~Hair();

  /* Geometry */
  void clear(bool preserve_shaders = false) override;

  void resize_curves(int numcurves, int numkeys);
  void reserve_curves(int numcurves, int numkeys);
  void add_curve_key(float3 loc, float radius);
  void add_curve(int first_key, int shader);

  void copy_center_to_motion_step(const int motion_step);

  void compute_bounds() override;
  void apply_transform(const Transform &tfm, const bool apply_to_motion) override;

  /* Curves */
  Curve get_curve(size_t i) const
  {
    int first = curve_first_key[i];
    int next_first = (i + 1 < curve_first_key.size()) ? curve_first_key[i + 1] : curve_keys.size();

    Curve curve = {first, next_first - first};
    return curve;
  }

  size_t num_keys() const
  {
    return curve_keys.size();
  }

  size_t num_curves() const
  {
    return curve_first_key.size();
  }

  size_t num_segments() const
  {
    return curve_keys.size() - curve_first_key.size();
  }

  /* UDIM */
  void get_uv_tiles(ustring map, unordered_set<int> &tiles) override;

  /* BVH */
  void pack_curves(Scene *scene,
                   float4 *curve_key_co,
                   KernelCurve *curve,
                   KernelCurveSegment *curve_segments);

  PrimitiveType primitive_type() const override;

  /* Attributes */
  bool need_shadow_transparency();
  bool update_shadow_transparency(Device *device, Scene *scene, Progress &progress);
};

CCL_NAMESPACE_END

#endif /* __HAIR_H__ */
