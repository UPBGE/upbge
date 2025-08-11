/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.hh"

#include "DNA_userdef_types.h"

#include "BLI_string.h"
#include "BLI_time.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>

#include <cstring>

#include "GPU_platform.hh"
#include "GPU_vertex_format.hh"

#include "gpu_shader_dependency_private.hh"
#include "mtl_common.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_pso_descriptor_state.hh"
#include "mtl_shader.hh"
#include "mtl_shader_generator.hh"
#include "mtl_shader_interface.hh"
#include "mtl_shader_log.hh"
#include "mtl_texture.hh"
#include "mtl_vertex_buffer.hh"

#include "GHOST_C-api.h"

extern const char datatoc_mtl_shader_common_msl[];

using namespace blender;
using namespace blender::gpu;
using namespace blender::gpu::shader;

namespace blender::gpu {

const char *to_string(ShaderStage stage)
{
  switch (stage) {
    case ShaderStage::VERTEX:
      return "Vertex Shader";
    case ShaderStage::FRAGMENT:
      return "Fragment Shader";
    case ShaderStage::COMPUTE:
      return "Compute Shader";
    case ShaderStage::ANY:
      break;
  }
  return "Unknown Shader Stage";
}

/* -------------------------------------------------------------------- */
/** \name Creation / Destruction.
 * \{ */

/* Create empty shader to be populated later. */
MTLShader::MTLShader(MTLContext *ctx, const char *name) : Shader(name)
{
  context_ = ctx;

  /* Create SHD builder to hold temporary resources until compilation is complete. */
  shd_builder_ = new MTLShaderBuilder();

#ifndef NDEBUG
  /* Remove invalid symbols from shader name to ensure debug entry-point function name is valid. */
  for (uint i : IndexRange(strlen(this->name))) {
    char c = this->name[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
    }
    else {
      this->name[i] = '_';
    }
  }
#endif
}

/* Create shader from MSL source. */
MTLShader::MTLShader(MTLContext *ctx,
                     MTLShaderInterface *interface,
                     const char *name,
                     NSString *input_vertex_source,
                     NSString *input_fragment_source,
                     NSString *vert_function_name,
                     NSString *frag_function_name)
    : MTLShader(ctx, name)
{
  BLI_assert([vert_function_name length]);
  BLI_assert([frag_function_name length]);

  this->set_vertex_function_name(vert_function_name);
  this->set_fragment_function_name(frag_function_name);
  this->shader_source_from_msl(input_vertex_source, input_fragment_source);
  this->set_interface(interface);
  this->finalize(nullptr);
}

MTLShader::~MTLShader()
{
  if (this->is_valid()) {

    /* Free uniform data block. */
    if (push_constant_data_ != nullptr) {
      MEM_freeN(push_constant_data_);
      push_constant_data_ = nullptr;
    }

    /* Free Metal resources.
     * This is done in the order of:
     * 1. PipelineState objects
     * 2. MTLFunctions
     * 3. MTLLibraries
     * So that each object releases it's references to the one following it. */
    if (pso_descriptor_ != nil) {
      [pso_descriptor_ release];
      pso_descriptor_ = nil;
    }

    /* Free Pipeline Cache. */
    pso_cache_lock_.lock();
    for (const MTLRenderPipelineStateInstance *pso_inst : pso_cache_.values()) {
      /* Free pipeline state object. */
      if (pso_inst->pso) {
        [pso_inst->pso release];
      }
      /* Free vertex function. */
      if (pso_inst->vert) {
        [pso_inst->vert release];
      }
      /* Free fragment function. */
      if (pso_inst->frag) {
        [pso_inst->frag release];
      }
      delete pso_inst;
    }
    pso_cache_.clear();

    /* Free Compute pipeline cache. */
    for (const MTLComputePipelineStateInstance *pso_inst : compute_pso_cache_.values()) {
      /* Free pipeline state object. */
      if (pso_inst->pso) {
        [pso_inst->pso release];
      }
      /* Free compute function. */
      if (pso_inst->compute) {
        [pso_inst->compute release];
      }
    }
    compute_pso_cache_.clear();
    pso_cache_lock_.unlock();

    /* Free shader libraries. */
    if (shader_library_vert_ != nil) {
      [shader_library_vert_ release];
      shader_library_vert_ = nil;
    }
    if (shader_library_frag_ != nil) {
      [shader_library_frag_ release];
      shader_library_frag_ = nil;
    }
    if (shader_library_compute_ != nil) {
      [shader_library_compute_ release];
      shader_library_compute_ = nil;
    }

    /* NOTE(Metal): #ShaderInterface deletion is handled in the super destructor `~Shader()`. */
  }
  valid_ = false;

  if (shd_builder_ != nullptr) {
    delete shd_builder_;
    shd_builder_ = nullptr;
  }
}

void MTLShader::init(const shader::ShaderCreateInfo & /*info*/, bool is_batch_compilation)
{
  async_compilation_ = is_batch_compilation;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader stage creation.
 * \{ */

void MTLShader::vertex_shader_from_glsl(MutableSpan<StringRefNull> sources)
{
  /* Flag source as not being compiled from native MSL. */
  BLI_assert(shd_builder_ != nullptr);
  shd_builder_->source_from_msl_ = false;

  /* Remove #version tag entry. */
  sources[SOURCES_INDEX_VERSION] = "";

  /* Consolidate GLSL vertex sources. */
  std::stringstream ss;
  for (int i = 0; i < sources.size(); i++) {
    ss << sources[i] << std::endl;
  }
  shd_builder_->glsl_vertex_source_ = ss.str();
}

void MTLShader::geometry_shader_from_glsl(MutableSpan<StringRefNull> /*sources*/)
{
  MTL_LOG_ERROR("MTLShader::geometry_shader_from_glsl - Geometry shaders unsupported!");
}

void MTLShader::fragment_shader_from_glsl(MutableSpan<StringRefNull> sources)
{
  /* Flag source as not being compiled from native MSL. */
  BLI_assert(shd_builder_ != nullptr);
  shd_builder_->source_from_msl_ = false;

  /* Remove #version tag entry. */
  sources[SOURCES_INDEX_VERSION] = "";

  /* Consolidate GLSL fragment sources. */
  std::stringstream ss;
  int i;
  for (i = 0; i < sources.size(); i++) {
    ss << sources[i] << '\n';
  }
  shd_builder_->glsl_fragment_source_ = ss.str();
}

void MTLShader::compute_shader_from_glsl(MutableSpan<StringRefNull> sources)
{
  /* Flag source as not being compiled from native MSL. */
  BLI_assert(shd_builder_ != nullptr);
  shd_builder_->source_from_msl_ = false;

  /* Remove #version tag entry. */
  sources[SOURCES_INDEX_VERSION] = "";

  /* Consolidate GLSL compute sources. */
  std::stringstream ss;
  for (int i = 0; i < sources.size(); i++) {
    ss << sources[i] << std::endl;
  }
  shd_builder_->glsl_compute_source_ = ss.str();
}

bool MTLShader::finalize(const shader::ShaderCreateInfo *info)
{
  /* Check if Shader has already been finalized. */
  if (this->is_valid()) {
    MTL_LOG_ERROR("Shader (%p) '%s' has already been finalized!", this, this->name_get().c_str());
  }

  /* Compute shaders. */
  bool is_compute = false;
  if (shd_builder_->glsl_compute_source_.empty() == false) {
    BLI_assert_msg(info != nullptr, "Compute shaders must use CreateInfo.\n");
    BLI_assert_msg(!shd_builder_->source_from_msl_, "Compute shaders must compile from GLSL.");
    is_compute = true;
  }

  /* Perform GLSL to MSL source translation. */
  BLI_assert(shd_builder_ != nullptr);
  if (!shd_builder_->source_from_msl_) {
    bool success = generate_msl_from_glsl(info);
    if (!success) {
      /* GLSL to MSL translation has failed, or is unsupported for this shader. */
      valid_ = false;
      BLI_assert_msg(false, "Shader translation from GLSL to MSL has failed. \n");

      /* Create empty interface to allow shader to be silently used. */
      MTLShaderInterface *mtl_interface = new MTLShaderInterface(this->name_get().c_str());
      this->set_interface(mtl_interface);

      /* Release temporary compilation resources. */
      delete shd_builder_;
      shd_builder_ = nullptr;
      return false;
    }
  }

  /** Extract desired custom parameters from CreateInfo. */
  /* Tuning parameters for compute kernels. */
  if (is_compute) {
    int threadgroup_tuning_param = info->mtl_max_threads_per_threadgroup_;
    if (threadgroup_tuning_param > 0) {
      maxTotalThreadsPerThreadgroup_Tuning_ = threadgroup_tuning_param;
    }
  }

  /* Ensure we have a valid shader interface. */
  MTLShaderInterface *mtl_interface = this->get_interface();
  BLI_assert(mtl_interface != nullptr);

  /* Verify Context handle, fetch device and compile shader. */
  BLI_assert(context_);
  id<MTLDevice> device = context_->device;
  BLI_assert(device != nil);

  /* Ensure source and stage entry-point names are set. */
  BLI_assert(shd_builder_ != nullptr);
  if (is_compute) {
    /* Compute path. */
    BLI_assert([compute_function_name_ length] > 0);
    BLI_assert([shd_builder_->msl_source_compute_ length] > 0);
  }
  else {
    /* Vertex/Fragment path. */
    BLI_assert([vertex_function_name_ length] > 0);
    BLI_assert([fragment_function_name_ length] > 0);
    BLI_assert([shd_builder_->msl_source_vert_ length] > 0);
  }

  @autoreleasepool {
    MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
    options.languageVersion = MTLLanguageVersion2_2;
    options.fastMathEnabled = YES;
    options.preserveInvariance = YES;

    /* Raster order groups for tile data in struct require Metal 2.3.
     * Retaining Metal 2.2. for old shaders to maintain backwards
     * compatibility for existing features. */
    if (info->subpass_inputs_.is_empty() == false) {
      options.languageVersion = MTLLanguageVersion2_3;
    }
#if defined(MAC_OS_VERSION_14_0)
    if (@available(macOS 14.00, *)) {
      /* Texture atomics require Metal 3.1. */
      if (bool(info->builtins_ & BuiltinBits::TEXTURE_ATOMIC)) {
        options.languageVersion = MTLLanguageVersion3_1;
      }
    }
#endif

    NSString *source_to_compile = shd_builder_->msl_source_vert_;

    /* Vertex/Fragment compile stages 0 and/or 1.
     * Compute shaders compile as stage 2. */
    ShaderStage initial_stage = (is_compute) ? ShaderStage::COMPUTE : ShaderStage::VERTEX;
    ShaderStage src_stage = initial_stage;
    uint8_t total_stages = (is_compute) ? 1 : 2;

    for (int stage_count = 0; stage_count < total_stages; stage_count++) {

      source_to_compile = (src_stage == ShaderStage::VERTEX) ?
                              shd_builder_->msl_source_vert_ :
                              ((src_stage == ShaderStage::COMPUTE) ?
                                   shd_builder_->msl_source_compute_ :
                                   shd_builder_->msl_source_frag_);

      /* Concatenate common source. */
      NSString *str = [NSString stringWithUTF8String:datatoc_mtl_shader_common_msl];
      NSString *source_with_header_a = [str stringByAppendingString:source_to_compile];

      /* Inject unique context ID to avoid cross-context shader cache collisions.
       * Required on macOS 11.0. */
      NSString *source_with_header = source_with_header_a;
      [source_with_header retain];

      /* Prepare Shader Library. */
      NSError *error = nullptr;
      id<MTLLibrary> library = [device newLibraryWithSource:source_with_header
                                                    options:options
                                                      error:&error];
      if (error) {
        /* Only exit out if genuine error and not warning. */
        if ([[error localizedDescription] rangeOfString:@"Compilation succeeded"].location ==
            NSNotFound)
        {
          const char *errors_c_str = [[error localizedDescription] UTF8String];
          const StringRefNull source = (is_compute) ? shd_builder_->glsl_compute_source_ :
                                                      shd_builder_->glsl_fragment_source_;

          MTLLogParser parser;
          print_log({source}, errors_c_str, to_string(src_stage), true, &parser);

          /* Release temporary compilation resources. */
          delete shd_builder_;
          shd_builder_ = nullptr;
          return false;
        }
      }

      BLI_assert(library != nil);

      switch (src_stage) {
        case ShaderStage::VERTEX: {
          /* Store generated library and assign debug name. */
          shader_library_vert_ = library;
          shader_library_vert_.label = [NSString stringWithUTF8String:this->name];
        } break;
        case ShaderStage::FRAGMENT: {
          /* Store generated library for fragment shader and assign debug name. */
          shader_library_frag_ = library;
          shader_library_frag_.label = [NSString stringWithUTF8String:this->name];
        } break;
        case ShaderStage::COMPUTE: {
          /* Store generated library for fragment shader and assign debug name. */
          shader_library_compute_ = library;
          shader_library_compute_.label = [NSString stringWithUTF8String:this->name];
        } break;
        case ShaderStage::ANY: {
          /* Suppress warnings. */
          BLI_assert_unreachable();
        } break;
      }

      [source_with_header autorelease];

      /* Move onto next compilation stage. */
      if (!is_compute) {
        src_stage = ShaderStage::FRAGMENT;
      }
      else {
        break;
      }
    }

    /* Create descriptors.
     * Each shader type requires a differing descriptor. */
    if (!is_compute) {
      /* Prepare Render pipeline descriptor. */
      pso_descriptor_ = [[MTLRenderPipelineDescriptor alloc] init];
      pso_descriptor_.label = [NSString stringWithUTF8String:this->name];
    }

    /* Shader has successfully been created. */
    valid_ = true;

    /* Prepare backing data storage for local uniforms. */
    const MTLShaderBufferBlock &push_constant_block = mtl_interface->get_push_constant_block();
    if (push_constant_block.size > 0) {
      push_constant_data_ = MEM_callocN(push_constant_block.size, __func__);
      this->push_constant_bindstate_mark_dirty(true);
    }
    else {
      push_constant_data_ = nullptr;
    }

    /* If this is a compute shader, bake base PSO for compute straight-away.
     * NOTE: This will compile the base unspecialized variant. */
    if (is_compute) {
      /* Set descriptor to default shader constants */
      MTLComputePipelineStateDescriptor compute_pipeline_descriptor(this->constants->values);

      this->bake_compute_pipeline_state(context_, compute_pipeline_descriptor);
    }
  }

  /* Release temporary compilation resources. */
  delete shd_builder_;
  shd_builder_ = nullptr;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Binding.
 * \{ */

void MTLShader::bind(const shader::SpecializationConstants *constants_state)
{
  MTLContext *ctx = MTLContext::get();
  /* Copy constants state. */
  ctx->specialization_constants_set(constants_state);

  if (interface == nullptr || !this->is_valid()) {
    MTL_LOG_WARNING(
        "MTLShader::bind - Shader '%s' has no valid implementation in Metal, draw calls will be "
        "skipped.",
        this->name_get().c_str());
  }
  ctx->pipeline_state.active_shader = this;
}

void MTLShader::unbind()
{
  MTLContext *ctx = MTLContext::get();
  ctx->pipeline_state.active_shader = nullptr;
}

void MTLShader::uniform_float(int location, int comp_len, int array_size, const float *data)
{
  BLI_assert(this);
  if (!this->is_valid()) {
    return;
  }
  MTLShaderInterface *mtl_interface = get_interface();
  if (location < 0 || location >= mtl_interface->get_total_uniforms()) {
    MTL_LOG_WARNING(
        "Uniform location %d is not valid in Shader %s", location, this->name_get().c_str());
    return;
  }

  /* Fetch more information about uniform from interface. */
  const MTLShaderUniform &uniform = mtl_interface->get_uniform(location);

  /* Prepare to copy data into local shader push constant memory block. */
  BLI_assert(push_constant_data_ != nullptr);
  uint8_t *dest_ptr = (uint8_t *)push_constant_data_;
  dest_ptr += uniform.byte_offset;
  uint32_t copy_size = sizeof(float) * comp_len * array_size;

  /* Test per-element size. It is valid to copy less array elements than the total, but each
   * array element needs to match. */
  uint32_t source_per_element_size = sizeof(float) * comp_len;
  uint32_t dest_per_element_size = uniform.size_in_bytes / uniform.array_len;
  BLI_assert_msg(
      source_per_element_size <= dest_per_element_size,
      "source Per-array-element size must be smaller than destination storage capacity for "
      "that data");

  if (source_per_element_size < dest_per_element_size) {
    switch (uniform.type) {

      /* Special case for handling 'vec3' array upload. */
      case MTL_DATATYPE_FLOAT3: {
        int numvecs = uniform.array_len;
        uint8_t *data_c = (uint8_t *)data;

        /* It is more efficient on the host to only modify data if it has changed.
         * Data modifications are small, so memory comparison is cheap.
         * If uniforms have remained unchanged, then we avoid both copying
         * data into the local uniform struct, and upload of the modified uniform
         * contents in the command stream. */
        bool changed = false;
        for (int i = 0; i < numvecs; i++) {
          changed = changed || (memcmp((void *)dest_ptr, (void *)data_c, sizeof(float) * 3) != 0);
          if (changed) {
            memcpy((void *)dest_ptr, (void *)data_c, sizeof(float) * 3);
          }
          data_c += sizeof(float) * 3;
          dest_ptr += sizeof(float) * 4;
        }
        if (changed) {
          this->push_constant_bindstate_mark_dirty(true);
        }
        return;
      }

      /* Special case for handling 'mat3' upload. */
      case MTL_DATATYPE_FLOAT3x3: {
        int numvecs = 3 * uniform.array_len;
        uint8_t *data_c = (uint8_t *)data;

        /* It is more efficient on the host to only modify data if it has changed.
         * Data modifications are small, so memory comparison is cheap.
         * If uniforms have remained unchanged, then we avoid both copying
         * data into the local uniform struct, and upload of the modified uniform
         * contents in the command stream. */
        bool changed = false;
        for (int i = 0; i < numvecs; i++) {
          changed = changed || (memcmp((void *)dest_ptr, (void *)data_c, sizeof(float) * 3) != 0);
          if (changed) {
            memcpy((void *)dest_ptr, (void *)data_c, sizeof(float) * 3);
          }
          data_c += sizeof(float) * 3;
          dest_ptr += sizeof(float) * 4;
        }
        if (changed) {
          this->push_constant_bindstate_mark_dirty(true);
        }
        return;
      }
      default:
        shader_debug_printf("INCOMPATIBLE UNIFORM TYPE: %d\n", uniform.type);
        break;
    }
  }

  /* Debug checks. */
  BLI_assert_msg(
      copy_size <= uniform.size_in_bytes,
      "Size of provided uniform data is greater than size specified in Shader interface\n");

  /* Only flag UBO as modified if data is different -- This can avoid re-binding of unmodified
   * local uniform data. */
  bool data_changed = (memcmp((void *)dest_ptr, (void *)data, copy_size) != 0);
  if (data_changed) {
    this->push_constant_bindstate_mark_dirty(true);
    memcpy((void *)dest_ptr, (void *)data, copy_size);
  }
}

void MTLShader::uniform_int(int location, int comp_len, int array_size, const int *data)
{
  BLI_assert(this);
  if (!this->is_valid()) {
    return;
  }

  /* NOTE(Metal): Invalidation warning for uniform re-mapping of texture slots, unsupported in
   * Metal, as we cannot point a texture binding at a different slot. */
  MTLShaderInterface *mtl_interface = this->get_interface();
  if (location >= mtl_interface->get_total_uniforms() &&
      location < (mtl_interface->get_total_uniforms() + mtl_interface->get_total_textures()))
  {
    MTL_LOG_WARNING(
        "Texture uniform location re-mapping unsupported in Metal. (Possibly also bad uniform "
        "location %d)",
        location);
    return;
  }

  if (location < 0 || location >= mtl_interface->get_total_uniforms()) {
    MTL_LOG_WARNING(
        "Uniform is not valid at location %d - Shader %s", location, this->name_get().c_str());
    return;
  }

  /* Fetch more information about uniform from interface. */
  const MTLShaderUniform &uniform = mtl_interface->get_uniform(location);

  /* Determine data location in uniform block. */
  BLI_assert(push_constant_data_ != nullptr);
  uint8_t *ptr = (uint8_t *)push_constant_data_;
  ptr += uniform.byte_offset;

  /** Determine size of data to copy. */
  const char *data_to_copy = (char *)data;
  uint data_size_to_copy = sizeof(int) * comp_len * array_size;

  /* Special cases for small types support where storage is shader push constant buffer is smaller
   * than the incoming data. */
  ushort us;
  uchar uc;
  if (uniform.size_in_bytes == 1) {
    /* Convert integer storage value down to uchar. */
    data_size_to_copy = uniform.size_in_bytes;
    uc = *data;
    data_to_copy = (char *)&uc;
  }
  else if (uniform.size_in_bytes == 2) {
    /* Convert integer storage value down to ushort. */
    data_size_to_copy = uniform.size_in_bytes;
    us = *data;
    data_to_copy = (char *)&us;
  }
  else {
    BLI_assert_msg(
        (mtl_get_data_type_alignment(uniform.type) % sizeof(int)) == 0,
        "When uniform inputs are provided as integers, the underlying type must adhere "
        "to alignment per-component. If this test fails, the input data cannot be directly copied "
        "to the buffer. e.g. Array of small types uchar/bool/ushort etc; are currently not "
        "handled.");
  }

  /* Copy data into local block. Only flag UBO as modified if data is different
   * This can avoid re-binding of unmodified local uniform data, reducing
   * the total number of copy operations needed and data transfers between
   * CPU and GPU. */
  bool data_changed = (memcmp((void *)ptr, (void *)data_to_copy, data_size_to_copy) != 0);
  if (data_changed) {
    this->push_constant_bindstate_mark_dirty(true);
    memcpy((void *)ptr, (void *)data_to_copy, data_size_to_copy);
  }
}

bool MTLShader::get_push_constant_is_dirty()
{
  return push_constant_modified_;
}

void MTLShader::push_constant_bindstate_mark_dirty(bool is_dirty)
{
  push_constant_modified_ = is_dirty;
}

/* Attempts to pre-generate a PSO based on the parent shaders PSO
 * (Render shaders only) */
void MTLShader::warm_cache(int limit)
{
  if (parent_shader_ != nullptr) {
    MTLContext *ctx = MTLContext::get();
    MTLShader *parent_mtl = static_cast<MTLShader *>(parent_shader_);

    /* Extract PSO descriptors from parent shader. */
    blender::Vector<MTLRenderPipelineStateDescriptor> descriptors;
    blender::Vector<MTLPrimitiveTopologyClass> prim_classes;

    parent_mtl->pso_cache_lock_.lock();
    for (const auto &pso_entry : parent_mtl->pso_cache_.items()) {
      const MTLRenderPipelineStateDescriptor &pso_descriptor = pso_entry.key;
      const MTLRenderPipelineStateInstance *pso_inst = pso_entry.value;
      descriptors.append(pso_descriptor);
      prim_classes.append(pso_inst->prim_type);
    }
    parent_mtl->pso_cache_lock_.unlock();

    /* Warm shader cache with applied limit.
     * If limit is <= 0, compile all PSO permutations. */
    limit = (limit > 0) ? limit : descriptors.size();
    for (int i : IndexRange(min_ii(descriptors.size(), limit))) {
      const MTLRenderPipelineStateDescriptor &pso_descriptor = descriptors[i];
      const MTLPrimitiveTopologyClass &prim_class = prim_classes[i];
      bake_pipeline_state(ctx, prim_class, pso_descriptor);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name METAL Custom Behavior
 * \{ */

void MTLShader::set_vertex_function_name(NSString *vert_function_name)
{
  vertex_function_name_ = vert_function_name;
}

void MTLShader::set_fragment_function_name(NSString *frag_function_name)
{
  fragment_function_name_ = frag_function_name;
}

void MTLShader::set_compute_function_name(NSString *compute_function_name)
{
  compute_function_name_ = compute_function_name;
}

void MTLShader::shader_source_from_msl(NSString *input_vertex_source,
                                       NSString *input_fragment_source)
{
  BLI_assert(shd_builder_ != nullptr);
  shd_builder_->msl_source_vert_ = input_vertex_source;
  shd_builder_->msl_source_frag_ = input_fragment_source;
  shd_builder_->source_from_msl_ = true;
}

void MTLShader::shader_compute_source_from_msl(NSString *input_compute_source)
{
  BLI_assert(shd_builder_ != nullptr);
  shd_builder_->msl_source_compute_ = input_compute_source;
  shd_builder_->source_from_msl_ = true;
}

void MTLShader::set_interface(MTLShaderInterface *interface)
{
  /* Assign gpu::Shader super-class interface. */
  BLI_assert(Shader::interface == nullptr);
  Shader::interface = interface;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader specialization common utilities.
 *
 * \{ */

/**
 * Populates `values` with the given `SpecializationStateDescriptor` values.
 */
static void populate_specialization_constant_values(
    MTLFunctionConstantValues *values,
    const shader::SpecializationConstants &shader_constants,
    const SpecializationStateDescriptor &specialization_descriptor)
{
  for (auto i : shader_constants.types.index_range()) {
    const shader::SpecializationConstant::Value &value = specialization_descriptor.values[i];

    uint index = i + MTL_SHADER_SPECIALIZATION_CONSTANT_BASE_ID;
    switch (shader_constants.types[i]) {
      case Type::int_t:
        [values setConstantValue:&value.i type:MTLDataTypeInt atIndex:index];
        break;
      case Type::uint_t:
        [values setConstantValue:&value.u type:MTLDataTypeUInt atIndex:index];
        break;
      case Type::bool_t:
        [values setConstantValue:&value.u type:MTLDataTypeBool atIndex:index];
        break;
      case Type::float_t:
        [values setConstantValue:&value.f type:MTLDataTypeFloat atIndex:index];
        break;
      default:
        BLI_assert_msg(false, "Unsupported custom constant type.");
        break;
    }
  }
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake Pipeline State Objects
 * \{ */

/**
 * Bakes or fetches a pipeline state using the current
 * #MTLRenderPipelineStateDescriptor state.
 *
 * This state contains information on shader inputs/outputs, such
 * as the vertex descriptor, used to control vertex assembly for
 * current vertex data, and active render target information,
 * describing the output attachment pixel formats.
 *
 * Other rendering parameters such as global point-size, blend state, color mask
 * etc; are also used. See mtl_shader.h for full #MLRenderPipelineStateDescriptor.
 */
MTLRenderPipelineStateInstance *MTLShader::bake_current_pipeline_state(
    MTLContext *ctx, MTLPrimitiveTopologyClass prim_type)
{
  /** Populate global pipeline descriptor and use this to prepare new PSO. */
  /* NOTE(Metal): PSO cache can be accessed from multiple threads, though these operations should
   * be thread-safe due to organization of high-level renderer. If there are any issues, then
   * access can be guarded as appropriate. */
  BLI_assert(this->is_valid());

  /* NOTE(Metal): Vertex input assembly description will have been populated externally
   * via #MTLBatch or #MTLImmediate during binding or draw. */

  /* Resolve Context Frame-buffer state. */
  MTLFrameBuffer *framebuffer = ctx->get_current_framebuffer();

  /* Update global pipeline descriptor. */
  MTLStateManager *state_manager = static_cast<MTLStateManager *>(
      MTLContext::get()->state_manager);
  MTLRenderPipelineStateDescriptor &pipeline_descriptor = state_manager->get_pipeline_descriptor();

  pipeline_descriptor.num_color_attachments = 0;
  for (int attachment = 0; attachment < GPU_FB_MAX_COLOR_ATTACHMENT; attachment++) {
    MTLAttachment color_attachment = framebuffer->get_color_attachment(attachment);

    if (color_attachment.used) {
      /* If SRGB is disabled and format is SRGB, use color data directly with no conversions
       * between linear and SRGB. */
      MTLPixelFormat mtl_format = gpu_texture_format_to_metal(
          color_attachment.texture->format_get());
      if (framebuffer->get_is_srgb() && !framebuffer->get_srgb_enabled()) {
        mtl_format = MTLPixelFormatRGBA8Unorm;
      }
      pipeline_descriptor.color_attachment_format[attachment] = mtl_format;
    }
    else {
      pipeline_descriptor.color_attachment_format[attachment] = MTLPixelFormatInvalid;
    }

    pipeline_descriptor.num_color_attachments += (color_attachment.used) ? 1 : 0;
  }
  MTLAttachment depth_attachment = framebuffer->get_depth_attachment();
  MTLAttachment stencil_attachment = framebuffer->get_stencil_attachment();
  pipeline_descriptor.depth_attachment_format = (depth_attachment.used) ?
                                                    gpu_texture_format_to_metal(
                                                        depth_attachment.texture->format_get()) :
                                                    MTLPixelFormatInvalid;
  pipeline_descriptor.stencil_attachment_format =
      (stencil_attachment.used) ?
          gpu_texture_format_to_metal(stencil_attachment.texture->format_get()) :
          MTLPixelFormatInvalid;

  /* Resolve Context Pipeline State (required by PSO). */
  pipeline_descriptor.color_write_mask = ctx->pipeline_state.color_write_mask;
  pipeline_descriptor.blending_enabled = ctx->pipeline_state.blending_enabled;
  pipeline_descriptor.alpha_blend_op = ctx->pipeline_state.alpha_blend_op;
  pipeline_descriptor.rgb_blend_op = ctx->pipeline_state.rgb_blend_op;
  pipeline_descriptor.dest_alpha_blend_factor = ctx->pipeline_state.dest_alpha_blend_factor;
  pipeline_descriptor.dest_rgb_blend_factor = ctx->pipeline_state.dest_rgb_blend_factor;
  pipeline_descriptor.src_alpha_blend_factor = ctx->pipeline_state.src_alpha_blend_factor;
  pipeline_descriptor.src_rgb_blend_factor = ctx->pipeline_state.src_rgb_blend_factor;
  pipeline_descriptor.point_size = ctx->pipeline_state.point_size;

  /* Resolve clipping plane enablement. */
  pipeline_descriptor.clipping_plane_enable_mask = 0;
  for (const int plane : IndexRange(6)) {
    pipeline_descriptor.clipping_plane_enable_mask =
        pipeline_descriptor.clipping_plane_enable_mask |
        ((ctx->pipeline_state.clip_distance_enabled[plane]) ? (1 << plane) : 0);
  }

  /* Primitive Type -- Primitive topology class needs to be specified for layered rendering. */
  bool requires_specific_topology_class = uses_gpu_layer || uses_gpu_viewport_index ||
                                          prim_type == MTLPrimitiveTopologyClassPoint;
  pipeline_descriptor.vertex_descriptor.prim_topology_class =
      (requires_specific_topology_class) ? prim_type : MTLPrimitiveTopologyClassUnspecified;

  /* Specialization configuration. */
  pipeline_descriptor.specialization_state = {ctx->constants_state.values};

  /* Bake pipeline state using global descriptor. */
  return bake_pipeline_state(ctx, prim_type, pipeline_descriptor);
}

/* Variant which bakes a pipeline state based on an existing MTLRenderPipelineStateDescriptor.
 * This function should be callable from a secondary compilation thread. */
MTLRenderPipelineStateInstance *MTLShader::bake_pipeline_state(
    MTLContext *ctx,
    MTLPrimitiveTopologyClass prim_type,
    const MTLRenderPipelineStateDescriptor &pipeline_descriptor)
{
  /* Fetch shader interface. */
  MTLShaderInterface *mtl_interface = this->get_interface();
  BLI_assert(mtl_interface);
  BLI_assert(this->is_valid());

  /* Check if current PSO exists in the cache. */
  pso_cache_lock_.lock();
  MTLRenderPipelineStateInstance **pso_lookup = pso_cache_.lookup_ptr(pipeline_descriptor);
  MTLRenderPipelineStateInstance *pipeline_state = (pso_lookup) ? *pso_lookup : nullptr;
  pso_cache_lock_.unlock();

  if (pipeline_state != nullptr) {
    return pipeline_state;
  }

  /* TODO: When fetching a specialized variant of a shader, if this does not yet exist, verify
   * whether the base unspecialized variant exists:
   * - If unspecialized version exists: Compile specialized PSO asynchronously, returning base PSO
   * and flagging state of specialization in cache as being built.
   * - If unspecialized does NOT exist, build specialized version straight away, as we pay the
   * cost of compilation in both cases regardless. */

  /* Generate new Render Pipeline State Object (PSO). */
  @autoreleasepool {
    /* Prepare Render Pipeline Descriptor. */

    /* Setup function specialization constants, used to modify and optimize
     * generated code based on current render pipeline configuration. */
    MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];

    /* Custom function constant values: */
    populate_specialization_constant_values(
        values, *this->constants, pipeline_descriptor.specialization_state);

    /* Prepare Vertex descriptor based on current pipeline vertex binding state. */
    MTLRenderPipelineDescriptor *desc = pso_descriptor_;
    [desc reset];
    pso_descriptor_.label = [NSString stringWithUTF8String:this->name];

    /* Offset the bind index for Uniform buffers such that they begin after the VBO
     * buffer bind slots. `MTL_uniform_buffer_base_index` is passed as a function
     * specialization constant, customized per unique pipeline state permutation.
     *
     * NOTE: For binding point compaction, we could use the number of VBOs present
     * in the current PSO configuration `pipeline_descriptors.vertex_descriptor.num_vert_buffers`).
     * However, it is more efficient to simply offset the uniform buffer base index to the
     * maximal number of VBO bind-points, as then UBO bind-points for similar draw calls
     * will align and avoid the requirement for additional binding. */
    int MTL_uniform_buffer_base_index = pipeline_descriptor.vertex_descriptor.num_vert_buffers + 1;

    /* Null buffer index is used if an attribute is not found in the
     * bound VBOs #VertexFormat. */
    int null_buffer_index = pipeline_descriptor.vertex_descriptor.num_vert_buffers;
    bool using_null_buffer = false;

    {
      for (const uint i :
           IndexRange(pipeline_descriptor.vertex_descriptor.max_attribute_value + 1))
      {

        /* Metal back-end attribute descriptor state. */
        const MTLVertexAttributeDescriptorPSO &attribute_desc =
            pipeline_descriptor.vertex_descriptor.attributes[i];

        /* Flag format conversion */
        /* In some cases, Metal cannot implicitly convert between data types.
         * In these instances, the fetch mode #GPUVertFetchMode as provided in the vertex format
         * is passed in, and used to populate function constants named: MTL_AttributeConvert0..15.
         *
         * It is then the responsibility of the vertex shader to perform any necessary type
         * casting.
         *
         * See `mtl_shader.hh` for more information. Relevant Metal API documentation:
         * https://developer.apple.com/documentation/metal/mtlvertexattributedescriptor/1516081-format?language=objc
         */
        if (attribute_desc.format == MTLVertexFormatInvalid) {
#if 0 /* Disable warning as it is too verbose and is supported. */
          MTL_LOG_WARNING(
              "MTLShader: baking pipeline state for '%s'- skipping input attribute at "
              "index '%d' but none was specified in the current vertex state",
              mtl_interface->get_name(),
              i);
#endif
          /* Write out null conversion constant if attribute unused. */
          int MTL_attribute_conversion_mode = 0;
          [values setConstantValue:&MTL_attribute_conversion_mode
                              type:MTLDataTypeInt
                          withName:[NSString stringWithFormat:@"MTL_AttributeConvert%d", i]];
          continue;
        }

        int MTL_attribute_conversion_mode = (int)attribute_desc.format_conversion_mode;
        [values setConstantValue:&MTL_attribute_conversion_mode
                            type:MTLDataTypeInt
                        withName:[NSString stringWithFormat:@"MTL_AttributeConvert%d", i]];
        if (MTL_attribute_conversion_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) {
          shader_debug_printf(
              "TODO(Metal): Shader %s needs to support internal format conversion\n",
              mtl_interface->get_name());
        }

        /* Copy metal back-end attribute descriptor state into PSO descriptor.
         * NOTE: need to copy each element due to direct assignment restrictions.
         * Also note */
        MTLVertexAttributeDescriptor *mtl_attribute = desc.vertexDescriptor.attributes[i];

        mtl_attribute.format = attribute_desc.format;
        mtl_attribute.offset = attribute_desc.offset;
        mtl_attribute.bufferIndex = attribute_desc.buffer_index;
      }

      for (const uint i : IndexRange(pipeline_descriptor.vertex_descriptor.num_vert_buffers)) {
        /* Metal back-end state buffer layout. */
        const MTLVertexBufferLayoutDescriptorPSO &buf_layout =
            pipeline_descriptor.vertex_descriptor.buffer_layouts[i];
        /* Copy metal back-end buffer layout state into PSO descriptor.
         * NOTE: need to copy each element due to copying from internal
         * back-end descriptor to Metal API descriptor. */
        MTLVertexBufferLayoutDescriptor *mtl_buf_layout = desc.vertexDescriptor.layouts[i];

        mtl_buf_layout.stepFunction = buf_layout.step_function;
        mtl_buf_layout.stepRate = buf_layout.step_rate;
        mtl_buf_layout.stride = buf_layout.stride;
      }

      /* Mark empty attribute conversion. */
      for (int i = pipeline_descriptor.vertex_descriptor.max_attribute_value + 1;
           i < GPU_VERT_ATTR_MAX_LEN;
           i++)
      {
        int MTL_attribute_conversion_mode = 0;
        [values setConstantValue:&MTL_attribute_conversion_mode
                            type:MTLDataTypeInt
                        withName:[NSString stringWithFormat:@"MTL_AttributeConvert%d", i]];
      }

      /* DEBUG: Missing/empty attributes. */
      /* Attributes are normally mapped as part of the state setting based on the used
       * #GPUVertFormat, however, if attributes have not been set, we can sort them out here. */
      for (const uint i : IndexRange(mtl_interface->get_total_attributes())) {
        const MTLShaderInputAttribute &attribute = mtl_interface->get_attribute(i);
        MTLVertexAttributeDescriptor *current_attribute =
            desc.vertexDescriptor.attributes[attribute.location];

        if (current_attribute.format == MTLVertexFormatInvalid) {
#if MTL_DEBUG_SHADER_ATTRIBUTES == 1
          printf("-> Filling in unbound attribute '%s' for shader PSO '%s' with location: %u\n",
                 mtl_interface->get_name_at_offset(attribute.name_offset),
                 mtl_interface->get_name(),
                 attribute.location);
#endif
          current_attribute.format = attribute.format;
          current_attribute.offset = 0;
          current_attribute.bufferIndex = null_buffer_index;

          /* Add Null vert buffer binding for invalid attributes. */
          if (!using_null_buffer) {
            MTLVertexBufferLayoutDescriptor *null_buf_layout =
                desc.vertexDescriptor.layouts[null_buffer_index];

            /* Use constant step function such that null buffer can
             * contain just a singular dummy attribute. */
            null_buf_layout.stepFunction = MTLVertexStepFunctionConstant;
            null_buf_layout.stepRate = 0;
            null_buf_layout.stride = max_ii(null_buf_layout.stride, attribute.size);

            /* If we are using the maximum number of vertex buffers, or tight binding indices,
             * MTL_uniform_buffer_base_index needs shifting to the bind slot after the null buffer
             * index. */
            if (null_buffer_index >= MTL_uniform_buffer_base_index) {
              MTL_uniform_buffer_base_index = null_buffer_index + 1;
            }
            using_null_buffer = true;
#if MTL_DEBUG_SHADER_ATTRIBUTES == 1
            MTL_LOG_DEBUG("Setting up buffer binding for null attribute with buffer index %d",
                          null_buffer_index);
#endif
          }
        }
      }

      /* Primitive Topology. */
      desc.inputPrimitiveTopology = pipeline_descriptor.vertex_descriptor.prim_topology_class;
    }

    /* Update constant value for 'MTL_uniform_buffer_base_index'. */
    [values setConstantValue:&MTL_uniform_buffer_base_index
                        type:MTLDataTypeInt
                    withName:@"MTL_uniform_buffer_base_index"];

    /* Storage buffer bind index.
     * This is always relative to MTL_uniform_buffer_base_index, plus the number of active buffers,
     * and an additional space for the push constant block.
     * If the shader does not have any uniform blocks, then we can place directly after the push
     * constant block. As we do not need an extra spot for the UBO at index '0'. */
    int MTL_storage_buffer_base_index = MTL_uniform_buffer_base_index + 1 +
                                        ((mtl_interface->get_total_uniform_blocks() > 0) ?
                                             mtl_interface->get_total_uniform_blocks() :
                                             0);
    [values setConstantValue:&MTL_storage_buffer_base_index
                        type:MTLDataTypeInt
                    withName:@"MTL_storage_buffer_base_index"];

    /* Clipping planes. */
    int MTL_clip_distances_enabled = (pipeline_descriptor.clipping_plane_enable_mask > 0) ? 1 : 0;

    /* Only define specialization constant if planes are required.
     * We guard clip_planes usage on this flag. */
    [values setConstantValue:&MTL_clip_distances_enabled
                        type:MTLDataTypeInt
                    withName:@"MTL_clip_distances_enabled"];

    if (MTL_clip_distances_enabled > 0) {
      /* Assign individual enablement flags. Only define a flag function constant
       * if it is used. */
      for (const int plane : IndexRange(6)) {
        int plane_enabled = ctx->pipeline_state.clip_distance_enabled[plane] ? 1 : 0;
        if (plane_enabled) {
          [values
              setConstantValue:&plane_enabled
                          type:MTLDataTypeInt
                      withName:[NSString stringWithFormat:@"MTL_clip_distance_enabled%d", plane]];
        }
      }
    }

    /* gl_PointSize constant. */
    bool null_pointsize = true;
    float MTL_pointsize = pipeline_descriptor.point_size;
    if (pipeline_descriptor.vertex_descriptor.prim_topology_class ==
        MTLPrimitiveTopologyClassPoint)
    {
      /* `if pointsize is > 0.0`, PROGRAM_POINT_SIZE is enabled, and `gl_PointSize` shader keyword
       * overrides the value. Otherwise, if < 0.0, use global constant point size. */
      if (MTL_pointsize < 0.0) {
        MTL_pointsize = fabsf(MTL_pointsize);
        [values setConstantValue:&MTL_pointsize
                            type:MTLDataTypeFloat
                        withName:@"MTL_global_pointsize"];
        null_pointsize = false;
      }
    }

    if (null_pointsize) {
      MTL_pointsize = 0.0f;
      [values setConstantValue:&MTL_pointsize
                          type:MTLDataTypeFloat
                      withName:@"MTL_global_pointsize"];
    }

    /* Compile functions */
    NSError *error = nullptr;
    desc.vertexFunction = [shader_library_vert_ newFunctionWithName:vertex_function_name_
                                                     constantValues:values
                                                              error:&error];
    if (error) {
      bool has_error = (
          [[error localizedDescription] rangeOfString:@"Compilation succeeded"].location ==
          NSNotFound);

      const char *errors_c_str = [[error localizedDescription] UTF8String];
      const StringRefNull source = shd_builder_->glsl_fragment_source_.c_str();

      MTLLogParser parser;
      print_log({source}, errors_c_str, "VertShader", has_error, &parser);

      /* Only exit out if genuine error and not warning */
      if (has_error) {
        return nullptr;
      }
    }

    desc.fragmentFunction = [shader_library_frag_ newFunctionWithName:fragment_function_name_
                                                       constantValues:values
                                                                error:&error];
    if (error) {
      bool has_error = (
          [[error localizedDescription] rangeOfString:@"Compilation succeeded"].location ==
          NSNotFound);

      const char *errors_c_str = [[error localizedDescription] UTF8String];
      const StringRefNull source = shd_builder_->glsl_fragment_source_;

      MTLLogParser parser;
      print_log({source}, errors_c_str, "FragShader", has_error, &parser);

      /* Only exit out if genuine error and not warning */
      if (has_error) {
        return nullptr;
      }
    }

    /* Setup pixel format state */
    for (int color_attachment = 0; color_attachment < GPU_FB_MAX_COLOR_ATTACHMENT;
         color_attachment++)
    {
      /* Fetch color attachment pixel format in back-end pipeline state. */
      MTLPixelFormat pixel_format = pipeline_descriptor.color_attachment_format[color_attachment];
      /* Populate MTL API PSO attachment descriptor. */
      MTLRenderPipelineColorAttachmentDescriptor *col_attachment =
          desc.colorAttachments[color_attachment];

      col_attachment.pixelFormat = pixel_format;
      if (pixel_format != MTLPixelFormatInvalid) {
        bool format_supports_blending = mtl_format_supports_blending(pixel_format);

        col_attachment.writeMask = pipeline_descriptor.color_write_mask;
        col_attachment.blendingEnabled = pipeline_descriptor.blending_enabled &&
                                         format_supports_blending;
        if (format_supports_blending && pipeline_descriptor.blending_enabled) {
          col_attachment.alphaBlendOperation = pipeline_descriptor.alpha_blend_op;
          col_attachment.rgbBlendOperation = pipeline_descriptor.rgb_blend_op;
          col_attachment.destinationAlphaBlendFactor = pipeline_descriptor.dest_alpha_blend_factor;
          col_attachment.destinationRGBBlendFactor = pipeline_descriptor.dest_rgb_blend_factor;
          col_attachment.sourceAlphaBlendFactor = pipeline_descriptor.src_alpha_blend_factor;
          col_attachment.sourceRGBBlendFactor = pipeline_descriptor.src_rgb_blend_factor;
        }
        else {
          if (pipeline_descriptor.blending_enabled && !format_supports_blending) {
            shader_debug_printf(
                "[Warning] Attempting to Bake PSO, but MTLPixelFormat %d does not support "
                "blending\n",
                *((int *)&pixel_format));
          }
        }
      }
    }
    desc.depthAttachmentPixelFormat = pipeline_descriptor.depth_attachment_format;
    desc.stencilAttachmentPixelFormat = pipeline_descriptor.stencil_attachment_format;

    /* Bind-point range validation.
     * We need to ensure that the PSO will have valid bind-point ranges, or is using the
     * appropriate bindless fallback path if any bind limits are exceeded. */
#ifdef NDEBUG
    /* Ensure Buffer bindings are within range. */
    BLI_assert_msg((MTL_uniform_buffer_base_index + get_max_ubo_index() + 2) <
                       MTL_MAX_BUFFER_BINDINGS,
                   "UBO and SSBO bindings exceed the fragment bind table limit.");

    /* Argument buffer. */
    if (mtl_interface->uses_argument_buffer_for_samplers()) {
      BLI_assert_msg(mtl_interface->get_argument_buffer_bind_index() < MTL_MAX_BUFFER_BINDINGS,
                     "Argument buffer binding exceeds the fragment bind table limit.");
    }
#endif

    /* Compile PSO */
    MTLAutoreleasedRenderPipelineReflection reflection_data;
    id<MTLRenderPipelineState> pso = [ctx->device
        newRenderPipelineStateWithDescriptor:desc
                                     options:MTLPipelineOptionBufferTypeInfo
                                  reflection:&reflection_data
                                       error:&error];
    if (error) {
      NSLog(@"Failed to create PSO for shader: %s error %@\n", this->name, error);
      BLI_assert(false);
      return nullptr;
    }
    else if (!pso) {
      NSLog(@"Failed to create PSO for shader: %s, but no error was provided!\n", this->name);
      BLI_assert(false);
      return nullptr;
    }
    else {
#if 0
      NSLog(@"Successfully compiled PSO for shader: %s (Metal Context: %p)\n", this->name, ctx);
#endif
    }

    /* Prepare pipeline state instance. */
    MTLRenderPipelineStateInstance *pso_inst = new MTLRenderPipelineStateInstance();
    pso_inst->vert = desc.vertexFunction;
    pso_inst->frag = desc.fragmentFunction;
    pso_inst->pso = pso;
    pso_inst->base_uniform_buffer_index = MTL_uniform_buffer_base_index;
    pso_inst->base_storage_buffer_index = MTL_storage_buffer_base_index;
    pso_inst->null_attribute_buffer_index = (using_null_buffer) ? null_buffer_index : -1;
    pso_inst->prim_type = prim_type;

    pso_inst->reflection_data_available = (reflection_data != nil);
    if (reflection_data != nil) {

      /* Extract shader reflection data for buffer bindings.
       * This reflection data is used to contrast the binding information
       * we know about in the interface against the bindings in the finalized
       * PSO. This accounts for bindings which have been stripped out during
       * optimization, and allows us to both avoid over-binding and also
       * allows us to verify size-correctness for bindings, to ensure
       * that buffers bound are not smaller than the size of expected data. */
      NSArray<MTLArgument *> *vert_args = [reflection_data vertexArguments];

      pso_inst->buffer_bindings_reflection_data_vert.clear();
      int buffer_binding_max_ind = 0;

      for (int i = 0; i < [vert_args count]; i++) {
        MTLArgument *arg = [vert_args objectAtIndex:i];
        if ([arg type] == MTLArgumentTypeBuffer) {
          int buf_index = [arg index] - MTL_uniform_buffer_base_index;
          if (buf_index >= 0) {
            buffer_binding_max_ind = max_ii(buffer_binding_max_ind, buf_index);
          }
        }
      }
      pso_inst->buffer_bindings_reflection_data_vert.resize(buffer_binding_max_ind + 1);
      for (int i = 0; i < buffer_binding_max_ind + 1; i++) {
        pso_inst->buffer_bindings_reflection_data_vert[i] = {0, 0, 0, false};
      }

      for (int i = 0; i < [vert_args count]; i++) {
        MTLArgument *arg = [vert_args objectAtIndex:i];
        if ([arg type] == MTLArgumentTypeBuffer) {
          int buf_index = [arg index] - MTL_uniform_buffer_base_index;

          if (buf_index >= 0) {
            pso_inst->buffer_bindings_reflection_data_vert[buf_index] = {
                (uint32_t)([arg index]),
                (uint32_t)([arg bufferDataSize]),
                (uint32_t)([arg bufferAlignment]),
                ([arg isActive] == YES) ? true : false};
          }
        }
      }

      NSArray<MTLArgument *> *frag_args = [reflection_data fragmentArguments];

      pso_inst->buffer_bindings_reflection_data_frag.clear();
      buffer_binding_max_ind = 0;

      for (int i = 0; i < [frag_args count]; i++) {
        MTLArgument *arg = [frag_args objectAtIndex:i];
        if ([arg type] == MTLArgumentTypeBuffer) {
          int buf_index = [arg index] - MTL_uniform_buffer_base_index;
          if (buf_index >= 0) {
            buffer_binding_max_ind = max_ii(buffer_binding_max_ind, buf_index);
          }
        }
      }
      pso_inst->buffer_bindings_reflection_data_frag.resize(buffer_binding_max_ind + 1);
      for (int i = 0; i < buffer_binding_max_ind + 1; i++) {
        pso_inst->buffer_bindings_reflection_data_frag[i] = {0, 0, 0, false};
      }

      for (int i = 0; i < [frag_args count]; i++) {
        MTLArgument *arg = [frag_args objectAtIndex:i];
        if ([arg type] == MTLArgumentTypeBuffer) {
          int buf_index = [arg index] - MTL_uniform_buffer_base_index;
          shader_debug_printf(" BUF IND: %d (arg name: %s)\n", buf_index, [[arg name] UTF8String]);
          if (buf_index >= 0) {
            pso_inst->buffer_bindings_reflection_data_frag[buf_index] = {
                (uint32_t)([arg index]),
                (uint32_t)([arg bufferDataSize]),
                (uint32_t)([arg bufferAlignment]),
                ([arg isActive] == YES) ? true : false};
          }
        }
      }
    }

    /* Insert into pso cache. */
    pso_cache_lock_.lock();
    pso_inst->shader_pso_index = pso_cache_.size();
    pso_cache_.add(pipeline_descriptor, pso_inst);
    pso_cache_lock_.unlock();
    shader_debug_printf(
        "PSO CACHE: Stored new variant in PSO cache for shader '%s' Hash: '%llu'\n",
        this->name,
        pipeline_descriptor.hash());
    return pso_inst;
  }
}

MTLComputePipelineStateInstance *MTLShader::bake_compute_pipeline_state(
    MTLContext *ctx, MTLComputePipelineStateDescriptor &compute_pipeline_descriptor)
{
  /* NOTE(Metal): Bakes and caches a PSO for compute. */
  BLI_assert(this);
  MTLShaderInterface *mtl_interface = this->get_interface();
  BLI_assert(mtl_interface);
  BLI_assert(this->is_valid());
  BLI_assert(shader_library_compute_ != nil);

  /* Check if current PSO exists in the cache. */
  pso_cache_lock_.lock();
  MTLComputePipelineStateInstance *const *pso_lookup = compute_pso_cache_.lookup_ptr(
      compute_pipeline_descriptor);
  MTLComputePipelineStateInstance *pipeline_state = (pso_lookup) ? *pso_lookup : nullptr;
  pso_cache_lock_.unlock();

  if (pipeline_state != nullptr) {
    /* Return cached PSO state. */
    BLI_assert(pipeline_state->pso != nil);
    return pipeline_state;
  }
  else {
    /* Prepare Compute Pipeline Descriptor. */

    /* Setup function specialization constants, used to modify and optimize
     * generated code based on current render pipeline configuration. */
    MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];

    /* TODO: Compile specialized shader variants asynchronously. */

    /* Custom function constant values: */
    populate_specialization_constant_values(
        values, *this->constants, compute_pipeline_descriptor.specialization_state);

    /* Offset the bind index for Uniform buffers such that they begin after the VBO
     * buffer bind slots. `MTL_uniform_buffer_base_index` is passed as a function
     * specialization constant, customized per unique pipeline state permutation.
     *
     * For Compute shaders, this offset is always zero, but this needs setting as
     * it is expected as part of the common Metal shader header. */
    int MTL_uniform_buffer_base_index = 0;
    [values setConstantValue:&MTL_uniform_buffer_base_index
                        type:MTLDataTypeInt
                    withName:@"MTL_uniform_buffer_base_index"];

    /* Storage buffer bind index.
     * This is always relative to MTL_uniform_buffer_base_index, plus the number of active buffers,
     * and an additional space for the push constant block.
     * If the shader does not have any uniform blocks, then we can place directly after the push
     * constant block. As we do not need an extra spot for the UBO at index '0'. */
    int MTL_storage_buffer_base_index = MTL_uniform_buffer_base_index + 1 +
                                        ((mtl_interface->get_total_uniform_blocks() > 0) ?
                                             mtl_interface->get_total_uniform_blocks() :
                                             0);

    [values setConstantValue:&MTL_storage_buffer_base_index
                        type:MTLDataTypeInt
                    withName:@"MTL_storage_buffer_base_index"];

    /* Compile compute function. */
    NSError *error = nullptr;
    id<MTLFunction> compute_function = [shader_library_compute_
        newFunctionWithName:compute_function_name_
             constantValues:values
                      error:&error];
    compute_function.label = [NSString stringWithUTF8String:this->name];

    if (error) {
      NSLog(@"Compile Error - Metal Shader compute function, error %@", error);

      /* Only exit out if genuine error and not warning */
      if ([[error localizedDescription] rangeOfString:@"Compilation succeeded"].location ==
          NSNotFound)
      {
        BLI_assert(false);
        return nullptr;
      }
    }

    /* Compile PSO. */
    MTLComputePipelineDescriptor *desc = [[MTLComputePipelineDescriptor alloc] init];
    desc.label = [NSString stringWithUTF8String:this->name];
    desc.computeFunction = compute_function;

    /** If Max Total threads per threadgroup tuning parameters are specified, compile with these.
     * This enables the compiler to make informed decisions based on the upper bound of threads
     * issued for a given compute call.
     * This per-shader tuning can reduce the static register memory allocation by reducing the
     * worst-case allocation and increasing thread occupancy.
     *
     * NOTE: This is only enabled on Apple M1 and M2 GPUs. Apple M3 GPUs feature dynamic caching
     * which controls register allocation dynamically based on the runtime state. */
    const MTLCapabilities &capabilities = MTLBackend::get_capabilities();
    if (ELEM(capabilities.gpu, APPLE_GPU_M1, APPLE_GPU_M2)) {
      if (maxTotalThreadsPerThreadgroup_Tuning_ > 0) {
        desc.maxTotalThreadsPerThreadgroup = this->maxTotalThreadsPerThreadgroup_Tuning_;
        MTL_LOG_DEBUG("Using custom parameter for shader %s value %u\n",
                      this->name,
                      maxTotalThreadsPerThreadgroup_Tuning_);
      }
    }

    id<MTLComputePipelineState> pso = [ctx->device
        newComputePipelineStateWithDescriptor:desc
                                      options:MTLPipelineOptionNone
                                   reflection:nullptr
                                        error:&error];

    /* If PSO has compiled but max theoretical threads-per-threadgroup is lower than required
     * dispatch size, recompile with increased limit. NOTE: This will result in a performance drop,
     * ideally the source shader should be modified to reduce local register pressure, or, local
     * work-group size should be reduced.
     * Similarly, the custom tuning parameter "mtl_max_total_threads_per_threadgroup" can be
     * specified to a sufficiently large value to avoid this. */
    if (pso) {
      uint num_required_threads_per_threadgroup = compute_pso_common_state_.threadgroup_x_len *
                                                  compute_pso_common_state_.threadgroup_y_len *
                                                  compute_pso_common_state_.threadgroup_z_len;
      if (pso.maxTotalThreadsPerThreadgroup < num_required_threads_per_threadgroup) {
        MTL_LOG_WARNING(
            "Shader '%s' requires %u threads per threadgroup, but PSO limit is: %lu. Recompiling "
            "with increased limit on descriptor.\n",
            this->name,
            num_required_threads_per_threadgroup,
            (unsigned long)pso.maxTotalThreadsPerThreadgroup);
        [pso release];
        pso = nil;
        desc.maxTotalThreadsPerThreadgroup = 1024;
        pso = [ctx->device newComputePipelineStateWithDescriptor:desc
                                                         options:MTLPipelineOptionNone
                                                      reflection:nullptr
                                                           error:&error];
      }
    }

    if (error) {
      NSLog(@"Failed to create PSO for compute shader: %s error %@\n", this->name, error);
      BLI_assert(false);
      return nullptr;
    }
    else if (!pso) {
      NSLog(@"Failed to create PSO for compute shader: %s, but no error was provided!\n",
            this->name);
      BLI_assert(false);
      return nullptr;
    }
    else {
#if 0
      NSLog(@"Successfully compiled compute PSO for shader: %s (Metal Context: %p)\n",
            this->name,
            ctx);
#endif
    }

    [desc release];

    /* Gather reflection data and create MTLComputePipelineStateInstance to store results. */
    MTLComputePipelineStateInstance *compute_pso_instance = new MTLComputePipelineStateInstance();
    compute_pso_instance->compute = compute_function;
    compute_pso_instance->pso = pso;
    compute_pso_instance->base_uniform_buffer_index = MTL_uniform_buffer_base_index;
    compute_pso_instance->base_storage_buffer_index = MTL_storage_buffer_base_index;
    pso_cache_lock_.lock();
    compute_pso_instance->shader_pso_index = compute_pso_cache_.size();
    compute_pso_cache_.add(compute_pipeline_descriptor, compute_pso_instance);
    pso_cache_lock_.unlock();

    return compute_pso_instance;
  }
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name MTLShaderCompiler
 * \{ */

MTLShaderCompiler::MTLShaderCompiler()
    : ShaderCompiler(GPU_max_parallel_compilations(), GPUWorker::ContextType::PerThread, true)
{
}

Shader *MTLShaderCompiler::compile_shader(const shader::ShaderCreateInfo &info)
{
  MTLShader *shader = static_cast<MTLShader *>(compile(info, true));

  if (shader) {
    /* Generate and cache any render PSOs if possible (typically materials only)
     * (Finalize() will already bake a Compute PSO if possible) */
    shader->warm_cache(-1);
  }

  return shader;
}

void MTLShaderCompiler::specialize_shader(ShaderSpecialization &specialization)
{
  MTLShader *shader = static_cast<MTLShader *>(specialization.shader);

  BLI_assert_msg(shader->is_valid(),
                 "Shader must be finalized before precompiling specializations");

  if (!shader->has_compute_shader_lib()) {
    /* Currently only support Compute */
    return;
  }

  /* Create descriptor using these specialization constants. */
  MTLComputePipelineStateDescriptor compute_pipeline_descriptor(specialization.constants.values);

  MTLContext *metal_context = static_cast<MTLContext *>(Context::get());
  shader->bake_compute_pipeline_state(metal_context, compute_pipeline_descriptor);
}

/** \} */

}  // namespace blender::gpu
