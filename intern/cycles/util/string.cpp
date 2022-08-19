/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <cctype>

#include "util/foreach.h"
#include "util/string.h"
#include "util/windows.h"

#ifdef _WIN32
#  ifndef vsnprintf
#    define vsnprintf _vsnprintf
#  endif
#endif /* _WIN32 */

CCL_NAMESPACE_BEGIN

string string_printf(const char *format, ...)
{
  vector<char> str(128, 0);

  while (1) {
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(&str[0], str.size(), format, args);
    va_end(args);

    if (result == -1) {
      /* not enough space or formatting error */
      if (str.size() > 65536) {
        assert(0);
        return string("");
      }

      str.resize(str.size() * 2, 0);
      continue;
    }
    else if (result >= (int)str.size()) {
      /* not enough space */
      str.resize(result + 1, 0);
      continue;
    }

    return string(&str[0]);
  }
}

bool string_iequals(const string &a, const string &b)
{
  if (a.size() == b.size()) {
    for (size_t i = 0; i < a.size(); i++)
      if (toupper(a[i]) != toupper(b[i]))
        return false;

    return true;
  }

  return false;
}

void string_split(vector<string> &tokens,
                  const string &str,
                  const string &separators,
                  bool skip_empty_tokens)
{
  size_t token_start = 0, token_length = 0;
  for (size_t i = 0; i < str.size(); ++i) {
    const char ch = str[i];
    if (separators.find(ch) == string::npos) {
      /* Current character is not a separator,
       * append it to token by increasing token length.
       */
      ++token_length;
    }
    else {
      /* Current character is a separator,
       * append current token to the list.
       */
      if (!skip_empty_tokens || token_length > 0) {
        string token = str.substr(token_start, token_length);
        tokens.push_back(token);
      }
      token_start = i + 1;
      token_length = 0;
    }
  }
  /* Append token from the tail of the string if exists. */
  if (token_length) {
    string token = str.substr(token_start, token_length);
    tokens.push_back(token);
  }
}

bool string_startswith(const string_view s, const string_view start)
{
  const size_t len = start.size();

  if (len > s.size()) {
    return false;
  }

  return strncmp(s.c_str(), start.data(), len) == 0;
}

bool string_endswith(const string_view s, const string_view end)
{
  const size_t len = end.size();

  if (len > s.size()) {
    return false;
  }

  return strncmp(s.c_str() + s.size() - len, end.data(), len) == 0;
}

string string_strip(const string &s)
{
  string result = s;
  result.erase(0, result.find_first_not_of(' '));
  result.erase(result.find_last_not_of(' ') + 1);
  return result;
}

void string_replace(string &haystack, const string &needle, const string &other)
{
  size_t i = 0, index;
  while ((index = haystack.find(needle, i)) != string::npos) {
    haystack.replace(index, needle.size(), other);
    i = index + other.size();
  }
}

void string_replace_same_length(string &haystack, const string &needle, const string &other)
{
  assert(needle.size() == other.size());
  size_t pos = 0;
  while (pos != string::npos) {
    pos = haystack.find(needle, pos);
    if (pos != string::npos) {
      memcpy(haystack.data() + pos, other.data(), other.size());
      pos += other.size();
    }
  }
}

string string_remove_trademark(const string &s)
{
  string result = s;

  /* Special case, so we don't leave sequential spaces behind. */
  /* TODO(sergey): Consider using regex perhaps? */
  string_replace(result, " (TM)", "");
  string_replace(result, " (R)", "");

  string_replace(result, "(TM)", "");
  string_replace(result, "(R)", "");

  return string_strip(result);
}

string string_from_bool(bool var)
{
  if (var)
    return "True";
  else
    return "False";
}

string to_string(const char *str)
{
  return string(str);
}

string to_string(const float4 &v)
{
  return string_printf("%f,%f,%f,%f", v.x, v.y, v.z, v.w);
}

string string_to_lower(const string &s)
{
  string r = s;
  std::transform(r.begin(), r.end(), r.begin(), [](char c) { return std::tolower(c); });
  return r;
}

/* Wide char strings helpers for Windows. */

#ifdef _WIN32

wstring string_to_wstring(const string &str)
{
  const int length_wc = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
  wstring str_wc(length_wc, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), &str_wc[0], length_wc);
  return str_wc;
}

string string_from_wstring(const wstring &str)
{
  int length_mb = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.size(), NULL, 0, NULL, NULL);
  string str_mb(length_mb, 0);
  WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.size(), &str_mb[0], length_mb, NULL, NULL);
  return str_mb;
}

string string_to_ansi(const string &str)
{
  const int length_wc = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), NULL, 0);
  wstring str_wc(length_wc, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), &str_wc[0], length_wc);

  int length_mb = WideCharToMultiByte(
      CP_ACP, 0, str_wc.c_str(), str_wc.size(), NULL, 0, NULL, NULL);

  string str_mb(length_mb, 0);
  WideCharToMultiByte(CP_ACP, 0, str_wc.c_str(), str_wc.size(), &str_mb[0], length_mb, NULL, NULL);

  return str_mb;
}

#endif /* _WIN32 */

string string_human_readable_size(size_t size)
{
  static const char suffixes[] = "BKMGTPEZY";

  const char *suffix = suffixes;
  size_t r = 0;

  while (size >= 1024) {
    r = size % 1024;
    size /= 1024;
    suffix++;
  }

  if (*suffix != 'B')
    return string_printf("%.2f%c", double(size * 1024 + r) / 1024.0, *suffix);
  else
    return string_printf("%zu", size);
}

string string_human_readable_number(size_t num)
{
  if (num == 0) {
    return "0";
  }

  /* Add thousands separators. */
  char buf[32];

  char *p = buf + 31;
  *p = '\0';

  int i = -1;
  while (num) {
    if (++i && i % 3 == 0)
      *(--p) = ',';

    *(--p) = '0' + (num % 10);

    num /= 10;
  }

  return p;
}

CCL_NAMESPACE_END
