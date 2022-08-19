/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include "testing/testing.h"
#include "util/system.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

static bool validate_cpu_capabilities()
{

#ifdef __KERNEL_AVX2__
  return system_cpu_support_avx2();
#else
#  ifdef __KERNEL_AVX__
  return system_cpu_support_avx();
#  endif
#endif
}

#define INIT_AVX_TEST \
  if (!validate_cpu_capabilities()) \
    return; \
\
  const avxf avxf_a(0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f); \
  const avxf avxf_b(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f); \
  const avxf avxf_c(1.1f, 2.2f, 3.3f, 4.4f, 5.5f, 6.6f, 7.7f, 8.8f);

#define compare_vector_scalar(a, b) \
  for (size_t index = 0; index < a.size; index++) \
    EXPECT_FLOAT_EQ(a[index], b);

#define compare_vector_vector(a, b) \
  for (size_t index = 0; index < a.size; index++) \
    EXPECT_FLOAT_EQ(a[index], b[index]);

#define compare_vector_vector_near(a, b, abserror) \
  for (size_t index = 0; index < a.size; index++) \
    EXPECT_NEAR(a[index], b[index], abserror);

#define basic_test_vv(a, b, op) \
  INIT_AVX_TEST \
  avxf c = a op b; \
  for (size_t i = 0; i < a.size; i++) \
    EXPECT_FLOAT_EQ(c[i], a[i] op b[i]);

/* vector op float tests */
#define basic_test_vf(a, b, op) \
  INIT_AVX_TEST \
  avxf c = a op b; \
  for (size_t i = 0; i < a.size; i++) \
    EXPECT_FLOAT_EQ(c[i], a[i] op b);

static const float float_b = 1.5f;

TEST(TEST_CATEGORY_NAME, avxf_add_vv){basic_test_vv(avxf_a, avxf_b, +)} TEST(TEST_CATEGORY_NAME,
                                                                             avxf_sub_vv){
    basic_test_vv(avxf_a, avxf_b, -)} TEST(TEST_CATEGORY_NAME, avxf_mul_vv){
    basic_test_vv(avxf_a, avxf_b, *)} TEST(TEST_CATEGORY_NAME, avxf_div_vv){
    basic_test_vv(avxf_a, avxf_b, /)} TEST(TEST_CATEGORY_NAME, avxf_add_vf){
    basic_test_vf(avxf_a, float_b, +)} TEST(TEST_CATEGORY_NAME, avxf_sub_vf){
    basic_test_vf(avxf_a, float_b, -)} TEST(TEST_CATEGORY_NAME, avxf_mul_vf){
    basic_test_vf(avxf_a, float_b, *)} TEST(TEST_CATEGORY_NAME,
                                            avxf_div_vf){basic_test_vf(avxf_a, float_b, /)}

TEST(TEST_CATEGORY_NAME, avxf_ctor)
{
  INIT_AVX_TEST
  compare_vector_scalar(avxf(7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f),
                        static_cast<float>(index));
  compare_vector_scalar(avxf(1.0f), 1.0f);
  compare_vector_vector(avxf(1.0f, 2.0f), avxf(1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f, 2.0f));
  compare_vector_vector(avxf(1.0f, 2.0f, 3.0f, 4.0f),
                        avxf(1.0f, 2.0f, 3.0f, 4.0f, 1.0f, 2.0f, 3.0f, 4.0f));
  compare_vector_vector(avxf(make_float3(1.0f, 2.0f, 3.0f)),
                        avxf(0.0f, 3.0f, 2.0f, 1.0f, 0.0f, 3.0f, 2.0f, 1.0f));
}

TEST(TEST_CATEGORY_NAME, avxf_sqrt)
{
  INIT_AVX_TEST
  compare_vector_vector(mm256_sqrt(avxf(1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f, 49.0f, 64.0f)),
                        avxf(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f));
}

TEST(TEST_CATEGORY_NAME, avxf_min_max)
{
  INIT_AVX_TEST
  compare_vector_vector(min(avxf_a, avxf_b), avxf_a);
  compare_vector_vector(max(avxf_a, avxf_b), avxf_b);
}

TEST(TEST_CATEGORY_NAME, avxf_set_sign)
{
  INIT_AVX_TEST
  avxf res = set_sign_bit<1, 0, 0, 0, 0, 0, 0, 0>(avxf_a);
  compare_vector_vector(res, avxf(0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, -0.8f));
}

TEST(TEST_CATEGORY_NAME, avxf_msub)
{
  INIT_AVX_TEST
  avxf res = msub(avxf_a, avxf_b, avxf_c);
  avxf exp = avxf((avxf_a[7] * avxf_b[7]) - avxf_c[7],
                  (avxf_a[6] * avxf_b[6]) - avxf_c[6],
                  (avxf_a[5] * avxf_b[5]) - avxf_c[5],
                  (avxf_a[4] * avxf_b[4]) - avxf_c[4],
                  (avxf_a[3] * avxf_b[3]) - avxf_c[3],
                  (avxf_a[2] * avxf_b[2]) - avxf_c[2],
                  (avxf_a[1] * avxf_b[1]) - avxf_c[1],
                  (avxf_a[0] * avxf_b[0]) - avxf_c[0]);
  compare_vector_vector(res, exp);
}

TEST(TEST_CATEGORY_NAME, avxf_madd)
{
  INIT_AVX_TEST
  avxf res = madd(avxf_a, avxf_b, avxf_c);
  avxf exp = avxf((avxf_a[7] * avxf_b[7]) + avxf_c[7],
                  (avxf_a[6] * avxf_b[6]) + avxf_c[6],
                  (avxf_a[5] * avxf_b[5]) + avxf_c[5],
                  (avxf_a[4] * avxf_b[4]) + avxf_c[4],
                  (avxf_a[3] * avxf_b[3]) + avxf_c[3],
                  (avxf_a[2] * avxf_b[2]) + avxf_c[2],
                  (avxf_a[1] * avxf_b[1]) + avxf_c[1],
                  (avxf_a[0] * avxf_b[0]) + avxf_c[0]);
  compare_vector_vector(res, exp);
}

TEST(TEST_CATEGORY_NAME, avxf_nmadd)
{
  INIT_AVX_TEST
  avxf res = nmadd(avxf_a, avxf_b, avxf_c);
  avxf exp = avxf(avxf_c[7] - (avxf_a[7] * avxf_b[7]),
                  avxf_c[6] - (avxf_a[6] * avxf_b[6]),
                  avxf_c[5] - (avxf_a[5] * avxf_b[5]),
                  avxf_c[4] - (avxf_a[4] * avxf_b[4]),
                  avxf_c[3] - (avxf_a[3] * avxf_b[3]),
                  avxf_c[2] - (avxf_a[2] * avxf_b[2]),
                  avxf_c[1] - (avxf_a[1] * avxf_b[1]),
                  avxf_c[0] - (avxf_a[0] * avxf_b[0]));
  compare_vector_vector(res, exp);
}

TEST(TEST_CATEGORY_NAME, avxf_compare)
{
  INIT_AVX_TEST
  avxf a(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
  avxf b(7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f);
  avxb res = a <= b;
  int exp[8] = {
      a[0] <= b[0] ? -1 : 0,
      a[1] <= b[1] ? -1 : 0,
      a[2] <= b[2] ? -1 : 0,
      a[3] <= b[3] ? -1 : 0,
      a[4] <= b[4] ? -1 : 0,
      a[5] <= b[5] ? -1 : 0,
      a[6] <= b[6] ? -1 : 0,
      a[7] <= b[7] ? -1 : 0,
  };
  compare_vector_vector(res, exp);
}

TEST(TEST_CATEGORY_NAME, avxf_permute)
{
  INIT_AVX_TEST
  avxf res = permute<3, 0, 1, 7, 6, 5, 2, 4>(avxf_b);
  compare_vector_vector(res, avxf(4.0f, 6.0f, 3.0f, 2.0f, 1.0f, 7.0f, 8.0f, 5.0f));
}

TEST(TEST_CATEGORY_NAME, avxf_blend)
{
  INIT_AVX_TEST
  avxf res = blend<0, 0, 1, 0, 1, 0, 1, 0>(avxf_a, avxf_b);
  compare_vector_vector(res, avxf(0.1f, 0.2f, 3.0f, 0.4f, 5.0f, 0.6f, 7.0f, 0.8f));
}

TEST(TEST_CATEGORY_NAME, avxf_shuffle)
{
  INIT_AVX_TEST
  avxf res = shuffle<0, 1, 2, 3, 1, 3, 2, 0>(avxf_a);
  compare_vector_vector(res, avxf(0.4f, 0.2f, 0.1f, 0.3f, 0.5f, 0.6f, 0.7f, 0.8f));
}

TEST(TEST_CATEGORY_NAME, avxf_cross)
{
  INIT_AVX_TEST
  avxf res = cross(avxf_b, avxf_c);
  compare_vector_vector_near(res,
                             avxf(0.0f,
                                  -9.5367432e-07f,
                                  0.0f,
                                  4.7683716e-07f,
                                  0.0f,
                                  -3.8146973e-06f,
                                  3.8146973e-06f,
                                  3.8146973e-06f),
                             0.000002000f);
}

TEST(TEST_CATEGORY_NAME, avxf_dot3)
{
  INIT_AVX_TEST
  float den, den2;
  dot3(avxf_a, avxf_b, den, den2);
  EXPECT_FLOAT_EQ(den, 14.9f);
  EXPECT_FLOAT_EQ(den2, 2.9f);
}

CCL_NAMESPACE_END
