/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_binarycrypt
 */

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#include "BC_binarycrypt.hh"
#include "bc_runtime.hh"
#include "binarycrypt_format.h"

/* The keys are written exactly once at startup, before any thread that reads files exists, and
 * are read-only afterwards: plain globals need no synchronization. 64 keys are 2 KiB plus 512
 * bytes of ids; a lookup compares 8 byte ids once per opened file. */
constexpr size_t BC_KEY_TABLE_SIZE = size_t(BC_KEYS_MAX) * BC_KEY_SIZE;
static uint8_t (*g_keys)[BC_KEY_SIZE] = nullptr;
static uint8_t g_key_ids[BC_KEYS_MAX][BC_KEY_ID_SIZE];
static int g_key_count = 0;

/* Set by whichever thread opens a container (asynchronous library loading included) and read by
 * the blend writer. It publishes no other data, so relaxed ordering is enough. */
static std::atomic<bool> g_tainted{false};

static void bc_runtime_wipe()
{
  if (g_keys != nullptr) {
    crypto_wipe(g_keys, BC_KEY_TABLE_SIZE);
  }
  g_key_count = 0;
}

/**
 * The key table gets a page of its own: locked, so it is never written to the swap file or the
 * page file, and on Linux left out of core dumps (a crash must not leave the keys on disk). Best
 * effort: where locking is refused (RLIMIT_MEMLOCK) the table still works, merely pageable. The
 * page is never freed: it is wiped at exit and the process ends right after.
 */
static uint8_t *bc_key_table_alloc()
{
#ifdef _WIN32
  void *page = VirtualAlloc(nullptr, BC_KEY_TABLE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (page != nullptr) {
    VirtualLock(page, BC_KEY_TABLE_SIZE);
  }
#else
  void *page = mmap(nullptr, BC_KEY_TABLE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    return nullptr;
  }
  mlock(page, BC_KEY_TABLE_SIZE);
#  ifdef MADV_DONTDUMP
  madvise(page, BC_KEY_TABLE_SIZE, MADV_DONTDUMP);
#  endif
#endif
  return static_cast<uint8_t *>(page);
}

/** Strict decimal parse: digits only, no sign, no overflow. */
static bool bc_parse_u64(const char *str, uint64_t *r_value)
{
  if (*str == '\0') {
    return false;
  }
  uint64_t value = 0;
  for (const char *c = str; *c != '\0'; c++) {
    if (*c < '0' || *c > '9') {
      return false;
    }
    const uint64_t digit = uint64_t(*c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *r_value = value;
  return true;
}

static void bc_unset_key_pipe_env()
{
#ifdef _WIN32
  _putenv_s(BC_KEY_PIPE_ENV, "");
#else
  unsetenv(BC_KEY_PIPE_ENV);
#endif
}

#ifdef _WIN32
using BC_Pipe = HANDLE;
#else
using BC_Pipe = int;
#endif

/** Read \a size bytes, fewer only at end of file. \return the bytes read, -1 on a read error. */
static int64_t bc_pipe_read(const BC_Pipe pipe, uint8_t *dst, const size_t size)
{
  size_t done = 0;
  while (done < size) {
#ifdef _WIN32
    DWORD n = 0;
    if (!ReadFile(pipe, dst + done, DWORD(size - done), &n, nullptr)) {
      /* Broken pipe: the launcher closed its end, end of file. */
      return GetLastError() == ERROR_BROKEN_PIPE ? int64_t(done) : -1;
    }
#else
    const ssize_t n = read(pipe, dst + done, size - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
#endif
    if (n == 0) {
      break;
    }
    done += size_t(n);
  }
  return int64_t(done);
}

/**
 * Read the hand-off from the inherited pipe, then close it: the header, the keys straight into the
 * locked table, the extension a newer launcher may add (no feature uses one yet: read and dropped),
 * then end of file. \return the number of keys, 0 on any deviation.
 */
static int bc_read_keys_from_pipe(const uint64_t pipe_value, uint8_t *dst)
{
#ifdef _WIN32
  const BC_Pipe pipe = HANDLE(uintptr_t(pipe_value));
#else
  if (pipe_value > uint64_t(INT_MAX)) {
    return 0;
  }
  const BC_Pipe pipe = int(pipe_value);
#endif
  uint8_t header[BC_HANDOFF_HEADER_SIZE];
  uint8_t extension[BC_HANDOFF_EXTENSION_MAX];
  uint8_t trailing;
  uint32_t extension_size = 0;
  int count = 0;
  if (bc_pipe_read(pipe, header, sizeof(header)) == int64_t(sizeof(header))) {
    count = bc_handoff_header_read(header, &extension_size);
  }
  const size_t keys_size = size_t(count) * BC_KEY_SIZE;
  const bool ok = count > 0 && bc_pipe_read(pipe, dst, keys_size) == int64_t(keys_size) &&
                  bc_pipe_read(pipe, extension, extension_size) == int64_t(extension_size) &&
                  bc_pipe_read(pipe, &trailing, 1) == 0;
  crypto_wipe(extension, sizeof(extension));
  /* Closing keeps the pipe from leaking into processes the game may spawn later. */
#ifdef _WIN32
  CloseHandle(pipe);
#else
  close(pipe);
#endif
  return ok ? count : 0;
}

bool BC_runtime_init_from_launcher()
{
  if (g_key_count > 0) {
    return true;
  }
  const char *env = getenv(BC_KEY_PIPE_ENV);
  if (env == nullptr) {
    return false; /* Not started by a launcher: unprotected game. */
  }
  uint64_t pipe_value = 0;
  const bool parsed = bc_parse_u64(env, &pipe_value);
  /* Drop the variable before anything else can observe it: neither Python nor processes spawned
   * by the game need to know a key pipe ever existed. `env` is invalid from here on. */
  bc_unset_key_pipe_env();

  uint8_t *table = bc_key_table_alloc();
  if (table == nullptr) {
    fprintf(stderr, "BinaryCrypt: cannot allocate the key table\n");
    return false;
  }
  g_keys = reinterpret_cast<uint8_t(*)[BC_KEY_SIZE]>(table);
  const int count = parsed ? bc_read_keys_from_pipe(pipe_value, table) : 0;
  if (count == 0) {
    crypto_wipe(table, BC_KEY_TABLE_SIZE);
    fprintf(stderr, "BinaryCrypt: failed to receive the keys from the launcher\n");
    return false;
  }
  for (int i = 0; i < count; i++) {
    bc_key_id(g_keys[i], g_key_ids[i]);
  }
  g_key_count = count;
  atexit(bc_runtime_wipe);
  return true;
}

bool BC_runtime_has_key()
{
  return g_key_count > 0;
}

bool BC_runtime_is_tainted()
{
  return g_tainted.load(std::memory_order_relaxed);
}

const uint8_t *bc_runtime_find_key(const uint8_t key_id[BC_KEY_ID_SIZE])
{
  for (int i = 0; i < g_key_count; i++) {
    if (memcmp(g_key_ids[i], key_id, BC_KEY_ID_SIZE) == 0) {
      return g_keys[i];
    }
  }
  return nullptr;
}

void bc_runtime_mark_tainted()
{
  g_tainted.store(true, std::memory_order_relaxed);
}
