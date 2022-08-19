/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. All rights reserved. */
#include "IO_abstract_hierarchy_iterator.h"

#include "testing/testing.h"

#include "BLI_utildefines.h"

namespace blender::io {

namespace {

Object *fake_pointer(int value)
{
  return static_cast<Object *>(POINTER_FROM_INT(value));
}

}  // namespace

class HierarchyContextOrderTest : public testing::Test {
};

TEST_F(HierarchyContextOrderTest, ObjectPointerTest)
{
  HierarchyContext ctx_a = {nullptr};
  ctx_a.object = fake_pointer(1);
  ctx_a.duplicator = nullptr;

  HierarchyContext ctx_b = {nullptr};
  ctx_b.object = fake_pointer(2);
  ctx_b.duplicator = nullptr;

  EXPECT_LT(ctx_a, ctx_b);
  EXPECT_FALSE(ctx_b < ctx_a);
  EXPECT_FALSE(ctx_a < ctx_a);
}

TEST_F(HierarchyContextOrderTest, DuplicatorPointerTest)
{
  HierarchyContext ctx_a = {nullptr};
  ctx_a.object = fake_pointer(1);
  ctx_a.duplicator = fake_pointer(1);
  ctx_a.export_name = "A";

  HierarchyContext ctx_b = {nullptr};
  ctx_b.object = fake_pointer(1);
  ctx_b.duplicator = fake_pointer(1);
  ctx_b.export_name = "B";

  EXPECT_LT(ctx_a, ctx_b);
  EXPECT_FALSE(ctx_b < ctx_a);
  EXPECT_FALSE(ctx_a < ctx_a);
}

TEST_F(HierarchyContextOrderTest, ExportParentTest)
{
  HierarchyContext ctx_a = {nullptr};
  ctx_a.object = fake_pointer(1);
  ctx_a.export_parent = fake_pointer(1);

  HierarchyContext ctx_b = {nullptr};
  ctx_b.object = fake_pointer(1);
  ctx_b.export_parent = fake_pointer(2);

  EXPECT_LT(ctx_a, ctx_b);
  EXPECT_FALSE(ctx_b < ctx_a);
  EXPECT_FALSE(ctx_a < ctx_a);
}

TEST_F(HierarchyContextOrderTest, TransitiveTest)
{
  HierarchyContext ctx_a = {nullptr};
  ctx_a.object = fake_pointer(1);
  ctx_a.export_parent = fake_pointer(1);
  ctx_a.duplicator = nullptr;
  ctx_a.export_name = "A";

  HierarchyContext ctx_b = {nullptr};
  ctx_b.object = fake_pointer(2);
  ctx_b.export_parent = nullptr;
  ctx_b.duplicator = fake_pointer(1);
  ctx_b.export_name = "B";

  HierarchyContext ctx_c = {nullptr};
  ctx_c.object = fake_pointer(2);
  ctx_c.export_parent = fake_pointer(2);
  ctx_c.duplicator = fake_pointer(1);
  ctx_c.export_name = "C";

  HierarchyContext ctx_d = {nullptr};
  ctx_d.object = fake_pointer(2);
  ctx_d.export_parent = fake_pointer(3);
  ctx_d.duplicator = nullptr;
  ctx_d.export_name = "D";

  EXPECT_LT(ctx_a, ctx_b);
  EXPECT_LT(ctx_a, ctx_c);
  EXPECT_LT(ctx_a, ctx_d);
  EXPECT_LT(ctx_b, ctx_c);
  EXPECT_LT(ctx_b, ctx_d);
  EXPECT_LT(ctx_c, ctx_d);

  EXPECT_FALSE(ctx_b < ctx_a);
  EXPECT_FALSE(ctx_c < ctx_a);
  EXPECT_FALSE(ctx_d < ctx_a);
  EXPECT_FALSE(ctx_c < ctx_b);
  EXPECT_FALSE(ctx_d < ctx_b);
  EXPECT_FALSE(ctx_d < ctx_c);
}

}  // namespace blender::io
