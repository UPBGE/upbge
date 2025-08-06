/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <atomic>

#include "device/device.h"

#include "util/array.h"
#include "util/set.h"
#include "util/string.h"

#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#ifdef WITH_OSL
#  include <OSL/llvm_util.h>
#  include <OSL/oslcomp.h>
#  include <OSL/oslexec.h>
#  include <OSL/oslquery.h>
#endif

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;
class ImageManager;
class OSLRenderServices;
struct OSLGlobals;
class Scene;
class ShaderGraph;
class ShaderNode;
class ShaderInput;
class ShaderOutput;

#ifdef WITH_OSL

/* OSL Shader Info
 * to auto detect closures in the shader for MIS and transparent shadows */

struct OSLShaderInfo {
  OSLShaderInfo() = default;

  OSL::OSLQuery query;
  bool has_surface_emission = false;
  bool has_surface_transparent = false;
  bool has_surface_bssrdf = false;
};

#endif

class OSLManager {
 public:
  OSLManager(Device *device);
  ~OSLManager();

  static void free_memory();

  void reset(Scene *scene);

  void device_update_pre(Device *device, Scene *scene);
  void device_update_post(Device *device,
                          Scene *scene,
                          Progress &progress,
                          const bool reload_kernels);
  void device_free(Device *device, DeviceScene *dscene, Scene *scene);

#ifdef WITH_OSL
  /* osl compile and query */
  static bool osl_compile(const string &inputfile, const string &outputfile);
  static bool osl_query(OSL::OSLQuery &query, const string &filepath);

  /* shader file loading, all functions return pointer to hash string if found */
  const char *shader_test_loaded(const string &hash);
  const char *shader_load_bytecode(const string &hash, const string &bytecode);
  const char *shader_load_filepath(string filepath);
  OSLShaderInfo *shader_loaded_info(const string &hash);

  OSL::ShadingSystem *get_shading_system(Device *sub_device);
  OSL::TextureSystem *get_texture_system();
  static void foreach_osl_device(Device *device,
                                 const std::function<void(Device *, OSLGlobals *)> &callback);
#endif

  void tag_update();
  bool need_update() const;

 private:
#ifdef WITH_OSL
  void texture_system_init();
  void texture_system_free();

  void shading_system_init();
  void shading_system_free();

  void foreach_shading_system(const std::function<void(OSL::ShadingSystem *)> &callback);
  void foreach_render_services(const std::function<void(OSLRenderServices *)> &callback);

  Device *device_;
  map<string, OSLShaderInfo> loaded_shaders;

  std::shared_ptr<OSL::TextureSystem> ts;
  map<DeviceType, std::shared_ptr<OSL::ShadingSystem>> ss_map;

  bool need_update_;
#endif
};

#ifdef WITH_OSL

/* Shader Manage */

class OSLShaderManager : public ShaderManager {
 public:
  OSLShaderManager() = default;
  ~OSLShaderManager() override = default;

  bool use_osl() override
  {
    return true;
  }

  uint64_t get_attribute_id(ustring name) override;
  uint64_t get_attribute_id(AttributeStandard std) override;

  void device_update_specific(Device *device,
                              DeviceScene *dscene,
                              Scene *scene,
                              Progress &progress) override;
  void device_free(Device *device, DeviceScene *dscene, Scene *scene) override;

  /* create OSL node using OSLQuery */
  static OSLNode *osl_node(ShaderGraph *graph,
                           Scene *scene,
                           const std::string &filepath,
                           const std::string &bytecode_hash = "",
                           const std::string &bytecode = "");

  /* Get image slots used by OSL services on device. */
  static void osl_image_slots(Device *device, ImageManager *image_manager, set<int> &image_slots);
};

#endif

/* Graph Compiler */

class OSLCompiler {
 public:
#ifdef WITH_OSL
  OSLCompiler(OSL::ShadingSystem *ss, Scene *scene);
#endif
  void compile(Shader *shader);

  void add(ShaderNode *node, const char *name, bool isfilepath = false);

  void parameter(ShaderNode *node, const char *name);

  void parameter(const char *name, const float f);
  void parameter_color(const char *name, const float3 f);
  void parameter_vector(const char *name, const float3 f);
  void parameter_normal(const char *name, const float3 f);
  void parameter_point(const char *name, const float3 f);
  void parameter(const char *name, const int f);
  void parameter(const char *name, const char *s);
  void parameter(const char *name, ustring str);
  void parameter(const char *name, const Transform &tfm);

  void parameter_array(const char *name, const float f[], int arraylen);
  void parameter_color_array(const char *name, const array<float3> &f);

  void parameter_attribute(const char *name, ustring s);

  void parameter_texture(const char *name, ustring filename, ustring colorspace);
  void parameter_texture(const char *name, const ImageHandle &handle);
  void parameter_texture_ies(const char *name, const int svm_slot);

  ShaderType output_type()
  {
    return current_type;
  }

  bool background;
  Scene *scene;

 private:
#ifdef WITH_OSL
  string id(ShaderNode *node);
  OSL::ShaderGroupRef compile_type(Shader *shader, ShaderGraph *graph, ShaderType type);
  bool node_skip_input(ShaderNode *node, ShaderInput *input);
  string compatible_name(ShaderNode *node, ShaderInput *input);
  string compatible_name(ShaderNode *node, ShaderOutput *output);

  void find_dependencies(ShaderNodeSet &dependencies, ShaderInput *input);
  void generate_nodes(const ShaderNodeSet &nodes);

  OSLRenderServices *services;
  OSL::ShadingSystem *ss;
  OSL::ShaderGroupRef current_group;
#endif

  ShaderType current_type;
  Shader *current_shader;

  static std::atomic<int> texture_shared_unique_id;
};

CCL_NAMESPACE_END
