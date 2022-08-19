/* SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include <array>
#include <initializer_list>
#include <ostream> /* NOLINT */
#include <string>
#include <utility>
#include <vector>

#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

using std::initializer_list;
using std::pair;
using std::string;
using std::vector;

/* -------------------------------------------------------------------- */
/* tests */

/* BLI_str_partition */
TEST(string, StrPartition)
{
  const char delim[] = {'-', '.', '_', '~', '\\', '\0'};
  const char *sep, *suf;
  size_t pre_len;

  {
    const char *str = "mat.e-r_ial";

    /* "mat.e-r_ial" -> "mat", '.', "e-r_ial", 3 */
    pre_len = BLI_str_partition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 3);
    EXPECT_EQ(&str[3], sep);
    EXPECT_STREQ("e-r_ial", suf);
  }

  /* Corner cases. */
  {
    const char *str = ".mate-rial--";

    /* ".mate-rial--" -> "", '.', "mate-rial--", 0 */
    pre_len = BLI_str_partition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(&str[0], sep);
    EXPECT_STREQ("mate-rial--", suf);
  }

  {
    const char *str = ".__.--_";

    /* ".__.--_" -> "", '.', "__.--_", 0 */
    pre_len = BLI_str_partition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(&str[0], sep);
    EXPECT_STREQ("__.--_", suf);
  }

  {
    const char *str = "";

    /* "" -> "", NULL, NULL, 0 */
    pre_len = BLI_str_partition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }

  {
    const char *str = "material";

    /* "material" -> "material", NULL, NULL, 8 */
    pre_len = BLI_str_partition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 8);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_rpartition */
TEST(string, StrRPartition)
{
  const char delim[] = {'-', '.', '_', '~', '\\', '\0'};
  const char *sep, *suf;
  size_t pre_len;

  {
    const char *str = "mat.e-r_ial";

    /* "mat.e-r_ial" -> "mat.e-r", '_', "ial", 7 */
    pre_len = BLI_str_rpartition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 7);
    EXPECT_EQ(&str[7], sep);
    EXPECT_STREQ("ial", suf);
  }

  /* Corner cases. */
  {
    const char *str = ".mate-rial--";

    /* ".mate-rial--" -> ".mate-rial-", '-', "", 11 */
    pre_len = BLI_str_rpartition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 11);
    EXPECT_EQ(&str[11], sep);
    EXPECT_STREQ("", suf);
  }

  {
    const char *str = ".__.--_";

    /* ".__.--_" -> ".__.--", '_', "", 6 */
    pre_len = BLI_str_rpartition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 6);
    EXPECT_EQ(&str[6], sep);
    EXPECT_STREQ("", suf);
  }

  {
    const char *str = "";

    /* "" -> "", NULL, NULL, 0 */
    pre_len = BLI_str_rpartition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }

  {
    const char *str = "material";

    /* "material" -> "material", NULL, NULL, 8 */
    pre_len = BLI_str_rpartition(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 8);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_partition_ex */
TEST(string, StrPartitionEx)
{
  const char delim[] = {'-', '.', '_', '~', '\\', '\0'};
  const char *sep, *suf;
  size_t pre_len;

  /* Only considering 'from_right' cases here. */

  {
    const char *str = "mat.e-r_ia.l";

    /* "mat.e-r_ia.l" over "mat.e-r" -> "mat.e", '.', "r_ia.l", 3 */
    pre_len = BLI_str_partition_ex(str, str + 6, delim, &sep, &suf, true);
    EXPECT_EQ(pre_len, 5);
    EXPECT_EQ(&str[5], sep);
    EXPECT_STREQ("r_ia.l", suf);
  }

  /* Corner cases. */
  {
    const char *str = "mate.rial";

    /* "mate.rial" over "mate" -> "mate.rial", NULL, NULL, 4 */
    pre_len = BLI_str_partition_ex(str, str + 4, delim, &sep, &suf, true);
    EXPECT_EQ(pre_len, 4);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_partition_utf8 */
TEST(string, StrPartitionUtf8)
{
  const unsigned int delim[] = {'-', '.', '_', 0x00F1 /* n tilde */, 0x262F /* ying-yang */, '\0'};
  const char *sep, *suf;
  size_t pre_len;

  {
    const char *str = "ma\xc3\xb1te-r\xe2\x98\xafial";

    /* "ma\xc3\xb1te-r\xe2\x98\xafial" -> "ma", '\xc3\xb1', "te-r\xe2\x98\xafial", 2 */
    pre_len = BLI_str_partition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 2);
    EXPECT_EQ(&str[2], sep);
    EXPECT_STREQ("te-r\xe2\x98\xafial", suf);
  }

  /* Corner cases. */
  {
    const char *str = "\xe2\x98\xafmate-rial-\xc3\xb1";

    /* "\xe2\x98\xafmate-rial-\xc3\xb1" -> "", '\xe2\x98\xaf', "mate-rial-\xc3\xb1", 0 */
    pre_len = BLI_str_partition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(&str[0], sep);
    EXPECT_STREQ("mate-rial-\xc3\xb1", suf);
  }

  {
    const char *str = "\xe2\x98\xaf.\xc3\xb1_.--\xc3\xb1";

    /* "\xe2\x98\xaf.\xc3\xb1_.--\xc3\xb1" -> "", '\xe2\x98\xaf', ".\xc3\xb1_.--\xc3\xb1", 0 */
    pre_len = BLI_str_partition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(&str[0], sep);
    EXPECT_STREQ(".\xc3\xb1_.--\xc3\xb1", suf);
  }

  {
    const char *str = "";

    /* "" -> "", NULL, NULL, 0 */
    pre_len = BLI_str_partition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }

  {
    const char *str = "material";

    /* "material" -> "material", NULL, NULL, 8 */
    pre_len = BLI_str_partition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 8);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_rpartition_utf8 */
TEST(string, StrRPartitionUtf8)
{
  const unsigned int delim[] = {'-', '.', '_', 0x00F1 /* n tilde */, 0x262F /* ying-yang */, '\0'};
  const char *sep, *suf;
  size_t pre_len;

  {
    const char *str = "ma\xc3\xb1te-r\xe2\x98\xafial";

    /* "ma\xc3\xb1te-r\xe2\x98\xafial" -> "mat\xc3\xb1te-r", '\xe2\x98\xaf', "ial", 8 */
    pre_len = BLI_str_rpartition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 8);
    EXPECT_EQ(&str[8], sep);
    EXPECT_STREQ("ial", suf);
  }

  /* Corner cases. */
  {
    const char *str = "\xe2\x98\xafmate-rial-\xc3\xb1";

    /* "\xe2\x98\xafmate-rial-\xc3\xb1" -> "\xe2\x98\xafmate-rial-", '\xc3\xb1', "", 13 */
    pre_len = BLI_str_rpartition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 13);
    EXPECT_EQ(&str[13], sep);
    EXPECT_STREQ("", suf);
  }

  {
    const char *str = "\xe2\x98\xaf.\xc3\xb1_.--\xc3\xb1";

    /* "\xe2\x98\xaf.\xc3\xb1_.--\xc3\xb1" -> "\xe2\x98\xaf.\xc3\xb1_.--", '\xc3\xb1', "", 10 */
    pre_len = BLI_str_rpartition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 10);
    EXPECT_EQ(&str[10], sep);
    EXPECT_STREQ("", suf);
  }

  {
    const char *str = "";

    /* "" -> "", NULL, NULL, 0 */
    pre_len = BLI_str_rpartition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 0);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }

  {
    const char *str = "material";

    /* "material" -> "material", NULL, NULL, 8 */
    pre_len = BLI_str_rpartition_utf8(str, delim, &sep, &suf);
    EXPECT_EQ(pre_len, 8);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_partition_ex_utf8 */
TEST(string, StrPartitionExUtf8)
{
  const unsigned int delim[] = {'-', '.', '_', 0x00F1 /* n tilde */, 0x262F /* ying-yang */, '\0'};
  const char *sep, *suf;
  size_t pre_len;

  /* Only considering 'from_right' cases here. */

  {
    const char *str = "ma\xc3\xb1te-r\xe2\x98\xafial";

    /* "ma\xc3\xb1te-r\xe2\x98\xafial" over
     * "ma\xc3\xb1te" -> "ma", '\xc3\xb1', "te-r\xe2\x98\xafial", 2 */
    pre_len = BLI_str_partition_ex_utf8(str, str + 6, delim, &sep, &suf, true);
    EXPECT_EQ(pre_len, 2);
    EXPECT_EQ(&str[2], sep);
    EXPECT_STREQ("te-r\xe2\x98\xafial", suf);
  }

  /* Corner cases. */
  {
    const char *str = "mate\xe2\x98\xafrial";

    /* "mate\xe2\x98\xafrial" over "mate" -> "mate\xe2\x98\xafrial", NULL, NULL, 4 */
    pre_len = BLI_str_partition_ex_utf8(str, str + 4, delim, &sep, &suf, true);
    EXPECT_EQ(pre_len, 4);
    EXPECT_EQ(sep, (void *)nullptr);
    EXPECT_EQ(suf, (void *)nullptr);
  }
}

/* BLI_str_format_int_grouped */
TEST(string, StrFormatIntGrouped)
{
  char number_str[16];
  int number;

  BLI_str_format_int_grouped(number_str, number = 0);
  EXPECT_STREQ("0", number_str);

  BLI_str_format_int_grouped(number_str, number = 1);
  EXPECT_STREQ("1", number_str);

  BLI_str_format_int_grouped(number_str, number = -1);
  EXPECT_STREQ("-1", number_str);

  BLI_str_format_int_grouped(number_str, number = -2147483648);
  EXPECT_STREQ("-2,147,483,648", number_str);

  BLI_str_format_int_grouped(number_str, number = 2147483647);
  EXPECT_STREQ("2,147,483,647", number_str);

  BLI_str_format_int_grouped(number_str, number = 1000);
  EXPECT_STREQ("1,000", number_str);

  BLI_str_format_int_grouped(number_str, number = -1000);
  EXPECT_STREQ("-1,000", number_str);

  BLI_str_format_int_grouped(number_str, number = 999);
  EXPECT_STREQ("999", number_str);

  BLI_str_format_int_grouped(number_str, number = -999);
  EXPECT_STREQ("-999", number_str);
}

/* BLI_str_format_byte_unit */
TEST(string, StrFormatByteUnits)
{
  char size_str[15];
  long long int size;

  /* Base 10 */
  BLI_str_format_byte_unit(size_str, size = 0, true);
  EXPECT_STREQ("0 B", size_str);
  BLI_str_format_byte_unit(size_str, size = -0, true);
  EXPECT_STREQ("0 B", size_str);

  BLI_str_format_byte_unit(size_str, size = 1, true);
  EXPECT_STREQ("1 B", size_str);
  BLI_str_format_byte_unit(size_str, size = -1, true);
  EXPECT_STREQ("-1 B", size_str);

  BLI_str_format_byte_unit(size_str, size = 1000, true);
  EXPECT_STREQ("1 KB", size_str);
  BLI_str_format_byte_unit(size_str, size = -1000, true);
  EXPECT_STREQ("-1 KB", size_str);

  BLI_str_format_byte_unit(size_str, size = 1024, true);
  EXPECT_STREQ("1 KB", size_str);
  BLI_str_format_byte_unit(size_str, size = -1024, true);
  EXPECT_STREQ("-1 KB", size_str);

  /* LLONG_MAX - largest possible value */
  BLI_str_format_byte_unit(size_str, size = 9223372036854775807, true);
  EXPECT_STREQ("9223.372 PB", size_str);
  BLI_str_format_byte_unit(size_str, size = -9223372036854775807, true);
  EXPECT_STREQ("-9223.372 PB", size_str);

  /* Base 2 */
  BLI_str_format_byte_unit(size_str, size = 0, false);
  EXPECT_STREQ("0 B", size_str);
  BLI_str_format_byte_unit(size_str, size = -0, false);
  EXPECT_STREQ("0 B", size_str);

  BLI_str_format_byte_unit(size_str, size = 1, false);
  EXPECT_STREQ("1 B", size_str);
  BLI_str_format_byte_unit(size_str, size = -1, false);
  EXPECT_STREQ("-1 B", size_str);

  BLI_str_format_byte_unit(size_str, size = 1000, false);
  EXPECT_STREQ("1000 B", size_str);
  BLI_str_format_byte_unit(size_str, size = -1000, false);
  EXPECT_STREQ("-1000 B", size_str);

  BLI_str_format_byte_unit(size_str, size = 1024, false);
  EXPECT_STREQ("1 KiB", size_str);
  BLI_str_format_byte_unit(size_str, size = -1024, false);
  EXPECT_STREQ("-1 KiB", size_str);

  /* LLONG_MAX - largest possible value */
  BLI_str_format_byte_unit(size_str, size = 9223372036854775807, false);
  EXPECT_STREQ("8192.0 PiB", size_str);
  BLI_str_format_byte_unit(size_str, size = -9223372036854775807, false);
  EXPECT_STREQ("-8192.0 PiB", size_str);

  /* Test maximum string length. */
  BLI_str_format_byte_unit(size_str, size = -9223200000000000000, false);
  EXPECT_STREQ("-8191.8472 PiB", size_str);
}

/* BLI_str_format_decimal_unit */
TEST(string, StrFormatDecimalUnits)
{
  char size_str[7];
  int size;

  BLI_str_format_decimal_unit(size_str, size = 0);
  EXPECT_STREQ("0", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1);
  EXPECT_STREQ("1", size_str);
  BLI_str_format_decimal_unit(size_str, size = 10);
  EXPECT_STREQ("10", size_str);
  BLI_str_format_decimal_unit(size_str, size = 15);
  EXPECT_STREQ("15", size_str);
  BLI_str_format_decimal_unit(size_str, size = 100);
  EXPECT_STREQ("100", size_str);
  BLI_str_format_decimal_unit(size_str, size = 155);
  EXPECT_STREQ("155", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1000);
  EXPECT_STREQ("1.0K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1555);
  EXPECT_STREQ("1.6K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 10000);
  EXPECT_STREQ("10.0K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 15555);
  EXPECT_STREQ("15.6K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 100000);
  EXPECT_STREQ("100K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 100000);
  EXPECT_STREQ("100K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 155555);
  EXPECT_STREQ("156K", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1000000);
  EXPECT_STREQ("1.0M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1555555);
  EXPECT_STREQ("1.6M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 10000000);
  EXPECT_STREQ("10.0M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 15555555);
  EXPECT_STREQ("15.6M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 100000000);
  EXPECT_STREQ("100M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 155555555);
  EXPECT_STREQ("156M", size_str);
  BLI_str_format_decimal_unit(size_str, size = 1000000000);
  EXPECT_STREQ("1.0B", size_str);

  /* Largest possible value. */
  BLI_str_format_decimal_unit(size_str, size = INT32_MAX);
  EXPECT_STREQ("2.1B", size_str);

  BLI_str_format_decimal_unit(size_str, size = -0);
  EXPECT_STREQ("0", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1);
  EXPECT_STREQ("-1", size_str);
  BLI_str_format_decimal_unit(size_str, size = -10);
  EXPECT_STREQ("-10", size_str);
  BLI_str_format_decimal_unit(size_str, size = -15);
  EXPECT_STREQ("-15", size_str);
  BLI_str_format_decimal_unit(size_str, size = -100);
  EXPECT_STREQ("-100", size_str);
  BLI_str_format_decimal_unit(size_str, size = -155);
  EXPECT_STREQ("-155", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1000);
  EXPECT_STREQ("-1.0K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1555);
  EXPECT_STREQ("-1.6K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -10000);
  EXPECT_STREQ("-10.0K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -15555);
  EXPECT_STREQ("-15.6K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -100000);
  EXPECT_STREQ("-100K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -155555);
  EXPECT_STREQ("-156K", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1000000);
  EXPECT_STREQ("-1.0M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1555555);
  EXPECT_STREQ("-1.6M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -10000000);
  EXPECT_STREQ("-10.0M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -15555555);
  EXPECT_STREQ("-15.6M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -100000000);
  EXPECT_STREQ("-100M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -155555555);
  EXPECT_STREQ("-156M", size_str);
  BLI_str_format_decimal_unit(size_str, size = -1000000000);
  EXPECT_STREQ("-1.0B", size_str);

  /* Smallest possible value. */
  BLI_str_format_decimal_unit(size_str, size = -INT32_MAX);
  EXPECT_STREQ("-2.1B", size_str);
}

struct WordInfo {
  WordInfo() = default;
  WordInfo(int start, int end) : start(start), end(end)
  {
  }
  bool operator==(const WordInfo &other) const
  {
    return start == other.start && end == other.end;
  }
  int start, end;
};
static std::ostream &operator<<(std::ostream &os, const WordInfo &word_info)
{
  os << "start: " << word_info.start << ", end: " << word_info.end;
  return os;
}

class StringFindSplitWords : public testing::Test {
 protected:
  StringFindSplitWords() = default;

  /* If max_words is -1 it will be initialized from the number of expected
   * words +1. This way there is no need to pass an explicit number of words,
   * but is also making it possible to catch situation when too many words
   * are being returned. */
  void testStringFindSplitWords(const string &str,
                                const size_t max_length,
                                initializer_list<WordInfo> expected_words_info_init,
                                int max_words = -1)
  {
    const vector<WordInfo> expected_words_info = expected_words_info_init;
    if (max_words != -1) {
      CHECK_LE(max_words, expected_words_info.size() - 1);
    }
    /* Since number of word info is used here, this makes it so we allow one
     * extra word to be collected from the input. This allows to catch possible
     * issues with word splitting not doing a correct thing. */
    const int effective_max_words = (max_words == -1) ? expected_words_info.size() : max_words;
    /* One extra element for the {-1, -1}. */
    vector<WordInfo> actual_word_info(effective_max_words + 1, WordInfo(-1, -1));
    const int actual_word_num = BLI_string_find_split_words(
        str.c_str(),
        max_length,
        ' ',
        reinterpret_cast<int(*)[2]>(actual_word_info.data()),
        effective_max_words);
    /* Schrink actual array to an actual number of words, so we can compare
     * vectors as-is. */
    EXPECT_LE(actual_word_num, actual_word_info.size() - 1);
    actual_word_info.resize(actual_word_num + 1);
    /* Perform actual comparison. */
    EXPECT_EQ_VECTOR(actual_word_info, expected_words_info);
  }

  void testStringFindSplitWords(const string &str,
                                initializer_list<WordInfo> expected_words_info_init)
  {
    testStringFindSplitWords(str, str.length(), expected_words_info_init);
  }
};

/* BLI_string_find_split_words */
TEST_F(StringFindSplitWords, Simple)
{
  testStringFindSplitWords("t", {{0, 1}, {-1, -1}});
  testStringFindSplitWords("test", {{0, 4}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Triple)
{
  testStringFindSplitWords("f t w", {{0, 1}, {2, 1}, {4, 1}, {-1, -1}});
  testStringFindSplitWords("find three words", {{0, 4}, {5, 5}, {11, 5}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Spacing)
{
  testStringFindSplitWords("# ## ### ####", {{0, 1}, {2, 2}, {5, 3}, {9, 4}, {-1, -1}});
  testStringFindSplitWords("#  #   #    #", {{0, 1}, {3, 1}, {7, 1}, {12, 1}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Trailing_Left)
{
  testStringFindSplitWords("   t", {{3, 1}, {-1, -1}});
  testStringFindSplitWords("   test", {{3, 4}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Trailing_Right)
{
  testStringFindSplitWords("t   ", {{0, 1}, {-1, -1}});
  testStringFindSplitWords("test   ", {{0, 4}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Trailing_LeftRight)
{
  testStringFindSplitWords("   surrounding space test   123   ",
                           {{3, 11}, {15, 5}, {21, 4}, {28, 3}, {-1, -1}});
}
TEST_F(StringFindSplitWords, Blank)
{
  testStringFindSplitWords("", {{-1, -1}});
}
TEST_F(StringFindSplitWords, Whitespace)
{
  testStringFindSplitWords(" ", {{-1, -1}});
  testStringFindSplitWords("    ", {{-1, -1}});
}
TEST_F(StringFindSplitWords, LimitWords)
{
  const string words = "too many chars";
  const int words_len = words.length();
  testStringFindSplitWords(words, words_len, {{0, 3}, {4, 4}, {9, 5}, {-1, -1}}, 3);
  testStringFindSplitWords(words, words_len, {{0, 3}, {4, 4}, {-1, -1}}, 2);
  testStringFindSplitWords(words, words_len, {{0, 3}, {-1, -1}}, 1);
  testStringFindSplitWords(words, words_len, {{-1, -1}}, 0);
}
TEST_F(StringFindSplitWords, LimitChars)
{
  const string words = "too many chars";
  const int words_len = words.length();
  testStringFindSplitWords(words, words_len, {{0, 3}, {4, 4}, {9, 5}, {-1, -1}});
  testStringFindSplitWords(words, words_len - 1, {{0, 3}, {4, 4}, {9, 4}, {-1, -1}});
  testStringFindSplitWords(words, words_len - 5, {{0, 3}, {4, 4}, {-1, -1}});
  testStringFindSplitWords(words, 1, {{0, 1}, {-1, -1}});
  testStringFindSplitWords(words, 0, {{-1, -1}});
}

/* BLI_strncasestr */
TEST(string, StringStrncasestr)
{
  const char *str_test0 = "search here";
  const char *res;

  res = BLI_strncasestr(str_test0, "", 0);
  EXPECT_EQ(res, str_test0);

  res = BLI_strncasestr(str_test0, " ", 1);
  EXPECT_EQ(res, str_test0 + 6);

  res = BLI_strncasestr(str_test0, "her", 3);
  EXPECT_EQ(res, str_test0 + 7);

  res = BLI_strncasestr(str_test0, "ARCh", 4);
  EXPECT_EQ(res, str_test0 + 2);

  res = BLI_strncasestr(str_test0, "earcq", 4);
  EXPECT_EQ(res, str_test0 + 1);

  res = BLI_strncasestr(str_test0, "not there", 9);
  EXPECT_EQ(res, (void *)nullptr);
}

/* BLI_string_max_possible_word_count */
TEST(string, StringMaxPossibleWordCount)
{
  EXPECT_EQ(BLI_string_max_possible_word_count(0), 1);
  EXPECT_EQ(BLI_string_max_possible_word_count(1), 1);
  EXPECT_EQ(BLI_string_max_possible_word_count(2), 2);
  EXPECT_EQ(BLI_string_max_possible_word_count(3), 2);
  EXPECT_EQ(BLI_string_max_possible_word_count(10), 6);
}

/* BLI_string_is_decimal */
TEST(string, StrIsDecimal)
{
  EXPECT_FALSE(BLI_string_is_decimal(""));
  EXPECT_FALSE(BLI_string_is_decimal("je moeder"));
  EXPECT_FALSE(BLI_string_is_decimal("je møder"));
  EXPECT_FALSE(BLI_string_is_decimal("Agent 327"));
  EXPECT_FALSE(BLI_string_is_decimal("Agent\000327"));
  EXPECT_FALSE(BLI_string_is_decimal("\000327"));
  EXPECT_FALSE(BLI_string_is_decimal("0x16"));
  EXPECT_FALSE(BLI_string_is_decimal("16.4"));
  EXPECT_FALSE(BLI_string_is_decimal("-1"));

  EXPECT_TRUE(BLI_string_is_decimal("0"));
  EXPECT_TRUE(BLI_string_is_decimal("1"));
  EXPECT_TRUE(BLI_string_is_decimal("001"));
  EXPECT_TRUE(BLI_string_is_decimal("11342908713948713498745980171334059871345098713405981734"));
}

/* BLI_strcasecmp_natural */
class StringCasecmpNatural : public testing::Test {
 protected:
  StringCasecmpNatural() = default;

  using CompareWordsArray = vector<std::array<const char *, 2>>;

  void testReturnsZeroForAll(const CompareWordsArray &items)
  {
    for (const auto &item : items) {
      int res = BLI_strcasecmp_natural(item[0], item[1]);
      EXPECT_EQ(res, 0);
    }
  }
  void testReturnsLessThanZeroForAll(const CompareWordsArray &items)
  {
    for (const auto &item : items) {
      int res = BLI_strcasecmp_natural(item[0], item[1]);
      EXPECT_LT(res, 0);
    }
  }
  void testReturnsMoreThanZeroForAll(const CompareWordsArray &items)
  {
    for (const auto &item : items) {
      int res = BLI_strcasecmp_natural(item[0], item[1]);
      EXPECT_GT(res, 0);
    }
  }

  CompareWordsArray copyWithSwappedWords(const CompareWordsArray &items)
  {
    CompareWordsArray ret_array;

    /* E.g. {{"a", "b"}, {"ab", "cd"}} becomes {{"b", "a"}, {"cd", "ab"}} */

    ret_array.reserve(items.size());
    for (const auto &item : items) {
      ret_array.push_back({item[1], item[0]});
    }

    return ret_array;
  }
};

TEST_F(StringCasecmpNatural, Empty)
{
  const CompareWordsArray equal{
      {"", ""},
  };
  const CompareWordsArray negative{
      {"", "a"},
      {"", "A"},
  };
  CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, Whitespace)
{
  const CompareWordsArray equal{
      {" ", " "},
      {" a", " a"},
      {" a ", " a "},
  };
  const CompareWordsArray negative{
      {"", " "},
      {"", " a"},
      {"", " a "},
      {" ", " a"},
  };
  CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, TextOnlyLowerCase)
{
  const CompareWordsArray equal{
      {"a", "a"},
      {"aa", "aa"},
      {"ab", "ab"},
      {"ba", "ba"},
      {"je møder", "je møder"},
  };
  const CompareWordsArray negative{
      {"a", "b"},
      {"a", "aa"},
      {"a", "ab"},
      {"aa", "b"},
      {"je møda", "je møder"},
  };
  CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, TextMixedCase)
{
  const CompareWordsArray equal{
      {"A", "A"},
      {"AA", "AA"},
      {"AB", "AB"},
      {"Ab", "Ab"},
      {"aB", "aB"},
  };
  const CompareWordsArray negative{
      {"A", "a"},
      {"A", "B"},
      {"A", "b"},
      {"a", "B"},
      {"AA", "aA"},
      {"AA", "aA"},
      {"Ab", "ab"},
      {"AB", "Ab"},
      /* Different lengths */
      {"A", "ab"},
      {"Aa", "b"},
      {"aA", "b"},
      {"AA", "b"},
      {"A", "Ab"},
      {"A", "aB"},
      {"Aa", "B"},
      {"aA", "B"},
      {"AA", "B"},
  };
  CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, Period)
{
  const CompareWordsArray equal{
      {".", "."},
      {". ", ". "},
      {" .", " ."},
      {" . ", " . "},
  };
  const CompareWordsArray negative{
      {".", ". "},
      {" .", " . "},
      {"foo.bar", "foo 1.bar"},
  };
  CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, OnlyNumbers)
{
  const CompareWordsArray equal{
      {"0", "0"},
      {"0001", "0001"},
      {"42", "42"},
      {"0042", "0042"},
  };
  const CompareWordsArray negative{
      /* If numeric values are equal, number of leading zeros is used as tiebreaker. */
      {"1", "0001"},
      {"01", "001"},
      {"0042", "0043"},
      {"0042", "43"},
  };
  const CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

TEST_F(StringCasecmpNatural, TextAndNumbers)
{
  const CompareWordsArray equal{
      {"00je møder1", "00je møder1"},
      {".0 ", ".0 "},
      {" 1.", " 1."},
      {" .0 ", " .0 "},
  };
  const CompareWordsArray negative{
      {"00je møder0", "00je møder1"},
      {"05je møder0", "06je møder1"},
      {"Cube", "Cube.001"},
      {"Cube.001", "Cube.002"},
      {"CUbe.001", "Cube.002"},
      {"CUbe.002", "Cube.002"},
  };
  const CompareWordsArray positive = copyWithSwappedWords(negative);

  testReturnsZeroForAll(equal);
  testReturnsLessThanZeroForAll(negative);
  testReturnsMoreThanZeroForAll(positive);
}

/* BLI_str_escape, BLI_str_unescape */

class StringEscape : public testing::Test {
 protected:
  StringEscape() = default;

  using CompareWordsArray = vector<std::array<const char *, 2>>;

  void testEscapeWords(const CompareWordsArray &items)
  {
    size_t dst_test_len;
    char dst_test[64];
    for (const auto &item : items) {
      /* Escape the string. */
      dst_test_len = BLI_str_escape(dst_test, item[0], SIZE_MAX);
      EXPECT_STREQ(dst_test, item[1]);
      EXPECT_EQ(dst_test_len, strlen(dst_test));
      /* Escape back. */
      dst_test_len = BLI_str_unescape(dst_test, item[1], strlen(item[1]));
      EXPECT_STREQ(dst_test, item[0]);
      EXPECT_EQ(dst_test_len, strlen(dst_test));
    }
  }
};

TEST_F(StringEscape, Simple)
{
  /* NOTE: clang-tidy `modernize-raw-string-literal` is disabled as it causes errors with MSVC.
   * TODO: investigate resolving with `/Zc:preprocessor` flag. */

  const CompareWordsArray equal{
      {"", ""},
      {"/", "/"},
      {"'", "'"},
      {"?", "?"},
  };

  const CompareWordsArray escaped{
      {"\\", "\\\\"},
      {"A\\", "A\\\\"},
      {"\\A", "\\\\A"},
      {"A\\B", "A\\\\B"},
      {"?", "?"},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\"\\", "\\\"\\\\"},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\\\"", "\\\\\\\""},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\"\\\"", "\\\"\\\\\\\""},

      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\"\"\"", "\\\"\\\"\\\""},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\\\\\\", "\\\\\\\\\\\\"},
  };

  testEscapeWords(equal);
  testEscapeWords(escaped);
}

TEST_F(StringEscape, Control)
{
  const CompareWordsArray escaped{
      {"\n", "\\n"},
      {"\r", "\\r"},
      {"\t", "\\t"},
      {"\a", "\\a"},
      {"\b", "\\b"},
      {"\f", "\\f"},
      {"A\n", "A\\n"},
      {"\nA", "\\nA"},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\n\r\t\a\b\f", "\\n\\r\\t\\a\\b\\f"},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\n_\r_\t_\a_\b_\f", "\\n_\\r_\\t_\\a_\\b_\\f"},
      /* NOLINTNEXTLINE: modernize-raw-string-literal. */
      {"\n\\\r\\\t\\\a\\\b\\\f", "\\n\\\\\\r\\\\\\t\\\\\\a\\\\\\b\\\\\\f"},
  };

  testEscapeWords(escaped);
}
