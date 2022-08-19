/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. */

/** \file
 * \ingroup EEVEE
 *
 * This file is only there to handle ShaderCreateInfos.
 */

#include "GPU_shader.h"

#include "BLI_string_ref.hh"

#include "gpu_shader_create_info.hh"

#include "eevee_private.h"

using blender::gpu::shader::StageInterfaceInfo;

static StageInterfaceInfo *stage_interface = nullptr;

void eevee_shader_extra_init()
{
  if (stage_interface != nullptr) {
    return;
  }

  using namespace blender::gpu::shader;
  stage_interface = new StageInterfaceInfo("ShaderStageInterface", "");
  stage_interface->smooth(Type::VEC3, "worldPosition");
  stage_interface->smooth(Type::VEC3, "viewPosition");
  stage_interface->smooth(Type::VEC3, "worldNormal");
  stage_interface->smooth(Type::VEC3, "viewNormal");
  stage_interface->flat(Type::INT, "resourceIDFrag");
}

void eevee_shader_extra_exit()
{
  delete stage_interface;
}

void eevee_shader_material_create_info_amend(GPUMaterial *gpumat,
                                             GPUCodegenOutput *codegen_,
                                             char *frag,
                                             char *vert,
                                             char *geom,
                                             char *defines)
{
  using namespace blender::gpu::shader;

  uint64_t options = GPU_material_uuid_get(gpumat);
  const bool is_background = (options & (VAR_WORLD_PROBE | VAR_WORLD_BACKGROUND)) != 0;
  const bool is_volume = (options & (VAR_MAT_VOLUME)) != 0;
  const bool is_hair = (options & (VAR_MAT_HAIR)) != 0;
  const bool is_mesh = (options & (VAR_MAT_MESH)) != 0;
  const bool is_point_cloud = (options & (VAR_MAT_POINTCLOUD)) != 0;

  GPUCodegenOutput &codegen = *codegen_;
  ShaderCreateInfo &info = *reinterpret_cast<ShaderCreateInfo *>(codegen.create_info);

  info.legacy_resource_location(true);
  info.auto_resource_location(true);

  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SUBSURFACE)) {
    info.define("USE_SSS");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_TO_RGBA)) {
    info.define("USE_SHADER_TO_RGBA");
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_BARYCENTRIC) && !is_volume && !is_hair &&
      !is_point_cloud && !is_background) {
    info.define("USE_BARYCENTRICS");
    info.builtins(BuiltinBits::BARYCENTRIC_COORD);
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_BARYCENTRIC) && is_hair) {
    info.define("USE_BARYCENTRICS");
  }

  std::stringstream attr_load;

  const bool do_fragment_attrib_load = is_background || is_volume;

  if (is_hair && !info.vertex_out_interfaces_.is_empty()) {
    /** Hair attributes come from sampler buffer. Transfer attributes to sampler. */
    for (auto &input : info.vertex_inputs_) {
      info.sampler(0, ImageType::FLOAT_BUFFER, input.name, Frequency::BATCH);
    }
    info.vertex_inputs_.clear();
  }
  else if (do_fragment_attrib_load && !info.vertex_out_interfaces_.is_empty()) {
    /* Codegen outputs only one interface. */
    const StageInterfaceInfo &iface = *info.vertex_out_interfaces_.first();
    /* Globals the attrib_load() can write to when it is in the fragment shader. */
    attr_load << "struct " << iface.name << " {\n";
    for (const auto &inout : iface.inouts) {
      attr_load << "  " << inout.type << " " << inout.name << ";\n";
    }
    attr_load << "};\n";
    attr_load << iface.name << " " << iface.instance_name << ";\n";
    if (!is_volume) {
      /* Global vars just to make code valid. Only Orco is supported. */
      for (const ShaderCreateInfo::VertIn &in : info.vertex_inputs_) {
        attr_load << in.type << " " << in.name << ";\n";
      }
    }
    info.vertex_out_interfaces_.clear();
  }
  if (is_volume) {
    /** Volume grid attributes come from 3D textures. Transfer attributes to samplers. */
    for (auto &input : info.vertex_inputs_) {
      info.sampler(0, ImageType::FLOAT_3D, input.name, Frequency::BATCH);
    }
    info.additional_info("draw_volume_infos");
    /* Do not add twice. */
    if (!GPU_material_flag_get(gpumat, GPU_MATFLAG_OBJECT_INFO)) {
      info.additional_info("draw_object_infos");
    }
    info.vertex_inputs_.clear();
  }

  if (is_hair) {
    info.additional_info("draw_curves_infos");
  }

  if (!is_volume) {
    info.define("EEVEE_GENERATED_INTERFACE");
    info.vertex_out(*stage_interface);
  }

  attr_load << "void attrib_load()\n";
  attr_load << "{\n";
  attr_load << ((codegen.attr_load) ? codegen.attr_load : "");
  attr_load << "}\n\n";

  std::stringstream vert_gen, frag_gen, geom_gen;

  if (do_fragment_attrib_load) {
    frag_gen << attr_load.str();
  }
  else {
    vert_gen << attr_load.str();
  }

  {
    vert_gen << vert;
    info.vertex_source_generated = vert_gen.str();
    /* Everything is in generated source. */
    info.vertex_source(is_volume ? "eevee_empty_volume.glsl" : "eevee_empty.glsl");
  }

  {
    frag_gen << frag;
    if (codegen.material_functions) {
      frag_gen << codegen.material_functions;
    }
    frag_gen << "Closure nodetree_exec()\n";
    frag_gen << "{\n";
    if (is_volume) {
      frag_gen << ((codegen.volume) ? codegen.volume : "return CLOSURE_DEFAULT;\n");
    }
    else {
      frag_gen << ((codegen.surface) ? codegen.surface : "return CLOSURE_DEFAULT;\n");
    }
    frag_gen << "}\n\n";

    if (codegen.displacement && (is_hair || is_mesh)) {
      info.define("EEVEE_DISPLACEMENT_BUMP");

      frag_gen << "vec3 displacement_exec()\n";
      frag_gen << "{\n";
      frag_gen << codegen.displacement;
      frag_gen << "}\n\n";
    }

    info.fragment_source_generated = frag_gen.str();
    /* Everything is in generated source. */
    info.fragment_source(is_volume ? "eevee_empty_volume.glsl" : "eevee_empty.glsl");
  }

  if (geom) {
    geom_gen << geom;
    info.geometry_source_generated = geom_gen.str();
    info.geometry_layout(PrimitiveIn::TRIANGLES, PrimitiveOut::TRIANGLE_STRIP, 3);
    /* Everything is in generated source. */
    info.geometry_source("eevee_empty.glsl");
  }

  if (defines) {
    info.typedef_source_generated += blender::StringRefNull(defines);
  }
}
