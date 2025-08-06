/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_string_search.hh"
#include "BLI_vector.hh"

namespace blender::string_search::tests {

/* Right arrow, keep in sync with #UI_MENU_ARROW_SEP in `UI_interface.hh`. */
#define UI_MENU_ARROW_SEP "\xe2\x96\xb8"

TEST(string_search, damerau_levenshtein_distance)
{
  EXPECT_EQ(damerau_levenshtein_distance("test", "test"), 0);
  EXPECT_EQ(damerau_levenshtein_distance("hello", "ell"), 2);
  EXPECT_EQ(damerau_levenshtein_distance("hello", "hel"), 2);
  EXPECT_EQ(damerau_levenshtein_distance("ell", "hello"), 2);
  EXPECT_EQ(damerau_levenshtein_distance("hell", "hello"), 1);
  EXPECT_EQ(damerau_levenshtein_distance("hello", "hallo"), 1);
  EXPECT_EQ(damerau_levenshtein_distance("test", ""), 4);
  EXPECT_EQ(damerau_levenshtein_distance("", "hello"), 5);
  EXPECT_EQ(damerau_levenshtein_distance("Test", "test"), 1);
  EXPECT_EQ(damerau_levenshtein_distance("ab", "ba"), 1);
  EXPECT_EQ(damerau_levenshtein_distance("what", "waht"), 1);
  EXPECT_EQ(damerau_levenshtein_distance("what", "ahwt"), 2);
}

TEST(string_search, get_fuzzy_match_errors)
{
  EXPECT_EQ(get_fuzzy_match_errors("a", "b"), -1);
  EXPECT_EQ(get_fuzzy_match_errors("", "abc"), 0);
  EXPECT_EQ(get_fuzzy_match_errors("hello", "hallo"), 1);
  EXPECT_EQ(get_fuzzy_match_errors("hap", "hello"), -1);
  EXPECT_EQ(get_fuzzy_match_errors("armature", UI_MENU_ARROW_SEP "restore"), -1);
  EXPECT_EQ(get_fuzzy_match_errors("bluir", "blur"), get_fuzzy_match_errors("blur", "bluir"));
}

TEST(string_search, extract_normalized_words)
{
  LinearAllocator<> allocator;
  Vector<StringRef, 64> words;
  Vector<int, 64> word_group_ids;
  extract_normalized_words("hello world" UI_MENU_ARROW_SEP "test   another test" UI_MENU_ARROW_SEP
                           " 3",
                           allocator,
                           words,
                           word_group_ids);
  EXPECT_EQ(words.size(), 6);
  EXPECT_EQ(words[0], "hello");
  EXPECT_EQ(word_group_ids[0], 0);
  EXPECT_EQ(words[1], "world");
  EXPECT_EQ(word_group_ids[1], 0);
  EXPECT_EQ(words[2], "test");
  EXPECT_EQ(word_group_ids[2], 1);
  EXPECT_EQ(words[3], "another");
  EXPECT_EQ(word_group_ids[3], 1);
  EXPECT_EQ(words[4], "test");
  EXPECT_EQ(word_group_ids[4], 1);
  EXPECT_EQ(words[5], "3");
  EXPECT_EQ(word_group_ids[5], 2);
}

}  // namespace blender::string_search::tests
