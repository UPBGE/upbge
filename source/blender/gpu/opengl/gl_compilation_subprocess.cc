/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gl_compilation_subprocess.hh"

#if BLI_SUBPROCESS_SUPPORT

#  include "BKE_appdir.hh"
#  include "BLI_fileops.hh"
#  include "BLI_hash.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "BLI_threads.h"
#  include "CLG_log.h"
#  include "GHOST_C-api.h"
#  include "GPU_context.hh"
#  include "GPU_init_exit.hh"
#  include "gpu_capabilities_private.hh"
#  include <iostream>
#  include <string>

#  ifndef _WIN32
#    include <unistd.h>
#  else
#    include "BLI_winstuff.h"
#  endif

/* Include after `BLI_winstuff.h` to avoid APIENTRY redefinition. */
#  include <epoxy/gl.h>

namespace blender::gpu {

class SubprocessShader {
  GLuint comp_ = 0;
  GLuint vert_ = 0;
  GLuint geom_ = 0;
  GLuint frag_ = 0;
  GLuint program_ = 0;
  bool success_ = false;

 public:
  SubprocessShader(const char *comp_src,
                   const char *vert_src,
                   const char *geom_src,
                   const char *frag_src)
  {
    GLint status;
    program_ = glCreateProgram();

    auto compile_stage = [&](const char *src, GLenum stage) -> GLuint {
      if (src == nullptr) {
        /* We only want status errors if compilation fails. */
        status = GL_TRUE;
        return 0;
      }

      GLuint shader = glCreateShader(stage);
      glShaderSource(shader, 1, &src, nullptr);
      glCompileShader(shader);
      glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
      glAttachShader(program_, shader);
      return shader;
    };

    comp_ = compile_stage(comp_src, GL_COMPUTE_SHADER);
    if (!status) {
      return;
    }

    vert_ = compile_stage(vert_src, GL_VERTEX_SHADER);
    if (!status) {
      return;
    }

    geom_ = compile_stage(geom_src, GL_GEOMETRY_SHADER);
    if (!status) {
      return;
    }

    frag_ = compile_stage(frag_src, GL_FRAGMENT_SHADER);
    if (!status) {
      return;
    }

    glLinkProgram(program_);
    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (!status) {
      return;
    }

    success_ = true;
  }

  ~SubprocessShader()
  {
    glDeleteShader(comp_);
    glDeleteShader(vert_);
    glDeleteShader(geom_);
    glDeleteShader(frag_);
    glDeleteProgram(program_);
  }

  ShaderBinaryHeader *get_binary(void *memory)
  {
    ShaderBinaryHeader *bin = reinterpret_cast<ShaderBinaryHeader *>(memory);
    bin->format = 0;
    bin->size = 0;

    if (success_) {
      glGetProgramiv(program_, GL_PROGRAM_BINARY_LENGTH, &bin->size);
      if (bin->size > sizeof(ShaderBinaryHeader::data)) {
        bin->size = 0;
        return nullptr;
      }
      glGetProgramBinary(program_, bin->size, nullptr, &bin->format, bin->data);
    }

    return bin;
  }
};

/* Check if the binary is valid and can be loaded by the driver. */
static bool validate_binary(void *binary)
{
  ShaderBinaryHeader *bin = reinterpret_cast<ShaderBinaryHeader *>(binary);
  GLuint program = glCreateProgram();
  glProgramBinary(program, bin->format, bin->data, bin->size);
  GLint status;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  glDeleteProgram(program);
  return status;
}

std::string GL_shader_cache_dir_get()
{
  static char tmp_dir_buffer[1024];
  BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

  std::string cache_dir = std::string(tmp_dir_buffer) + "gl-shader-cache" + SEP_STR;
  BLI_dir_create_recursive(cache_dir.c_str());

  return cache_dir;
}

}  // namespace blender::gpu

void GPU_compilation_subprocess_run(const char *subprocess_name)
{
  using namespace blender;
  using namespace blender::gpu;

#  ifndef _WIN32
  /** NOTE: Technically, the parent process could have crashed before this. */
  pid_t ppid = getppid();
#  endif

  CLG_init();
  BLI_threadapi_init();

  /* Prevent the ShaderCompiler from spawning extra threads/contexts, we don't need them. */
  GCaps.use_main_context_workaround = true;

  std::string name = subprocess_name;
  SharedMemory shared_mem(name, compilation_subprocess_shared_memory_size, false);
  if (!shared_mem.get_data()) {
    std::cerr << "Compilation Subprocess: Failed to open shared memory " << subprocess_name
              << "\n";
    return;
  }
  SharedSemaphore start_semaphore(name + "_START", true);
  SharedSemaphore end_semaphore(name + "_END", true);
  SharedSemaphore close_semaphore(name + "_CLOSE", true);

  GHOST_SystemHandle ghost_system = GHOST_CreateSystemBackground();
  BLI_assert(ghost_system);
  GPU_backend_ghost_system_set(ghost_system);
  GHOST_GPUSettings gpu_settings = {0};
  gpu_settings.context_type = GHOST_kDrawingContextTypeOpenGL;
  GHOST_ContextHandle ghost_context = GHOST_CreateGPUContext(ghost_system, gpu_settings);
  if (ghost_context == nullptr) {
    std::cerr << "Compilation Subprocess: Failed to initialize GHOST context for "
              << subprocess_name << "\n";
    GHOST_DisposeSystem(ghost_system);
    return;
  }
  GHOST_ActivateGPUContext(ghost_context);
  GPUContext *gpu_context = GPU_context_create(nullptr, ghost_context);
  GPU_init();

  std::string cache_dir = GL_shader_cache_dir_get();

  while (true) {
    /* Process events to avoid crashes on Wayland.
     * See https://bugreports.qt.io/browse/QTBUG-81504 */
    GHOST_ProcessEvents(ghost_system, false);

#  ifdef _WIN32
    start_semaphore.decrement();
#  else
    bool lost_parent = false;
    while (!lost_parent && !start_semaphore.try_decrement(1000)) {
      lost_parent = getppid() != ppid;
    }
    if (lost_parent) {
      std::cerr << "Compilation Subprocess: Lost parent process\n";
      break;
    }
#  endif

    if (close_semaphore.try_decrement()) {
      break;
    }

    ShaderSourceHeader *source = reinterpret_cast<ShaderSourceHeader *>(shared_mem.get_data());
    const char *next_src = source->sources;
    const char *comp_src = nullptr;
    const char *vert_src = nullptr;
    const char *geom_src = nullptr;
    const char *frag_src = nullptr;

    DefaultHash<StringRefNull> hasher;
    std::string hash_str = "_";

    auto get_src = [&]() {
      const char *src = next_src;
      next_src += strlen(src) + sizeof('\0');
      hash_str += std::to_string(hasher(src)) + "_";
      return src;
    };

    if (source->type == ShaderSourceHeader::Type::COMPUTE) {
      comp_src = get_src();
    }
    else {
      vert_src = get_src();
      if (source->type == ShaderSourceHeader::Type::GRAPHICS_WITH_GEOMETRY_STAGE) {
        geom_src = get_src();
      }
      frag_src = get_src();
    }

    std::string cache_path = cache_dir + SEP_STR + hash_str;

    /* TODO: This should lock the files? */
    if (BLI_exists(cache_path.c_str())) {
      {
        /* Store the source hash in the shared memory.
         * If the subprocess crashes, the main process will delete the cache file. */
        std::string source_hash = "SOURCE_HASH:" + hash_str;
        BLI_strncpy(reinterpret_cast<char *>(shared_mem.get_data()),
                    source_hash.c_str(),
                    source_hash.size() + 1);
      }
      /* Prevent old cache files from being deleted if they're still being used. */
      BLI_file_touch(cache_path.c_str());
      /* Read cached binary. */
      fstream file(cache_path, std::ios::binary | std::ios::in | std::ios::ate);
      std::streamsize size = file.tellg();
      if (size <= compilation_subprocess_shared_memory_size) {
        file.seekg(0, std::ios::beg);
        /* Use temp memory so we don't overwrite the source hash. */
        static char tmp_mem[compilation_subprocess_shared_memory_size];
        file.read(tmp_mem, size);
        /* Ensure it's valid. */
        if (!validate_binary(tmp_mem)) {
          std::cout << "Compilation Subprocess: Failed to load cached shader binary " << hash_str
                    << "\n";
          /* TODO: No longer true. */
          /* We can't compile the shader anymore since we have written over the source code,
           * but we delete the cache for the next time this shader is requested. */
          file.close();
          BLI_delete(cache_path.c_str(), false, false);
        }
        /* Copy the temp memory to the shared memory now that we know loading the shader doesn't
         * crash the driver. */
        memcpy(shared_mem.get_data(), tmp_mem, size);
        end_semaphore.increment();
        continue;
      }
      else {
        /* This should never happen, since shaders larger than the pool size should be discarded
         * and compiled in the main Blender process. */
        std::cerr << "Compilation Subprocess: Wrong size for cached shader binary " << hash_str
                  << "\n";
        BLI_assert_unreachable();
      }
    }

    SubprocessShader shader(comp_src, vert_src, geom_src, frag_src);
    ShaderBinaryHeader *binary = shader.get_binary(shared_mem.get_data());

    end_semaphore.increment();

    if (binary) {
      fstream file(cache_path, std::ios::binary | std::ios::out);
      file.write(reinterpret_cast<char *>(shared_mem.get_data()),
                 binary->size + offsetof(ShaderBinaryHeader, data));
    }
  }

  GPU_exit();
  GPU_context_discard(gpu_context);
  GHOST_DisposeGPUContext(ghost_system, ghost_context);
  GHOST_DisposeSystem(ghost_system);
}

namespace blender::gpu {
void GL_shader_cache_dir_clear_old()
{
  std::string cache_dir = GL_shader_cache_dir_get();

  direntry *entries = nullptr;
  uint32_t dir_len = BLI_filelist_dir_contents(cache_dir.c_str(), &entries);
  for (int i : blender::IndexRange(dir_len)) {
    direntry entry = entries[i];
    if (S_ISDIR(entry.s.st_mode)) {
      continue;
    }
    const time_t ts_now = time(nullptr);
    const time_t delete_threshold = 60 /*seconds*/ * 60 /*minutes*/ * 24 /*hours*/ * 30 /*days*/;
    if (entry.s.st_mtime + delete_threshold < ts_now) {
      BLI_delete(entry.path, false, false);
    }
  }
  BLI_filelist_free(entries, dir_len);
}
}  // namespace blender::gpu

#endif
