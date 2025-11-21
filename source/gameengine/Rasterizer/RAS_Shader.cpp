/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/RAS_Shader.cpp
 *  \ingroup bgerast
 */

#include "RAS_Shader.h"

#include <set>
#include <sstream>

#include "GPU_immediate.hh"
#include "GPU_uniform_buffer.hh"

#include "CM_Message.h"

using namespace blender::gpu::shader;

RAS_Shader::RAS_Uniform::RAS_Uniform(int data_size)
    : m_loc(-1), m_count(1), m_dirty(true), m_type(UNI_NONE), m_transpose(0), m_dataLen(data_size)
{
#ifdef SORT_UNIFORMS
  m_data = (void *)MEM_mallocN(m_dataLen, "shader-uniform-alloc");
#endif
}

RAS_Shader::RAS_Uniform::~RAS_Uniform()
{
#ifdef SORT_UNIFORMS
  if (m_data) {
    MEM_freeN(m_data);
    m_data = nullptr;
  }
#endif
}

void RAS_Shader::RAS_Uniform::Apply(RAS_Shader *shader)
{
#ifdef SORT_UNIFORMS
  BLI_assert(m_type > UNI_NONE && m_type < UNI_MAX && m_data);

  if (!m_dirty) {
    return;
  }

  blender::gpu::Shader *gpushader = shader->GetGPUShader();
  switch (m_type) {
    case UNI_FLOAT: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 1, m_count, (float *)f);
      break;
    }
    case UNI_INT: {
      int *f = (int *)m_data;
      GPU_shader_uniform_int_ex(gpushader, m_loc, 1, m_count, (int *)f);
      break;
    }
    case UNI_FLOAT2: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 2, m_count, (float *)f);
      break;
    }
    case UNI_FLOAT3: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 3, m_count, (float *)f);
      break;
    }
    case UNI_FLOAT4: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 4, m_count, (float *)f);
      break;
    }
    case UNI_INT2: {
      int *f = (int *)m_data;
      GPU_shader_uniform_int_ex(gpushader, m_loc, 2, m_count, (int *)f);
      break;
    }
    case UNI_INT3: {
      int *f = (int *)m_data;
      GPU_shader_uniform_int_ex(gpushader, m_loc, 3, m_count, (int *)f);
      break;
    }
    case UNI_INT4: {
      int *f = (int *)m_data;
      GPU_shader_uniform_int_ex(gpushader, m_loc, 4, m_count, (int *)f);
      break;
    }
    case UNI_MAT4: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 16, m_count, (float *)f);
      break;
    }
    case UNI_MAT3: {
      float *f = (float *)m_data;
      GPU_shader_uniform_float_ex(gpushader, m_loc, 9, m_count, (float *)f);
      break;
    }
  }
  m_dirty = false;
#endif
}

void RAS_Shader::RAS_Uniform::SetData(int location, int type, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
  m_type = type;
  m_loc = location;
  m_count = count;
  m_dirty = true;
#endif
}

int RAS_Shader::RAS_Uniform::GetLocation()
{
  return m_loc;
}

void *RAS_Shader::RAS_Uniform::GetData()
{
  return m_data;
}

bool RAS_Shader::Ok() const
{
  return (m_shader && m_use);
}

RAS_Shader::RAS_Shader() : m_shader(nullptr), m_use(0), m_error(0), m_dirty(true)
{
  for (unsigned short i = 0; i < MAX_PROGRAM; ++i) {
    m_progs[i] = "";
  }
  m_constantUniforms = {};
  m_samplerUniforms = {};
}

RAS_Shader::~RAS_Shader()
{
  ClearUniforms();

  DeleteShader();

  GPU_uniformbuf_free(m_ubo);
  m_ubo = nullptr;
}

void RAS_Shader::ClearUniforms()
{
  for (RAS_Uniform *uni : m_uniforms) {
    delete uni;
  }
  m_uniforms.clear();

  for (RAS_DefUniform *uni : m_preDef) {
    delete uni;
  }
  m_preDef.clear();
}

RAS_Shader::RAS_Uniform *RAS_Shader::FindUniform(const int location)
{
#ifdef SORT_UNIFORMS
  for (RAS_Uniform *uni : m_uniforms) {
    if (uni->GetLocation() == location) {
      return uni;
    }
  }
#endif
  return nullptr;
}

void RAS_Shader::SetUniformfv(
    int location, int type, float *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
  RAS_Uniform *uni = FindUniform(location);

  if (uni) {
    memcpy(uni->GetData(), param, size);
    uni->SetData(location, type, count, transpose);
  }
  else {
    uni = new RAS_Uniform(size);
    memcpy(uni->GetData(), param, size);
    uni->SetData(location, type, count, transpose);
    m_uniforms.push_back(uni);
  }

  m_dirty = true;
#endif
}

void RAS_Shader::SetUniformiv(
    int location, int type, int *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
  RAS_Uniform *uni = FindUniform(location);

  if (uni) {
    memcpy(uni->GetData(), param, size);
    uni->SetData(location, type, count, transpose);
  }
  else {
    uni = new RAS_Uniform(size);
    memcpy(uni->GetData(), param, size);
    uni->SetData(location, type, count, transpose);
    m_uniforms.push_back(uni);
  }

  m_dirty = true;
#endif
}

void RAS_Shader::ApplyShader()
{
#ifdef SORT_UNIFORMS
  if (!m_dirty) {
    return;
  }

  for (unsigned int i = 0; i < m_uniforms.size(); i++) {
    m_uniforms[i]->Apply(this);
  }

  m_dirty = false;
#endif
}

void RAS_Shader::UnloadShader()
{
  //
}

void RAS_Shader::DeleteShader()
{
  if (m_shader) {
    GPU_shader_free(m_shader);
    m_shader = nullptr;
  }
}

void RAS_Shader::AppendUniformInfos(std::string type, std::string name)
{
  if (type == "float") {
    m_constantUniforms.push_back(UniformConstant({Type::float_t, name}));
  }
  else if (type == "int") {
    m_constantUniforms.push_back(UniformConstant({Type::int_t, name}));
  }
  else if (type == "vec2") {
    m_constantUniforms.push_back(UniformConstant({Type::float2_t, name}));
  }
  else if (type == "vec3") {
    m_constantUniforms.push_back(UniformConstant({Type::float3_t, name}));
  }
  else if (type == "vec4") {
    m_constantUniforms.push_back(UniformConstant({Type::float4_t, name}));
  }
  else if (type == "mat3") {
    m_constantUniforms.push_back(UniformConstant({Type::float3x3_t, name}));
  }
  else if (type == "mat4") {
    m_constantUniforms.push_back(UniformConstant({Type::float4x4_t, name}));
  }
  else if (type == "sampler2D") {
    if (m_samplerUniforms.size() > 7) {
      CM_Warning("RAS_Shader: Sampler index can't be > 7");
    }
    else {
      m_samplerUniforms.push_back({m_samplerUniforms.size(), name});
    }
  }
  else {
    CM_Warning("Invalid/unsupported uniform type: " << name);
  }
}

std::string RAS_Shader::GetParsedProgram(ProgramType type)
{
  // List of built-in uniforms to ignore (already handled by the engine).
  static const std::set<std::string> builtin_uniforms = {"bgl_RenderedTexture",
                                                         "bgl_DepthTexture"};

  // List of built-in in/out variables to ignore (already provided by ShaderCreateInfo).
  static const std::set<std::string> builtin_inout_vars = {"fragColor", "bgl_TexCoord"};

  std::string prog = m_progs[type];
  if (prog.empty()) {
    return prog;
  }

  // Remove redundant #version directive.
  const size_t pos = prog.find("#version");
  if (pos != std::string::npos) {
    CM_Warning("found redundant #version directive in shader program, directive ignored.");
    const size_t nline = prog.find("\n", pos);
    prog.erase(pos, (nline != std::string::npos) ? (nline - pos) : std::string::npos);
  }

  std::istringstream input(prog);
  std::string line;
  std::string output;

  while (std::getline(input, line)) {
    std::string trimmed = line;
    // Skip already commented lines.
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    if (trimmed.rfind("//", 0) == 0) {
      // Still do the replacements in commented lines for consistency.
      // (optionally, you can skip this if you prefer)
      // Replace built-in variable usages
      size_t pos;
      while ((pos = line.find("bgl_TextureCoordinateOffset[")) != std::string::npos) {
        line.replace(pos, strlen("bgl_TextureCoordinateOffset"), "g_data.coo_offset");
      }
      while ((pos = line.find("bgl_RenderedTextureWidth")) != std::string::npos) {
        line.replace(pos, strlen("bgl_RenderedTextureWidth"), "g_data.width");
      }
      while ((pos = line.find("bgl_RenderedTextureHeight")) != std::string::npos) {
        line.replace(pos, strlen("bgl_RenderedTextureHeight"), "g_data.height");
      }
      output += line + "\n";
      continue;
    }

    // Look for "uniform" at the start of the line (after spaces).
    size_t uniform_pos = trimmed.find("uniform ");
    if (uniform_pos == 0) {
      // Find the type (after "uniform ").
      size_t type_start = 8;
      while (type_start < trimmed.size() && std::isspace(trimmed[type_start]))
        ++type_start;
      size_t type_end = type_start;
      while (type_end < trimmed.size() &&
             (std::isalnum(trimmed[type_end]) || trimmed[type_end] == '_'))
        ++type_end;
      std::string type = trimmed.substr(type_start, type_end - type_start);

      // After the type, get the variable list up to the ';'
      size_t vars_start = type_end;
      while (vars_start < trimmed.size() && std::isspace(trimmed[vars_start]))
        ++vars_start;
      size_t vars_end = trimmed.find(';', vars_start);
      if (vars_end == std::string::npos) {
        // Malformed line, just comment it out.
        output += "// " + line + "\n";
        continue;
      }
      std::string vars = trimmed.substr(vars_start, vars_end - vars_start);

      // Split the variable list by ','
      std::istringstream varstream(vars);
      std::string var;
      while (std::getline(varstream, var, ',')) {
        // Trim spaces
        size_t vstart = var.find_first_not_of(" \t");
        size_t vend = var.find_last_not_of(" \t");
        if (vstart == std::string::npos || vend == std::string::npos)
          continue;
        std::string varname = var.substr(vstart, vend - vstart + 1);

        // Handle arrays (e.g. foo[4])
        size_t arr_pos = varname.find('[');
        std::string base_name = (arr_pos != std::string::npos) ? varname.substr(0, arr_pos) :
                                                                 varname;

        // Ignore built-in uniforms
        if (builtin_uniforms.count(base_name)) {
          continue;
        }
        // Register user uniform as push_constant
        AppendUniformInfos(type, varname);
      }

      // Always comment out the uniform line (even if all variables are ignored)
      output += "// " + line + "\n";
      continue;
    }

    // Look for "in" or "out" declarations and ignore built-in interface vars.
    size_t in_pos = trimmed.find("in ");
    size_t out_pos = trimmed.find("out ");
    if (in_pos == 0 || out_pos == 0) {
      size_t prefix_len = (in_pos == 0) ? 3 : 4;  // length of "in " or "out "
      size_t type_start = prefix_len;
      while (type_start < trimmed.size() && std::isspace(trimmed[type_start]))
        ++type_start;
      size_t type_end = type_start;
      while (type_end < trimmed.size() &&
             (std::isalnum(trimmed[type_end]) || trimmed[type_end] == '_'))
        ++type_end;

      // After the type, get the variable list up to the ';'
      size_t vars_start = type_end;
      while (vars_start < trimmed.size() && std::isspace(trimmed[vars_start]))
        ++vars_start;
      size_t vars_end = trimmed.find(';', vars_start);
      if (vars_end == std::string::npos) {
        // Malformed line, just comment it out.
        output += "// " + line + "\n";
        continue;
      }
      std::string vars = trimmed.substr(vars_start, vars_end - vars_start);

      // Split the variable list by ','
      std::istringstream varstream(vars);
      std::string var;
      bool has_builtin = false;
      while (std::getline(varstream, var, ',')) {
        // Trim spaces
        size_t vstart = var.find_first_not_of(" \t");
        size_t vend = var.find_last_not_of(" \t");
        if (vstart == std::string::npos || vend == std::string::npos)
          continue;
        std::string varname = var.substr(vstart, vend - vstart + 1);

        // Handle arrays (e.g. foo[4])
        size_t arr_pos = varname.find('[');
        std::string base_name = (arr_pos != std::string::npos) ? varname.substr(0, arr_pos) :
                                                                 varname;

        if (builtin_inout_vars.count(base_name)) {
          has_builtin = true;
        }
      }

      if (has_builtin) {
        // Comment out the declaration of built-in in/out variables to avoid redeclaration.
        output += "// " + line + "\n";
        continue;
      }
    }

    // Replace built-in variable usages in the line
    size_t pos2;
    while ((pos2 = line.find("bgl_TextureCoordinateOffset[")) != std::string::npos) {
      line.replace(pos2, strlen("bgl_TextureCoordinateOffset"), "g_data.coo_offset");
    }
    while ((pos2 = line.find("bgl_RenderedTextureWidth")) != std::string::npos) {
      line.replace(pos2, strlen("bgl_RenderedTextureWidth"), "g_data.width");
    }
    while ((pos2 = line.find("bgl_RenderedTextureHeight")) != std::string::npos) {
      line.replace(pos2, strlen("bgl_RenderedTextureHeight"), "g_data.height");
    }

    // Normal line, just copy (with replacements)
    output += line + "\n";
  }

  return "\n" + output;
}

static int ConstantTypeSize(Type type)
{
  using namespace blender::gpu::shader;
  switch (type) {
    case Type::bool_t:
    case Type::float_t:
    case Type::int_t:
    case Type::uint_t:
    case Type::uchar4_t:
    case Type::char4_t:
    case Type::float3_10_10_10_2_t:
    case Type::ushort2_t:
    case Type::short2_t:
      return 4;
    case Type::ushort3_t:
    case Type::short3_t:
      return 6;
    case Type::float2_t:
    case Type::uint2_t:
    case Type::int2_t:
    case Type::ushort4_t:
    case Type::short4_t:
      return 8;
    case Type::float3_t:
    case Type::uint3_t:
    case Type::int3_t:
      return 12;
    case Type::float4_t:
    case Type::uint4_t:
    case Type::int4_t:
      return 16;
    case Type::float3x3_t:
      return 36 + 3 * 4;
    case Type::float4x4_t:
      return 64;
    case Type::uchar_t:
    case Type::char_t:
      return 1;
    case Type::uchar2_t:
    case Type::char2_t:
    case Type::ushort_t:
    case Type::short_t:
      return 2;
    case Type::uchar3_t:
    case Type::char3_t:
      return 3;
  }
  BLI_assert(false);
  return -1;
}

static int CalcPushConstantsSize(const std::vector<UniformConstant> &constants)
{
  int size_prev = 0;
  int size_last = 0;
  for (const UniformConstant &uni : constants) {
    int pad = 0;
    int size = ConstantTypeSize(uni.type);
    if (size_last && size_last != size) {
      int pack = (size == 8) ? 8 : 16;
      if (size_last < size) {
        pad = pack - (size_last % pack);
      }
      else {
        pad = size_prev % pack;
      }
    }
    else if (size == 12) {
      pad = 4;
    }
    size_prev += pad + size;
    size_last = size;
  }
  return size_prev + (size_prev % 16);
}

bool RAS_Shader::LinkProgram()
{
  std::string vert = GetParsedProgram(VERTEX_PROGRAM);
  std::string frag = GetParsedProgram(FRAGMENT_PROGRAM);
  std::string geom = GetParsedProgram(GEOMETRY_PROGRAM);

  if (m_progs[VERTEX_PROGRAM].empty() || m_progs[FRAGMENT_PROGRAM].empty()) {
    CM_Error("invalid GLSL sources.");
    return false;
  }

  if (vert.empty()) {
    CM_Error("Parsed vertex shader is empty!");
  }
  if (frag.empty()) {
    CM_Error("Parsed fragment shader is empty!");
  }

  m_ubo = GPU_uniformbuf_create_ex(sizeof(m_uboData), nullptr, "g_data");

  // Use "pyGPU_Shader"
  ShaderCreateInfo info("pyGPU_Shader");

  // === Typedef header (structure bgl_Data) ===
  std::string typedef_header = "struct bgl_Data {float width; float height; vec4 coo_offset[9];};";
  typedef_header += "\n";

  // === Interface ===
  StageInterfaceInfo iface("bge_interface", "");
  iface.smooth(Type::float4_t, "bgl_TexCoord");
  info.vertex_out(iface);

  // === Ressources ===
  info.uniform_buf(0, "bgl_Data", "g_data", Frequency::BATCH);

  // Force info_name
  for (auto &res : info.batch_resources_) {
    res.info_name = "pyGPU_Shader";
  }
  for (auto &res : info.pass_resources_) {
    res.info_name = "pyGPU_Shader";
  }
  for (auto &res : info.geometry_resources_) {
    res.info_name = "pyGPU_Shader";
  }

  // Samplers
  for (std::pair<int, std::string> &sampler : m_samplerUniforms) {
    info.sampler(sampler.first, ImageType::Float2D, sampler.second);
  }
  info.sampler(8, ImageType::Float2D, "bgl_RenderedTexture");
  info.sampler(9, ImageType::Float2D, "bgl_DepthTexture");

  // Push constants
  for (UniformConstant &constant : m_constantUniforms) {
    info.push_constant(constant.type, constant.name);
  }

  int size = CalcPushConstantsSize(m_constantUniforms);
  if (size > 128) {
    CM_Error("Push constants size exceeds 128 bytes");
  }

  info.fragment_out(0, Type::float4_t, "fragColor");

  // === Includes ===
  blender::Vector<blender::StringRefNull> includes = {
      "draw_colormanagement_lib.glsl",
      "gpu_shader_python_typedef_lib.glsl",
  };

  auto add_resource_macros = [&](const std::string &input_src) -> std::string {
    std::string processed_str;
    processed_str += "#ifdef CREATE_INFO_RES_PASS_pyGPU_Shader\n";
    processed_str += "CREATE_INFO_RES_PASS_pyGPU_Shader\n";
    processed_str += "#endif\n";
    processed_str += "#ifdef CREATE_INFO_RES_BATCH_pyGPU_Shader\n";
    processed_str += "CREATE_INFO_RES_BATCH_pyGPU_Shader\n";
    processed_str += "#endif\n";
    processed_str += "#ifdef CREATE_INFO_RES_GEOMETRY_pyGPU_Shader\n";
    processed_str += "CREATE_INFO_RES_GEOMETRY_pyGPU_Shader\n";
    processed_str += "#endif\n";
    processed_str += "\n";
    processed_str += input_src;
    return processed_str;
  };
  // === Typedef header ===
  info.generated_sources.append({"gpu_shader_python_typedef_lib.glsl", {}, typedef_header});

  // === Vertex ===
  info.vertex_source("gpu_shader_python_vert.glsl");
  std::string processed_vert = add_resource_macros(vert);
  info.generated_sources.append({"gpu_shader_python_vert.glsl", includes, processed_vert});

  // === Fragment ===
  info.fragment_source("gpu_shader_python_frag.glsl");
  std::string processed_frag = add_resource_macros(frag);

  CM_Debug("Processed fragment shader length: " << processed_frag.length());

  info.generated_sources.append({"gpu_shader_python_frag.glsl", includes, processed_frag});

  if (m_error) {
    goto program_error;
  }

  m_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);

  if (!m_shader) {
    CM_Error("GPU_shader_create_from_info returned nullptr");
    goto program_error;
  }

  m_error = 0;
  return true;

program_error: {
  CM_Error("Shader compilation failed");
  m_use = 0;
  m_error = 1;
  return false;
}
}

void RAS_Shader::ValidateProgram()
{
}

bool RAS_Shader::GetError()
{
  return m_error;
}

blender::gpu::Shader *RAS_Shader::GetGPUShader()
{
  return m_shader;
}

void RAS_Shader::SetSampler(int loc, int unit)
{
  //GPU_shader_uniform_int(m_shader, loc, unit);
}

void RAS_Shader::SetProg(bool enable)
{
  if (m_shader && enable) {
    immBindShader(m_shader);
  }
  else {
    immUnbindProgram();
  }
}

void RAS_Shader::SetEnabled(bool enabled)
{
  m_use = enabled;
}

bool RAS_Shader::GetEnabled() const
{
  return m_use;
}

void RAS_Shader::Update(RAS_Rasterizer *rasty, const MT_Matrix4x4 model)
{
  if (!Ok() || m_preDef.empty()) {
    return;
  }

  const MT_Matrix4x4 &view = rasty->GetViewMatrix();

  for (RAS_DefUniform *uni : m_preDef) {
    if (uni->m_loc == -1) {
      continue;
    }

    switch (uni->m_type) {
      case MODELMATRIX: {
        SetUniform(uni->m_loc, model);
        break;
      }
      case MODELMATRIX_TRANSPOSE: {
        SetUniform(uni->m_loc, model, true);
        break;
      }
      case MODELMATRIX_INVERSE: {
        SetUniform(uni->m_loc, model.inverse());
        break;
      }
      case MODELMATRIX_INVERSETRANSPOSE: {
        SetUniform(uni->m_loc, model.inverse(), true);
        break;
      }
      case MODELVIEWMATRIX: {
        SetUniform(uni->m_loc, view * model);
        break;
      }
      case MODELVIEWMATRIX_TRANSPOSE: {
        MT_Matrix4x4 mat(view * model);
        SetUniform(uni->m_loc, mat, true);
        break;
      }
      case MODELVIEWMATRIX_INVERSE: {
        MT_Matrix4x4 mat(view * model);
        SetUniform(uni->m_loc, mat.inverse());
        break;
      }
      case MODELVIEWMATRIX_INVERSETRANSPOSE: {
        MT_Matrix4x4 mat(view * model);
        SetUniform(uni->m_loc, mat.inverse(), true);
        break;
      }
      case CAM_POS: {
        MT_Vector3 pos(rasty->GetCameraPosition());
        SetUniform(uni->m_loc, pos);
        break;
      }
      case VIEWMATRIX: {
        SetUniform(uni->m_loc, view);
        break;
      }
      case VIEWMATRIX_TRANSPOSE: {
        SetUniform(uni->m_loc, view, true);
        break;
      }
      case VIEWMATRIX_INVERSE: {
        SetUniform(uni->m_loc, view.inverse());
        break;
      }
      case VIEWMATRIX_INVERSETRANSPOSE: {
        SetUniform(uni->m_loc, view.inverse(), true);
        break;
      }
      case CONSTANT_TIMER: {
        SetUniform(uni->m_loc, (float)rasty->GetTime());
        break;
      }
      case EYE: {
        SetUniform(uni->m_loc,
                   (rasty->GetEye() == RAS_Rasterizer::RAS_STEREO_LEFTEYE) ? 0.0f : 0.5f);
      }
      default:
        break;
    }
  }
}

int RAS_Shader::GetAttribLocation(const std::string &name)
{
  return GPU_shader_get_attribute(m_shader, name.c_str());
}

//void RAS_Shader::BindAttributes(const std::unordered_map<int, std::string> &attrs)
//{
//  const unsigned short len = attrs.size();
//  int *locations = (int *)BLI_array_alloca(locations, len);
//  const char **names = (const char **)BLI_array_alloca(names, len);
//
//  unsigned short i = 0;
//  for (const std::pair<int, std::string> &pair : attrs) {
//    locations[i] = pair.first;
//    names[i] = pair.second.c_str();
//    ++i;
//  }
//
//  GPU_shader_bind_attributes(m_shader, locations, (const char **)names, len);
//}

int RAS_Shader::GetUniformLocation(const std::string &name, bool debug)
{
  BLI_assert(m_shader != nullptr);
  int location = GPU_shader_get_uniform(m_shader, name.c_str());

  if (location == -1 && debug) {
    CM_Error("invalid uniform value: " << name << ".");
  }

  return location;
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector2 &vec)
{
  float value[2];
  vec.getValue(value);
  GPU_shader_uniform_float_ex(m_shader, uniform, 2, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector3 &vec)
{
  float value[3];
  vec.getValue(value);
  GPU_shader_uniform_float_ex(m_shader, uniform, 3, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Vector4 &vec)
{
  float value[4];
  vec.getValue(value);
  GPU_shader_uniform_float_ex(m_shader, uniform, 4, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const unsigned int &val)
{
  GPU_shader_uniform_int_ex(m_shader, uniform, 1, 1, (int *)&val);
}

void RAS_Shader::SetUniform(int uniform, const int val)
{
  GPU_shader_uniform_int_ex(m_shader, uniform, 1, 1, (int *)&val);
}

void RAS_Shader::SetUniform(int uniform, const float &val)
{
  GPU_shader_uniform_float_ex(m_shader, uniform, 1, 1, (float *)&val);
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose)
{
  float value[16];
  // note: getValue gives back column major as needed by OpenGL
  vec.getValue(value);
  GPU_shader_uniform_float_ex(m_shader, uniform, 16, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose)
{
  float value[9];
  value[0] = (float)vec[0][0];
  value[1] = (float)vec[1][0];
  value[2] = (float)vec[2][0];
  value[3] = (float)vec[0][1];
  value[4] = (float)vec[1][1];
  value[5] = (float)vec[2][1];
  value[6] = (float)vec[0][2];
  value[7] = (float)vec[1][2];
  value[8] = (float)vec[2][2];
  GPU_shader_uniform_float_ex(m_shader, uniform, 9, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const float *val, int len)
{
  if (len >= 2 && len <= 4) {
    GPU_shader_uniform_float_ex(m_shader, uniform, len, 1, (float *)val);
  }
  else {
    BLI_assert(0);
  }
}

void RAS_Shader::SetUniform(int uniform, const int *val, int len)
{
  if (len >= 2 && len <= 4) {
    GPU_shader_uniform_int_ex(m_shader, uniform, len, 1, (int *)val);
  }
  else {
    BLI_assert(0);
  }
}
