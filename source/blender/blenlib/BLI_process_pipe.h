/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Run an external process with stdin/stdout pipes and optional read timeout.
 * Used for LSP (e.g. typescript-language-server) over JSON-RPC.
 */

#include "BLI_sys_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BLI_process_pipe;

/**
 * Spawn a process with stdin and stdout connected to pipes.
 *
 * \param argv: Null-terminated array of arguments. argv[0] is the executable
 *              (searched in PATH on Unix; on Windows the first token is used).
 *              E.g. {"npx", "typescript-language-server", "--stdio", nullptr}
 * \return A handle on success, NULL on failure (executable not found, fork/ spawn error).
 */
struct BLI_process_pipe *BLI_process_pipe_create(const char *const *argv);

/**
 * Close pipes and terminate the process. Safe to call with NULL.
 */
void BLI_process_pipe_destroy(struct BLI_process_pipe *pipe);

/**
 * Write bytes to the process stdin.
 * \return true on success, false on write error or closed pipe.
 */
bool BLI_process_pipe_write(struct BLI_process_pipe *pipe, const char *data, size_t len);

/**
 * Read from the process stdout with a timeout.
 *
 * \param buf: Output buffer.
 * \param buf_size: Size of \a buf.
 * \param timeout_ms: Max milliseconds to wait for data. 0 = non-blocking; <0 = wait forever.
 * \return Number of bytes read; 0 if timeout or no data; -1 on error or EOF.
 */
int BLI_process_pipe_read(
    struct BLI_process_pipe *pipe, char *buf, size_t buf_size, int timeout_ms);

/**
 * Check if the process is still running.
 */
bool BLI_process_pipe_is_alive(const struct BLI_process_pipe *pipe);

#ifdef __cplusplus
}
#endif
