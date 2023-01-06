/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2022 Blender Foundation */

#ifdef WITH_METAL

#  include "device/metal/kernel.h"
#  include "device/metal/device_impl.h"
#  include "kernel/device/metal/function_constants.h"
#  include "util/md5.h"
#  include "util/path.h"
#  include "util/tbb.h"
#  include "util/time.h"
#  include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

/* limit to 2 MTLCompiler instances */
int max_mtlcompiler_threads = 2;

const char *kernel_type_as_string(MetalPipelineType pso_type)
{
  switch (pso_type) {
    case PSO_GENERIC:
      return "PSO_GENERIC";
    case PSO_SPECIALIZED_INTERSECT:
      return "PSO_SPECIALIZED_INTERSECT";
    case PSO_SPECIALIZED_SHADE:
      return "PSO_SPECIALIZED_SHADE";
    default:
      assert(0);
  }
  return "";
}

bool kernel_has_intersection(DeviceKernel device_kernel)
{
  return (device_kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST ||
          device_kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW ||
          device_kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE ||
          device_kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK ||
          device_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE ||
          device_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE);
}

struct ShaderCache {
  ShaderCache(id<MTLDevice> _mtlDevice) : mtlDevice(_mtlDevice)
  {
    /* Initialize occupancy tuning LUT. */
    if (MetalInfo::get_device_vendor(mtlDevice) == METAL_GPU_APPLE) {
      switch (MetalInfo::get_apple_gpu_architecture(mtlDevice)) {
        default:
        case APPLE_M2:
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_STATES] = {32, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA] = {832, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST] = {64, 64};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW] = {64, 64};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE] = {704, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY] = {1024, 256};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND] = {64, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW] = {256, 256};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE] = {448, 384};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY] = {1024, 1024};
          break;
        case APPLE_M1:
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_STATES] = {256, 128};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA] = {768, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST] = {512, 128};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW] = {384, 128};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE] = {512, 64};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY] = {512, 256};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND] = {512, 128};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW] = {384, 32};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE] = {576, 384};
          occupancy_tuning[DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY] = {832, 832};
          break;
      }
    }
  }
  ~ShaderCache();

  /* Get the fastest available pipeline for the specified kernel. */
  MetalKernelPipeline *get_best_pipeline(DeviceKernel kernel, const MetalDevice *device);

  /* Non-blocking request for a kernel, optionally specialized to the scene being rendered by
   * device. */
  void load_kernel(DeviceKernel kernel, MetalDevice *device, MetalPipelineType pso_type);

  bool should_load_kernel(DeviceKernel device_kernel,
                          MetalDevice const *device,
                          MetalPipelineType pso_type);

  void wait_for_all();

  friend ShaderCache *get_shader_cache(id<MTLDevice> mtlDevice);

  void compile_thread_func(int thread_index);

  using PipelineCollection = std::vector<unique_ptr<MetalKernelPipeline>>;

  struct OccupancyTuningParameters {
    int threads_per_threadgroup = 0;
    int num_threads_per_block = 0;
  } occupancy_tuning[DEVICE_KERNEL_NUM];

  std::mutex cache_mutex;

  PipelineCollection pipelines[DEVICE_KERNEL_NUM];
  id<MTLDevice> mtlDevice;

  static bool running;
  std::condition_variable cond_var;
  std::deque<MetalKernelPipeline *> request_queue;
  std::vector<std::thread> compile_threads;
  std::atomic_int incomplete_requests = 0;
  std::atomic_int incomplete_specialization_requests = 0;
};

bool ShaderCache::running = true;

const int MAX_POSSIBLE_GPUS_ON_SYSTEM = 8;
using DeviceShaderCache = std::pair<id<MTLDevice>, unique_ptr<ShaderCache>>;
int g_shaderCacheCount = 0;
DeviceShaderCache g_shaderCache[MAX_POSSIBLE_GPUS_ON_SYSTEM];

ShaderCache *get_shader_cache(id<MTLDevice> mtlDevice)
{
  for (int i = 0; i < g_shaderCacheCount; i++) {
    if (g_shaderCache[i].first == mtlDevice) {
      return g_shaderCache[i].second.get();
    }
  }

  static thread_mutex g_shaderCacheCountMutex;
  g_shaderCacheCountMutex.lock();
  int index = g_shaderCacheCount++;
  g_shaderCacheCountMutex.unlock();

  assert(index < MAX_POSSIBLE_GPUS_ON_SYSTEM);
  g_shaderCache[index].first = mtlDevice;
  g_shaderCache[index].second = make_unique<ShaderCache>(mtlDevice);
  return g_shaderCache[index].second.get();
}

ShaderCache::~ShaderCache()
{
  running = false;
  cond_var.notify_all();

  int num_incomplete = int(incomplete_requests);
  if (num_incomplete) {
    /* Shutting down the app with incomplete shader compilation requests. Give 1 second's grace for
     * clean shutdown. */
    metal_printf("ShaderCache busy (incomplete_requests = %d)...\n", num_incomplete);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    num_incomplete = int(incomplete_requests);
  }

  if (num_incomplete && !MetalDeviceKernels::is_benchmark_warmup()) {
    metal_printf("ShaderCache still busy (incomplete_requests = %d). Terminating...\n",
                 num_incomplete);
    std::terminate();
  }

  metal_printf("ShaderCache idle. Shutting down.\n");
  for (auto &thread : compile_threads) {
    thread.join();
  }
}

void ShaderCache::wait_for_all()
{
  while (incomplete_requests > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void ShaderCache::compile_thread_func(int thread_index)
{
  while (running) {

    /* wait for / acquire next request */
    MetalKernelPipeline *pipeline;
    {
      thread_scoped_lock lock(cache_mutex);
      cond_var.wait(lock, [&] { return !running || !request_queue.empty(); });
      if (!running || request_queue.empty()) {
        continue;
      }

      pipeline = request_queue.front();
      request_queue.pop_front();
    }

    /* Service the request. */
    DeviceKernel device_kernel = pipeline->device_kernel;
    MetalPipelineType pso_type = pipeline->pso_type;

    if (MetalDevice::is_device_cancelled(pipeline->originating_device_id)) {
      /* The originating MetalDevice is no longer active, so this request is obsolete. */
      metal_printf("Cancelling compilation of %s (%s)\n",
                   device_kernel_as_string(device_kernel),
                   kernel_type_as_string(pso_type));
    }
    else {
      /* Do the actual compilation. */
      pipeline->compile();

      thread_scoped_lock lock(cache_mutex);
      auto &collection = pipelines[device_kernel];

      /* Cache up to 3 kernel variants with the same pso_type in memory, purging oldest first. */
      int max_entries_of_same_pso_type = 3;
      for (int i = (int)collection.size() - 1; i >= 0; i--) {
        if (collection[i]->pso_type == pso_type) {
          max_entries_of_same_pso_type -= 1;
          if (max_entries_of_same_pso_type == 0) {
            metal_printf("Purging oldest %s:%s kernel from ShaderCache\n",
                         kernel_type_as_string(pso_type),
                         device_kernel_as_string(device_kernel));
            collection.erase(collection.begin() + i);
            break;
          }
        }
      }
      collection.push_back(unique_ptr<MetalKernelPipeline>(pipeline));
    }
    incomplete_requests--;
    if (pso_type != PSO_GENERIC) {
      incomplete_specialization_requests--;
    }
  }
}

bool ShaderCache::should_load_kernel(DeviceKernel device_kernel,
                                     MetalDevice const *device,
                                     MetalPipelineType pso_type)
{
  if (!running) {
    return false;
  }

  if (device_kernel == DEVICE_KERNEL_INTEGRATOR_MEGAKERNEL) {
    /* Skip megakernel. */
    return false;
  }

  if (device_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE) {
    if ((device->kernel_features & KERNEL_FEATURE_NODE_RAYTRACE) == 0) {
      /* Skip shade_surface_raytrace kernel if the scene doesn't require it. */
      return false;
    }
  }

  if (device_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE) {
    if ((device->kernel_features & KERNEL_FEATURE_MNEE) == 0) {
      /* Skip shade_surface_mnee kernel if the scene doesn't require it. */
      return false;
    }
  }

  if (pso_type != PSO_GENERIC) {
    /* Only specialize kernels where it can make an impact. */
    if (device_kernel < DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST ||
        device_kernel > DEVICE_KERNEL_INTEGRATOR_MEGAKERNEL) {
      return false;
    }

    /* Only specialize shading / intersection kernels as requested. */
    bool is_shade_kernel = (device_kernel >= DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND);
    bool is_shade_pso = (pso_type == PSO_SPECIALIZED_SHADE);
    if (is_shade_pso != is_shade_kernel) {
      return false;
    }
  }

  {
    /* check whether the kernel has already been requested / cached */
    thread_scoped_lock lock(cache_mutex);
    for (auto &pipeline : pipelines[device_kernel]) {
      if (pipeline->source_md5 == device->source_md5[pso_type]) {
        return false;
      }
    }
  }

  return true;
}

void ShaderCache::load_kernel(DeviceKernel device_kernel,
                              MetalDevice *device,
                              MetalPipelineType pso_type)
{
  {
    /* create compiler threads on first run */
    thread_scoped_lock lock(cache_mutex);
    if (compile_threads.empty()) {
      for (int i = 0; i < max_mtlcompiler_threads; i++) {
        compile_threads.push_back(std::thread([&] { compile_thread_func(i); }));
      }
    }
  }

  if (!should_load_kernel(device_kernel, device, pso_type)) {
    return;
  }

  incomplete_requests++;
  if (pso_type != PSO_GENERIC) {
    incomplete_specialization_requests++;
  }

  MetalKernelPipeline *pipeline = new MetalKernelPipeline;

  /* Keep track of the originating device's ID so that we can cancel requests if the device ceases
   * to be active. */
  pipeline->originating_device_id = device->device_id;
  memcpy(&pipeline->kernel_data_, &device->launch_params.data, sizeof(pipeline->kernel_data_));
  pipeline->pso_type = pso_type;
  pipeline->mtlDevice = mtlDevice;
  pipeline->source_md5 = device->source_md5[pso_type];
  pipeline->mtlLibrary = device->mtlLibrary[pso_type];
  pipeline->device_kernel = device_kernel;
  pipeline->threads_per_threadgroup = device->max_threads_per_threadgroup;

  if (occupancy_tuning[device_kernel].threads_per_threadgroup) {
    pipeline->threads_per_threadgroup = occupancy_tuning[device_kernel].threads_per_threadgroup;
    pipeline->num_threads_per_block = occupancy_tuning[device_kernel].num_threads_per_block;
  }

  /* metalrt options */
  pipeline->use_metalrt = device->use_metalrt;
  pipeline->metalrt_features = device->use_metalrt ?
                                   (device->kernel_features & METALRT_FEATURE_MASK) :
                                   0;

  {
    thread_scoped_lock lock(cache_mutex);
    request_queue.push_back(pipeline);
  }
  cond_var.notify_one();
}

MetalKernelPipeline *ShaderCache::get_best_pipeline(DeviceKernel kernel, const MetalDevice *device)
{
  /* metalrt options */
  bool use_metalrt = device->use_metalrt;
  bool device_metalrt_hair = use_metalrt && device->kernel_features & KERNEL_FEATURE_HAIR;
  bool device_metalrt_hair_thick = use_metalrt &&
                                   device->kernel_features & KERNEL_FEATURE_HAIR_THICK;
  bool device_metalrt_pointcloud = use_metalrt &&
                                   device->kernel_features & KERNEL_FEATURE_POINTCLOUD;
  bool device_metalrt_motion = use_metalrt &&
                               device->kernel_features & KERNEL_FEATURE_OBJECT_MOTION;

  MetalKernelPipeline *best_pipeline = nullptr;
  while (!best_pipeline) {
    {
      thread_scoped_lock lock(cache_mutex);
      for (auto &pipeline : pipelines[kernel]) {
        if (!pipeline->loaded) {
          /* still loading - ignore */
          continue;
        }

        bool pipeline_metalrt_hair = pipeline->metalrt_features & KERNEL_FEATURE_HAIR;
        bool pipeline_metalrt_hair_thick = pipeline->metalrt_features & KERNEL_FEATURE_HAIR_THICK;
        bool pipeline_metalrt_pointcloud = pipeline->metalrt_features & KERNEL_FEATURE_POINTCLOUD;
        bool pipeline_metalrt_motion = use_metalrt &&
                                       pipeline->metalrt_features & KERNEL_FEATURE_OBJECT_MOTION;

        if (pipeline->use_metalrt != use_metalrt || pipeline_metalrt_hair != device_metalrt_hair ||
            pipeline_metalrt_hair_thick != device_metalrt_hair_thick ||
            pipeline_metalrt_pointcloud != device_metalrt_pointcloud ||
            pipeline_metalrt_motion != device_metalrt_motion) {
          /* wrong combination of metalrt options */
          continue;
        }

        if (pipeline->pso_type != PSO_GENERIC) {
          if (pipeline->source_md5 == device->source_md5[PSO_SPECIALIZED_INTERSECT] ||
              pipeline->source_md5 == device->source_md5[PSO_SPECIALIZED_SHADE]) {
            best_pipeline = pipeline.get();
          }
        }
        else if (!best_pipeline) {
          best_pipeline = pipeline.get();
        }
      }
    }

    if (!best_pipeline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  if (best_pipeline->usage_count == 0 && best_pipeline->pso_type != PSO_GENERIC) {
    metal_printf("Swapping in %s version of %s\n",
                 kernel_type_as_string(best_pipeline->pso_type),
                 device_kernel_as_string(kernel));
  }
  best_pipeline->usage_count += 1;

  return best_pipeline;
}

bool MetalKernelPipeline::should_use_binary_archive() const
{
  /* Issues with binary archives in older macOS versions. */
  if (@available(macOS 13.0, *)) {
    if (auto str = getenv("CYCLES_METAL_DISABLE_BINARY_ARCHIVES")) {
      if (atoi(str) != 0) {
        /* Don't archive if we have opted out by env var. */
        return false;
      }
    }

    /* Workaround for Intel GPU having issue using Binary Archives */
    MetalGPUVendor gpu_vendor = MetalInfo::get_device_vendor(mtlDevice);
    if (gpu_vendor == METAL_GPU_INTEL) {
      return false;
    }

    if (pso_type == PSO_GENERIC) {
      /* Archive the generic kernels. */
      return true;
    }

    if (device_kernel >= DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND &&
        device_kernel <= DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW) {
      /* Archive all shade kernels - they take a long time to compile. */
      return true;
    }

    /* The remaining kernels are all fast to compile. They may get cached by the system shader
     * cache, but will be quick to regenerate if not. */
  }
  return false;
}

static MTLFunctionConstantValues *GetConstantValues(KernelData const *data = nullptr)
{
  MTLFunctionConstantValues *constant_values = [MTLFunctionConstantValues new];

  MTLDataType MTLDataType_int = MTLDataTypeInt;
  MTLDataType MTLDataType_float = MTLDataTypeFloat;
  MTLDataType MTLDataType_float4 = MTLDataTypeFloat4;
  KernelData zero_data = {0};
  if (!data) {
    data = &zero_data;
  }
  int zero_int = 0;
  [constant_values setConstantValue:&zero_int type:MTLDataType_int atIndex:Kernel_DummyConstant];

#  define KERNEL_STRUCT_MEMBER(parent, _type, name) \
    [constant_values setConstantValue:&data->parent.name \
                                 type:MTLDataType_##_type \
                              atIndex:KernelData_##parent##_##name];

#  include "kernel/data_template.h"

  return constant_values;
}

void MetalKernelPipeline::compile()
{
  const std::string function_name = std::string("cycles_metal_") +
                                    device_kernel_as_string(device_kernel);

  NSString *entryPoint = [@(function_name.c_str()) copy];

  NSError *error = NULL;
  if (@available(macOS 11.0, *)) {
    MTLFunctionDescriptor *func_desc = [MTLIntersectionFunctionDescriptor functionDescriptor];
    func_desc.name = entryPoint;

    if (pso_type != PSO_GENERIC) {
      func_desc.constantValues = GetConstantValues(&kernel_data_);
    }
    else {
      func_desc.constantValues = GetConstantValues();
    }

    function = [mtlLibrary newFunctionWithDescriptor:func_desc error:&error];
  }

  [entryPoint release];

  if (function == nil) {
    NSString *err = [error localizedDescription];
    string errors = [err UTF8String];
    metal_printf("Error getting function \"%s\": %s", function_name.c_str(), errors.c_str());
    return;
  }

  function.label = [entryPoint copy];

  if (use_metalrt) {
    if (@available(macOS 11.0, *)) {
      /* create the id<MTLFunction> for each intersection function */
      const char *function_names[] = {
          "__anyhit__cycles_metalrt_visibility_test_tri",
          "__anyhit__cycles_metalrt_visibility_test_box",
          "__anyhit__cycles_metalrt_shadow_all_hit_tri",
          "__anyhit__cycles_metalrt_shadow_all_hit_box",
          "__anyhit__cycles_metalrt_local_hit_tri",
          "__anyhit__cycles_metalrt_local_hit_box",
          "__intersection__curve_ribbon",
          "__intersection__curve_ribbon_shadow",
          "__intersection__curve_all",
          "__intersection__curve_all_shadow",
          "__intersection__point",
          "__intersection__point_shadow",
      };
      assert(sizeof(function_names) / sizeof(function_names[0]) == METALRT_FUNC_NUM);

      MTLFunctionDescriptor *desc = [MTLIntersectionFunctionDescriptor functionDescriptor];
      for (int i = 0; i < METALRT_FUNC_NUM; i++) {
        const char *function_name = function_names[i];
        desc.name = [@(function_name) copy];

        if (pso_type != PSO_GENERIC) {
          desc.constantValues = GetConstantValues(&kernel_data_);
        }
        else {
          desc.constantValues = GetConstantValues();
        }

        NSError *error = NULL;
        rt_intersection_function[i] = [mtlLibrary newFunctionWithDescriptor:desc error:&error];

        if (rt_intersection_function[i] == nil) {
          NSString *err = [error localizedDescription];
          string errors = [err UTF8String];

          error_str = string_printf(
              "Error getting intersection function \"%s\": %s", function_name, errors.c_str());
          break;
        }

        rt_intersection_function[i].label = [@(function_name) copy];
      }
    }
  }

  NSArray *table_functions[METALRT_TABLE_NUM] = {nil};
  NSArray *linked_functions = nil;

  bool metalrt_hair = use_metalrt && (metalrt_features & KERNEL_FEATURE_HAIR);
  bool metalrt_hair_thick = use_metalrt && (metalrt_features & KERNEL_FEATURE_HAIR_THICK);
  bool metalrt_pointcloud = use_metalrt && (metalrt_features & KERNEL_FEATURE_POINTCLOUD);

  if (use_metalrt) {
    id<MTLFunction> curve_intersect_default = nil;
    id<MTLFunction> curve_intersect_shadow = nil;
    id<MTLFunction> point_intersect_default = nil;
    id<MTLFunction> point_intersect_shadow = nil;
    if (metalrt_hair) {
      /* Add curve intersection programs. */
      if (metalrt_hair_thick) {
        /* Slower programs for thick hair since that also slows down ribbons.
         * Ideally this should not be needed. */
        curve_intersect_default = rt_intersection_function[METALRT_FUNC_CURVE_ALL];
        curve_intersect_shadow = rt_intersection_function[METALRT_FUNC_CURVE_ALL_SHADOW];
      }
      else {
        curve_intersect_default = rt_intersection_function[METALRT_FUNC_CURVE_RIBBON];
        curve_intersect_shadow = rt_intersection_function[METALRT_FUNC_CURVE_RIBBON_SHADOW];
      }
    }
    if (metalrt_pointcloud) {
      point_intersect_default = rt_intersection_function[METALRT_FUNC_POINT];
      point_intersect_shadow = rt_intersection_function[METALRT_FUNC_POINT_SHADOW];
    }
    table_functions[METALRT_TABLE_DEFAULT] = [NSArray
        arrayWithObjects:rt_intersection_function[METALRT_FUNC_DEFAULT_TRI],
                         curve_intersect_default ?
                             curve_intersect_default :
                             rt_intersection_function[METALRT_FUNC_DEFAULT_BOX],
                         point_intersect_default ?
                             point_intersect_default :
                             rt_intersection_function[METALRT_FUNC_DEFAULT_BOX],
                         nil];
    table_functions[METALRT_TABLE_SHADOW] = [NSArray
        arrayWithObjects:rt_intersection_function[METALRT_FUNC_SHADOW_TRI],
                         curve_intersect_shadow ?
                             curve_intersect_shadow :
                             rt_intersection_function[METALRT_FUNC_SHADOW_BOX],
                         point_intersect_shadow ?
                             point_intersect_shadow :
                             rt_intersection_function[METALRT_FUNC_SHADOW_BOX],
                         nil];
    table_functions[METALRT_TABLE_LOCAL] = [NSArray
        arrayWithObjects:rt_intersection_function[METALRT_FUNC_LOCAL_TRI],
                         rt_intersection_function[METALRT_FUNC_LOCAL_BOX],
                         rt_intersection_function[METALRT_FUNC_LOCAL_BOX],
                         nil];

    NSMutableSet *unique_functions = [NSMutableSet
        setWithArray:table_functions[METALRT_TABLE_DEFAULT]];
    [unique_functions addObjectsFromArray:table_functions[METALRT_TABLE_SHADOW]];
    [unique_functions addObjectsFromArray:table_functions[METALRT_TABLE_LOCAL]];

    if (kernel_has_intersection(device_kernel)) {
      linked_functions = [[NSArray arrayWithArray:[unique_functions allObjects]]
          sortedArrayUsingComparator:^NSComparisonResult(id<MTLFunction> f1, id<MTLFunction> f2) {
            return [f1.label compare:f2.label];
          }];
    }
    unique_functions = nil;
  }

  MTLComputePipelineDescriptor *computePipelineStateDescriptor =
      [[MTLComputePipelineDescriptor alloc] init];

  computePipelineStateDescriptor.buffers[0].mutability = MTLMutabilityImmutable;
  computePipelineStateDescriptor.buffers[1].mutability = MTLMutabilityImmutable;
  computePipelineStateDescriptor.buffers[2].mutability = MTLMutabilityImmutable;

  if (@available(macos 10.14, *)) {
    computePipelineStateDescriptor.maxTotalThreadsPerThreadgroup = threads_per_threadgroup;
  }
  computePipelineStateDescriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = true;

  computePipelineStateDescriptor.computeFunction = function;

  if (@available(macOS 11.0, *)) {
    /* Attach the additional functions to an MTLLinkedFunctions object */
    if (linked_functions) {
      computePipelineStateDescriptor.linkedFunctions = [[MTLLinkedFunctions alloc] init];
      computePipelineStateDescriptor.linkedFunctions.functions = linked_functions;
    }
    computePipelineStateDescriptor.maxCallStackDepth = 1;
    if (use_metalrt) {
      computePipelineStateDescriptor.maxCallStackDepth = 8;
    }
  }

  MTLPipelineOption pipelineOptions = MTLPipelineOptionNone;

  bool use_binary_archive = should_use_binary_archive();

  id<MTLBinaryArchive> archive = nil;
  string metalbin_path;
  string metalbin_name;
  if (use_binary_archive) {
    NSProcessInfo *processInfo = [NSProcessInfo processInfo];
    string osVersion = [[processInfo operatingSystemVersionString] UTF8String];
    MD5Hash local_md5;
    local_md5.append(source_md5);
    local_md5.append(osVersion);
    local_md5.append((uint8_t *)&this->threads_per_threadgroup,
                     sizeof(this->threads_per_threadgroup));

    string options;
    if (use_metalrt && kernel_has_intersection(device_kernel)) {
      /* incorporate any MetalRT specializations into the archive name */
      options += string_printf(".hair_%d.hair_thick_%d.pointcloud_%d",
                               metalrt_hair ? 1 : 0,
                               metalrt_hair_thick ? 1 : 0,
                               metalrt_pointcloud ? 1 : 0);
    }

    /* Replace non-alphanumerical characters with underscores. */
    string device_name = [mtlDevice.name UTF8String];
    for (char &c : device_name) {
      if ((c < '0' || c > '9') && (c < 'a' || c > 'z') && (c < 'A' || c > 'Z')) {
        c = '_';
      }
    }

    metalbin_name = device_name;
    metalbin_name = path_join(metalbin_name, device_kernel_as_string(device_kernel));
    metalbin_name = path_join(metalbin_name, kernel_type_as_string(pso_type));
    metalbin_name = path_join(metalbin_name, local_md5.get_hex() + options + ".bin");

    metalbin_path = path_cache_get(path_join("kernels", metalbin_name));
    path_create_directories(metalbin_path);

    /* Retrieve shader binary from disk, and update the file timestamp for LRU purging to work as
     * intended. */
    if (use_binary_archive && path_cache_kernel_exists_and_mark_used(metalbin_path)) {
      if (@available(macOS 11.0, *)) {
        MTLBinaryArchiveDescriptor *archiveDesc = [[MTLBinaryArchiveDescriptor alloc] init];
        archiveDesc.url = [NSURL fileURLWithPath:@(metalbin_path.c_str())];
        archive = [mtlDevice newBinaryArchiveWithDescriptor:archiveDesc error:nil];
        [archiveDesc release];
      }
    }
  }

  __block bool creating_new_archive = false;
  if (@available(macOS 11.0, *)) {
    if (use_binary_archive) {
      if (!archive) {
        MTLBinaryArchiveDescriptor *archiveDesc = [[MTLBinaryArchiveDescriptor alloc] init];
        archiveDesc.url = nil;
        archive = [mtlDevice newBinaryArchiveWithDescriptor:archiveDesc error:nil];
        creating_new_archive = true;
      }
      computePipelineStateDescriptor.binaryArchives = [NSArray arrayWithObjects:archive, nil];
      pipelineOptions = MTLPipelineOptionFailOnBinaryArchiveMiss;
    }
  }

  double starttime = time_dt();

  /* Block on load to ensure we continue with a valid kernel function */
  if (creating_new_archive) {
    starttime = time_dt();
    NSError *error;
    if (![archive addComputePipelineFunctionsWithDescriptor:computePipelineStateDescriptor
                                                      error:&error]) {
      NSString *errStr = [error localizedDescription];
      metal_printf("Failed to add PSO to archive:\n%s\n", errStr ? [errStr UTF8String] : "nil");
    }
  }

  pipeline = [mtlDevice newComputePipelineStateWithDescriptor:computePipelineStateDescriptor
                                                      options:pipelineOptions
                                                   reflection:nullptr
                                                        error:&error];

  bool recreate_archive = false;
  if (pipeline == nil && archive) {
    NSString *errStr = [error localizedDescription];
    metal_printf(
        "Failed to create compute pipeline state \"%s\" from archive - attempting recreation... "
        "(error: %s)\n",
        device_kernel_as_string((DeviceKernel)device_kernel),
        errStr ? [errStr UTF8String] : "nil");
    pipeline = [mtlDevice newComputePipelineStateWithDescriptor:computePipelineStateDescriptor
                                                        options:MTLPipelineOptionNone
                                                     reflection:nullptr
                                                          error:&error];
    recreate_archive = true;
  }

  double duration = time_dt() - starttime;

  if (pipeline == nil) {
    NSString *errStr = [error localizedDescription];
    error_str = string_printf("Failed to create compute pipeline state \"%s\", error: \n",
                              device_kernel_as_string((DeviceKernel)device_kernel));
    error_str += (errStr ? [errStr UTF8String] : "nil");
    metal_printf("%16s | %2d | %-55s | %7.2fs | FAILED!\n",
                 kernel_type_as_string(pso_type),
                 device_kernel,
                 device_kernel_as_string((DeviceKernel)device_kernel),
                 duration);
    return;
  }

  if (!num_threads_per_block) {
    num_threads_per_block = round_down(pipeline.maxTotalThreadsPerThreadgroup,
                                       pipeline.threadExecutionWidth);
    num_threads_per_block = std::max(num_threads_per_block, (int)pipeline.threadExecutionWidth);
  }

  if (@available(macOS 11.0, *)) {
    if (ShaderCache::running) {
      if (creating_new_archive || recreate_archive) {
        if (![archive serializeToURL:[NSURL fileURLWithPath:@(metalbin_path.c_str())]
                               error:&error]) {
          metal_printf("Failed to save binary archive, error:\n%s\n",
                       [[error localizedDescription] UTF8String]);
        }
        else {
          path_cache_kernel_mark_added_and_clear_old(metalbin_path);
        }
      }
    }
  }

  this->loaded = true;
  [computePipelineStateDescriptor release];
  computePipelineStateDescriptor = nil;

  if (use_metalrt && linked_functions) {
    for (int table = 0; table < METALRT_TABLE_NUM; table++) {
      if (@available(macOS 11.0, *)) {
        MTLIntersectionFunctionTableDescriptor *ift_desc =
            [[MTLIntersectionFunctionTableDescriptor alloc] init];
        ift_desc.functionCount = table_functions[table].count;
        intersection_func_table[table] = [this->pipeline
            newIntersectionFunctionTableWithDescriptor:ift_desc];

        /* Finally write the function handles into this pipeline's table */
        int size = (int)[table_functions[table] count];
        for (int i = 0; i < size; i++) {
          id<MTLFunctionHandle> handle = [pipeline
              functionHandleWithFunction:table_functions[table][i]];
          [intersection_func_table[table] setFunction:handle atIndex:i];
        }
      }
    }
  }

  if (!use_binary_archive) {
    metal_printf("%16s | %2d | %-55s | %7.2fs\n",
                 kernel_type_as_string(pso_type),
                 int(device_kernel),
                 device_kernel_as_string(device_kernel),
                 duration);
  }
  else {
    metal_printf("%16s | %2d | %-55s | %7.2fs | %s: %s\n",
                 kernel_type_as_string(pso_type),
                 device_kernel,
                 device_kernel_as_string((DeviceKernel)device_kernel),
                 duration,
                 creating_new_archive ? " new" : "load",
                 metalbin_name.c_str());
  }
}

bool MetalDeviceKernels::load(MetalDevice *device, MetalPipelineType pso_type)
{
  auto shader_cache = get_shader_cache(device->mtlDevice);
  for (int i = 0; i < DEVICE_KERNEL_NUM; i++) {
    shader_cache->load_kernel((DeviceKernel)i, device, pso_type);
  }
  return true;
}

void MetalDeviceKernels::wait_for_all()
{
  for (int i = 0; i < g_shaderCacheCount; i++) {
    g_shaderCache[i].second->wait_for_all();
  }
}

bool MetalDeviceKernels::any_specialization_happening_now()
{
  /* Return true if any ShaderCaches have ongoing specialization requests (typically there will be
   * only 1). */
  for (int i = 0; i < g_shaderCacheCount; i++) {
    if (g_shaderCache[i].second->incomplete_specialization_requests > 0) {
      return true;
    }
  }
  return false;
}

int MetalDeviceKernels::get_loaded_kernel_count(MetalDevice const *device,
                                                MetalPipelineType pso_type)
{
  auto shader_cache = get_shader_cache(device->mtlDevice);
  int loaded_count = DEVICE_KERNEL_NUM;
  for (int i = 0; i < DEVICE_KERNEL_NUM; i++) {
    if (shader_cache->should_load_kernel((DeviceKernel)i, device, pso_type)) {
      loaded_count -= 1;
    }
  }
  return loaded_count;
}

bool MetalDeviceKernels::should_load_kernels(MetalDevice const *device, MetalPipelineType pso_type)
{
  return get_loaded_kernel_count(device, pso_type) != DEVICE_KERNEL_NUM;
}

const MetalKernelPipeline *MetalDeviceKernels::get_best_pipeline(const MetalDevice *device,
                                                                 DeviceKernel kernel)
{
  return get_shader_cache(device->mtlDevice)->get_best_pipeline(kernel, device);
}

bool MetalDeviceKernels::is_benchmark_warmup()
{
  NSArray *args = [[NSProcessInfo processInfo] arguments];
  for (int i = 0; i < args.count; i++) {
    if (const char *arg = [[args objectAtIndex:i] cStringUsingEncoding:NSASCIIStringEncoding]) {
      if (!strcmp(arg, "--warm-up")) {
        return true;
      }
    }
  }
  return false;
}

CCL_NAMESPACE_END

#endif /* WITH_METAL*/
