/* SPDX-License-Identifier: Apache-2.0 */

#include "draw_testing.hh"

#include "GPU_shader.h"

#include "draw_manager_testing.h"

namespace blender::draw {

/* Base class for draw test cases. It will setup and tear down the GPU part around each test. */
void DrawOpenGLTest::SetUp()
{
  GPUOpenGLTest::SetUp();
  DRW_draw_state_init_gtests(GPU_SHADER_CFG_DEFAULT);
}

}  // namespace blender::draw
