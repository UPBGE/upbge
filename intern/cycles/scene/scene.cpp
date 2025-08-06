/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdlib>

#include "bvh/bvh.h"
#include "device/device.h"
#include "scene/alembic.h"
#include "scene/background.h"
#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/curves.h"
#include "scene/devicescene.h"
#include "scene/film.h"
#include "scene/hair.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/osl.h"
#include "scene/particles.h"
#include "scene/pointcloud.h"
#include "scene/procedural.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/svm.h"
#include "scene/tables.h"
#include "scene/volume.h"
#include "session/session.h"

#include "util/guarded_allocator.h"
#include "util/log.h"
#include "util/progress.h"

CCL_NAMESPACE_BEGIN

Scene ::Scene(const SceneParams &params_, Device *device)
    : name("Scene"),
      default_surface(nullptr),
      default_volume(nullptr),
      default_light(nullptr),
      default_background(nullptr),
      default_empty(nullptr),
      device(device),
      dscene(device),
      params(params_),
      update_stats(nullptr),
      kernels_loaded(false),
      /* TODO(sergey): Check if it's indeed optimal value for the split kernel.
       */
      max_closure_global(1)
{
  memset((void *)&dscene.data, 0, sizeof(dscene.data));

  osl_manager = make_unique<OSLManager>(device);
  shader_manager = ShaderManager::create(device->info.has_osl ? params.shadingsystem :
                                                                SHADINGSYSTEM_SVM);

  light_manager = make_unique<LightManager>();
  geometry_manager = make_unique<GeometryManager>();
  object_manager = make_unique<ObjectManager>();
  image_manager = make_unique<ImageManager>(device->info);
  particle_system_manager = make_unique<ParticleSystemManager>();
  bake_manager = make_unique<BakeManager>();
  procedural_manager = make_unique<ProceduralManager>();

  /* Create nodes after managers, since create_node() can tag the managers. */
  camera = create_node<Camera>();
  dicing_camera = create_node<Camera>();
  lookup_tables = make_unique<LookupTables>();
  film = create_node<Film>();
  background = create_node<Background>();
  integrator = create_node<Integrator>();

  ccl::Film::add_default(this);
  ccl::ShaderManager::add_default(this);
}

Scene::~Scene()
{
  free_memory(true);
}

void Scene::free_memory(bool final)
{
  bvh.reset();

  /* The order of deletion is important to make sure data is freed based on
   * possible dependencies as the Nodes' reference counts are decremented in the
   * destructors:
   *
   * - Procedurals can create and hold pointers to any other types.
   * - Objects can hold pointers to Geometries and ParticleSystems
   * - Lights and Geometries can hold pointers to Shaders.
   *
   * Similarly, we first delete all nodes and their associated device data, and
   * then the managers and their associated device data.
   */
  procedurals.clear();
  objects.clear();
  geometry.clear();
  particle_systems.clear();
  passes.clear();

  if (device) {
    camera->device_free(device, &dscene, this);
    film->device_free(device, &dscene, this);
    background->device_free(device, &dscene);
    integrator->device_free(device, &dscene, true);
  }

  if (final) {
    cameras.clear();
    integrators.clear();
    films.clear();
    backgrounds.clear();

    camera = nullptr;
    dicing_camera = nullptr;
    integrator = nullptr;
    film = nullptr;
    background = nullptr;
  }

  /* Delete Shaders after every other nodes to ensure that we do not try to
   * decrement the reference count on some dangling pointer. */
  shaders.clear();

  /* Now that all nodes have been deleted, we can safely delete managers and
   * device data. */
  if (device) {
    object_manager->device_free(device, &dscene, true);
    geometry_manager->device_free(device, &dscene, true);
    shader_manager->device_free(device, &dscene, this);
    osl_manager->device_free(device, &dscene, this);
    light_manager->device_free(device, &dscene);

    particle_system_manager->device_free(device, &dscene);

    bake_manager->device_free(device, &dscene);

    if (final) {
      image_manager->device_free(device);
    }
    else {
      image_manager->device_free_builtin(device);
    }

    lookup_tables->device_free(device, &dscene);
  }

  if (final) {
    lookup_tables.reset();
    object_manager.reset();
    geometry_manager.reset();
    shader_manager.reset();
    osl_manager.reset();
    light_manager.reset();
    particle_system_manager.reset();
    image_manager.reset();
    bake_manager.reset();
    update_stats.reset();
    procedural_manager.reset();
  }
}

void Scene::device_update(Device *device_, Progress &progress)
{
  if (!device) {
    device = device_;
  }

  const bool print_stats = need_data_update();

  if (update_stats) {
    update_stats->clear();
  }

  const scoped_callback_timer timer([this, print_stats](double time) {
    if (update_stats) {
      update_stats->scene.times.add_entry({"device_update", time});

      if (print_stats) {
        printf("Update statistics:\n%s\n", update_stats->full_report().c_str());
      }
    }
  });

  /* The order of updates is important, because there's dependencies between
   * the different managers, using data computed by previous managers. */

  if (film->update_lightgroups(this)) {
    light_manager->tag_update(this, ccl::LightManager::LIGHT_MODIFIED);
    object_manager->tag_update(this, ccl::ObjectManager::OBJECT_MODIFIED);
    background->tag_modified();
  }
  if (film->exposure_is_modified()) {
    integrator->tag_modified();
  }

  /* Compile shaders and get information about features they used. */
  progress.set_status("Updating Shaders");
  osl_manager->device_update_pre(device, this);
  shader_manager->device_update_pre(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Passes. After shader manager as this depends on the shaders. */
  film->update_passes(this);

  /* Update kernel features. After shaders and passes since those affect features. */
  update_kernel_features();

  /* Load render kernels, before uploading most data to the GPU, and before displacement and
   * background light need to run kernels.
   *
   * Do it outside of the scene mutex since the heavy part of the loading (i.e. kernel
   * compilation) does not depend on the scene and some other functionality (like display
   * driver) might be waiting on the scene mutex to synchronize display pass. */
  mutex.unlock();
  const bool kernels_reloaded = load_kernels(progress);
  mutex.lock();

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Upload shaders to GPU and compile OSL kernels, after kernels have been loaded. */
  shader_manager->device_update_post(device, &dscene, this, progress);
  osl_manager->device_update_post(device, this, progress, kernels_reloaded);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  procedural_manager->update(this, progress);

  if (progress.get_cancel()) {
    return;
  }

  progress.set_status("Updating Background");
  background->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Camera will be used by adaptive subdivision, so do early. */
  progress.set_status("Updating Camera");
  camera->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  geometry_manager->device_update_preprocess(device, this, progress);
  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Update objects after geometry preprocessing. */
  progress.set_status("Updating Objects");
  object_manager->device_update(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Particle Systems");
  particle_system_manager->device_update(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Camera and shaders must be ready here for adaptive subdivision and displacement. */
  progress.set_status("Updating Meshes");
  geometry_manager->device_update(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Update object flags with final geometry. */
  progress.set_status("Updating Objects Flags");
  object_manager->device_update_flags(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Update BVH primitive objects with final geometry. */
  progress.set_status("Updating Primitive Offsets");
  object_manager->device_update_prim_offsets(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Images last, as they should be more likely to use host memory fallback than geometry.
   * Some images may have been uploaded early for displacement already at this point. */
  progress.set_status("Updating Images");
  image_manager->device_update(device, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Camera Volume");
  camera->device_update_volume(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Lookup Tables");
  lookup_tables->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Light manager needs shaders and final meshes for triangles in light tree. */
  progress.set_status("Updating Lights");
  light_manager->device_update(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Integrator");
  integrator->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Film");
  film->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  /* Update lookup tables a second time for film tables. */
  progress.set_status("Updating Lookup Tables");
  lookup_tables->device_update(device, &dscene, this);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  progress.set_status("Updating Baking");
  bake_manager->device_update(device, &dscene, this, progress);

  if (progress.get_cancel() || device->have_error()) {
    return;
  }

  if (device->have_error() == false) {
    dscene.data.volume_stack_size = get_volume_stack_size();

    progress.set_status("Updating Device", "Writing constant memory");
    device->const_copy_to("data", &dscene.data, sizeof(dscene.data));
  }

  device->optimize_for_scene(this);

  if (print_stats) {
    const size_t mem_used = util_guarded_get_mem_used();
    const size_t mem_peak = util_guarded_get_mem_peak();

    LOG_INFO << "System memory statistics after full device sync:\n"
             << "  Usage: " << string_human_readable_number(mem_used) << " ("
             << string_human_readable_size(mem_used) << ")\n"
             << "  Peak: " << string_human_readable_number(mem_peak) << " ("
             << string_human_readable_size(mem_peak) << ")";
  }
}

Scene::MotionType Scene::need_motion() const
{
  if (integrator->get_motion_blur()) {
    return MOTION_BLUR;
  }
  if (Pass::contains(passes, PASS_MOTION)) {
    return MOTION_PASS;
  }
  return MOTION_NONE;
}

float Scene::motion_shutter_time()
{
  if (need_motion() == Scene::MOTION_PASS) {
    return 2.0f;
  }
  return camera->get_shuttertime();
}

bool Scene::need_global_attribute(AttributeStandard std)
{
  if (std == ATTR_STD_UV) {
    return Pass::contains(passes, PASS_UV);
  }
  if (std == ATTR_STD_MOTION_VERTEX_POSITION) {
    return need_motion() != MOTION_NONE;
  }
  if (std == ATTR_STD_MOTION_VERTEX_NORMAL) {
    return need_motion() == MOTION_BLUR;
  }
  if (std == ATTR_STD_VOLUME_VELOCITY || std == ATTR_STD_VOLUME_VELOCITY_X ||
      std == ATTR_STD_VOLUME_VELOCITY_Y || std == ATTR_STD_VOLUME_VELOCITY_Z)
  {
    return need_motion() != MOTION_NONE;
  }

  return false;
}

void Scene::need_global_attributes(AttributeRequestSet &attributes)
{
  for (int std = ATTR_STD_NONE; std < ATTR_STD_NUM; std++) {
    if (need_global_attribute((AttributeStandard)std)) {
      attributes.add((AttributeStandard)std);
    }
  }
}

bool Scene::need_update()
{
  return (need_reset() || film->is_modified());
}

bool Scene::need_data_update()
{
  return (background->is_modified() || image_manager->need_update() ||
          object_manager->need_update() || geometry_manager->need_update() ||
          light_manager->need_update() || lookup_tables->need_update() ||
          integrator->is_modified() || shader_manager->need_update() ||
          particle_system_manager->need_update() || bake_manager->need_update() ||
          film->is_modified() || procedural_manager->need_update());
}

bool Scene::need_reset(const bool check_camera)
{
  return need_data_update() || (check_camera && camera->is_modified());
}

void Scene::reset()
{
  osl_manager->reset(this);
  ShaderManager::add_default(this);

  /* ensure all objects are updated */
  camera->tag_modified();
  dicing_camera->tag_modified();
  film->tag_modified();
  background->tag_modified();

  background->tag_update(this);
  integrator->tag_update(this, Integrator::UPDATE_ALL);
  object_manager->tag_update(this, ObjectManager::UPDATE_ALL);
  geometry_manager->tag_update(this, GeometryManager::UPDATE_ALL);
  light_manager->tag_update(this, LightManager::UPDATE_ALL);
  particle_system_manager->tag_update(this);
  procedural_manager->tag_update();
}

void Scene::device_free()
{
  free_memory(false);
}

void Scene::collect_statistics(RenderStats *stats)
{
  geometry_manager->collect_statistics(this, stats);
  image_manager->collect_statistics(stats);
}

void Scene::enable_update_stats()
{
  if (!update_stats) {
    update_stats = make_unique<SceneUpdateStats>();
  }
}

void Scene::update_kernel_features()
{
  if (!need_update()) {
    return;
  }

  /* These features are not being tweaked as often as shaders,
   * so could be done selective magic for the viewport as well. */
  uint kernel_features = shader_manager->get_kernel_features(this);

  const bool use_motion = need_motion() == Scene::MotionType::MOTION_BLUR;
  kernel_features |= KERNEL_FEATURE_PATH_TRACING;
  if (params.hair_shape == CURVE_THICK || params.hair_shape == CURVE_THICK_LINEAR) {
    kernel_features |= KERNEL_FEATURE_HAIR_THICK;
  }

  /* Track the max prim count in case the backend needs to rebuild BVHs or
   * kernels to support different limits. */
  size_t kernel_max_prim_count = 0;

  /* Figure out whether the scene will use shader ray-trace we need at least
   * one caustic light, one caustic caster and one caustic receiver to use
   * and enable the MNEE code path. */
  bool has_caustics_receiver = false;
  bool has_caustics_caster = false;
  bool has_caustics_light = false;

  for (Object *object : objects) {
    if (object->get_is_caustics_caster()) {
      has_caustics_caster = true;
    }
    else if (object->get_is_caustics_receiver()) {
      has_caustics_receiver = true;
    }
    Geometry *geom = object->get_geometry();
    if (use_motion) {
      if (object->use_motion() || geom->get_use_motion_blur()) {
        kernel_features |= KERNEL_FEATURE_OBJECT_MOTION;
      }
    }
    if (object->get_is_shadow_catcher() && !geom->is_light()) {
      kernel_features |= KERNEL_FEATURE_SHADOW_CATCHER;
    }
    if (geom->is_hair()) {
      kernel_features |= KERNEL_FEATURE_HAIR;
      kernel_max_prim_count = max(kernel_max_prim_count,
                                  static_cast<Hair *>(geom)->num_segments());
    }
    else if (geom->is_pointcloud()) {
      kernel_features |= KERNEL_FEATURE_POINTCLOUD;
      kernel_max_prim_count = max(kernel_max_prim_count,
                                  static_cast<PointCloud *>(geom)->num_points());
    }
    else if (geom->is_mesh()) {
      kernel_max_prim_count = max(kernel_max_prim_count,
                                  static_cast<Mesh *>(geom)->num_triangles());
    }
    else if (geom->is_light()) {
      const Light *light = static_cast<const Light *>(object->get_geometry());
      if (light->get_use_caustics()) {
        has_caustics_light = true;
      }
    }
    if (object->has_light_linking()) {
      kernel_features |= KERNEL_FEATURE_LIGHT_LINKING;
    }
    if (object->has_shadow_linking()) {
      kernel_features |= KERNEL_FEATURE_SHADOW_LINKING;
    }
  }

  dscene.data.integrator.use_caustics = false;
  if (device->info.has_mnee && has_caustics_caster && has_caustics_receiver && has_caustics_light)
  {
    dscene.data.integrator.use_caustics = true;
    kernel_features |= KERNEL_FEATURE_MNEE;
  }

  if (integrator->get_guiding_params(device).use) {
    kernel_features |= KERNEL_FEATURE_PATH_GUIDING;
  }

  if (bake_manager->get_baking()) {
    kernel_features |= KERNEL_FEATURE_BAKING;
  }

  kernel_features |= film->get_kernel_features(this);
  kernel_features |= integrator->get_kernel_features();
  kernel_features |= camera->get_kernel_features();

  dscene.data.kernel_features = kernel_features;

  /* Currently viewport render is faster with higher max_closures, needs
   * investigating. */
  const uint max_closures = (params.background) ? get_max_closure_count() : MAX_CLOSURE;
  dscene.data.max_closures = max_closures;
  dscene.data.max_shaders = shaders.size();

  /* Inform the device of the BVH limits. If this returns true, all BVHs
   * and kernels need to be rebuilt. */
  if (device->set_bvh_limits(objects.size(), kernel_max_prim_count)) {
    kernels_loaded = false;
    for (Geometry *geom : geometry) {
      geom->need_update_rebuild = true;
      geom->tag_modified();
    }
  }
}

bool Scene::update(Progress &progress)
{
  if (!need_update()) {
    return false;
  }

  /* Upload scene data to the GPU. */
  progress.set_status("Updating Scene");
  MEM_GUARDED_CALL(&progress, device_update, device, progress);

  return true;
}

bool Scene::update_camera_resolution(Progress &progress, int width, int height)
{
  if (!camera->set_screen_size(width, height)) {
    return false;
  }

  camera->device_update(device, &dscene, this);

  progress.set_status("Updating Device", "Writing constant memory");
  device->const_copy_to("data", &dscene.data, sizeof(dscene.data));
  return true;
}

static void log_kernel_features(const uint features)
{
  LOG_INFO << "Requested features:";
  LOG_INFO << "Use BSDF " << string_from_bool(features & KERNEL_FEATURE_NODE_BSDF);
  LOG_INFO << "Use Emission " << string_from_bool(features & KERNEL_FEATURE_NODE_EMISSION);
  LOG_INFO << "Use Volume " << string_from_bool(features & KERNEL_FEATURE_NODE_VOLUME);
  LOG_INFO << "Use Bump " << string_from_bool(features & KERNEL_FEATURE_NODE_BUMP);
  LOG_INFO << "Use Voronoi " << string_from_bool(features & KERNEL_FEATURE_NODE_VORONOI_EXTRA);
  LOG_INFO << "Use Shader Raytrace " << string_from_bool(features & KERNEL_FEATURE_NODE_RAYTRACE);
  LOG_INFO << "Use MNEE " << string_from_bool(features & KERNEL_FEATURE_MNEE);
  LOG_INFO << "Use Transparent " << string_from_bool(features & KERNEL_FEATURE_TRANSPARENT);
  LOG_INFO << "Use Denoising " << string_from_bool(features & KERNEL_FEATURE_DENOISING);
  LOG_INFO << "Use Path Tracing " << string_from_bool(features & KERNEL_FEATURE_PATH_TRACING);
  LOG_INFO << "Use Hair " << string_from_bool(features & KERNEL_FEATURE_HAIR);
  LOG_INFO << "Use Pointclouds " << string_from_bool(features & KERNEL_FEATURE_POINTCLOUD);
  LOG_INFO << "Use Object Motion " << string_from_bool(features & KERNEL_FEATURE_OBJECT_MOTION);
  LOG_INFO << "Use Baking " << string_from_bool(features & KERNEL_FEATURE_BAKING);
  LOG_INFO << "Use Subsurface " << string_from_bool(features & KERNEL_FEATURE_SUBSURFACE);
  LOG_INFO << "Use Volume " << string_from_bool(features & KERNEL_FEATURE_VOLUME);
  LOG_INFO << "Use Shadow Catcher " << string_from_bool(features & KERNEL_FEATURE_SHADOW_CATCHER);
  LOG_INFO << "Use Portal Node " << string_from_bool(features & KERNEL_FEATURE_NODE_PORTAL);
}

bool Scene::load_kernels(Progress &progress)
{
  const uint kernel_features = dscene.data.kernel_features;

  if (!kernels_loaded || loaded_kernel_features != kernel_features) {
    progress.set_status("Loading render kernels (may take a few minutes the first time)");

    const scoped_timer timer;

    log_kernel_features(kernel_features);
    if (!device->load_kernels(kernel_features)) {
      string message = device->error_message();
      if (message.empty()) {
        message = "Failed loading render kernel, see console for errors";
      }

      progress.set_error(message);
      progress.set_status(message);
      progress.set_update();
      return false;
    }

    kernels_loaded = true;
    loaded_kernel_features = kernel_features;
    return true;
  }
  return false;
}

int Scene::get_max_closure_count()
{
  if (shader_manager->use_osl()) {
    /* OSL always needs the maximum as we can't predict the
     * number of closures a shader might generate. */
    return MAX_CLOSURE;
  }

  int max_closures = 0;
  for (int i = 0; i < shaders.size(); i++) {
    Shader *shader = shaders[i];
    if (shader->reference_count()) {
      const int num_closures = shader->graph->get_num_closures();
      max_closures = max(max_closures, num_closures);
    }
  }
  max_closure_global = max(max_closure_global, max_closures);

  if (max_closure_global > MAX_CLOSURE) {
    /* This is usually harmless as more complex shader tend to get many
     * closures discarded due to mixing or low weights. We need to limit
     * to MAX_CLOSURE as this is hardcoded in CPU/mega kernels, and it
     * avoids excessive memory usage for split kernels. */
    LOG_WARNING << "Maximum number of closures exceeded: " << max_closure_global << " > "
                << MAX_CLOSURE;

    max_closure_global = MAX_CLOSURE;
  }

  return max_closure_global;
}

int Scene::get_volume_stack_size() const
{
  int volume_stack_size = 0;

  /* Space for background volume and terminator.
   * Don't do optional here because camera ray initialization expects that there
   * is space for at least those elements (avoiding extra condition to check if
   * there is actual volume or not).
   */
  volume_stack_size += 2;

  /* Quick non-expensive check. Can over-estimate maximum possible nested level,
   * but does not require expensive calculation during pre-processing. */
  bool has_volume_object = false;
  for (const Object *object : objects) {
    if (!object->get_geometry()->has_volume) {
      continue;
    }

    if (object->intersects_volume) {
      /* Object intersects another volume, assume it's possible to go deeper in
       * the stack. */
      /* TODO(sergey): This might count nesting twice (A intersects B and B
       * intersects A), but can't think of a computationally cheap algorithm.
       * Dividing my 2 doesn't work because of Venn diagram example with 3
       * circles. */
      ++volume_stack_size;
    }
    else if (!has_volume_object) {
      /* Allocate space for at least one volume object. */
      ++volume_stack_size;
    }

    has_volume_object = true;

    if (volume_stack_size == MAX_VOLUME_STACK_SIZE) {
      break;
    }
  }

  volume_stack_size = min(volume_stack_size, MAX_VOLUME_STACK_SIZE);

  LOG_WORK << "Detected required volume stack size " << volume_stack_size;

  return volume_stack_size;
}

bool Scene::has_shadow_catcher()
{
  if (shadow_catcher_modified_) {
    has_shadow_catcher_ = false;
    for (Object *object : objects) {
      /* Shadow catcher flags on lights only controls effect on other objects, it's
       * not catching shadows itself. This is on by default, so ignore to avoid
       * performance impact when there is no actual shadow catcher. */
      if (object->get_is_shadow_catcher() && !object->get_geometry()->is_light()) {
        has_shadow_catcher_ = true;
        break;
      }
    }

    shadow_catcher_modified_ = false;
  }

  return has_shadow_catcher_;
}

void Scene::tag_shadow_catcher_modified()
{
  shadow_catcher_modified_ = true;
}

template<> Light *Scene::create_node<Light>()
{
  unique_ptr<Light> node = make_unique<Light>();
  Light *node_ptr = node.get();
  node->set_owner(this);
  geometry.push_back(std::move(node));
  light_manager->tag_update(this, LightManager::LIGHT_ADDED);
  return node_ptr;
}

template<> Mesh *Scene::create_node<Mesh>()
{
  unique_ptr<Mesh> node = make_unique<Mesh>();
  Mesh *node_ptr = node.get();
  node->set_owner(this);
  geometry.push_back(std::move(node));
  geometry_manager->tag_update(this, GeometryManager::MESH_ADDED);
  return node_ptr;
}

template<> Hair *Scene::create_node<Hair>()
{
  unique_ptr<Hair> node = make_unique<Hair>();
  Hair *node_ptr = node.get();
  node->set_owner(this);
  geometry.push_back(std::move(node));
  geometry_manager->tag_update(this, GeometryManager::HAIR_ADDED);
  return node_ptr;
}

template<> Volume *Scene::create_node<Volume>()
{
  unique_ptr<Volume> node = make_unique<Volume>();
  Volume *node_ptr = node.get();
  node->set_owner(this);
  geometry.push_back(std::move(node));
  geometry_manager->tag_update(this, GeometryManager::MESH_ADDED);
  return node_ptr;
}

template<> PointCloud *Scene::create_node<PointCloud>()
{
  unique_ptr<PointCloud> node = make_unique<PointCloud>();
  PointCloud *node_ptr = node.get();
  node->set_owner(this);
  geometry.push_back(std::move(node));
  geometry_manager->tag_update(this, GeometryManager::POINT_ADDED);
  return node_ptr;
}

template<> Object *Scene::create_node<Object>()
{
  unique_ptr<Object> node = make_unique<Object>();
  Object *node_ptr = node.get();
  node->set_owner(this);
  objects.push_back(std::move(node));
  object_manager->tag_update(this, ObjectManager::OBJECT_ADDED);
  return node_ptr;
}

template<> ParticleSystem *Scene::create_node<ParticleSystem>()
{
  unique_ptr<ParticleSystem> node = make_unique<ParticleSystem>();
  ParticleSystem *node_ptr = node.get();
  node->set_owner(this);
  particle_systems.push_back(std::move(node));
  particle_system_manager->tag_update(this);
  return node_ptr;
}

template<> Shader *Scene::create_node<Shader>()
{
  unique_ptr<Shader> node = make_unique<Shader>();
  Shader *node_ptr = node.get();
  node->set_owner(this);
  shaders.push_back(std::move(node));
  shader_manager->tag_update(this, ShaderManager::SHADER_ADDED);
  return node_ptr;
}

template<> AlembicProcedural *Scene::create_node<AlembicProcedural>()
{
#ifdef WITH_ALEMBIC
  unique_ptr<AlembicProcedural> node = make_unique<AlembicProcedural>();
  AlembicProcedural *node_ptr = node.get();
  node->set_owner(this);
  procedurals.push_back(std::move(node));
  procedural_manager->tag_update();
  return node_ptr;
#else
  return nullptr;
#endif
}

template<> Pass *Scene::create_node<Pass>()
{
  unique_ptr<Pass> node = make_unique<Pass>();
  Pass *node_ptr = node.get();
  node->set_owner(this);
  passes.push_back(std::move(node));
  film->tag_modified();
  return node_ptr;
}

template<> Camera *Scene::create_node<Camera>()
{
  unique_ptr<Camera> node = make_unique<Camera>();
  Camera *node_ptr = node.get();
  node->set_owner(this);
  cameras.push_back(std::move(node));
  return node_ptr;
}

template<> Integrator *Scene::create_node<Integrator>()
{
  unique_ptr<Integrator> node = make_unique<Integrator>();
  Integrator *node_ptr = node.get();
  node->set_owner(this);
  integrators.push_back(std::move(node));
  return node_ptr;
}

template<> Background *Scene::create_node<Background>()
{
  unique_ptr<Background> node = make_unique<Background>();
  Background *node_ptr = node.get();
  node->set_owner(this);
  backgrounds.push_back(std::move(node));
  return node_ptr;
}

template<> Film *Scene::create_node<Film>()
{
  unique_ptr<Film> node = make_unique<Film>();
  Film *node_ptr = node.get();
  node->set_owner(this);
  films.push_back(std::move(node));
  return node_ptr;
}

template<> void Scene::delete_node(Light *node)
{
  assert(node->get_owner() == this);
  geometry.erase_by_swap(node);
  light_manager->tag_update(this, LightManager::LIGHT_REMOVED);
}

template<> void Scene::delete_node(Mesh *node)
{
  assert(node->get_owner() == this);
  geometry.erase_by_swap(node);
  geometry_manager->tag_update(this, GeometryManager::MESH_REMOVED);
}

template<> void Scene::delete_node(Hair *node)
{
  assert(node->get_owner() == this);
  geometry.erase_by_swap(node);
  geometry_manager->tag_update(this, GeometryManager::HAIR_REMOVED);
}

template<> void Scene::delete_node(Volume *node)
{
  assert(node->get_owner() == this);
  geometry.erase_by_swap(node);
  geometry_manager->tag_update(this, GeometryManager::MESH_REMOVED);
}

template<> void Scene::delete_node(PointCloud *node)
{
  assert(node->get_owner() == this);
  geometry.erase_by_swap(node);
  geometry_manager->tag_update(this, GeometryManager::POINT_REMOVED);
}

template<> void Scene::delete_node(Geometry *node)
{
  assert(node->get_owner() == this);

  uint flag;
  if (node->is_hair()) {
    flag = GeometryManager::HAIR_REMOVED;
  }
  else {
    flag = GeometryManager::MESH_REMOVED;
  }

  geometry.erase_by_swap(node);
  geometry_manager->tag_update(this, flag);
}

template<> void Scene::delete_node(Object *node)
{
  assert(node->get_owner() == this);
  objects.erase_by_swap(node);
  object_manager->tag_update(this, ObjectManager::OBJECT_REMOVED);
}

template<> void Scene::delete_node(ParticleSystem *node)
{
  assert(node->get_owner() == this);
  particle_systems.erase_by_swap(node);
  particle_system_manager->tag_update(this);
}

template<> void Scene::delete_node(Shader *node)
{
  assert(node->get_owner() == this);
  /* don't delete unused shaders, not supported */
  node->clear_reference_count();
}

template<> void Scene::delete_node(Procedural *node)
{
  assert(node->get_owner() == this);
  procedurals.erase_by_swap(node);
  procedural_manager->tag_update();
}

template<> void Scene::delete_node(AlembicProcedural *node)
{
#ifdef WITH_ALEMBIC
  delete_node(static_cast<Procedural *>(node));
#else
  (void)node;
#endif
}

template<> void Scene::delete_node(Pass *node)
{
  assert(node->get_owner() == this);
  passes.erase_by_swap(node);
  film->tag_modified();
}

template<typename T> static void assert_same_owner(const set<T *> &nodes, const NodeOwner *owner)
{
#ifdef NDEBUG
  (void)nodes;
  (void)owner;
#else
  for (const T *node : nodes) {
    assert(node->get_owner() == owner);
  }
#endif
}

template<> void Scene::delete_nodes(const set<Geometry *> &nodes, const NodeOwner *owner)
{
  assert_same_owner(nodes, owner);
  geometry.erase_in_set(nodes);
  geometry_manager->tag_update(this, GeometryManager::GEOMETRY_REMOVED);
  light_manager->tag_update(this, LightManager::LIGHT_REMOVED);
}

template<> void Scene::delete_nodes(const set<Object *> &nodes, const NodeOwner *owner)
{
  assert_same_owner(nodes, owner);
  objects.erase_in_set(nodes);
  object_manager->tag_update(this, ObjectManager::OBJECT_REMOVED);
}

template<> void Scene::delete_nodes(const set<ParticleSystem *> &nodes, const NodeOwner *owner)
{
  assert_same_owner(nodes, owner);
  particle_systems.erase_in_set(nodes);
  particle_system_manager->tag_update(this);
}

template<> void Scene::delete_nodes(const set<Shader *> &nodes, const NodeOwner * /*owner*/)
{
  /* don't delete unused shaders, not supported */
  for (Shader *shader : nodes) {
    shader->clear_reference_count();
  }
}

template<> void Scene::delete_nodes(const set<Procedural *> &nodes, const NodeOwner *owner)
{
  assert_same_owner(nodes, owner);
  procedurals.erase_in_set(nodes);
  procedural_manager->tag_update();
}

template<> void Scene::delete_nodes(const set<Pass *> &nodes, const NodeOwner *owner)
{
  assert_same_owner(nodes, owner);
  passes.erase_in_set(nodes);
  film->tag_modified();
}

CCL_NAMESPACE_END
