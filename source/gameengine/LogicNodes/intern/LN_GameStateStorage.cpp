/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateStorage.cpp
 *  \ingroup logicnodes
 */

#include "LN_GameStateInternal.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#ifdef WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_system.h"
#include BLI_SYSTEM_PID_H

namespace upbge::logic_nodes::game_state {

namespace {

struct PathReservation {
  const void *owner = nullptr;
  size_t count = 0;
};

struct PathCoordinator {
  std::mutex mutex;
  std::unordered_map<std::string, PathReservation> reservations;
};

PathCoordinator &path_coordinator()
{
  /* Game-state services can be destroyed during global engine shutdown. Keep this small
   * process-wide coordinator alive until process exit so destruction order cannot invalidate it. */
  static PathCoordinator *coordinator = new PathCoordinator();
  return *coordinator;
}

}  // namespace

bool reserve_process_path(const std::string &filepath,
                          const void *owner,
                          std::string &r_error)
{
  PathCoordinator &coordinator = path_coordinator();
  std::lock_guard lock(coordinator.mutex);
  const auto found = coordinator.reservations.find(filepath);
  if (found != coordinator.reservations.end()) {
    if (found->second.owner != owner) {
      r_error = "save path is already in use by another active runtime scene";
      return false;
    }
    found->second.count++;
    return true;
  }
  try {
    coordinator.reservations.emplace(filepath, PathReservation{owner, 1});
  }
  catch (...) {
    r_error = "could not reserve the save path";
    return false;
  }
  return true;
}

void release_process_path(const std::string &filepath, const void *owner, const size_t count)
{
  PathCoordinator &coordinator = path_coordinator();
  std::lock_guard lock(coordinator.mutex);
  const auto found = coordinator.reservations.find(filepath);
  if (found == coordinator.reservations.end() || found->second.owner != owner) {
    return;
  }
  if (found->second.count <= count) {
    coordinator.reservations.erase(found);
  }
  else {
    found->second.count -= count;
  }
}

namespace {

bool is_regular_file(const blender::BLI_stat_t &file_stat)
{
#ifdef WIN32
  return (file_stat.st_mode & _S_IFMT) == _S_IFREG;
#else
  return S_ISREG(file_stat.st_mode);
#endif
}

int close_file_descriptor(const int descriptor)
{
#ifdef WIN32
  return _close(descriptor);
#else
  return close(descriptor);
#endif
}

FILE *open_file_stream(const int descriptor, const char *mode)
{
#ifdef WIN32
  return _fdopen(descriptor, mode);
#else
  return fdopen(descriptor, mode);
#endif
}

bool sync_file_descriptor(const int descriptor)
{
#ifdef WIN32
  return _commit(descriptor) == 0;
#else
  return fsync(descriptor) == 0;
#endif
}

bool sync_parent_directory(const std::string &filepath)
{
#ifdef WIN32
  return true;
#else
  if (filepath.size() >= FILE_MAX) {
    return false;
  }
  char directory[FILE_MAX];
  std::snprintf(directory, sizeof(directory), "%s", filepath.c_str());
  if (!blender::BLI_path_parent_dir(directory)) {
    return false;
  }
  int flags = O_RDONLY;
#  ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#  endif
  const int descriptor = blender::BLI_open(directory, flags, 0);
  if (descriptor < 0) {
    return false;
  }
  const bool success = fsync(descriptor) == 0;
  const bool closed = close_file_descriptor(descriptor) == 0;
  return success && closed;
#endif
}

}  // namespace

uint64_t next_request_id()
{
  static std::atomic<uint64_t> request_id{1};
  return request_id.fetch_add(1, std::memory_order_relaxed);
}

namespace {

std::string temporary_save_path(const std::string &filepath, const uint64_t request_id)
{
  static const uint64_t process_nonce = uint64_t(
      std::chrono::system_clock::now().time_since_epoch().count());
  return filepath + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(process_nonce) +
         "-" + std::to_string(request_id);
}

}  // namespace

LN_GameStateResult commit_checkpoint_temporary(const std::string &temporary_path,
                                               const std::string &destination_path,
                                               std::string &r_error)
{
  blender::BLI_stat_t destination_stat;
  if (blender::BLI_stat(destination_path.c_str(), &destination_stat) == 0 &&
      !is_regular_file(destination_stat))
  {
    r_error = "save destination exists and is not a regular file";
    return LN_GameStateResult::PathUnavailable;
  }
  if (blender::BLI_rename_overwrite(temporary_path.c_str(), destination_path.c_str()) != 0) {
    r_error = "could not atomically replace the save file";
    return LN_GameStateResult::IoError;
  }
  return LN_GameStateResult::Success;
}

LN_GameStateResult write_checkpoint_atomic(const std::string &filepath,
                                           const Checkpoint &checkpoint,
                                           const uint64_t request_id,
                                           const CommitTemporaryFn &commit_temporary,
                                           std::string &r_error)
{
  std::string contents;
  const LN_GameStateResult encode_result = encode_checkpoint(checkpoint, contents, r_error);
  if (!LN_GameStateSucceeded(encode_result)) {
    return encode_result;
  }
  if (!blender::BLI_file_ensure_parent_dir_exists(filepath.c_str())) {
    r_error = "could not create the save directory";
    return LN_GameStateResult::IoError;
  }

  const std::string temporary_path = temporary_save_path(filepath, request_id);
  const int descriptor = blender::BLI_open(
      temporary_path.c_str(), O_BINARY | O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (descriptor < 0) {
    r_error = "could not open the temporary Save Game file";
    return LN_GameStateResult::IoError;
  }
  FILE *file = open_file_stream(descriptor, "wb");
  if (file == nullptr) {
    close_file_descriptor(descriptor);
    blender::BLI_delete(temporary_path.c_str(), false, false);
    r_error = "could not create the temporary Save Game stream";
    return LN_GameStateResult::IoError;
  }

  bool success = contents.empty() ||
                 fwrite(contents.data(), 1, contents.size(), file) == contents.size();
  if (success) {
    success = fflush(file) == 0;
  }
  if (success) {
    success = sync_file_descriptor(descriptor);
  }
  if (fclose(file) != 0) {
    success = false;
  }
  if (!success) {
    blender::BLI_delete(temporary_path.c_str(), false, false);
    r_error = "could not write the complete Save Game file";
    return LN_GameStateResult::IoError;
  }
  const LN_GameStateResult commit_result = commit_temporary(temporary_path, filepath, r_error);
  if (!LN_GameStateSucceeded(commit_result)) {
    blender::BLI_delete(temporary_path.c_str(), false, false);
    return commit_result;
  }
  if (!sync_parent_directory(filepath)) {
    r_error = "Save Game file was replaced, but its directory could not be synchronized";
    return LN_GameStateResult::IoError;
  }
  return LN_GameStateResult::Success;
}

LN_GameStateResult read_checkpoint(const std::string &filepath,
                                   Checkpoint &r_checkpoint,
                                   std::string &r_error)
{
  int open_flags = O_BINARY | O_RDONLY;
#ifndef WIN32
  open_flags |= O_NONBLOCK;
#endif
  const int descriptor = blender::BLI_open(filepath.c_str(), open_flags, 0);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      r_error = "save file does not exist";
      return LN_GameStateResult::NotFound;
    }
    r_error = "save file is not accessible";
    return LN_GameStateResult::IoError;
  }

  blender::BLI_stat_t file_stat;
  if (blender::BLI_fstat(descriptor, &file_stat) != 0) {
    close_file_descriptor(descriptor);
    r_error = "could not inspect the opened save file";
    return LN_GameStateResult::IoError;
  }
  if (!is_regular_file(file_stat)) {
    close_file_descriptor(descriptor);
    r_error = "save path is not a regular file";
    return LN_GameStateResult::IoError;
  }
  if (file_stat.st_size < 0 || uint64_t(file_stat.st_size) > kMaxFileBytes) {
    close_file_descriptor(descriptor);
    r_error = "save file exceeds the file-size limit";
    return LN_GameStateResult::CorruptData;
  }

  std::string contents(size_t(file_stat.st_size), '\0');
  const bool success = contents.empty() ||
                       blender::BLI_read(descriptor, contents.data(), contents.size()) ==
                           int64_t(contents.size());
  char extra_byte = '\0';
  const int64_t extra_bytes = success ? blender::BLI_read(descriptor, &extra_byte, 1) : -1;
  const bool complete = success && extra_bytes == 0;
  const bool close_success = close_file_descriptor(descriptor) == 0;
  if (!complete || !close_success) {
    r_error = "save file changed or could not be read completely";
    return LN_GameStateResult::IoError;
  }
  return decode_checkpoint(std::move(contents), r_checkpoint, r_error);
}

}  // namespace upbge::logic_nodes::game_state
