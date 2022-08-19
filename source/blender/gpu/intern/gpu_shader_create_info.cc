/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 *
 * Descriptor type used to define shader structure, resources and interfaces.
 */

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"

#include "GPU_capabilities.h"
#include "GPU_platform.h"
#include "GPU_shader.h"
#include "GPU_texture.h"

#include "gpu_shader_create_info.hh"
#include "gpu_shader_create_info_private.hh"
#include "gpu_shader_dependency_private.h"

#undef GPU_SHADER_INTERFACE_INFO
#undef GPU_SHADER_CREATE_INFO

namespace blender::gpu::shader {

using CreateInfoDictionnary = Map<StringRef, ShaderCreateInfo *>;
using InterfaceDictionnary = Map<StringRef, StageInterfaceInfo *>;

static CreateInfoDictionnary *g_create_infos = nullptr;
static InterfaceDictionnary *g_interfaces = nullptr;

void ShaderCreateInfo::finalize()
{
  if (finalized_) {
    return;
  }
  finalized_ = true;

  Set<StringRefNull> deps_merged;

  validate_vertex_attributes();

  for (auto &info_name : additional_infos_) {
    const ShaderCreateInfo &info = *reinterpret_cast<const ShaderCreateInfo *>(
        gpu_shader_create_info_get(info_name.c_str()));

    /* Recursive. */
    const_cast<ShaderCreateInfo &>(info).finalize();

    interface_names_size_ += info.interface_names_size_;

    vertex_inputs_.extend(info.vertex_inputs_);
    fragment_outputs_.extend(info.fragment_outputs_);
    vertex_out_interfaces_.extend(info.vertex_out_interfaces_);
    geometry_out_interfaces_.extend(info.geometry_out_interfaces_);

    validate_vertex_attributes(&info);

    push_constants_.extend(info.push_constants_);
    defines_.extend(info.defines_);

    batch_resources_.extend(info.batch_resources_);
    pass_resources_.extend(info.pass_resources_);
    typedef_sources_.extend_non_duplicates(info.typedef_sources_);

    if (info.early_fragment_test_) {
      early_fragment_test_ = true;
    }
    if (info.depth_write_ != DepthWrite::ANY) {
      depth_write_ = info.depth_write_;
    }

    validate_merge(info);

    auto assert_no_overlap = [&](const bool test, const StringRefNull error) {
      if (!test) {
        std::cout << name_ << ": Validation failed while merging " << info.name_ << " : ";
        std::cout << error << std::endl;
        BLI_assert(0);
      }
    };

    if (!deps_merged.add(info.name_)) {
      assert_no_overlap(false, "additional info already merged via another info");
    }

    if (info.compute_layout_.local_size_x != -1) {
      assert_no_overlap(compute_layout_.local_size_x == -1, "Compute layout already defined");
      compute_layout_ = info.compute_layout_;
    }

    if (!info.vertex_source_.is_empty()) {
      assert_no_overlap(vertex_source_.is_empty(), "Vertex source already existing");
      vertex_source_ = info.vertex_source_;
    }
    if (!info.geometry_source_.is_empty()) {
      assert_no_overlap(geometry_source_.is_empty(), "Geometry source already existing");
      geometry_source_ = info.geometry_source_;
      geometry_layout_ = info.geometry_layout_;
    }
    if (!info.fragment_source_.is_empty()) {
      assert_no_overlap(fragment_source_.is_empty(), "Fragment source already existing");
      fragment_source_ = info.fragment_source_;
    }
    if (!info.compute_source_.is_empty()) {
      assert_no_overlap(compute_source_.is_empty(), "Compute source already existing");
      compute_source_ = info.compute_source_;
    }
  }

  if (auto_resource_location_) {
    int images = 0, samplers = 0, ubos = 0, ssbos = 0;

    auto set_resource_slot = [&](Resource &res) {
      switch (res.bind_type) {
        case Resource::BindType::UNIFORM_BUFFER:
          res.slot = ubos++;
          break;
        case Resource::BindType::STORAGE_BUFFER:
          res.slot = ssbos++;
          break;
        case Resource::BindType::SAMPLER:
          res.slot = samplers++;
          break;
        case Resource::BindType::IMAGE:
          res.slot = images++;
          break;
      }
    };

    for (auto &res : batch_resources_) {
      set_resource_slot(res);
    }
    for (auto &res : pass_resources_) {
      set_resource_slot(res);
    }
  }
}

std::string ShaderCreateInfo::check_error() const
{
  std::string error;

  /* At least a vertex shader and a fragment shader are required, or only a compute shader. */
  if (this->compute_source_.is_empty()) {
    if (this->vertex_source_.is_empty()) {
      error += "Missing vertex shader in " + this->name_ + ".\n";
    }
    if (this->fragment_source_.is_empty()) {
      error += "Missing fragment shader in " + this->name_ + ".\n";
    }
  }
  else {
    if (!this->vertex_source_.is_empty()) {
      error += "Compute shader has vertex_source_ shader attached in " + this->name_ + ".\n";
    }
    if (!this->geometry_source_.is_empty()) {
      error += "Compute shader has geometry_source_ shader attached in " + this->name_ + ".\n";
    }
    if (!this->fragment_source_.is_empty()) {
      error += "Compute shader has fragment_source_ shader attached in " + this->name_ + ".\n";
    }
  }

  return error;
}

void ShaderCreateInfo::validate_merge(const ShaderCreateInfo &other_info)
{
  if (!auto_resource_location_) {
    /* Check same bind-points usage in OGL. */
    Set<int> images, samplers, ubos, ssbos;

    auto register_resource = [&](const Resource &res) -> bool {
      switch (res.bind_type) {
        case Resource::BindType::UNIFORM_BUFFER:
          return images.add(res.slot);
        case Resource::BindType::STORAGE_BUFFER:
          return samplers.add(res.slot);
        case Resource::BindType::SAMPLER:
          return ubos.add(res.slot);
        case Resource::BindType::IMAGE:
          return ssbos.add(res.slot);
        default:
          return false;
      }
    };

    auto print_error_msg = [&](const Resource &res) {
      std::cout << name_ << ": Validation failed : Overlapping ";

      switch (res.bind_type) {
        case Resource::BindType::UNIFORM_BUFFER:
          std::cout << "Uniform Buffer " << res.uniformbuf.name;
          break;
        case Resource::BindType::STORAGE_BUFFER:
          std::cout << "Storage Buffer " << res.storagebuf.name;
          break;
        case Resource::BindType::SAMPLER:
          std::cout << "Sampler " << res.sampler.name;
          break;
        case Resource::BindType::IMAGE:
          std::cout << "Image " << res.image.name;
          break;
        default:
          std::cout << "Unknown Type";
          break;
      }
      std::cout << " (" << res.slot << ") while merging " << other_info.name_ << std::endl;
    };

    for (auto &res : batch_resources_) {
      if (register_resource(res) == false) {
        print_error_msg(res);
      }
    }

    for (auto &res : pass_resources_) {
      if (register_resource(res) == false) {
        print_error_msg(res);
      }
    }
  }
}

void ShaderCreateInfo::validate_vertex_attributes(const ShaderCreateInfo *other_info)
{
  uint32_t attr_bits = 0;
  for (auto &attr : vertex_inputs_) {
    if (attr.index >= 16 || attr.index < 0) {
      std::cout << name_ << ": \"" << attr.name
                << "\" : Type::MAT3 unsupported as vertex attribute." << std::endl;
      BLI_assert(0);
    }
    if (attr.index >= 16 || attr.index < 0) {
      std::cout << name_ << ": Invalid index for attribute \"" << attr.name << "\"" << std::endl;
      BLI_assert(0);
    }
    uint32_t attr_new = 0;
    if (attr.type == Type::MAT4) {
      for (int i = 0; i < 4; i++) {
        attr_new |= 1 << (attr.index + i);
      }
    }
    else {
      attr_new |= 1 << attr.index;
    }

    if ((attr_bits & attr_new) != 0) {
      std::cout << name_ << ": Attribute \"" << attr.name
                << "\" overlap one or more index from another attribute."
                   " Note that mat4 takes up 4 indices.";
      if (other_info) {
        std::cout << " While merging " << other_info->name_ << std::endl;
      }
      std::cout << std::endl;
      BLI_assert(0);
    }
    attr_bits |= attr_new;
  }
}

}  // namespace blender::gpu::shader

using namespace blender::gpu::shader;

void gpu_shader_create_info_init()
{
  g_create_infos = new CreateInfoDictionnary();
  g_interfaces = new InterfaceDictionnary();

#define GPU_SHADER_INTERFACE_INFO(_interface, _inst_name) \
  auto *ptr_##_interface = new StageInterfaceInfo(#_interface, _inst_name); \
  auto &_interface = *ptr_##_interface; \
  g_interfaces->add_new(#_interface, ptr_##_interface); \
  _interface

#define GPU_SHADER_CREATE_INFO(_info) \
  auto *ptr_##_info = new ShaderCreateInfo(#_info); \
  auto &_info = *ptr_##_info; \
  g_create_infos->add_new(#_info, ptr_##_info); \
  _info

/* Declare, register and construct the infos. */
#include "gpu_shader_create_info_list.hh"

/* Baked shader data appended to create infos. */
/* TODO(jbakker): should call a function with a callback. so we could switch implementations.
 * We cannot compile bf_gpu twice. */
#ifdef GPU_RUNTIME
#  include "gpu_shader_baked.hh"
#endif

  /* WORKAROUND: Replace draw_mesh info with the legacy one for systems that have problems with UBO
   * indexing. */
  if (GPU_type_matches(GPU_DEVICE_INTEL | GPU_DEVICE_INTEL_UHD, GPU_OS_ANY, GPU_DRIVER_ANY) ||
      GPU_type_matches(GPU_DEVICE_ANY, GPU_OS_MAC, GPU_DRIVER_ANY) || GPU_crappy_amd_driver()) {
    draw_modelmat = draw_modelmat_legacy;
  }

  for (ShaderCreateInfo *info : g_create_infos->values()) {
    if (info->do_static_compilation_) {
      info->builtins_ |= gpu_shader_dependency_get_builtins(info->vertex_source_);
      info->builtins_ |= gpu_shader_dependency_get_builtins(info->fragment_source_);
      info->builtins_ |= gpu_shader_dependency_get_builtins(info->geometry_source_);
      info->builtins_ |= gpu_shader_dependency_get_builtins(info->compute_source_);

      /* Automatically amend the create info for ease of use of the debug feature. */
      if ((info->builtins_ & BuiltinBits::USE_DEBUG_DRAW) == BuiltinBits::USE_DEBUG_DRAW) {
        info->additional_info("draw_debug_draw");
      }
      if ((info->builtins_ & BuiltinBits::USE_DEBUG_PRINT) == BuiltinBits::USE_DEBUG_PRINT) {
        info->additional_info("draw_debug_print");
      }
    }
  }

  /* TEST */
  // gpu_shader_create_info_compile_all();
}

void gpu_shader_create_info_exit()
{
  for (auto *value : g_create_infos->values()) {
    delete value;
  }
  delete g_create_infos;

  for (auto *value : g_interfaces->values()) {
    delete value;
  }
  delete g_interfaces;
}

bool gpu_shader_create_info_compile_all()
{
  using namespace blender::gpu;
  int success = 0;
  int skipped = 0;
  int total = 0;
  for (ShaderCreateInfo *info : g_create_infos->values()) {
    info->finalize();
    if (info->do_static_compilation_) {
      if ((GPU_compute_shader_support() == false && info->compute_source_ != nullptr) ||
          (GPU_shader_image_load_store_support() == false && info->has_resource_image()) ||
          (GPU_shader_storage_buffer_objects_support() == false && info->has_resource_storage())) {
        skipped++;
        continue;
      }
      total++;
      GPUShader *shader = GPU_shader_create_from_info(
          reinterpret_cast<const GPUShaderCreateInfo *>(info));
      if (shader == nullptr) {
        printf("Compilation %s Failed\n", info->name_.c_str());
      }
      else {
        success++;

#if 0 /* TODO(fclem): This is too verbose for now. Make it a cmake option. */
        /* Test if any resource is optimized out and print a warning if that's the case. */
        /* TODO(fclem): Limit this to OpenGL backend. */
        const ShaderInterface *interface = unwrap(shader)->interface;

        blender::Vector<ShaderCreateInfo::Resource> all_resources;
        all_resources.extend(info->pass_resources_);
        all_resources.extend(info->batch_resources_);

        for (ShaderCreateInfo::Resource &res : all_resources) {
          blender::StringRefNull name = "";
          const ShaderInput *input = nullptr;

          switch (res.bind_type) {
            case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
              input = interface->ubo_get(res.slot);
              name = res.uniformbuf.name;
              break;
            case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
              input = interface->ssbo_get(res.slot);
              name = res.storagebuf.name;
              break;
            case ShaderCreateInfo::Resource::BindType::SAMPLER:
              input = interface->texture_get(res.slot);
              name = res.sampler.name;
              break;
            case ShaderCreateInfo::Resource::BindType::IMAGE:
              input = interface->texture_get(res.slot);
              name = res.image.name;
              break;
          }

          if (input == nullptr) {
            std::cout << "Error: " << info->name_;
            std::cout << ": Resource « " << name << " » not found in the shader interface\n";
          }
          else if (input->location == -1) {
            std::cout << "Warning: " << info->name_;
            std::cout << ": Resource « " << name << " » is optimized out\n";
          }
        }
#endif
      }
      GPU_shader_free(shader);
    }
  }
  printf("Shader Test compilation result: %d / %d passed", success, total);
  if (skipped > 0) {
    printf(" (skipped %d for compatibility reasons)", skipped);
  }
  printf("\n");
  return success == total;
}

/* Runtime create infos are not registered in the dictionary and cannot be searched. */
const GPUShaderCreateInfo *gpu_shader_create_info_get(const char *info_name)
{
  if (g_create_infos->contains(info_name) == false) {
    printf("Error: Cannot find shader create info named \"%s\"\n", info_name);
    return nullptr;
  }
  ShaderCreateInfo *info = g_create_infos->lookup(info_name);
  return reinterpret_cast<const GPUShaderCreateInfo *>(info);
}
