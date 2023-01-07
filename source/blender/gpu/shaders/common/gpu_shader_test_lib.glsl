
// clang-format off
#ifndef GPU_METAL
bool is_integer(bool v) { return true; }
#endif
bool is_integer(uint v) { return true; }
bool is_integer(int v) { return true; }
bool is_integer(float v) { return false; }
bool is_integer(ivec2 v) { return true; }
bool is_integer(ivec3 v) { return true; }
bool is_integer(ivec4 v) { return true; }
bool is_integer(uvec2 v) { return true; }
bool is_integer(uvec3 v) { return true; }
bool is_integer(uvec4 v) { return true; }
bool is_integer(vec2 v) { return false; }
bool is_integer(vec3 v) { return false; }
bool is_integer(vec4 v) { return false; }
bool is_integer(mat2x2 v) { return false; }
bool is_integer(mat2x3 v) { return false; }
bool is_integer(mat2x4 v) { return false; }
bool is_integer(mat3x2 v) { return false; }
bool is_integer(mat3x3 v) { return false; }
bool is_integer(mat3x4 v) { return false; }
bool is_integer(mat4x2 v) { return false; }
bool is_integer(mat4x3 v) { return false; }
bool is_integer(mat4x4 v) { return false; }

int mat_row_len(mat2x2 v) { return 2; }
int mat_row_len(mat2x3 v) { return 3; }
int mat_row_len(mat2x4 v) { return 4; }
int mat_row_len(mat3x2 v) { return 2; }
int mat_row_len(mat3x3 v) { return 3; }
int mat_row_len(mat3x4 v) { return 4; }
int mat_row_len(mat4x2 v) { return 2; }
int mat_row_len(mat4x3 v) { return 3; }
int mat_row_len(mat4x4 v) { return 4; }

int mat_col_len(mat2x2 v) { return 2; }
int mat_col_len(mat2x3 v) { return 2; }
int mat_col_len(mat2x4 v) { return 2; }
int mat_col_len(mat3x2 v) { return 3; }
int mat_col_len(mat3x3 v) { return 3; }
int mat_col_len(mat3x4 v) { return 3; }
int mat_col_len(mat4x2 v) { return 4; }
int mat_col_len(mat4x3 v) { return 4; }
int mat_col_len(mat4x4 v) { return 4; }
int mat_col_len(ivec2 v) { return 2; }
int mat_col_len(ivec3 v) { return 3; }
int mat_col_len(ivec4 v) { return 4; }
int mat_col_len(uvec2 v) { return 2; }
int mat_col_len(uvec3 v) { return 3; }
int mat_col_len(uvec4 v) { return 4; }
int mat_col_len(vec2 v) { return 2; }
int mat_col_len(vec3 v) { return 3; }
int mat_col_len(vec4 v) { return 4; }

#ifndef GPU_METAL
uint to_type(bool v) { return TEST_TYPE_BOOL; }
#endif
uint to_type(uint v) { return TEST_TYPE_UINT; }
uint to_type(int v) { return TEST_TYPE_INT; }
uint to_type(float v) { return TEST_TYPE_FLOAT; }
uint to_type(ivec2 v) { return TEST_TYPE_IVEC2; }
uint to_type(ivec3 v) { return TEST_TYPE_IVEC3; }
uint to_type(ivec4 v) { return TEST_TYPE_IVEC4; }
uint to_type(uvec2 v) { return TEST_TYPE_UVEC2; }
uint to_type(uvec3 v) { return TEST_TYPE_UVEC3; }
uint to_type(uvec4 v) { return TEST_TYPE_UVEC4; }
uint to_type(vec2 v) { return TEST_TYPE_VEC2; }
uint to_type(vec3 v) { return TEST_TYPE_VEC3; }
uint to_type(vec4 v) { return TEST_TYPE_VEC4; }
uint to_type(mat2x2 v) { return TEST_TYPE_MAT2X2; }
uint to_type(mat2x3 v) { return TEST_TYPE_MAT2X3; }
uint to_type(mat2x4 v) { return TEST_TYPE_MAT2X4; }
uint to_type(mat3x2 v) { return TEST_TYPE_MAT3X2; }
uint to_type(mat3x3 v) { return TEST_TYPE_MAT3X3; }
uint to_type(mat3x4 v) { return TEST_TYPE_MAT3X4; }
uint to_type(mat4x2 v) { return TEST_TYPE_MAT4X2; }
uint to_type(mat4x3 v) { return TEST_TYPE_MAT4X3; }
uint to_type(mat4x4 v) { return TEST_TYPE_MAT4X4; }
// clang-format on

#define WRITE_MATRIX(v) \
  TestOutputRawData raw; \
  for (int c = 0; c < mat_col_len(v); c++) { \
    for (int r = 0; r < mat_row_len(v); r++) { \
      raw.data[c * mat_row_len(v) + r] = floatBitsToUint(v[c][r]); \
    } \
  } \
  return raw;

#define WRITE_FLOAT_VECTOR(v) \
  TestOutputRawData raw; \
  for (int c = 0; c < mat_col_len(v); c++) { \
    raw.data[c] = floatBitsToUint(v[c]); \
  } \
  return raw;

#define WRITE_INT_VECTOR(v) \
  TestOutputRawData raw; \
  for (int c = 0; c < mat_col_len(v); c++) { \
    raw.data[c] = uint(v[c]); \
  } \
  return raw;

#define WRITE_FLOAT_SCALAR(v) \
  TestOutputRawData raw; \
  raw.data[0] = floatBitsToUint(v); \
  return raw;

#define WRITE_INT_SCALAR(v) \
  TestOutputRawData raw; \
  raw.data[0] = uint(v); \
  return raw;

// clang-format off
#ifndef GPU_METAL
TestOutputRawData as_raw_data(bool v) { WRITE_INT_SCALAR(v); }
#endif
TestOutputRawData as_raw_data(uint v) { WRITE_INT_SCALAR(v); }
TestOutputRawData as_raw_data(int v) { WRITE_INT_SCALAR(v); }
TestOutputRawData as_raw_data(float v) { WRITE_FLOAT_SCALAR(v); }
TestOutputRawData as_raw_data(ivec2 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(ivec3 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(ivec4 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(uvec2 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(uvec3 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(uvec4 v) { WRITE_INT_VECTOR(v); }
TestOutputRawData as_raw_data(vec2 v) { WRITE_FLOAT_VECTOR(v); }
TestOutputRawData as_raw_data(vec3 v) { WRITE_FLOAT_VECTOR(v); }
TestOutputRawData as_raw_data(vec4 v) { WRITE_FLOAT_VECTOR(v); }
TestOutputRawData as_raw_data(mat2x2 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat2x3 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat2x4 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat3x2 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat3x3 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat3x4 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat4x2 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat4x3 v) { WRITE_MATRIX(v); }
TestOutputRawData as_raw_data(mat4x4 v) { WRITE_MATRIX(v); }
// clang-format on

int g_test_id = 0;

#ifdef GPU_COMPUTE_SHADER

#  define EXPECT_OP(OP, val1, val2) \
    out_test[g_test_id++] = test_output( \
        as_raw_data(val1), as_raw_data(val2), bool(OP), int(__LINE__), to_type(val1))
#else

/** WORKAROUND: Fragment shader variant for older platform. */
#  define EXPECT_OP(OP, val1, val2) \
    g_test_id += 1; \
    if (g_test_id == 1) { \
      /* Avoid pixels with undefined values. */ \
      out_test = uvec4(0); \
    } \
    if (int(gl_FragCoord.y) == g_test_id - 1) { \
      TestOutput to = test_output( \
          as_raw_data(val1), as_raw_data(val2), bool(OP), int(__LINE__), to_type(val1)); \
      switch (int(gl_FragCoord.x)) { \
        case 0: \
          out_test = uvec4( \
              to.expect.data[0], to.expect.data[1], to.expect.data[2], to.expect.data[3]); \
          break; \
        case 1: \
          out_test = uvec4( \
              to.expect.data[4], to.expect.data[5], to.expect.data[6], to.expect.data[7]); \
          break; \
        case 2: \
          out_test = uvec4( \
              to.expect.data[8], to.expect.data[9], to.expect.data[10], to.expect.data[11]); \
          break; \
        case 3: \
          out_test = uvec4( \
              to.expect.data[12], to.expect.data[13], to.expect.data[14], to.expect.data[15]); \
          break; \
        case 4: \
          out_test = uvec4( \
              to.result.data[0], to.result.data[1], to.result.data[2], to.result.data[3]); \
          break; \
        case 5: \
          out_test = uvec4( \
              to.result.data[4], to.result.data[5], to.result.data[6], to.result.data[7]); \
          break; \
        case 6: \
          out_test = uvec4( \
              to.result.data[8], to.result.data[9], to.result.data[10], to.result.data[11]); \
          break; \
        case 7: \
          out_test = uvec4( \
              to.result.data[12], to.result.data[13], to.result.data[14], to.result.data[15]); \
          break; \
        case 8: \
          out_test = uvec4(to.status, to.line, to.type, 0); \
          break; \
      } \
    }

#endif

#define EXPECT_EQ(result, expect) EXPECT_OP((result) == (expect), result, expect)
#define EXPECT_NE(result, expect) EXPECT_OP((result) != (expect), result, expect)
#define EXPECT_LE(result, expect) EXPECT_OP((result) <= (expect), result, expect)
#define EXPECT_LT(result, expect) EXPECT_OP((result) < (expect), result, expect)
#define EXPECT_GE(result, expect) EXPECT_OP((result) >= (expect), result, expect)
#define EXPECT_GT(result, expect) EXPECT_OP((result) > (expect), result, expect)

#define EXPECT_TRUE(result) EXPECT_OP(result, result, true)
#define EXPECT_FALSE(result) EXPECT_OP(!result, result, false)

#define EXPECT_NEAR(result, expect, threshold) \
  EXPECT_OP(is_equal(result, expect, threshold), result, expect)
