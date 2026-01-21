/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Run an external process with stdin/stdout pipes. Used for LSP (e.g.
 * typescript-language-server) over JSON-RPC.
 */

#include "BLI_process_pipe.h"
#include "MEM_guardedalloc.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <unistd.h>
#  include <sys/wait.h>
#endif

struct BLI_process_pipe {
#ifdef _WIN32
  HANDLE process;
  HANDLE pipe_stdin;
  HANDLE pipe_stdout;
#else
  pid_t pid;
  int fd_stdin;
  int fd_stdout;
#endif
};

#ifdef _WIN32
/* Build a command line from argv for CreateProcess. Minimal quoting:
 * if arg contains space, tab or ", wrap in " and escape " as \". */
static void build_cmdline(const char *const *argv, char *out, size_t out_size)
{
  size_t n = 0;
  for (; *argv; argv++) {
    const char *a = *argv;
    bool need_quote = false;
    for (const char *p = a; *p; p++) {
      if (*p == ' ' || *p == '\t' || *p == '"') {
        need_quote = true;
        break;
      }
    }
    if (n > 0) {
      if (n >= out_size) {
        break;
      }
      out[n++] = ' ';
    }
    if (need_quote) {
      if (n >= out_size) {
        break;
      }
      out[n++] = '"';
      for (; *a && n < out_size; a++) {
        if (*a == '"') {
          if (n + 2 <= out_size) {
            out[n++] = '\\';
            out[n++] = '"';
          }
        }
        else {
          out[n++] = *a;
        }
      }
      if (n < out_size) {
        out[n++] = '"';
      }
    }
    else {
      while (*a && n < out_size) {
        out[n++] = *a++;
      }
    }
  }
  if (n < out_size) {
    out[n] = '\0';
  }
  else if (out_size > 0) {
    out[out_size - 1] = '\0';
  }
}
#endif

BLI_process_pipe *BLI_process_pipe_create(const char *const *argv)
{
  if (!argv || !argv[0]) {
    return nullptr;
  }

  BLI_process_pipe *pipe = static_cast<BLI_process_pipe *>(
      MEM_mallocN(sizeof(BLI_process_pipe), "BLI_process_pipe"));

#ifdef _WIN32
  pipe->process = nullptr;
  pipe->pipe_stdin = nullptr;
  pipe->pipe_stdout = nullptr;

  HANDLE h_stdin_r = nullptr, h_stdin_w = nullptr;
  HANDLE h_stdout_r = nullptr, h_stdout_w = nullptr;

  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  if (!CreatePipe(&h_stdin_r, &h_stdin_w, &sa, 0) ||
      !CreatePipe(&h_stdout_r, &h_stdout_w, &sa, 0))
  {
    if (h_stdin_r) {
      CloseHandle(h_stdin_r);
    }
    if (h_stdin_w) {
      CloseHandle(h_stdin_w);
    }
    if (h_stdout_r) {
      CloseHandle(h_stdout_r);
    }
    if (h_stdout_w) {
      CloseHandle(h_stdout_w);
    }
    MEM_freeN(pipe);
    return nullptr;
  }

  SetHandleInformation(h_stdin_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(h_stdout_r, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = h_stdin_r;
  si.hStdOutput = h_stdout_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  if (si.hStdError == INVALID_HANDLE_VALUE || !si.hStdError) {
    si.hStdError = h_stdout_w;
  }

  PROCESS_INFORMATION pi = {0};

  char cmdline[4096];
  build_cmdline(argv, cmdline, sizeof(cmdline));

  if (!CreateProcessA(nullptr,
                      cmdline,
                      nullptr,
                      nullptr,
                      TRUE,
                      CREATE_NO_WINDOW,
                      nullptr,
                      nullptr,
                      &si,
                      &pi))
  {
    CloseHandle(h_stdin_r);
    CloseHandle(h_stdin_w);
    CloseHandle(h_stdout_r);
    CloseHandle(h_stdout_w);
    MEM_freeN(pipe);
    return nullptr;
  }

  CloseHandle(pi.hThread);
  CloseHandle(h_stdin_r);
  CloseHandle(h_stdout_w);

  pipe->process = pi.hProcess;
  pipe->pipe_stdin = h_stdin_w;
  pipe->pipe_stdout = h_stdout_r;
  return pipe;

#else
  pipe->pid = -1;
  pipe->fd_stdin = -1;
  pipe->fd_stdout = -1;

  int stdin_p[2], stdout_p[2];
  if (::pipe(stdin_p) != 0 || ::pipe(stdout_p) != 0) {
    if (stdin_p[0] >= 0) {
      close(stdin_p[0]);
      close(stdin_p[1]);
    }
    if (stdout_p[0] >= 0) {
      close(stdout_p[0]);
      close(stdout_p[1]);
    }
    MEM_freeN(pipe);
    return nullptr;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdin_p[0]);
    close(stdin_p[1]);
    close(stdout_p[0]);
    close(stdout_p[1]);
    MEM_freeN(pipe);
    return nullptr;
  }

  if (pid == 0) {
    dup2(stdin_p[0], STDIN_FILENO);
    dup2(stdout_p[1], STDOUT_FILENO);
    dup2(stdout_p[1], STDERR_FILENO);
    close(stdin_p[0]);
    close(stdin_p[1]);
    close(stdout_p[0]);
    close(stdout_p[1]);

    /* Count args and build array for execvp. */
    int n = 0;
    for (const char *const *a = argv; *a; a++) {
      n++;
    }
    char **arr = static_cast<char **>(malloc(sizeof(char *) * (n + 1)));
    if (arr) {
      for (int i = 0; i < n; i++) {
        arr[i] = const_cast<char *>(argv[i]);
      }
      arr[n] = nullptr;
      execvp(argv[0], arr);
      free(arr);
    }
    _exit(127);
  }

  close(stdin_p[0]);
  close(stdout_p[1]);

  pipe->pid = pid;
  pipe->fd_stdin = stdin_p[1];
  pipe->fd_stdout = stdout_p[0];
  return pipe;
#endif
}

void BLI_process_pipe_destroy(BLI_process_pipe *pipe)
{
  if (!pipe) {
    return;
  }
#ifdef _WIN32
  if (pipe->pipe_stdin) {
    CloseHandle(pipe->pipe_stdin);
    pipe->pipe_stdin = nullptr;
  }
  if (pipe->pipe_stdout) {
    CloseHandle(pipe->pipe_stdout);
    pipe->pipe_stdout = nullptr;
  }
  if (pipe->process) {
    TerminateProcess(pipe->process, 0);
    CloseHandle(pipe->process);
    pipe->process = nullptr;
  }
#else
  if (pipe->fd_stdin >= 0) {
    close(pipe->fd_stdin);
    pipe->fd_stdin = -1;
  }
  if (pipe->fd_stdout >= 0) {
    close(pipe->fd_stdout);
    pipe->fd_stdout = -1;
  }
  if (pipe->pid > 0) {
    kill(pipe->pid, SIGTERM);
    waitpid(pipe->pid, nullptr, 0);
    pipe->pid = -1;
  }
#endif
  MEM_freeN(pipe);
}

bool BLI_process_pipe_write(BLI_process_pipe *pipe, const char *data, size_t len)
{
  if (!pipe || !data) {
    return false;
  }
#ifdef _WIN32
  if (!pipe->pipe_stdin) {
    return false;
  }
  while (len) {
    DWORD w = 0;
    DWORD chunk = (len > 0x7fffffff) ? 0x7fffffff : DWORD(len);
    if (!WriteFile(pipe->pipe_stdin, data, chunk, &w, nullptr) || w == 0) {
      return false;
    }
    data += w;
    len -= w;
  }
  return true;
#else
  if (pipe->fd_stdin < 0) {
    return false;
  }
  while (len) {
    ssize_t w = write(pipe->fd_stdin, data, len);
    if (w <= 0) {
      return false;
    }
    data += w;
    len -= size_t(w);
  }
  return true;
#endif
}

int BLI_process_pipe_read(BLI_process_pipe *pipe, char *buf, size_t buf_size, int timeout_ms)
{
  if (!pipe || !buf || buf_size == 0) {
    return -1;
  }
#ifdef _WIN32
  if (!pipe->pipe_stdout) {
    return -1;
  }
  if (timeout_ms >= 0) {
    DWORD start = GetTickCount();
    for (;;) {
      DWORD avail = 0;
      if (PeekNamedPipe(pipe->pipe_stdout, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD to_read = (avail > DWORD(buf_size)) ? DWORD(buf_size) : avail;
        DWORD n = 0;
        if (ReadFile(pipe->pipe_stdout, buf, to_read, &n, nullptr)) {
          return int(n);
        }
        return -1;
      }
      if (GetTickCount() - start >= DWORD(timeout_ms)) {
        return 0;
      }
      Sleep(10);
    }
  }
  else {
    DWORD n = 0;
    DWORD to_read = (buf_size > 0x7fffffff) ? 0x7fffffff : DWORD(buf_size);
    if (ReadFile(pipe->pipe_stdout, buf, to_read, &n, nullptr)) {
      return int(n);
    }
    return -1;
  }
#else
  if (pipe->fd_stdout < 0) {
    return -1;
  }
  if (timeout_ms >= 0) {
    struct pollfd pfd = {pipe->fd_stdout, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
      return -1;
    }
    if (r == 0 || !(pfd.revents & POLLIN)) {
      return 0;
    }
  }
  ssize_t n = read(pipe->fd_stdout, buf, buf_size);
  if (n < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  }
  return int(n);
#endif
}

bool BLI_process_pipe_is_alive(const BLI_process_pipe *pipe)
{
  if (!pipe) {
    return false;
  }
#ifdef _WIN32
  if (!pipe->process) {
    return false;
  }
  DWORD code = 0;
  return GetExitCodeProcess(pipe->process, &code) && code == STILL_ACTIVE;
#else
  if (pipe->pid <= 0) {
    return false;
  }
  return waitpid(pipe->pid, nullptr, WNOHANG) == 0;
#endif
}
