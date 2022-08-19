/* SPDX-License-Identifier: Apache-2.0 */

#include "BLI_exception_safety_test_utils.hh"
#include "BLI_strict_flags.h"
#include "BLI_vector.hh"
#include "testing/testing.h"
#include <forward_list>

namespace blender::tests {

TEST(vector, DefaultConstructor)
{
  Vector<int> vec;
  EXPECT_EQ(vec.size(), 0);
}

TEST(vector, SizeConstructor)
{
  Vector<int> vec(3);
  EXPECT_EQ(vec.size(), 3);
}

/**
 * Tests that the trivially constructible types are not zero-initialized. We do not want that for
 * performance reasons.
 */
TEST(vector, TrivialTypeSizeConstructor)
{
  Vector<char, 1> *vec = new Vector<char, 1>(1);
  char *ptr = &(*vec)[0];
  vec->~Vector();

  const char magic = 42;
  *ptr = magic;
  EXPECT_EQ(*ptr, magic);

  new (vec) Vector<char, 1>(1);
  EXPECT_EQ((*vec)[0], magic);
  EXPECT_EQ(*ptr, magic);
  delete vec;
}

TEST(vector, SizeValueConstructor)
{
  Vector<int> vec(4, 10);
  EXPECT_EQ(vec.size(), 4);
  EXPECT_EQ(vec[0], 10);
  EXPECT_EQ(vec[1], 10);
  EXPECT_EQ(vec[2], 10);
  EXPECT_EQ(vec[3], 10);
}

TEST(vector, InitializerListConstructor)
{
  Vector<int> vec = {1, 3, 4, 6};
  EXPECT_EQ(vec.size(), 4);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 3);
  EXPECT_EQ(vec[2], 4);
  EXPECT_EQ(vec[3], 6);
}

TEST(vector, ConvertingConstructor)
{
  std::array<float, 5> values = {5.4f, 7.3f, -8.1f, 5.0f, 0.0f};
  Vector<int> vec = values;
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ(vec[0], 5);
  EXPECT_EQ(vec[1], 7);
  EXPECT_EQ(vec[2], -8);
  EXPECT_EQ(vec[3], 5);
  EXPECT_EQ(vec[4], 0);
}

struct TestListValue {
  TestListValue *next, *prev;
  int value;
};

TEST(vector, ListBaseConstructor)
{
  TestListValue *value1 = new TestListValue{nullptr, nullptr, 4};
  TestListValue *value2 = new TestListValue{nullptr, nullptr, 5};
  TestListValue *value3 = new TestListValue{nullptr, nullptr, 6};

  ListBase list = {nullptr, nullptr};
  BLI_addtail(&list, value1);
  BLI_addtail(&list, value2);
  BLI_addtail(&list, value3);
  Vector<TestListValue *> vec(list);

  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0]->value, 4);
  EXPECT_EQ(vec[1]->value, 5);
  EXPECT_EQ(vec[2]->value, 6);

  delete value1;
  delete value2;
  delete value3;
}

TEST(vector, IteratorConstructor)
{
  std::forward_list<int> list;
  list.push_front(3);
  list.push_front(1);
  list.push_front(5);

  Vector<int> vec = Vector<int>(list.begin(), list.end());
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 5);
  EXPECT_EQ(vec[1], 1);
  EXPECT_EQ(vec[2], 3);
}

TEST(vector, CopyConstructor)
{
  Vector<int> vec1 = {1, 2, 3};
  Vector<int> vec2(vec1);
  EXPECT_EQ(vec2.size(), 3);
  EXPECT_EQ(vec2[0], 1);
  EXPECT_EQ(vec2[1], 2);
  EXPECT_EQ(vec2[2], 3);

  vec1[1] = 5;
  EXPECT_EQ(vec1[1], 5);
  EXPECT_EQ(vec2[1], 2);
}

TEST(vector, CopyConstructor2)
{
  Vector<int, 2> vec1 = {1, 2, 3, 4};
  Vector<int, 3> vec2(vec1);

  EXPECT_EQ(vec1.size(), 4);
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_NE(vec1.data(), vec2.data());
  EXPECT_EQ(vec2[0], 1);
  EXPECT_EQ(vec2[1], 2);
  EXPECT_EQ(vec2[2], 3);
  EXPECT_EQ(vec2[3], 4);
}

TEST(vector, CopyConstructor3)
{
  Vector<int, 20> vec1 = {1, 2, 3, 4};
  Vector<int, 1> vec2(vec1);

  EXPECT_EQ(vec1.size(), 4);
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_NE(vec1.data(), vec2.data());
  EXPECT_EQ(vec2[2], 3);
}

TEST(vector, CopyConstructor4)
{
  Vector<int, 5> vec1 = {1, 2, 3, 4};
  Vector<int, 6> vec2(vec1);

  EXPECT_EQ(vec1.size(), 4);
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_NE(vec1.data(), vec2.data());
  EXPECT_EQ(vec2[3], 4);
}

TEST(vector, MoveConstructor)
{
  Vector<int> vec1 = {1, 2, 3, 4};
  Vector<int> vec2(std::move(vec1));

  EXPECT_EQ(vec1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_EQ(vec2[0], 1);
  EXPECT_EQ(vec2[1], 2);
  EXPECT_EQ(vec2[2], 3);
  EXPECT_EQ(vec2[3], 4);
}

TEST(vector, MoveConstructor2)
{
  Vector<int, 2> vec1 = {1, 2, 3, 4};
  Vector<int, 3> vec2(std::move(vec1));

  EXPECT_EQ(vec1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_EQ(vec2[0], 1);
  EXPECT_EQ(vec2[1], 2);
  EXPECT_EQ(vec2[2], 3);
  EXPECT_EQ(vec2[3], 4);
}

TEST(vector, MoveConstructor3)
{
  Vector<int, 20> vec1 = {1, 2, 3, 4};
  Vector<int, 1> vec2(std::move(vec1));

  EXPECT_EQ(vec1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_EQ(vec2[2], 3);
}

TEST(vector, MoveConstructor4)
{
  Vector<int, 5> vec1 = {1, 2, 3, 4};
  Vector<int, 6> vec2(std::move(vec1));

  EXPECT_EQ(vec1.size(), 0); /* NOLINT: bugprone-use-after-move */
  EXPECT_EQ(vec2.size(), 4);
  EXPECT_EQ(vec2[3], 4);
}

TEST(vector, MoveAssignment)
{
  Vector<int> vec = {1, 2};
  EXPECT_EQ(vec.size(), 2);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);

  vec = Vector<int>({5});
  EXPECT_EQ(vec.size(), 1);
  EXPECT_EQ(vec[0], 5);
}

TEST(vector, CopyAssignment)
{
  Vector<int> vec1 = {1, 2, 3};
  Vector<int> vec2 = {4, 5};
  EXPECT_EQ(vec1.size(), 3);
  EXPECT_EQ(vec2.size(), 2);

  vec2 = vec1;
  EXPECT_EQ(vec2.size(), 3);

  vec1[0] = 7;
  EXPECT_EQ(vec1[0], 7);
  EXPECT_EQ(vec2[0], 1);
}

TEST(vector, Append)
{
  Vector<int> vec;
  vec.append(3);
  vec.append(6);
  vec.append(7);
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 3);
  EXPECT_EQ(vec[1], 6);
  EXPECT_EQ(vec[2], 7);
}

TEST(vector, AppendAs)
{
  Vector<StringRef> vec;
  vec.append_as("hello", 2);
  vec.append_as("world", 3);
  EXPECT_EQ(vec[0], "he");
  EXPECT_EQ(vec[1], "wor");
}

TEST(vector, AppendAndGetIndex)
{
  Vector<int> vec;
  EXPECT_EQ(vec.append_and_get_index(10), 0);
  EXPECT_EQ(vec.append_and_get_index(10), 1);
  EXPECT_EQ(vec.append_and_get_index(10), 2);
  vec.append(10);
  int value = 10;
  EXPECT_EQ(vec.append_and_get_index(value), 4);
}

TEST(vector, AppendNonDuplicates)
{
  Vector<int> vec;
  vec.append_non_duplicates(4);
  EXPECT_EQ(vec.size(), 1);
  vec.append_non_duplicates(5);
  EXPECT_EQ(vec.size(), 2);
  vec.append_non_duplicates(4);
  EXPECT_EQ(vec.size(), 2);
}

TEST(vector, ExtendNonDuplicates)
{
  Vector<int> vec;
  vec.extend_non_duplicates({1, 2});
  EXPECT_EQ(vec.size(), 2);
  vec.extend_non_duplicates({3, 4});
  EXPECT_EQ(vec.size(), 4);
  vec.extend_non_duplicates({0, 1, 2, 3});
  EXPECT_EQ(vec.size(), 5);
}

TEST(vector, ExtendIterator)
{
  Vector<int> vec = {3, 4, 5};
  std::forward_list<int> list = {8, 9};
  vec.extend(list.begin(), list.end());
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span({3, 4, 5, 8, 9}).data(), 5);
}

TEST(vector, Iterator)
{
  Vector<int> vec({1, 4, 9, 16});
  int i = 1;
  for (int value : vec) {
    EXPECT_EQ(value, i * i);
    i++;
  }
}

TEST(vector, BecomeLarge)
{
  Vector<int, 4> vec;
  for (int i = 0; i < 100; i++) {
    vec.append(i * 5);
  }
  EXPECT_EQ(vec.size(), 100);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(vec[i], static_cast<int>(i * 5));
  }
}

static Vector<int> return_by_value_helper()
{
  return Vector<int>({3, 5, 1});
}

TEST(vector, ReturnByValue)
{
  Vector<int> vec = return_by_value_helper();
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 3);
  EXPECT_EQ(vec[1], 5);
  EXPECT_EQ(vec[2], 1);
}

TEST(vector, VectorOfVectors_Append)
{
  Vector<Vector<int>> vec;
  EXPECT_EQ(vec.size(), 0);

  Vector<int> v({1, 2});
  vec.append(v);
  vec.append({7, 8});
  EXPECT_EQ(vec.size(), 2);
  EXPECT_EQ(vec[0][0], 1);
  EXPECT_EQ(vec[0][1], 2);
  EXPECT_EQ(vec[1][0], 7);
  EXPECT_EQ(vec[1][1], 8);
}

TEST(vector, RemoveLast)
{
  Vector<int> vec = {5, 6};
  EXPECT_EQ(vec.size(), 2);
  vec.remove_last();
  EXPECT_EQ(vec.size(), 1);
  vec.remove_last();
  EXPECT_EQ(vec.size(), 0);
}

TEST(vector, IsEmpty)
{
  Vector<int> vec;
  EXPECT_TRUE(vec.is_empty());
  vec.append(1);
  EXPECT_FALSE(vec.is_empty());
  vec.remove_last();
  EXPECT_TRUE(vec.is_empty());
}

TEST(vector, RemoveReorder)
{
  Vector<int> vec = {4, 5, 6, 7};
  vec.remove_and_reorder(1);
  EXPECT_EQ(vec[0], 4);
  EXPECT_EQ(vec[1], 7);
  EXPECT_EQ(vec[2], 6);
  vec.remove_and_reorder(2);
  EXPECT_EQ(vec[0], 4);
  EXPECT_EQ(vec[1], 7);
  vec.remove_and_reorder(0);
  EXPECT_EQ(vec[0], 7);
  vec.remove_and_reorder(0);
  EXPECT_TRUE(vec.is_empty());
}

TEST(vector, RemoveFirstOccurrenceAndReorder)
{
  Vector<int> vec = {4, 5, 6, 7};
  vec.remove_first_occurrence_and_reorder(5);
  EXPECT_EQ(vec[0], 4);
  EXPECT_EQ(vec[1], 7);
  EXPECT_EQ(vec[2], 6);
  vec.remove_first_occurrence_and_reorder(6);
  EXPECT_EQ(vec[0], 4);
  EXPECT_EQ(vec[1], 7);
  vec.remove_first_occurrence_and_reorder(4);
  EXPECT_EQ(vec[0], 7);
  vec.remove_first_occurrence_and_reorder(7);
  EXPECT_EQ(vec.size(), 0);
}

TEST(vector, Remove)
{
  Vector<int> vec = {1, 2, 3, 4, 5, 6};
  vec.remove(3);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({1, 2, 3, 5, 6}).begin()));
  vec.remove(0);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({2, 3, 5, 6}).begin()));
  vec.remove(3);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({2, 3, 5}).begin()));
  vec.remove(1);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({2, 5}).begin()));
  vec.remove(1);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({2}).begin()));
  vec.remove(0);
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), Span<int>({}).begin()));
}

TEST(vector, ExtendSmallVector)
{
  Vector<int> a = {2, 3, 4};
  Vector<int> b = {11, 12};
  b.extend(a);
  EXPECT_EQ(b.size(), 5);
  EXPECT_EQ(b[0], 11);
  EXPECT_EQ(b[1], 12);
  EXPECT_EQ(b[2], 2);
  EXPECT_EQ(b[3], 3);
  EXPECT_EQ(b[4], 4);
}

TEST(vector, ExtendArray)
{
  int array[] = {3, 4, 5, 6};

  Vector<int> a;
  a.extend(array, 2);

  EXPECT_EQ(a.size(), 2);
  EXPECT_EQ(a[0], 3);
  EXPECT_EQ(a[1], 4);
}

TEST(vector, Last)
{
  Vector<int> a{3, 5, 7};
  EXPECT_EQ(a.last(), 7);
  EXPECT_EQ(a.last(0), 7);
  EXPECT_EQ(a.last(1), 5);
  EXPECT_EQ(a.last(2), 3);
}

TEST(vector, AppendNTimes)
{
  Vector<int> a;
  a.append_n_times(5, 3);
  a.append_n_times(2, 2);
  EXPECT_EQ(a.size(), 5);
  EXPECT_EQ(a[0], 5);
  EXPECT_EQ(a[1], 5);
  EXPECT_EQ(a[2], 5);
  EXPECT_EQ(a[3], 2);
  EXPECT_EQ(a[4], 2);
}

TEST(vector, UniquePtrValue)
{
  Vector<std::unique_ptr<int>> vec;
  vec.append(std::make_unique<int>());
  vec.append(std::make_unique<int>());
  vec.append(std::make_unique<int>());
  vec.append(std::make_unique<int>());
  EXPECT_EQ(vec.size(), 4);

  std::unique_ptr<int> &a = vec.last();
  std::unique_ptr<int> b = vec.pop_last();
  vec.remove_and_reorder(0);
  vec.remove(0);
  EXPECT_EQ(vec.size(), 1);
  EXPECT_EQ(vec.append_and_get_index(std::make_unique<int>(4)), 1);

  UNUSED_VARS(a, b);
}

class TypeConstructMock {
 public:
  bool default_constructed = false;
  bool copy_constructed = false;
  bool move_constructed = false;
  bool copy_assigned = false;
  bool move_assigned = false;

  TypeConstructMock() : default_constructed(true)
  {
  }

  TypeConstructMock(const TypeConstructMock &UNUSED(other)) : copy_constructed(true)
  {
  }

  TypeConstructMock(TypeConstructMock &&UNUSED(other)) noexcept : move_constructed(true)
  {
  }

  TypeConstructMock &operator=(const TypeConstructMock &other)
  {
    if (this == &other) {
      return *this;
    }

    copy_assigned = true;
    return *this;
  }

  TypeConstructMock &operator=(TypeConstructMock &&other) noexcept
  {
    if (this == &other) {
      return *this;
    }

    move_assigned = true;
    return *this;
  }
};

TEST(vector, SizeConstructorCallsDefaultConstructor)
{
  Vector<TypeConstructMock> vec(3);
  EXPECT_TRUE(vec[0].default_constructed);
  EXPECT_TRUE(vec[1].default_constructed);
  EXPECT_TRUE(vec[2].default_constructed);
}

TEST(vector, SizeValueConstructorCallsCopyConstructor)
{
  Vector<TypeConstructMock> vec(3, TypeConstructMock());
  EXPECT_TRUE(vec[0].copy_constructed);
  EXPECT_TRUE(vec[1].copy_constructed);
  EXPECT_TRUE(vec[2].copy_constructed);
}

TEST(vector, AppendCallsCopyConstructor)
{
  Vector<TypeConstructMock> vec;
  TypeConstructMock value;
  vec.append(value);
  EXPECT_TRUE(vec[0].copy_constructed);
}

TEST(vector, AppendCallsMoveConstructor)
{
  Vector<TypeConstructMock> vec;
  vec.append(TypeConstructMock());
  EXPECT_TRUE(vec[0].move_constructed);
}

TEST(vector, SmallVectorCopyCallsCopyConstructor)
{
  Vector<TypeConstructMock, 2> src(2);
  Vector<TypeConstructMock, 2> dst(src);
  EXPECT_TRUE(dst[0].copy_constructed);
  EXPECT_TRUE(dst[1].copy_constructed);
}

TEST(vector, LargeVectorCopyCallsCopyConstructor)
{
  Vector<TypeConstructMock, 2> src(5);
  Vector<TypeConstructMock, 2> dst(src);
  EXPECT_TRUE(dst[0].copy_constructed);
  EXPECT_TRUE(dst[1].copy_constructed);
}

TEST(vector, SmallVectorMoveCallsMoveConstructor)
{
  Vector<TypeConstructMock, 2> src(2);
  Vector<TypeConstructMock, 2> dst(std::move(src));
  EXPECT_TRUE(dst[0].move_constructed);
  EXPECT_TRUE(dst[1].move_constructed);
}

TEST(vector, LargeVectorMoveCallsNoConstructor)
{
  Vector<TypeConstructMock, 2> src(5);
  Vector<TypeConstructMock, 2> dst(std::move(src));

  EXPECT_TRUE(dst[0].default_constructed);
  EXPECT_FALSE(dst[0].move_constructed);
  EXPECT_FALSE(dst[0].copy_constructed);
}

TEST(vector, Resize)
{
  std::string long_string = "012345678901234567890123456789";
  Vector<std::string> vec;
  EXPECT_EQ(vec.size(), 0);
  vec.resize(2);
  EXPECT_EQ(vec.size(), 2);
  EXPECT_EQ(vec[0], "");
  EXPECT_EQ(vec[1], "");
  vec.resize(5, long_string);
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ(vec[0], "");
  EXPECT_EQ(vec[1], "");
  EXPECT_EQ(vec[2], long_string);
  EXPECT_EQ(vec[3], long_string);
  EXPECT_EQ(vec[4], long_string);
  vec.resize(1);
  EXPECT_EQ(vec.size(), 1);
  EXPECT_EQ(vec[0], "");
}

TEST(vector, FirstIndexOf)
{
  Vector<int> vec = {2, 3, 5, 7, 5, 9};
  EXPECT_EQ(vec.first_index_of(2), 0);
  EXPECT_EQ(vec.first_index_of(5), 2);
  EXPECT_EQ(vec.first_index_of(9), 5);
}

TEST(vector, FirstIndexTryOf)
{
  Vector<int> vec = {2, 3, 5, 7, 5, 9};
  EXPECT_EQ(vec.first_index_of_try(2), 0);
  EXPECT_EQ(vec.first_index_of_try(4), -1);
  EXPECT_EQ(vec.first_index_of_try(5), 2);
  EXPECT_EQ(vec.first_index_of_try(9), 5);
  EXPECT_EQ(vec.first_index_of_try(1), -1);
}

TEST(vector, OveralignedValues)
{
  Vector<AlignedBuffer<1, 512>, 2> vec;
  for (int i = 0; i < 100; i++) {
    vec.append({});
    EXPECT_EQ((uintptr_t)&vec.last() % 512, 0);
  }
}

TEST(vector, ConstructVoidPointerVector)
{
  int a;
  float b;
  double c;
  Vector<void *> vec = {&a, &b, &c};
  EXPECT_EQ(vec.size(), 3);
}

TEST(vector, Fill)
{
  Vector<int> vec(5);
  vec.fill(3);
  EXPECT_EQ(vec.size(), 5u);
  EXPECT_EQ(vec[0], 3);
  EXPECT_EQ(vec[1], 3);
  EXPECT_EQ(vec[2], 3);
  EXPECT_EQ(vec[3], 3);
  EXPECT_EQ(vec[4], 3);
}

TEST(vector, InsertAtBeginning)
{
  Vector<int> vec = {1, 2, 3};
  vec.insert(0, {6, 7});
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span({6, 7, 1, 2, 3}).data(), 5);
}

TEST(vector, InsertAtEnd)
{
  Vector<int> vec = {1, 2, 3};
  vec.insert(3, {6, 7});
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span({1, 2, 3, 6, 7}).data(), 5);
}

TEST(vector, InsertInMiddle)
{
  Vector<int> vec = {1, 2, 3};
  vec.insert(1, {6, 7});
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span({1, 6, 7, 2, 3}).data(), 5);
}

TEST(vector, InsertAtIterator)
{
  Vector<std::string> vec = {"1", "2", "3"};
  Vector<std::string> other_vec = {"hello", "world"};
  vec.insert(vec.begin() + 1, other_vec.begin(), other_vec.end());
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span<std::string>({"1", "hello", "world", "2", "3"}).data(), 5);
}

TEST(vector, InsertMoveOnlyType)
{
  Vector<std::unique_ptr<int>> vec;
  vec.append(std::make_unique<int>(1));
  vec.append(std::make_unique<int>(2));
  vec.insert(1, std::make_unique<int>(30));
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(*vec[0], 1);
  EXPECT_EQ(*vec[1], 30);
  EXPECT_EQ(*vec[2], 2);
}

TEST(vector, Prepend)
{
  Vector<int> vec = {1, 2, 3};
  vec.prepend({7, 8});
  EXPECT_EQ(vec.size(), 5);
  EXPECT_EQ_ARRAY(vec.data(), Span({7, 8, 1, 2, 3}).data(), 5);
}

TEST(vector, PrependString)
{
  std::string s = "test";
  Vector<std::string> vec;
  vec.prepend(s);
  vec.prepend(std::move(s));
  EXPECT_EQ(vec.size(), 2);
  EXPECT_EQ(vec[0], "test");
  EXPECT_EQ(vec[1], "test");
}

TEST(vector, ReverseIterator)
{
  Vector<int> vec = {4, 5, 6, 7};
  Vector<int> reversed_vec;
  for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
    reversed_vec.append(*it);
  }
  EXPECT_EQ(reversed_vec.size(), 4);
  EXPECT_EQ_ARRAY(reversed_vec.data(), Span({7, 6, 5, 4}).data(), 4);
}

TEST(vector, SizeValueConstructorExceptions)
{
  ExceptionThrower value;
  value.throw_during_copy = true;
  EXPECT_ANY_THROW({ Vector<ExceptionThrower> vec(5, value); });
}

TEST(vector, SpanConstructorExceptions)
{
  std::array<ExceptionThrower, 5> values;
  values[3].throw_during_copy = true;
  EXPECT_ANY_THROW({ Vector<ExceptionThrower> vec(values); });
}

TEST(vector, MoveConstructorExceptions)
{
  Vector<ExceptionThrower, 4> vec(3);
  vec[2].throw_during_move = true;
  EXPECT_ANY_THROW({ Vector<ExceptionThrower> moved_vector{std::move(vec)}; });
}

TEST(vector, AppendExceptions)
{
  Vector<ExceptionThrower, 4> vec(2);
  ExceptionThrower *ptr1 = &vec.last();
  ExceptionThrower value;
  value.throw_during_copy = true;
  EXPECT_ANY_THROW({ vec.append(value); });
  EXPECT_EQ(vec.size(), 2);
  ExceptionThrower *ptr2 = &vec.last();
  EXPECT_EQ(ptr1, ptr2);
}

TEST(vector, ExtendExceptions)
{
  Vector<ExceptionThrower> vec(5);
  std::array<ExceptionThrower, 10> values;
  values[6].throw_during_copy = true;
  EXPECT_ANY_THROW({ vec.extend(values); });
  EXPECT_EQ(vec.size(), 5);
}

TEST(vector, Insert1Exceptions)
{
  Vector<ExceptionThrower> vec(10);
  std::array<ExceptionThrower, 5> values;
  values[3].throw_during_copy = true;
  EXPECT_ANY_THROW({ vec.insert(7, values); });
}

TEST(vector, Insert2Exceptions)
{
  Vector<ExceptionThrower> vec(10);
  vec.reserve(100);
  vec[8].throw_during_move = true;
  std::array<ExceptionThrower, 5> values;
  EXPECT_ANY_THROW({ vec.insert(3, values); });
}

TEST(vector, PopLastExceptions)
{
  Vector<ExceptionThrower> vec(10);
  vec.last().throw_during_move = true;
  EXPECT_ANY_THROW({ vec.pop_last(); }); /* NOLINT: bugprone-throw-keyword-missing */
  EXPECT_EQ(vec.size(), 10);
}

TEST(vector, RemoveAndReorderExceptions)
{
  Vector<ExceptionThrower> vec(10);
  vec.last().throw_during_move = true;
  EXPECT_ANY_THROW({ vec.remove_and_reorder(3); });
  EXPECT_EQ(vec.size(), 10);
}

TEST(vector, RemoveExceptions)
{
  Vector<ExceptionThrower> vec(10);
  vec[8].throw_during_move = true;
  EXPECT_ANY_THROW({ vec.remove(2); });
  EXPECT_EQ(vec.size(), 10);
}

TEST(vector, RemoveChunk)
{
  Vector<int> vec = {2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(vec.size(), 7);
  vec.remove(2, 4);
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 2);
  EXPECT_EQ(vec[1], 3);
  EXPECT_EQ(vec[2], 8);
  vec.remove(0, 1);
  EXPECT_EQ(vec.size(), 2);
  EXPECT_EQ(vec[0], 3);
  EXPECT_EQ(vec[1], 8);
  vec.remove(1, 1);
  EXPECT_EQ(vec.size(), 1);
  EXPECT_EQ(vec[0], 3);
  vec.remove(0, 1);
  EXPECT_EQ(vec.size(), 0);
  vec.remove(0, 0);
  EXPECT_EQ(vec.size(), 0);
}

TEST(vector, RemoveChunkExceptions)
{
  Vector<ExceptionThrower> vec(10);
  vec.remove(1, 3);
  EXPECT_EQ(vec.size(), 7);
  vec[5].throw_during_move = true;
  EXPECT_ANY_THROW({ vec.remove(2, 3); });
  EXPECT_EQ(vec.size(), 7);
}

}  // namespace blender::tests
