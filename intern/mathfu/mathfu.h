#ifndef __MATHFU_H__
#define __MATHFU_H__

#define MATHFU_COMPILE_WITH_SIMD

#include "mathfu/glsl_mappings.h"
#include "mathfu/constants.h"
#include "mathfu/frustum.h"
#include "mathfu/io.h"

namespace mt = mathfu;

namespace mathfu {

static const mt::vec3 zero3 = mt::kZeros3f;
static const mt::vec3 one3 = mt::kOnes3f;
static const mt::vec3 axisX3 = mt::kAxisX3f;
static const mt::vec3 axisY3 = mt::kAxisY3f;
static const mt::vec3 axisZ3 = mt::kAxisZ3f;

static const mt::vec2 zero2 = mt::kZeros2f;
static const mt::vec2 one2 = mt::kOnes2f;
static const mt::vec2 axisX2 = mt::kAxisX2f;
static const mt::vec2 axisY2 = mt::kAxisY2f;

static const mt::vec4 zero4 = mt::kZeros4f;
static const mt::vec4 one4 = mt::kOnes4f;
static const mt::vec4 axisX4 = mt::kAxisX4f;
static const mt::vec4 axisY4 = mt::kAxisY4f;
static const mt::vec4 axisW4 = mt::kAxisW4f;

}

#endif  // __MATHFU_H__
