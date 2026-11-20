/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Exercises the container reader through Blender's real entry point,
 * #BLO_file_reader_uncompressed, on top of Blender's memory reader, so the magic dispatch, the
 * recursion into the compression readers and the reader contract are all covered. */

#include "testing/testing.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

#include <zstd.h>

#include "BLI_filereader.hh"

#include "BLO_core_file_reader.hh"

#include "BC_binarycrypt.hh"
#include "binarycrypt_format.h"
#include "intern/bc_runtime.hh"

namespace blender::binarycrypt::tests {

static const uint8_t TEST_KEY[BC_KEY_SIZE] = {0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
                                              0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
                                              0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
                                              0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4};
static const uint8_t OTHER_KEY[BC_KEY_SIZE] = {1, 2, 3};
/** Handed over together with TEST_KEY: a game updated with a new key per update. */
static const uint8_t UPDATE_KEY[BC_KEY_SIZE] = {7, 7, 7, 7, 42};

/* Small chunks so that a few KiB of test data span many chunks. */
static constexpr int TEST_CHUNK_LOG2 = BC_CHUNK_LOG2_MIN;
static constexpr size_t TEST_CHUNK = size_t(1) << TEST_CHUNK_LOG2;

/** Deterministic stand-in for a .blend: the real magic followed by pseudo-random bytes. */
static std::vector<uint8_t> make_blend(const size_t size)
{
  std::vector<uint8_t> data(size);
  uint32_t state = 0x9E3779B9u;
  for (size_t i = 0; i < size; i++) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    data[i] = uint8_t(state);
  }
  memcpy(data.data(), "BLENDER", std::min<size_t>(7, size));
  return data;
}

/** Reference sealer: writes exactly what the UPBGE-BinaryCrypt tool writes. \a required,
 * \a optional and \a reserved_fill stand for what a newer tool may write. */
static std::vector<uint8_t> seal(const std::vector<uint8_t> &plain,
                                 const uint8_t key[BC_KEY_SIZE],
                                 const int chunk_log2 = TEST_CHUNK_LOG2,
                                 const uint8_t kind = BC_KIND_BLEND,
                                 const uint32_t required = 0,
                                 const uint32_t optional = 0,
                                 const uint8_t reserved_fill = 0)
{
  BC_Header header = {};
  header.version = BC_FORMAT_VERSION;
  header.required = required;
  header.optional = optional;
  header.chunk_log2 = uint8_t(chunk_log2);
  header.kind = kind;
  header.plain_size = plain.size();
  bc_key_id(key, header.key_id);
  for (int i = 0; i < BC_NONCE_PREFIX_SIZE; i++) {
    header.nonce_prefix[i] = uint8_t(0xA0 + i);
  }
  /* Opaque to readers: any value opens. */
  for (int i = 0; i < BC_SOURCE_ID_SIZE; i++) {
    header.source_id[i] = uint8_t(0x50 + i);
  }

  std::vector<uint8_t> out(size_t(bc_container_size(&header)));
  bc_header_write(&header, out.data());
  memset(out.data() + BC_OFS_RESERVED, reserved_fill, BC_RESERVED_SIZE);
  for (uint64_t i = 0; i < bc_chunk_count(&header); i++) {
    const size_t size = size_t(bc_chunk_plain_size(&header, i));
    uint8_t *dst = out.data() + bc_chunk_offset(&header, i);
    uint8_t nonce[BC_NONCE_SIZE];
    bc_chunk_nonce(header.nonce_prefix, i, nonce);
    crypto_aead_lock(dst,
                     dst + size,
                     key,
                     nonce,
                     out.data(),
                     BC_HEADER_SIZE,
                     plain.data() + (size_t(i) << chunk_log2),
                     size);
  }
  return out;
}

/** Hand the test keys to the runtime through a real pipe, as a newer launcher would: header,
 * keys, and an extension under an optional feature, which the player must read and drop. */
static bool ensure_key_loaded()
{
  if (BC_runtime_has_key()) {
    return true;
  }
  constexpr uint32_t extension_size = 100;
  uint8_t handoff[BC_HANDOFF_HEADER_SIZE + 2 * BC_KEY_SIZE + extension_size];
  bc_handoff_header_write(2, handoff);
  bc_store_u32(handoff + 16, 1u << 31); /* Optional feature. */
  bc_store_u32(handoff + 20, extension_size);
  uint8_t *keys = handoff + BC_HANDOFF_HEADER_SIZE;
  memcpy(keys, TEST_KEY, BC_KEY_SIZE);
  memcpy(keys + BC_KEY_SIZE, UPDATE_KEY, BC_KEY_SIZE);
  memset(keys + 2 * BC_KEY_SIZE, 0x5A, extension_size);
#ifdef _WIN32
  HANDLE read_end, write_end;
  if (!CreatePipe(&read_end, &write_end, nullptr, 0)) {
    return false;
  }
  DWORD written = 0;
  WriteFile(write_end, handoff, sizeof(handoff), &written, nullptr);
  CloseHandle(write_end);
  _putenv_s(BC_KEY_PIPE_ENV, std::to_string(uintptr_t(read_end)).c_str());
#else
  int fds[2];
  if (pipe(fds) != 0 || write(fds[1], handoff, sizeof(handoff)) != ssize_t(sizeof(handoff))) {
    return false;
  }
  close(fds[1]);
  setenv(BC_KEY_PIPE_ENV, std::to_string(fds[0]).c_str(), 1);
#endif
  return BC_runtime_init_from_launcher();
}

static FileReader *open_container(const std::vector<uint8_t> &data)
{
  return BLO_file_reader_uncompressed(BLI_filereader_new_memory(data.data(), data.size()));
}

/** Read everything with a repeating pattern of request sizes. Returns false on a read error. */
static bool read_in_pieces(FileReader *reader,
                           const std::vector<size_t> &pattern,
                           std::vector<uint8_t> &r_data)
{
  r_data.clear();
  uint8_t buffer[16384];
  for (size_t i = 0;; i++) {
    const int64_t n = reader->read(reader, buffer, pattern[i % pattern.size()]);
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    r_data.insert(r_data.end(), buffer, buffer + n);
  }
}

TEST(binarycrypt, header_layout_is_little_endian_and_stable)
{
  BC_Header header = {};
  header.version = BC_FORMAT_VERSION;
  header.chunk_log2 = BC_CHUNK_LOG2_DEFAULT;
  header.optional = 0x80000001u;
  header.plain_size = 0x0102030405ull;
  header.source_id[0] = 0x77;
  uint8_t raw[BC_HEADER_SIZE];
  bc_header_write(&header, raw);

  EXPECT_EQ(BC_HEADER_SIZE, 128);
  EXPECT_EQ(memcmp(raw, "UPBGEBC\x1A", 8), 0);
  EXPECT_EQ(raw[BC_OFS_VERSION], 1);
  EXPECT_EQ(raw[BC_OFS_VERSION + 1], 0);
  EXPECT_EQ(raw[BC_OFS_CHUNK_LOG2], 16);
  EXPECT_EQ(raw[BC_OFS_PLAIN_SIZE + 0], 0x05);
  EXPECT_EQ(raw[BC_OFS_PLAIN_SIZE + 4], 0x01);
  EXPECT_EQ(raw[BC_OFS_PLAIN_SIZE + 5], 0x00);
  EXPECT_EQ(raw[BC_OFS_SOURCE_ID], 0x77);
  EXPECT_EQ(raw[BC_OFS_OPTIONAL + 0], 0x01);
  EXPECT_EQ(raw[BC_OFS_OPTIONAL + 3], 0x80);
  EXPECT_EQ(BC_OFS_SOURCE_ID + BC_SOURCE_ID_SIZE, BC_OFS_OPTIONAL);
  EXPECT_EQ(BC_OFS_RESERVED + BC_RESERVED_SIZE, BC_HEADER_SIZE);
  for (int i = BC_OFS_RESERVED; i < BC_HEADER_SIZE; i++) {
    EXPECT_EQ(raw[i], 0) << i;
  }

  BC_Header parsed;
  ASSERT_EQ(bc_header_read(raw, &parsed), BC_HEADER_OK);
  EXPECT_EQ(parsed.plain_size, header.plain_size);
  EXPECT_EQ(parsed.optional, header.optional);
  EXPECT_EQ(parsed.source_id[0], 0x77);
  EXPECT_TRUE(BC_file_magic_is_container(reinterpret_cast<const char *>(raw)));
  EXPECT_FALSE(BC_file_magic_is_container("BLENDER"));
}

/* Must run before any test loads the key; skipped otherwise (e.g. shuffled order). */
TEST(binarycrypt, rejected_without_key)
{
  if (BC_runtime_has_key()) {
    GTEST_SKIP() << "key already loaded by an earlier test";
  }
  const std::vector<uint8_t> container = seal(make_blend(5000), TEST_KEY);
  EXPECT_EQ(open_container(container), nullptr);
  EXPECT_FALSE(BC_runtime_is_tainted());
}

TEST(binarycrypt, key_handoff_through_pipe)
{
  ASSERT_TRUE(ensure_key_loaded());
  EXPECT_TRUE(BC_runtime_has_key());
  /* The variable naming the descriptor must not survive the hand-off. */
  EXPECT_EQ(getenv(BC_KEY_PIPE_ENV), nullptr);
}

#ifdef __linux__
/* The key table has a page of its own, kept out of core dumps and (when RLIMIT_MEMLOCK allows a
 * page) out of swap: its mapping in /proc/self/smaps carries the "dd" and "lo" flags. */
TEST(binarycrypt, key_table_is_locked_and_not_dumped)
{
  ASSERT_TRUE(ensure_key_loaded());
  uint8_t id[BC_KEY_ID_SIZE];
  bc_key_id(TEST_KEY, id);
  const uintptr_t key = uintptr_t(bc_runtime_find_key(id));
  ASSERT_NE(key, uintptr_t(0));
  FILE *f = fopen("/proc/self/smaps", "r");
  ASSERT_NE(f, nullptr);
  char line[512];
  bool inside = false;
  std::string flags;
  while (flags.empty() && fgets(line, sizeof(line), f) != nullptr) {
    unsigned long begin, end;
    if (sscanf(line, "%lx-%lx ", &begin, &end) == 2) {
      inside = key >= begin && key < end;
    }
    else if (inside && strncmp(line, "VmFlags:", 8) == 0) {
      flags = line;
    }
  }
  fclose(f);
  ASSERT_FALSE(flags.empty());
  EXPECT_NE(flags.find(" dd"), std::string::npos) << flags;
#  if defined(__SANITIZE_ADDRESS__)
  /* AddressSanitizer turns mlock into a no-op. */
#  else
  struct rlimit limit;
  if (getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur >= 4096) {
    EXPECT_NE(flags.find(" lo"), std::string::npos) << flags;
  }
#  endif
}
#endif

TEST(binarycrypt, roundtrip_small_and_unaligned_reads)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(3 * TEST_CHUNK + 1234);
  const std::vector<uint8_t> container = seal(plain, TEST_KEY);

  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> data;
  EXPECT_TRUE(read_in_pieces(reader, {1, 7, 13, 64, TEST_CHUNK - 1, TEST_CHUNK + 1, 333}, data));
  EXPECT_EQ(data, plain);
  EXPECT_TRUE(BC_runtime_is_tainted());
  reader->close(reader);
}

TEST(binarycrypt, roundtrip_bulk_read_and_seeks)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(5 * TEST_CHUNK + 17);
  const std::vector<uint8_t> container = seal(plain, TEST_KEY);

  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr);

  /* One request spanning every chunk: exercises the decrypt-into-destination path. */
  std::vector<uint8_t> data(plain.size() + 100);
  EXPECT_EQ(reader->read(reader, data.data(), data.size()), int64_t(plain.size()));
  data.resize(plain.size());
  EXPECT_EQ(data, plain);
  EXPECT_EQ(reader->read(reader, data.data(), 1), 0);

  /* Random access, including chunk boundaries and both ends. */
  const off64_t offsets[] = {0,
                             1,
                             off64_t(TEST_CHUNK) - 3,
                             off64_t(TEST_CHUNK),
                             3 * off64_t(TEST_CHUNK) + 5,
                             off64_t(plain.size()) - 1};
  for (const off64_t offset : offsets) {
    ASSERT_EQ(reader->seek(reader, offset, SEEK_SET), offset);
    uint8_t buffer[100];
    const int64_t n = reader->read(reader, buffer, sizeof(buffer));
    const int64_t expected = std::min<int64_t>(sizeof(buffer), int64_t(plain.size()) - offset);
    ASSERT_EQ(n, expected);
    EXPECT_EQ(memcmp(buffer, plain.data() + offset, size_t(n)), 0) << "offset " << offset;
  }
  EXPECT_EQ(reader->seek(reader, -10, SEEK_END), off64_t(plain.size()) - 10);
  EXPECT_EQ(reader->seek(reader, -20, SEEK_CUR), off64_t(plain.size()) - 30);
  EXPECT_EQ(reader->seek(reader, 1, SEEK_END), -1);
  EXPECT_EQ(reader->seek(reader, -1, SEEK_SET), -1);
  reader->close(reader);
}

TEST(binarycrypt, exact_chunk_and_tiny_payloads)
{
  ASSERT_TRUE(ensure_key_loaded());
  for (const size_t size : {TEST_CHUNK, size_t(7), 2 * TEST_CHUNK}) {
    const std::vector<uint8_t> plain = make_blend(size);
    /* The memory reader does not own its buffer: keep the container alive while reading. */
    const std::vector<uint8_t> container = seal(plain, TEST_KEY);
    FileReader *reader = open_container(container);
    ASSERT_NE(reader, nullptr) << "size " << size;
    std::vector<uint8_t> data;
    EXPECT_TRUE(read_in_pieces(reader, {TEST_CHUNK}, data));
    EXPECT_EQ(data, plain) << "size " << size;
    reader->close(reader);
  }
}

TEST(binarycrypt, zstd_blend_inside_container)
{
  ASSERT_TRUE(ensure_key_loaded());
  /* Compressible payload, as real blend files are. */
  std::vector<uint8_t> plain = make_blend(40000);
  for (size_t i = 7; i < plain.size(); i++) {
    plain[i] = uint8_t((i / 97) & 0xFF);
  }
  std::vector<uint8_t> compressed(ZSTD_compressBound(plain.size()));
  const size_t compressed_size = ZSTD_compress(
      compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
  ASSERT_FALSE(ZSTD_isError(compressed_size));
  compressed.resize(compressed_size);

  const std::vector<uint8_t> container = seal(compressed, TEST_KEY);
  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr);
  /* Read exactly the payload, as the blend reader does (it stops at ENDB): Blender's streaming
   * zstd reader does not report EOF reliably once its input is exhausted, independently of the
   * container underneath, so the test must not rely on reading until 0. */
  std::vector<uint8_t> data(plain.size());
  for (size_t done = 0; done < data.size();) {
    const int64_t n = reader->read(
        reader, data.data() + done, std::min<size_t>(4096, data.size() - done));
    ASSERT_GT(n, 0);
    done += size_t(n);
  }
  EXPECT_EQ(data, plain);
  reader->close(reader);
}

TEST(binarycrypt, rejects_wrong_key)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> container = seal(make_blend(5000), OTHER_KEY);
  EXPECT_EQ(open_container(container), nullptr);
}

/* Files of one game sealed with different keys (a key per update): each opens with its own key
 * from the table, read through the same code paths. */
TEST(binarycrypt, opens_files_of_every_handed_over_key)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(3 * TEST_CHUNK + 17);
  for (const uint8_t *key : {TEST_KEY, UPDATE_KEY}) {
    const std::vector<uint8_t> container = seal(plain, key);
    FileReader *reader = open_container(container);
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> data;
    EXPECT_TRUE(read_in_pieces(reader, {1000, 1, TEST_CHUNK}, data));
    EXPECT_EQ(data, plain);
    reader->close(reader);
  }
}

TEST(binarycrypt, rejects_truncated_or_trailing_data)
{
  ASSERT_TRUE(ensure_key_loaded());
  std::vector<uint8_t> container = seal(make_blend(5000), TEST_KEY);
  container.push_back(0);
  EXPECT_EQ(open_container(container), nullptr);
  container.resize(container.size() - 2);
  EXPECT_EQ(open_container(container), nullptr);
}

TEST(binarycrypt, rejects_modified_header)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> container = seal(make_blend(5000), TEST_KEY);

  /* Fields that parse fine but are authenticated: the first chunk fails, so even the 7 byte magic
   * sniff of the decrypted stream cannot complete. */
  std::vector<uint8_t> modified = container;
  modified[BC_OFS_NONCE_PREFIX] ^= 1;
  EXPECT_EQ(open_container(modified), nullptr);
  modified = container;
  modified[BC_OFS_SOURCE_ID + 3] ^= 1;
  EXPECT_EQ(open_container(modified), nullptr);
  modified = container;
  modified[BC_OFS_OPTIONAL] ^= 1;
  EXPECT_EQ(open_container(modified), nullptr);
  modified = container;
  modified[BC_OFS_RESERVED + 7] = 1;
  EXPECT_EQ(open_container(modified), nullptr);

  /* Fields rejected by the strict parser: another version, an unknown required feature. */
  const char *error = nullptr;
  modified = container;
  modified[BC_OFS_VERSION] = BC_FORMAT_VERSION + 1;
  EXPECT_EQ(BC_filereader_open(BLI_filereader_new_memory(modified.data(), modified.size()), BCKind::Blend, &error),
            nullptr);
  EXPECT_STREQ(error, "unsupported container version or feature (sealed by a newer BinaryCrypt?)");
  const std::vector<uint8_t> required = seal(make_blend(5000), TEST_KEY, TEST_CHUNK_LOG2, BC_KIND_BLEND, 1u << 31);
  EXPECT_EQ(BC_filereader_open(BLI_filereader_new_memory(required.data(), required.size()), BCKind::Blend, &error),
            nullptr);
  EXPECT_STREQ(error, "unsupported container version or feature (sealed by a newer BinaryCrypt?)");
}

/* What a newer tool may add without breaking this reader: optional features and fields in the
 * reserved bytes. Both are authenticated and otherwise ignored. */
TEST(binarycrypt, opens_files_with_unknown_optional_features)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(70000);
  const std::vector<uint8_t> container = seal(plain, TEST_KEY, TEST_CHUNK_LOG2, BC_KIND_BLEND, 0, 0xFFFFFFFFu, 0xA5);
  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> data;
  EXPECT_TRUE(read_in_pieces(reader, {4096}, data));
  EXPECT_EQ(data, plain);
  reader->close(reader);
}

/* Each reader opens its own kind only: Blender's reader no module or data, the script API no
 * scene and no module source. */
TEST(binarycrypt, opens_only_its_own_kind)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::string source = "def answer():\n    return 42\n";
  const std::vector<uint8_t> plain(source.begin(), source.end());
  const std::vector<uint8_t> module = seal(plain, TEST_KEY, TEST_CHUNK_LOG2, BC_KIND_PYTHON);
  const std::vector<uint8_t> scene = seal(make_blend(5000), TEST_KEY);

  EXPECT_EQ(open_container(module), nullptr);

  const char *error = nullptr;
  FileReader *reader = BC_filereader_open(
      BLI_filereader_new_memory(module.data(), module.size()), BCKind::Python, &error);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(error, nullptr);
  std::vector<uint8_t> data;
  EXPECT_TRUE(read_in_pieces(reader, {7, 4096}, data));
  EXPECT_EQ(data, plain);
  reader->close(reader);

  EXPECT_EQ(BC_filereader_open(BLI_filereader_new_memory(module.data(), module.size()), BCKind::Data, &error),
            nullptr);
  EXPECT_STREQ(error, "the file is an encrypted Python module");
  EXPECT_EQ(BC_filereader_open(BLI_filereader_new_memory(scene.data(), scene.size()), BCKind::Data, &error),
            nullptr);
  EXPECT_STREQ(error, "the file is an encrypted .blend file");

  /* An unknown kind is a corrupt header for every reader. */
  std::vector<uint8_t> unknown = module;
  unknown[BC_OFS_KIND] = BC_KIND_MAX + 1;
  EXPECT_EQ(BC_filereader_open(BLI_filereader_new_memory(unknown.data(), unknown.size()), BCKind::Python, &error),
            nullptr);
  EXPECT_STREQ(error, "corrupt container header");

  uint8_t secret[4] = {1, 2, 3, 4};
  BC_wipe(secret, sizeof(secret));
  EXPECT_EQ(secret[0] | secret[1] | secret[2] | secret[3], 0);
}

TEST(binarycrypt, detects_corrupted_chunk)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(4 * TEST_CHUNK);
  std::vector<uint8_t> container = seal(plain, TEST_KEY);
  container[BC_HEADER_SIZE + 2 * (TEST_CHUNK + BC_MAC_SIZE) + 100] ^= 0x40;

  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr); /* Chunk 0 is intact. */
  std::vector<uint8_t> first(2 * TEST_CHUNK);
  EXPECT_EQ(reader->read(reader, first.data(), first.size()), int64_t(first.size()));
  uint8_t buffer[16];
  EXPECT_EQ(reader->read(reader, buffer, sizeof(buffer)), -1);
  reader->close(reader);
}

TEST(binarycrypt, detects_reordered_chunks)
{
  ASSERT_TRUE(ensure_key_loaded());
  const std::vector<uint8_t> plain = make_blend(4 * TEST_CHUNK);
  std::vector<uint8_t> container = seal(plain, TEST_KEY);
  const size_t stride = TEST_CHUNK + BC_MAC_SIZE;
  std::swap_ranges(container.begin() + BC_HEADER_SIZE + stride,
                   container.begin() + BC_HEADER_SIZE + 2 * stride,
                   container.begin() + BC_HEADER_SIZE + 2 * stride);

  FileReader *reader = open_container(container);
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> data;
  EXPECT_FALSE(read_in_pieces(reader, {TEST_CHUNK}, data));
  reader->close(reader);
}

}  // namespace blender::binarycrypt::tests
