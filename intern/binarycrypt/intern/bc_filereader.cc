/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_binarycrypt
 *
 * Decrypting #FileReader. It sits directly on top of the raw stream Blender opened (mmap, file or
 * memory reader) and below the compression readers, so a zstd compressed .blend sealed in a
 * container is unwrapped by the stock zstd reader on top of this one.
 *
 * Memory: one chunk of plaintext per open file (64 KiB by default) instead of the whole file.
 * With the mmap base reader, kernel read-ahead overlaps disk I/O with decryption without threads.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BLI_filereader.hh"

#include "MEM_guardedalloc.h"

#include "BC_binarycrypt.hh"
#include "bc_runtime.hh"
#include "binarycrypt_format.h"

namespace blender {

namespace {

constexpr uint64_t BC_NO_CHUNK = UINT64_MAX;
constexpr size_t BC_CACHE_LINE = 64;

struct BCFileReader {
  /** Must be first: Blender handles this object as a plain #FileReader. */
  FileReader reader;
  /** Raw container stream as opened by Blender. Owned. */
  FileReader *base;
  BC_Header header;
  /** Associated data of every chunk, exactly as stored on disk. */
  uint8_t header_raw[BC_HEADER_SIZE];
  /** Entry of the runtime key table named by the header's key id. */
  const uint8_t *key;
  /** Index of the chunk currently decrypted in #plain, #BC_NO_CHUNK when none. */
  uint64_t cached_chunk;
  /** One chunk of plaintext. */
  uint8_t *plain;
  uint8_t mac[BC_MAC_SIZE];
};

/**
 * Read chunk \a index from the container into \a dst and open it in place. The ciphertext lands
 * directly in the destination, so a single buffer is touched and the plaintext is still in cache
 * when the caller consumes it.
 */
bool bc_open_chunk(BCFileReader *r, const uint64_t index, uint8_t *dst)
{
  const uint64_t size = bc_chunk_plain_size(&r->header, index);
  const off64_t offset = off64_t(bc_chunk_offset(&r->header, index));
  FileReader *base = r->base;

  /* Sequential reading leaves the stream exactly at the next chunk: skip the seek (a system call
   * for the plain file reader) when it is already there. */
  if (base->offset != offset && base->seek(base, offset, SEEK_SET) != offset) {
    fprintf(stderr, "BinaryCrypt: seek error at chunk %llu\n", (unsigned long long)index);
    return false;
  }
  if (base->read(base, dst, size_t(size)) != int64_t(size) ||
      base->read(base, r->mac, BC_MAC_SIZE) != int64_t(BC_MAC_SIZE))
  {
    fprintf(stderr, "BinaryCrypt: read error at chunk %llu\n", (unsigned long long)index);
    return false;
  }

  uint8_t nonce[BC_NONCE_SIZE];
  bc_chunk_nonce(r->header.nonce_prefix, index, nonce);
  if (crypto_aead_unlock(dst,
                         r->mac,
                         r->key,
                         nonce,
                         r->header_raw,
                         BC_HEADER_SIZE,
                         dst,
                         size_t(size)) != 0)
  {
    fprintf(stderr,
            "BinaryCrypt: chunk %llu failed authentication (file corrupted or modified)\n",
            (unsigned long long)index);
    return false;
  }
  return true;
}

int64_t bc_read(FileReader *reader, void *buffer, size_t size)
{
  BCFileReader *r = reinterpret_cast<BCFileReader *>(reader);
  const uint64_t plain_size = r->header.plain_size;
  uint64_t offset = uint64_t(reader->offset);
  if (offset >= plain_size) {
    return 0;
  }
  size = size_t(std::min<uint64_t>(size, plain_size - offset));

  const int chunk_log2 = r->header.chunk_log2;
  const uint64_t chunk_mask = (uint64_t(1) << chunk_log2) - 1;
  uint8_t *dst = static_cast<uint8_t *>(buffer);
  size_t done = 0;

  while (done < size) {
    const uint64_t index = offset >> chunk_log2;
    const uint64_t in_chunk = offset & chunk_mask;
    const uint64_t chunk_size = bc_chunk_plain_size(&r->header, index);
    const uint64_t wanted = size - done;
    uint64_t n;

    if (in_chunk == 0 && wanted >= chunk_size && index != r->cached_chunk) {
      /* The request covers this whole chunk (bulk arrays: mesh, image, curve data): decrypt
       * straight into the caller's memory, no intermediate copy. */
      if (!bc_open_chunk(r, index, dst + done)) {
        return -1;
      }
      n = chunk_size;
    }
    else {
      /* Small or unaligned reads (block headers, small structs): decrypt the chunk once into the
       * cache and serve the following reads from it. */
      if (index != r->cached_chunk) {
        r->cached_chunk = BC_NO_CHUNK;
        if (!bc_open_chunk(r, index, r->plain)) {
          return -1;
        }
        r->cached_chunk = index;
      }
      n = std::min(chunk_size - in_chunk, wanted);
      memcpy(dst + done, r->plain + in_chunk, size_t(n));
    }
    done += size_t(n);
    offset += n;
  }

  reader->offset = off64_t(offset);
  return int64_t(done);
}

/* Seeking only moves the logical offset: the chunk is decrypted lazily by the next read. */
off64_t bc_seek(FileReader *reader, off64_t offset, int whence)
{
  BCFileReader *r = reinterpret_cast<BCFileReader *>(reader);
  const off64_t plain_size = off64_t(r->header.plain_size);
  off64_t new_offset;
  if (whence == SEEK_SET) {
    new_offset = offset;
  }
  else if (whence == SEEK_CUR) {
    new_offset = reader->offset + offset;
  }
  else if (whence == SEEK_END) {
    new_offset = plain_size + offset;
  }
  else {
    return -1;
  }
  if (new_offset < 0 || new_offset > plain_size) {
    return -1;
  }
  reader->offset = new_offset;
  return new_offset;
}

void bc_close(FileReader *reader)
{
  BCFileReader *r = reinterpret_cast<BCFileReader *>(reader);
  r->base->close(r->base);
  /* Do not leave the last decrypted chunk behind in freed heap memory. */
  crypto_wipe(r->plain, size_t(1) << r->header.chunk_log2);
  MEM_delete_void(static_cast<void *>(r->plain));
  MEM_delete(r);
}

}  // namespace

}  // namespace blender

static_assert(int(BCKind::Blend) == BC_KIND_BLEND && int(BCKind::Python) == BC_KIND_PYTHON &&
                  int(BCKind::Data) == BC_KIND_DATA,
              "BCKind must match the container format");

bool BC_file_magic_is_container(const char *first_bytes)
{
  return memcmp(first_bytes, BC_MAGIC, BC_MAGIC_SNIFF_SIZE) == 0;
}

void BC_wipe(void *data, const size_t size)
{
  crypto_wipe(data, size);
}

blender::FileReader *BC_filereader_new(blender::FileReader *base)
{
  const char *error = nullptr;
  blender::FileReader *reader = BC_filereader_open(base, BCKind::Blend, &error);
  if (reader == nullptr && error != nullptr) {
    fprintf(stderr, "BinaryCrypt: %s\n", error);
  }
  return reader;
}

blender::FileReader *BC_filereader_open(blender::FileReader *base, const BCKind kind, const char **r_error)
{
  using namespace blender;

  *r_error = nullptr;
  if (base == nullptr) {
    return nullptr;
  }
  if (!BC_runtime_has_key()) {
    *r_error = "encrypted files can only be opened by the UPBGE player started from its game executable";
    base->close(base);
    return nullptr;
  }

  uint8_t header_raw[BC_HEADER_SIZE];
  BC_Header header = {};
  const char *error = nullptr;

  if (base->seek(base, 0, SEEK_SET) != 0 ||
      base->read(base, header_raw, BC_HEADER_SIZE) != int64_t(BC_HEADER_SIZE))
  {
    error = "unable to read the container header";
  }
  else {
    switch (bc_header_read(header_raw, &header)) {
      case BC_HEADER_OK:
        break;
      case BC_HEADER_BAD_MAGIC:
        error = "not a BinaryCrypt container";
        break;
      case BC_HEADER_BAD_VERSION:
        error = "unsupported container version or feature (sealed by a newer BinaryCrypt?)";
        break;
      case BC_HEADER_CORRUPT:
        error = "corrupt container header";
        break;
    }
  }
  if (error == nullptr && header.kind != uint8_t(kind)) {
    /* Never another reader's content: a script must not get a scene or a module's source. */
    static const char *const held[] = {"the file is an encrypted .blend file",
                                       "the file is an encrypted Python module",
                                       "the file is an encrypted data file"};
    error = held[header.kind];
  }
  const uint8_t *key = error == nullptr ? bc_runtime_find_key(header.key_id) : nullptr;
  if (error == nullptr && key == nullptr) {
    error = "file sealed with a key the launcher did not provide (launcher and files come from different builds or updates)";
  }
  if (error == nullptr) {
    /* The exact size follows from the header: checking it here turns truncation into a clear
     * error at open time instead of a failure deep inside the blend reader. */
    const off64_t end = base->seek(base, 0, SEEK_END);
    if (end < 0 || uint64_t(end) != bc_container_size(&header)) {
      error = "container is truncated or has trailing data";
    }
  }
  if (error != nullptr) {
    *r_error = error;
    base->close(base);
    return nullptr;
  }

  BCFileReader *r = MEM_new_zeroed<BCFileReader>(__func__);
  r->base = base;
  r->header = header;
  memcpy(r->header_raw, header_raw, BC_HEADER_SIZE);
  r->key = key;
  r->cached_chunk = BC_NO_CHUNK;
  r->plain = static_cast<uint8_t *>(
      MEM_new_uninitialized_aligned(size_t(1) << header.chunk_log2, BC_CACHE_LINE, __func__));
  r->reader.read = bc_read;
  r->reader.seek = bc_seek;
  r->reader.close = bc_close;
  r->reader.offset = 0;

  bc_runtime_mark_tainted();
  return &r->reader;
}
