/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. */
#include "blendfile_loading_base_test.h"

class BlendfileLoadingTest : public BlendfileLoadingBaseTest {
};

TEST_F(BlendfileLoadingTest, CanaryTest)
{
  /* Load the smallest blend file we have in the SVN lib/tests directory. */
  if (!blendfile_load("modifier_stack/array_test.blend")) {
    return;
  }
  depsgraph_create(DAG_EVAL_RENDER);
  EXPECT_NE(nullptr, this->depsgraph);
}
