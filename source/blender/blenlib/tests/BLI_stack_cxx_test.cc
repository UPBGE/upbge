/* SPDX-License-Identifier: Apache-2.0 */

#include "BLI_exception_safety_test_utils.hh"
#include "BLI_stack.hh"
#include "BLI_strict_flags.h"
#include "BLI_vector.hh"
#include "testing/testing.h"

namespace blender::tests {

TEST(stack, DefaultConstructor)
{
  Stack<int> stack;
  EXPECT_EQ(stack.size(), 0);
  EXPECT_TRUE(stack.is_empty());
}

TEST(stack, SpanConstructor)
{
  std::array<int, 3> array = {4, 7, 2};
  Stack<int> stack(array);
  EXPECT_EQ(stack.size(), 3);
  EXPECT_EQ(stack.pop(), 2);
  EXPECT_EQ(stack.pop(), 7);
  EXPECT_EQ(stack.pop(), 4);
  EXPECT_TRUE(stack.is_empty());
}

TEST(stack, CopyConstructor)
{
  Stack<int> stack1 = {1, 2, 3, 4, 5, 6, 7};
  Stack<int> stack2 = stack1;
  EXPECT_EQ(stack1.size(), 7);
  EXPECT_EQ(stack2.size(), 7);
  for (int i = 7; i >= 1; i--) {
    EXPECT_FALSE(stack1.is_empty());
    EXPECT_FALSE(stack2.is_empty());
    EXPECT_EQ(stack1.pop(), i);
    EXPECT_EQ(stack2.pop(), i);
  }
  EXPECT_TRUE(stack1.is_empty());
  EXPECT_TRUE(stack2.is_empty());
}

TEST(stack, MoveConstructor)
{
  Stack<int> stack1 = {1, 2, 3, 4, 5, 6, 7};
  Stack<int> stack2 = std::move(stack1);
  EXPECT_EQ(stack1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(stack2.size(), 7);
  for (int i = 7; i >= 1; i--) {
    EXPECT_EQ(stack2.pop(), i);
  }
}

TEST(stack, CopyAssignment)
{
  Stack<int> stack1 = {1, 2, 3, 4, 5, 6, 7};
  Stack<int> stack2 = {2, 3, 4, 5, 6, 7};
  stack2 = stack1;

  EXPECT_EQ(stack1.size(), 7);
  EXPECT_EQ(stack2.size(), 7);
  for (int i = 7; i >= 1; i--) {
    EXPECT_FALSE(stack1.is_empty());
    EXPECT_FALSE(stack2.is_empty());
    EXPECT_EQ(stack1.pop(), i);
    EXPECT_EQ(stack2.pop(), i);
  }
  EXPECT_TRUE(stack1.is_empty());
  EXPECT_TRUE(stack2.is_empty());
}

TEST(stack, MoveAssignment)
{
  Stack<int> stack1 = {1, 2, 3, 4, 5, 6, 7};
  Stack<int> stack2 = {5, 3, 7, 2, 2};
  stack2 = std::move(stack1);
  EXPECT_EQ(stack1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(stack2.size(), 7);
  for (int i = 7; i >= 1; i--) {
    EXPECT_EQ(stack2.pop(), i);
  }
}

TEST(stack, Push)
{
  Stack<int> stack;
  EXPECT_EQ(stack.size(), 0);
  stack.push(3);
  EXPECT_EQ(stack.size(), 1);
  stack.push(5);
  EXPECT_EQ(stack.size(), 2);
}

TEST(stack, PushAs)
{
  Stack<StringRef> stack;
  stack.push_as("hello", 3);
  stack.push_as("world", 1);
  EXPECT_EQ(stack.pop(), "w");
  EXPECT_EQ(stack.pop(), "hel");
}

TEST(stack, PushMultiple)
{
  Stack<int> stack;
  EXPECT_EQ(stack.size(), 0);
  stack.push_multiple({1, 2, 3});
  EXPECT_EQ(stack.size(), 3);
  EXPECT_EQ(stack.pop(), 3);
  EXPECT_EQ(stack.pop(), 2);
  EXPECT_EQ(stack.pop(), 1);
}

TEST(stack, PushPopMany)
{
  Stack<int> stack;
  for (int i = 0; i < 1000; i++) {
    stack.push(i);
    EXPECT_EQ(stack.size(), static_cast<unsigned int>(i + 1));
  }
  for (int i = 999; i > 50; i--) {
    EXPECT_EQ(stack.pop(), i);
    EXPECT_EQ(stack.size(), static_cast<unsigned int>(i));
  }
  for (int i = 51; i < 5000; i++) {
    stack.push(i);
    EXPECT_EQ(stack.size(), static_cast<unsigned int>(i + 1));
  }
  for (int i = 4999; i >= 0; i--) {
    EXPECT_EQ(stack.pop(), i);
    EXPECT_EQ(stack.size(), static_cast<unsigned int>(i));
  }
}

TEST(stack, PushMultipleAfterPop)
{
  Stack<int> stack;
  for (int i = 0; i < 1000; i++) {
    stack.push(i);
  }
  for (int i = 999; i >= 0; i--) {
    EXPECT_EQ(stack.pop(), i);
  }

  Vector<int> values;
  for (int i = 0; i < 5000; i++) {
    values.append(i);
  }
  stack.push_multiple(values);
  EXPECT_EQ(stack.size(), 5000);

  for (int i = 4999; i >= 0; i--) {
    EXPECT_EQ(stack.pop(), i);
  }
}

TEST(stack, Pop)
{
  Stack<int> stack;
  stack.push(4);
  stack.push(6);
  EXPECT_EQ(stack.pop(), 6);
  EXPECT_EQ(stack.pop(), 4);
}

TEST(stack, Peek)
{
  Stack<int> stack;
  stack.push(3);
  stack.push(4);
  EXPECT_EQ(stack.peek(), 4);
  EXPECT_EQ(stack.peek(), 4);
  stack.pop();
  EXPECT_EQ(stack.peek(), 3);
}

TEST(stack, UniquePtrValues)
{
  Stack<std::unique_ptr<int>> stack;
  stack.push(std::make_unique<int>());
  stack.push(std::make_unique<int>());
  std::unique_ptr<int> a = stack.pop();
  std::unique_ptr<int> &b = stack.peek();
  UNUSED_VARS(a, b);
}

TEST(stack, OveralignedValues)
{
  Stack<AlignedBuffer<1, 512>, 2> stack;
  for (int i = 0; i < 100; i++) {
    stack.push({});
    EXPECT_EQ((uintptr_t)&stack.peek() % 512, 0);
  }
}

TEST(stack, SpanConstructorExceptions)
{
  std::array<ExceptionThrower, 5> values;
  values[3].throw_during_copy = true;
  EXPECT_ANY_THROW({ Stack<ExceptionThrower> stack(values); });
}

TEST(stack, MoveConstructorExceptions)
{
  Stack<ExceptionThrower, 4> stack;
  stack.push({});
  stack.push({});
  stack.peek().throw_during_move = true;
  EXPECT_ANY_THROW({ Stack<ExceptionThrower> moved_stack{std::move(stack)}; });
}

TEST(stack, PushExceptions)
{
  Stack<ExceptionThrower, 2> stack;
  stack.push({});
  stack.push({});
  ExceptionThrower *ptr1 = &stack.peek();
  ExceptionThrower value;
  value.throw_during_copy = true;
  EXPECT_ANY_THROW({ stack.push(value); });
  EXPECT_EQ(stack.size(), 2);
  ExceptionThrower *ptr2 = &stack.peek();
  EXPECT_EQ(ptr1, ptr2);
  EXPECT_TRUE(stack.is_invariant_maintained());
}

TEST(stack, PopExceptions)
{
  Stack<ExceptionThrower> stack;
  stack.push({});
  stack.peek().throw_during_move = true;
  stack.push({});
  stack.pop();                        /* NOLINT: bugprone-throw-keyword-missing */
  EXPECT_ANY_THROW({ stack.pop(); }); /* NOLINT: bugprone-throw-keyword-missing */
  EXPECT_EQ(stack.size(), 1);
  EXPECT_TRUE(stack.is_invariant_maintained());
}

TEST(stack, PushMultipleExceptions)
{
  Stack<ExceptionThrower> stack;
  stack.push({});
  std::array<ExceptionThrower, 100> values;
  values[6].throw_during_copy = true;
  EXPECT_ANY_THROW({ stack.push_multiple(values); });
  EXPECT_TRUE(stack.is_invariant_maintained());
  EXPECT_ANY_THROW({ stack.push_multiple(values); });
  EXPECT_TRUE(stack.is_invariant_maintained());
}

}  // namespace blender::tests
