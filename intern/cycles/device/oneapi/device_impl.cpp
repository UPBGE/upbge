/* SPDX-FileCopyrightText: 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_ONEAPI

/* <algorithm> is needed until included upstream in sycl/detail/property_list_base.hpp */
#  include <algorithm>
#  include <sycl/sycl.hpp>

#  include "device/oneapi/device_impl.h"

#  include "util/log.h"

#  ifdef WITH_EMBREE_GPU
#    include "bvh/embree.h"
#  endif

#  if defined(WITH_OPENIMAGEDENOISE)
#    include <OpenImageDenoise/config.h>
#    if OIDN_VERSION >= 20300
#      include "util/openimagedenoise.h"  // IWYU pragma: keep
#    endif
#  endif

#  include "kernel/device/oneapi/globals.h"
#  include "kernel/device/oneapi/kernel.h"

#  if defined(WITH_EMBREE_GPU) && defined(EMBREE_SYCL_SUPPORT) && !defined(SYCL_LANGUAGE_VERSION)
/* These declarations are missing from embree headers when compiling from a compiler that doesn't
 * support SYCL. */
extern "C" RTCDevice rtcNewSYCLDevice(sycl::context context, const char *config);
extern "C" bool rtcIsSYCLDeviceSupported(const sycl::device sycl_device);
extern "C" void rtcSetDeviceSYCLDevice(RTCDevice device, const sycl::device sycl_device);
#  endif

CCL_NAMESPACE_BEGIN

static std::vector<sycl::device> available_sycl_devices();
static int parse_driver_build_version(const sycl::device &device);

static void queue_error_cb(const char *message, void *user_ptr)
{
  if (user_ptr) {
    *reinterpret_cast<std::string *>(user_ptr) = message;
  }
}

OneapiDevice::OneapiDevice(const DeviceInfo &info, Stats &stats, Profiler &profiler, bool headless)
    : GPUDevice(info, stats, profiler, headless)
{
  /* Verify that base class types can be used with specific backend types */
  static_assert(sizeof(texMemObject) ==
                sizeof(sycl::ext::oneapi::experimental::sampled_image_handle));
  static_assert(sizeof(arrayMemObject) ==
                sizeof(sycl::ext::oneapi::experimental::image_mem_handle));

  need_texture_info = false;
  use_hardware_raytracing = info.use_hardware_raytracing;

  oneapi_set_error_cb(queue_error_cb, &oneapi_error_string_);

  bool is_finished_ok = create_queue(device_queue_,
                                     info.num,
#  ifdef WITH_EMBREE_GPU
                                     use_hardware_raytracing ? (void *)&embree_device : nullptr
#  else
                                     nullptr
#  endif
  );

  if (is_finished_ok == false) {
    set_error("oneAPI queue initialization error: got runtime exception \"" +
              oneapi_error_string_ + "\"");
  }
  else {
    LOG_DEBUG << "oneAPI queue has been successfully created for the device \"" << info.description
              << "\"";
    assert(device_queue_);
  }

#  ifdef WITH_EMBREE_GPU
  use_hardware_raytracing = use_hardware_raytracing && (embree_device != nullptr);
#  else
  use_hardware_raytracing = false;
#  endif

  if (use_hardware_raytracing) {
    LOG_INFO << "oneAPI will use hardware ray tracing for intersection acceleration.";
  }

  size_t globals_segment_size;
  is_finished_ok = kernel_globals_size(globals_segment_size);
  if (is_finished_ok == false) {
    set_error("oneAPI constant memory initialization got runtime exception \"" +
              oneapi_error_string_ + "\"");
  }
  else {
    LOG_DEBUG << "Successfully created global/constant memory segment (kernel globals object)";
  }

  kg_memory_ = usm_aligned_alloc_host(device_queue_, globals_segment_size, 16);
  usm_memset(device_queue_, kg_memory_, 0, globals_segment_size);

  kg_memory_device_ = usm_alloc_device(device_queue_, globals_segment_size);

  kg_memory_size_ = globals_segment_size;

  max_memory_on_device_ = get_memcapacity();
  init_host_memory();
  can_map_host = true;

  const char *headroom_str = getenv("CYCLES_ONEAPI_MEMORY_HEADROOM");
  if (headroom_str != nullptr) {
    const long long override_headroom = (float)atoll(headroom_str);
    device_working_headroom = override_headroom;
    device_texture_headroom = override_headroom;
  }
  LOG_DEBUG << "oneAPI memory headroom size: "
            << string_human_readable_size(device_working_headroom);
}

OneapiDevice::~OneapiDevice()
{
#  ifdef WITH_EMBREE_GPU
  if (embree_device) {
    rtcReleaseDevice(embree_device);
  }
#  endif

  texture_info.free();
  usm_free(device_queue_, kg_memory_);
  usm_free(device_queue_, kg_memory_device_);

  const_mem_map_.clear();

  if (device_queue_) {
    free_queue(device_queue_);
  }
}

bool OneapiDevice::check_peer_access(Device * /*peer_device*/)
{
  return false;
}

bool OneapiDevice::can_use_hardware_raytracing_for_features(const uint requested_features) const
{
  /* MNEE and Ray-trace kernels work correctly with Hardware Ray-tracing starting with Embree 4.1.
   */
#  if defined(RTC_VERSION) && RTC_VERSION < 40100
  return !(requested_features & (KERNEL_FEATURE_MNEE | KERNEL_FEATURE_NODE_RAYTRACE));
#  else
  (void)requested_features;
  return true;
#  endif
}

BVHLayoutMask OneapiDevice::get_bvh_layout_mask(const uint requested_features) const
{
  return (use_hardware_raytracing &&
          can_use_hardware_raytracing_for_features(requested_features)) ?
             BVH_LAYOUT_EMBREEGPU :
             BVH_LAYOUT_BVH2;
}

#  ifdef WITH_EMBREE_GPU
void OneapiDevice::build_bvh(BVH *bvh, Progress &progress, bool refit)
{
  if (embree_device && bvh->params.bvh_layout == BVH_LAYOUT_EMBREEGPU) {
    BVHEmbree *const bvh_embree = static_cast<BVHEmbree *>(bvh);
    if (refit) {
      bvh_embree->refit(progress);
    }
    else {
      bvh_embree->build(progress, &stats, embree_device, true);
    }

#    if RTC_VERSION >= 40302
    thread_scoped_lock lock(scene_data_mutex);
    all_embree_scenes.push_back(bvh_embree->scene);
#    endif

    if (bvh->params.top_level) {
#    if RTC_VERSION >= 40400
      embree_traversable = rtcGetSceneTraversable(bvh_embree->scene);
#    else
      embree_traversable = bvh_embree->scene;
#    endif
#    if RTC_VERSION >= 40302
      RTCError error_code = bvh_embree->offload_scenes_to_gpu(all_embree_scenes);
      if (error_code != RTC_ERROR_NONE) {
        set_error(
            string_printf("BVH failed to migrate to the GPU due to Embree library error (%s)",
                          bvh_embree->get_error_string(error_code)));
      }
      all_embree_scenes.clear();
#    endif
    }
  }
  else {
    Device::build_bvh(bvh, progress, refit);
  }
}
#  endif

size_t OneapiDevice::get_free_mem() const
{
  /* Accurate: Use device info, which is practically useful only on dGPU.
   * This is because for non-discrete GPUs, all GPU memory allocations would
   * be in the RAM, thus having the same performance for device and host pointers,
   * so there is no need to be very accurate about what would end where. */
  const sycl::device &device = reinterpret_cast<sycl::queue *>(device_queue_)->get_device();
  const bool is_integrated_gpu = device.get_info<sycl::info::device::host_unified_memory>();
  if (device.has(sycl::aspect::ext_intel_free_memory) && is_integrated_gpu == false) {
    return device.get_info<sycl::ext::intel::info::device::free_memory>();
  }
  /* Estimate: Capacity - in use. */
  if (device_mem_in_use < max_memory_on_device_) {
    return max_memory_on_device_ - device_mem_in_use;
  }
  return 0;
}

bool OneapiDevice::load_kernels(const uint requested_features)
{
  assert(device_queue_);

  /* Kernel loading is expected to be a cumulative operation; for example, if
   * a device is asked to load kernel A and then kernel B, then after these
   * operations, both A and B should be available for use. So we need to store
   * and use a cumulative mask of the requested kernel features, and not just
   * the latest requested features.
   */
  kernel_features |= requested_features;

  bool is_finished_ok = oneapi_run_test_kernel(device_queue_);
  if (is_finished_ok == false) {
    set_error("oneAPI test kernel execution: got a runtime exception \"" + oneapi_error_string_ +
              "\"");
    return false;
  }
  LOG_INFO << "Test kernel has been executed successfully for \"" << info.description << "\"";
  assert(device_queue_);

  if (use_hardware_raytracing && !can_use_hardware_raytracing_for_features(requested_features)) {
    LOG_INFO
        << "Hardware ray tracing disabled, not supported yet by oneAPI for requested features.";
    use_hardware_raytracing = false;
  }

  is_finished_ok = oneapi_load_kernels(
      device_queue_, (const unsigned int)requested_features, use_hardware_raytracing);
  if (is_finished_ok == false) {
    set_error("oneAPI kernels loading: got a runtime exception \"" + oneapi_error_string_ + "\"");
  }
  else {
    LOG_INFO << "Kernels loading (compilation) has been done for \"" << info.description << "\"";
  }

  if (is_finished_ok) {
    reserve_private_memory(requested_features);
    is_finished_ok = !have_error();
  }

  return is_finished_ok;
}

void OneapiDevice::reserve_private_memory(const uint kernel_features)
{
  size_t free_before = get_free_mem();

  /* Use the biggest kernel for estimation. */
  const DeviceKernel test_kernel = (kernel_features & KERNEL_FEATURE_NODE_RAYTRACE) ?
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE :
                                   (kernel_features & KERNEL_FEATURE_MNEE) ?
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE :
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE;

  {
    unique_ptr<DeviceQueue> queue = gpu_queue_create();

    device_ptr d_path_index = 0;
    device_ptr d_render_buffer = 0;
    int d_work_size = 0;
    DeviceKernelArguments args(&d_path_index, &d_render_buffer, &d_work_size);

    queue->init_execution();
    /* Launch of the kernel seems to be sufficient to reserve all
     * needed memory regardless of the execution global size.
     * So, the smallest possible size is used here. */
    queue->enqueue(test_kernel, 1, args);
    queue->synchronize();
  }

  size_t free_after = get_free_mem();

  LOG_INFO << "For kernel execution were reserved "
           << string_human_readable_number(free_before - free_after) << " bytes. ("
           << string_human_readable_size(free_before - free_after) << ")";
}

void OneapiDevice::get_device_memory_info(size_t &total, size_t &free)
{
  free = get_free_mem();
  total = max_memory_on_device_;
}

bool OneapiDevice::alloc_device(void *&device_pointer, const size_t size)
{
  bool allocation_success = false;
  device_pointer = usm_alloc_device(device_queue_, size);
  if (device_pointer != nullptr) {
    allocation_success = true;
    /* Due to lazy memory initialization in GPU runtime we will force memory to
     * appear in device memory via execution of a kernel using this memory. */
    if (!oneapi_zero_memory_on_device(device_queue_, device_pointer, size)) {
      set_error("oneAPI memory operation error: got runtime exception \"" + oneapi_error_string_ +
                "\"");
      usm_free(device_queue_, device_pointer);

      device_pointer = nullptr;
      allocation_success = false;
    }
  }

  return allocation_success;
}

void OneapiDevice::free_device(void *device_pointer)
{
  usm_free(device_queue_, device_pointer);
}

bool OneapiDevice::shared_alloc(void *&shared_pointer, const size_t size)
{
  shared_pointer = usm_aligned_alloc_host(device_queue_, size, 64);
  return shared_pointer != nullptr;
}

void OneapiDevice::shared_free(void *shared_pointer)
{
  usm_free(device_queue_, shared_pointer);
}

void *OneapiDevice::shared_to_device_pointer(const void *shared_pointer)
{
  /* Device and host pointer are in the same address space
   * as we're using Unified Shared Memory. */
  return const_cast<void *>(shared_pointer);
}

void OneapiDevice::copy_host_to_device(void *device_pointer, void *host_pointer, const size_t size)
{
  usm_memcpy(device_queue_, device_pointer, host_pointer, size);
}

/* TODO: Make sycl::queue part of OneapiQueue and avoid using pointers to sycl::queue. */
SyclQueue *OneapiDevice::sycl_queue()
{
  return device_queue_;
}

string OneapiDevice::oneapi_error_message()
{
  return string(oneapi_error_string_);
}

int OneapiDevice::scene_max_shaders()
{
  return scene_max_shaders_;
}

void *OneapiDevice::kernel_globals_device_pointer()
{
  return kg_memory_device_;
}

void *OneapiDevice::host_alloc(const MemoryType type, const size_t size)
{
  void *host_pointer = GPUDevice::host_alloc(type, size);

#  ifdef SYCL_EXT_ONEAPI_COPY_OPTIMIZE
  if (host_pointer) {
    /* Import host_pointer into USM memory for faster host<->device data transfers. */
    if (type == MEM_READ_WRITE || type == MEM_READ_ONLY) {
      sycl::queue *queue = reinterpret_cast<sycl::queue *>(device_queue_);
      /* This API is properly implemented only in Level-Zero backend at the moment and we don't
       * want it to fail at runtime, so we conservatively use it only for L0. */
      if (queue->get_backend() == sycl::backend::ext_oneapi_level_zero) {
        sycl::ext::oneapi::experimental::prepare_for_device_copy(host_pointer, size, *queue);
      }
    }
  }
#  endif

  return host_pointer;
}

void OneapiDevice::host_free(const MemoryType type, void *host_pointer, const size_t size)
{
#  ifdef SYCL_EXT_ONEAPI_COPY_OPTIMIZE
  if (type == MEM_READ_WRITE || type == MEM_READ_ONLY) {
    sycl::queue *queue = reinterpret_cast<sycl::queue *>(device_queue_);
    /* This API is properly implemented only in Level-Zero backend at the moment and we don't
     * want it to fail at runtime, so we conservatively use it only for L0. */
    if (queue->get_backend() == sycl::backend::ext_oneapi_level_zero) {
      sycl::ext::oneapi::experimental::release_from_device_copy(host_pointer, *queue);
    }
  }
#  endif

  GPUDevice::host_free(type, host_pointer, size);
}

void OneapiDevice::mem_alloc(device_memory &mem)
{
  if (mem.type == MEM_TEXTURE) {
    assert(!"mem_alloc not supported for textures.");
  }
  else if (mem.type == MEM_GLOBAL) {
    assert(!"mem_alloc not supported for global memory.");
  }
  else {
    if (mem.name) {
      LOG_DEBUG << "OneapiDevice::mem_alloc: \"" << mem.name << "\", "
                << string_human_readable_number(mem.memory_size()) << " bytes. ("
                << string_human_readable_size(mem.memory_size()) << ")";
    }
    generic_alloc(mem);
  }
}

void OneapiDevice::mem_copy_to(device_memory &mem)
{
  if (mem.name) {
    LOG_DEBUG << "OneapiDevice::mem_copy_to: \"" << mem.name << "\", "
              << string_human_readable_number(mem.memory_size()) << " bytes. ("
              << string_human_readable_size(mem.memory_size()) << ")";
  }

  /* After getting runtime errors we need to avoid performing oneAPI runtime operations
   * because the associated GPU context may be in an invalid state at this point. */
  if (have_error()) {
    return;
  }

  if (mem.type == MEM_GLOBAL) {
    global_copy_to(mem);
  }
  else if (mem.type == MEM_TEXTURE) {
    tex_copy_to((device_texture &)mem);
  }
  else {
    if (!mem.device_pointer) {
      generic_alloc(mem);
    }
    generic_copy_to(mem);
  }
}

void OneapiDevice::mem_move_to_host(device_memory &mem)
{
  if (mem.name) {
    LOG_DEBUG << "OneapiDevice::mem_move_to_host: \"" << mem.name << "\", "
              << string_human_readable_number(mem.memory_size()) << " bytes. ("
              << string_human_readable_size(mem.memory_size()) << ")";
  }

  /* After getting runtime errors we need to avoid performing oneAPI runtime operations
   * because the associated GPU context may be in an invalid state at this point. */
  if (have_error()) {
    return;
  }

  if (mem.type == MEM_GLOBAL) {
    global_free(mem);
    global_alloc(mem);
  }
  else if (mem.type == MEM_TEXTURE) {
    tex_free((device_texture &)mem);
    tex_alloc((device_texture &)mem);
  }
  else {
    assert(0);
  }
}

void OneapiDevice::mem_copy_from(
    device_memory &mem, const size_t y, size_t w, const size_t h, size_t elem)
{
  if (mem.type == MEM_TEXTURE || mem.type == MEM_GLOBAL) {
    assert(!"mem_copy_from not supported for textures.");
  }
  else if (mem.host_pointer) {
    const size_t size = (w > 0 || h > 0 || elem > 0) ? (elem * w * h) : mem.memory_size();
    const size_t offset = elem * y * w;

    if (mem.name) {
      LOG_DEBUG << "OneapiDevice::mem_copy_from: \"" << mem.name << "\" object of "
                << string_human_readable_number(mem.memory_size()) << " bytes. ("
                << string_human_readable_size(mem.memory_size()) << ") from offset " << offset
                << " data " << size << " bytes";
    }

    /* After getting runtime errors we need to avoid performing oneAPI runtime operations
     * because the associated GPU context may be in an invalid state at this point. */
    if (have_error()) {
      return;
    }

    assert(device_queue_);

    assert(size != 0);
    if (mem.device_pointer) {
      char *shifted_host = reinterpret_cast<char *>(mem.host_pointer) + offset;
      char *shifted_device = reinterpret_cast<char *>(mem.device_pointer) + offset;
      bool is_finished_ok = usm_memcpy(device_queue_, shifted_host, shifted_device, size);
      if (is_finished_ok == false) {
        set_error("oneAPI memory operation error: got runtime exception \"" +
                  oneapi_error_string_ + "\"");
      }
    }
  }
}

void OneapiDevice::mem_zero(device_memory &mem)
{
  if (mem.name) {
    LOG_DEBUG << "OneapiDevice::mem_zero: \"" << mem.name << "\", "
              << string_human_readable_number(mem.memory_size()) << " bytes. ("
              << string_human_readable_size(mem.memory_size()) << ")\n";
  }

  /* After getting runtime errors we need to avoid performing oneAPI runtime operations
   * because the associated GPU context may be in an invalid state at this point. */
  if (have_error()) {
    return;
  }

  if (!mem.device_pointer) {
    mem_alloc(mem);
  }
  if (!mem.device_pointer) {
    return;
  }

  assert(device_queue_);
  bool is_finished_ok = usm_memset(
      device_queue_, (void *)mem.device_pointer, 0, mem.memory_size());
  if (is_finished_ok == false) {
    set_error("oneAPI memory operation error: got runtime exception \"" + oneapi_error_string_ +
              "\"");
  }
}

void OneapiDevice::mem_free(device_memory &mem)
{
  if (mem.name) {
    LOG_DEBUG << "OneapiDevice::mem_free: \"" << mem.name << "\", "
              << string_human_readable_number(mem.device_size) << " bytes. ("
              << string_human_readable_size(mem.device_size) << ")\n";
  }

  if (mem.type == MEM_GLOBAL) {
    global_free(mem);
  }
  else if (mem.type == MEM_TEXTURE) {
    tex_free((device_texture &)mem);
  }
  else {
    generic_free(mem);
  }
}

device_ptr OneapiDevice::mem_alloc_sub_ptr(device_memory &mem,
                                           const size_t offset,
                                           size_t /*size*/)
{
  return reinterpret_cast<device_ptr>(reinterpret_cast<char *>(mem.device_pointer) +
                                      mem.memory_elements_size(offset));
}

void OneapiDevice::const_copy_to(const char *name, void *host, const size_t size)
{
  assert(name);

  LOG_DEBUG << "OneapiDevice::const_copy_to \"" << name << "\" object "
            << string_human_readable_number(size) << " bytes. ("
            << string_human_readable_size(size) << ")";

  if (strcmp(name, "data") == 0) {
    assert(size <= sizeof(KernelData));
    KernelData *const data = static_cast<KernelData *>(host);

    /* We need this value when allocating local memory for integrator_sort_bucket_pass
     * and integrator_sort_write_pass kernels. */
    scene_max_shaders_ = data->max_shaders;

#  ifdef WITH_EMBREE_GPU
    if (embree_traversable != nullptr) {
      /* Update scene handle (since it is different for each device on multi devices).
       * This must be a raw pointer copy since at some points during scene update this
       * pointer may be invalid. */
      data->device_bvh = embree_traversable;
    }
#  endif
  }

  ConstMemMap::iterator i = const_mem_map_.find(name);
  device_vector<uchar> *data;

  if (i == const_mem_map_.end()) {
    unique_ptr<device_vector<uchar>> data_ptr = make_unique<device_vector<uchar>>(
        this, name, MEM_READ_ONLY);
    data_ptr->alloc(size);
    data = data_ptr.get();
    const_mem_map_.insert(ConstMemMap::value_type(name, std::move(data_ptr)));
  }
  else {
    data = i->second.get();
  }

  assert(data->memory_size() <= size);
  memcpy(data->data(), host, size);
  data->copy_to_device();

  set_global_memory(device_queue_, kg_memory_, name, (void *)data->device_pointer);

  usm_memcpy(device_queue_, kg_memory_device_, kg_memory_, kg_memory_size_);
}

void OneapiDevice::global_alloc(device_memory &mem)
{
  assert(mem.name);

  size_t size = mem.memory_size();
  LOG_DEBUG << "OneapiDevice::global_alloc \"" << mem.name << "\" object "
            << string_human_readable_number(size) << " bytes. ("
            << string_human_readable_size(size) << ")";

  generic_alloc(mem);
  generic_copy_to(mem);

  set_global_memory(device_queue_, kg_memory_, mem.name, (void *)mem.device_pointer);

  usm_memcpy(device_queue_, kg_memory_device_, kg_memory_, kg_memory_size_);
}

void OneapiDevice::global_copy_to(device_memory &mem)
{
  if (!mem.device_pointer) {
    global_alloc(mem);
  }
  else {
    generic_copy_to(mem);
  }
}

void OneapiDevice::global_free(device_memory &mem)
{
  if (mem.device_pointer) {
    generic_free(mem);
  }
}

static sycl::ext::oneapi::experimental::image_descriptor image_desc(const device_texture &mem)
{
  /* Image Texture Storage */
  sycl::image_channel_type channel_type;

  switch (mem.data_type) {
    case TYPE_UCHAR:
      channel_type = sycl::image_channel_type::unorm_int8;
      break;
    case TYPE_UINT16:
      channel_type = sycl::image_channel_type::unorm_int16;
      break;
    case TYPE_FLOAT:
      channel_type = sycl::image_channel_type::fp32;
      break;
    case TYPE_HALF:
      channel_type = sycl::image_channel_type::fp16;
      break;
    default:
      assert(0);
  }

  sycl::ext::oneapi::experimental::image_descriptor param;
  param.width = mem.data_width;
  param.height = mem.data_height;
  param.num_channels = mem.data_elements;
  param.channel_type = channel_type;

  param.verify();

  return param;
}

void OneapiDevice::tex_alloc(device_texture &mem)
{
  assert(device_queue_);

  size_t size = mem.memory_size();

  sycl::addressing_mode address_mode = sycl::addressing_mode::none;
  switch (mem.info.extension) {
    case EXTENSION_REPEAT:
      address_mode = sycl::addressing_mode::repeat;
      break;
    case EXTENSION_EXTEND:
      address_mode = sycl::addressing_mode::clamp_to_edge;
      break;
    case EXTENSION_CLIP:
      address_mode = sycl::addressing_mode::clamp;
      break;
    case EXTENSION_MIRROR:
      address_mode = sycl::addressing_mode::mirrored_repeat;
      break;
    default:
      assert(0);
      break;
  }

  sycl::filtering_mode filter_mode;
  if (mem.info.interpolation == INTERPOLATION_CLOSEST) {
    filter_mode = sycl::filtering_mode::nearest;
  }
  else {
    filter_mode = sycl::filtering_mode::linear;
  }

  /* Image Texture Storage */
  sycl::image_channel_type channel_type;

  switch (mem.data_type) {
    case TYPE_UCHAR:
      channel_type = sycl::image_channel_type::unorm_int8;
      break;
    case TYPE_UINT16:
      channel_type = sycl::image_channel_type::unorm_int16;
      break;
    case TYPE_FLOAT:
      channel_type = sycl::image_channel_type::fp32;
      break;
    case TYPE_HALF:
      channel_type = sycl::image_channel_type::fp16;
      break;
    default:
      assert(0);
      return;
  }

  sycl::queue *queue = reinterpret_cast<sycl::queue *>(device_queue_);

  try {
    Mem *cmem = nullptr;
    sycl::ext::oneapi::experimental::image_mem_handle memHandle{0};
    sycl::ext::oneapi::experimental::image_descriptor desc{};

    if (mem.data_height > 0) {
      const sycl::device &device = reinterpret_cast<sycl::queue *>(queue)->get_device();
      const size_t max_width = device.get_info<sycl::info::device::image2d_max_width>();
      const size_t max_height = device.get_info<sycl::info::device::image2d_max_height>();

      if (mem.data_width > max_width || mem.data_height > max_height) {
        set_error(
            string_printf("Maximum GPU 2D texture size exceeded (max %zux%zu, found %zux%zu)",
                          max_width,
                          max_height,
                          mem.data_width,
                          mem.data_height));
        return;
      }

      /* 2D texture -- Tile optimized */
      desc = sycl::ext::oneapi::experimental::image_descriptor(
          {mem.data_width, mem.data_height, 0}, mem.data_elements, channel_type);

      LOG_WORK << "Array 2D/3D allocate: " << mem.name << ", "
               << string_human_readable_number(mem.memory_size()) << " bytes. ("
               << string_human_readable_size(mem.memory_size()) << ")";

      sycl::ext::oneapi::experimental::image_mem_handle memHandle =
          sycl::ext::oneapi::experimental::alloc_image_mem(desc, *queue);
      if (!memHandle.raw_handle) {
        set_error("GPU texture allocation failed: Raw handle is null");
        return;
      }

      /* Copy data from host to the texture properly based on the texture description */
      queue->ext_oneapi_copy(mem.host_pointer, memHandle, desc);

      mem.device_pointer = (device_ptr)memHandle.raw_handle;
      mem.device_size = size;
      stats.mem_alloc(size);

      thread_scoped_lock lock(device_mem_map_mutex);
      cmem = &device_mem_map[&mem];
      cmem->texobject = 0;
      cmem->array = (arrayMemObject)(memHandle.raw_handle);
    }
    else {
      /* 1D texture -- Linear memory */
      desc = sycl::ext::oneapi::experimental::image_descriptor(
          {mem.data_width}, mem.data_elements, channel_type);
      cmem = generic_alloc(mem);
      if (!cmem) {
        return;
      }

      queue->memcpy((void *)mem.device_pointer, mem.host_pointer, size);
    }

    queue->wait_and_throw();

    /* Set Mapping and tag that we need to (re-)upload to device */
    TextureInfo tex_info = mem.info;

    sycl::ext::oneapi::experimental::bindless_image_sampler samp(
        address_mode, sycl::coordinate_normalization_mode::normalized, filter_mode);

    if (!is_nanovdb_type(mem.info.data_type)) {
      sycl::ext::oneapi::experimental::sampled_image_handle imgHandle;

      if (memHandle.raw_handle) {
        /* Create 2D/3D texture handle */
        imgHandle = sycl::ext::oneapi::experimental::create_image(memHandle, samp, desc, *queue);
      }
      else {
        /* Create 1D texture */
        imgHandle = sycl::ext::oneapi::experimental::create_image(
            (void *)mem.device_pointer, 0, samp, desc, *queue);
      }

      thread_scoped_lock lock(device_mem_map_mutex);
      cmem = &device_mem_map[&mem];
      cmem->texobject = (texMemObject)(imgHandle.raw_handle);

      tex_info.data = (uint64_t)cmem->texobject;
    }
    else {
      tex_info.data = (uint64_t)mem.device_pointer;
    }

    {
      /* Update texture info. */
      thread_scoped_lock lock(texture_info_mutex);
      const uint slot = mem.slot;
      if (slot >= texture_info.size()) {
        /* Allocate some slots in advance, to reduce amount of re-allocations. */
        texture_info.resize(slot + 128);
      }
      texture_info[slot] = tex_info;
      need_texture_info = true;
    }
  }
  catch (sycl::exception const &e) {
    set_error("GPU texture allocation failed: runtime exception \"" + string(e.what()) + "\"");
  }
}

void OneapiDevice::tex_copy_to(device_texture &mem)
{
  if (!mem.device_pointer) {
    tex_alloc(mem);
  }
  else {
    if (mem.data_height > 0) {
      /* 2D/3D texture -- Tile optimized */
      sycl::ext::oneapi::experimental::image_descriptor desc = image_desc(mem);

      sycl::queue *queue = reinterpret_cast<sycl::queue *>(device_queue_);

      try {
        /* Copy data from host to the texture properly based on the texture description */
        thread_scoped_lock lock(device_mem_map_mutex);
        const Mem &cmem = device_mem_map[&mem];
        sycl::ext::oneapi::experimental::image_mem_handle image_handle{
            (sycl::ext::oneapi::experimental::image_mem_handle::raw_handle_type)cmem.array};
        queue->ext_oneapi_copy(mem.host_pointer, image_handle, desc);

#  ifdef WITH_CYCLES_DEBUG
        queue->wait_and_throw();
#  endif
      }
      catch (sycl::exception const &e) {
        set_error("oneAPI texture copy error: got runtime exception \"" + string(e.what()) + "\"");
      }
    }
    else {
      generic_copy_to(mem);
    }
  }
}

void OneapiDevice::tex_free(device_texture &mem)
{
  if (mem.device_pointer) {
    thread_scoped_lock lock(device_mem_map_mutex);
    DCHECK(device_mem_map.find(&mem) != device_mem_map.end());
    const Mem &cmem = device_mem_map[&mem];

    sycl::queue *queue = reinterpret_cast<sycl::queue *>(device_queue_);

    if (cmem.texobject) {
      /* Free bindless texture itself. */
      sycl::ext::oneapi::experimental::sampled_image_handle image(cmem.texobject);
      sycl::ext::oneapi::experimental::destroy_image_handle(image, *queue);
    }

    if (cmem.array) {
      /* Free texture memory. */
      sycl::ext::oneapi::experimental::image_mem_handle imgHandle{
          (sycl::ext::oneapi::experimental::image_mem_handle::raw_handle_type)cmem.array};

      try {
        /* We have allocated only standard textures, so we also deallocate only them. */
        sycl::ext::oneapi::experimental::free_image_mem(
            imgHandle, sycl::ext::oneapi::experimental::image_type::standard, *queue);
      }
      catch (sycl::exception const &e) {
        set_error("oneAPI texture deallocation error: got runtime exception \"" +
                  string(e.what()) + "\"");
      }

      stats.mem_free(mem.memory_size());
      mem.device_pointer = 0;
      mem.device_size = 0;
      device_mem_map.erase(device_mem_map.find(&mem));
    }
    else {
      lock.unlock();
      generic_free(mem);
    }
  }
}

unique_ptr<DeviceQueue> OneapiDevice::gpu_queue_create()
{
  return make_unique<OneapiDeviceQueue>(this);
}

bool OneapiDevice::should_use_graphics_interop(const GraphicsInteropDevice & /*interop_device*/,
                                               const bool /*log*/)
{
  /* NOTE(@nsirgien): oneAPI doesn't yet support direct writing into graphics API objects, so
   * return false. */
  return false;
}

void *OneapiDevice::usm_aligned_alloc_host(const size_t memory_size, const size_t alignment)
{
  assert(device_queue_);
  return usm_aligned_alloc_host(device_queue_, memory_size, alignment);
}

void OneapiDevice::usm_free(void *usm_ptr)
{
  assert(device_queue_);
  usm_free(device_queue_, usm_ptr);
}

void OneapiDevice::check_usm(SyclQueue *queue_, const void *usm_ptr, bool allow_host = false)
{
#  ifndef NDEBUG
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  sycl::info::device_type device_type =
      queue->get_device().get_info<sycl::info::device::device_type>();
  sycl::usm::alloc usm_type = get_pointer_type(usm_ptr, queue->get_context());
  (void)usm_type;
#    ifndef WITH_ONEAPI_SYCL_HOST_TASK
  const sycl::usm::alloc main_memory_type = sycl::usm::alloc::device;
#    else
  const sycl::usm::alloc main_memory_type = sycl::usm::alloc::host;
#    endif
  assert(usm_type == main_memory_type ||
         (usm_type == sycl::usm::alloc::host &&
          (allow_host || device_type == sycl::info::device_type::cpu)) ||
         usm_type == sycl::usm::alloc::unknown);
#  else
  /* Silence warning about unused arguments. */
  (void)queue_;
  (void)usm_ptr;
  (void)allow_host;
#  endif
}

bool OneapiDevice::create_queue(SyclQueue *&external_queue,
                                const int device_index,
                                void *embree_device_pointer)
{
  bool finished_correct = true;
  try {
    std::vector<sycl::device> devices = available_sycl_devices();
    if (device_index < 0 || device_index >= devices.size()) {
      return false;
    }

    sycl::queue *created_queue = nullptr;
    if (devices.size() == 1) {
      created_queue = new sycl::queue(devices[device_index], sycl::property::queue::in_order());
    }
    else {
      sycl::context device_context(devices[device_index]);
      created_queue = new sycl::queue(
          device_context, devices[device_index], sycl::property::queue::in_order());
      LOG_DEBUG << "Separate context was generated for the new queue, as several available SYCL "
                   "devices were detected";
    }
    external_queue = reinterpret_cast<SyclQueue *>(created_queue);

#  ifdef WITH_EMBREE_GPU
    if (embree_device_pointer) {
      RTCDevice *device_object_ptr = reinterpret_cast<RTCDevice *>(embree_device_pointer);
      *device_object_ptr = rtcNewSYCLDevice(created_queue->get_context(), "");
      if (*device_object_ptr == nullptr) {
        finished_correct = false;
        oneapi_error_string_ =
            "Hardware Raytracing is not available; please install "
            "\"intel-level-zero-gpu-raytracing\" to enable it or disable Embree on GPU.";
      }
      else {
        rtcSetDeviceSYCLDevice(*device_object_ptr, devices[device_index]);
      }
    }
#  else
    (void)embree_device_pointer;
#  endif
  }
  catch (const sycl::exception &e) {
    finished_correct = false;
    oneapi_error_string_ = e.what();
  }
  return finished_correct;
}

void OneapiDevice::free_queue(SyclQueue *queue_)
{
  assert(queue_);
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  delete queue;
}

void *OneapiDevice::usm_aligned_alloc_host(SyclQueue *queue_,
                                           size_t memory_size,
                                           const size_t alignment)
{
  assert(queue_);
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  return sycl::aligned_alloc_host(alignment, memory_size, *queue);
}

void *OneapiDevice::usm_alloc_device(SyclQueue *queue_, size_t memory_size)
{
  assert(queue_);
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  /* NOTE(@nsirgien): There are three types of Unified Shared Memory (USM) in oneAPI: host, device
   * and shared. For new project it could more beneficial to use USM shared memory, because it
   * provides automatic migration mechanism in order to allow to use the same pointer on host and
   * on device, without need to worry about explicit memory transfer operations, although usage of
   * USM shared imply some documented limitations on the memory usage in regards of parallel access
   * from different threads. But for Blender/Cycles this type of memory is not very suitable in
   * current application architecture, because Cycles is multi-thread application and already uses
   * two different pointer for host activity and device activity, and also has to perform all
   * needed memory transfer operations. So, USM device memory type has been used for oneAPI device
   * in order to better fit in Cycles architecture. */
#  ifndef WITH_ONEAPI_SYCL_HOST_TASK
  return sycl::malloc_device(memory_size, *queue);
#  else
  return sycl::malloc_host(memory_size, *queue);
#  endif
}

void OneapiDevice::usm_free(SyclQueue *queue_, void *usm_ptr)
{
  assert(queue_);
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  OneapiDevice::check_usm(queue_, usm_ptr, true);
  sycl::free(usm_ptr, *queue);
}

bool OneapiDevice::usm_memcpy(SyclQueue *queue_, void *dest, void *src, const size_t num_bytes)
{
  assert(queue_);
  /* sycl::queue::memcpy may crash if the queue is in an invalid state due to previous
   * runtime errors. It's better to avoid running memory operations in that case.
   * The render will be canceled and the queue will be destroyed anyway. */
  if (have_error()) {
    return false;
  }

  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  OneapiDevice::check_usm(queue_, dest, true);
  OneapiDevice::check_usm(queue_, src, true);
  sycl::usm::alloc dest_type = get_pointer_type(dest, queue->get_context());
  sycl::usm::alloc src_type = get_pointer_type(src, queue->get_context());
  /* Unknown here means, that this is not an USM allocation, which implies that this is
   * some generic C++ allocation, so we could use C++ memcpy directly with USM host. */
  if ((dest_type == sycl::usm::alloc::host || dest_type == sycl::usm::alloc::unknown) &&
      (src_type == sycl::usm::alloc::host || src_type == sycl::usm::alloc::unknown))
  {
    memcpy(dest, src, num_bytes);
    return true;
  }

  try {
    sycl::event mem_event = queue->memcpy(dest, src, num_bytes);
#  ifdef WITH_CYCLES_DEBUG
    /* NOTE(@nsirgien) Waiting on memory operation may give more precise error
     * messages. Due to impact on occupancy, it makes sense to enable it only during Cycles debug.
     */
    mem_event.wait_and_throw();
    return true;
#  else
    bool from_device_to_host = dest_type == sycl::usm::alloc::host &&
                               src_type == sycl::usm::alloc::device;
    bool host_or_device_memop_with_offset = dest_type == sycl::usm::alloc::unknown ||
                                            src_type == sycl::usm::alloc::unknown;
    /* NOTE(@sirgienko) Host-side blocking wait on this operation is mandatory, otherwise the host
     * may not wait until the end of the transfer before using the memory.
     */
    if (from_device_to_host || host_or_device_memop_with_offset) {
      mem_event.wait();
    }
    return true;
#  endif
  }
  catch (const sycl::exception &e) {
    oneapi_error_string_ = e.what();
    return false;
  }
}

bool OneapiDevice::usm_memset(SyclQueue *queue_,
                              void *usm_ptr,
                              unsigned char value,
                              const size_t num_bytes)
{
  assert(queue_);
  /* sycl::queue::memset may crash if the queue is in an invalid state due to previous
   * runtime errors. It's better to avoid running memory operations in that case.
   * The render will be canceled and the queue will be destroyed anyway. */
  if (have_error()) {
    return false;
  }

  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  OneapiDevice::check_usm(queue_, usm_ptr, true);
  try {
    sycl::event mem_event = queue->memset(usm_ptr, value, num_bytes);
#  ifdef WITH_CYCLES_DEBUG
    /* NOTE(@nsirgien) Waiting on memory operation may give more precise error
     * messages. Due to impact on occupancy, it makes sense to enable it only during Cycles debug.
     */
    mem_event.wait_and_throw();
#  else
    (void)mem_event;
#  endif
    return true;
  }
  catch (const sycl::exception &e) {
    oneapi_error_string_ = e.what();
    return false;
  }
}

bool OneapiDevice::queue_synchronize(SyclQueue *queue_)
{
  assert(queue_);
  sycl::queue *queue = reinterpret_cast<sycl::queue *>(queue_);
  try {
    queue->wait_and_throw();
    return true;
  }
  catch (const sycl::exception &e) {
    oneapi_error_string_ = e.what();
    return false;
  }
}

bool OneapiDevice::kernel_globals_size(size_t &kernel_global_size)
{
  kernel_global_size = sizeof(KernelGlobalsGPU);

  return true;
}

void OneapiDevice::set_global_memory(SyclQueue *queue_,
                                     void *kernel_globals,
                                     const char *memory_name,
                                     void *memory_device_pointer)
{
  assert(queue_);
  assert(kernel_globals);
  assert(memory_name);
  assert(memory_device_pointer);
  KernelGlobalsGPU *globals = (KernelGlobalsGPU *)kernel_globals;
  OneapiDevice::check_usm(queue_, memory_device_pointer, true);
  OneapiDevice::check_usm(queue_, kernel_globals, true);

  std::string matched_name(memory_name);

/* This macro will change global ptr of KernelGlobals via name matching. */
#  define KERNEL_DATA_ARRAY(type, name) \
    else if (#name == matched_name) { \
      globals->__##name = (type *)memory_device_pointer; \
      return; \
    }
  if (false) {
  }
  else if ("integrator_state" == matched_name) {
    globals->integrator_state = (IntegratorStateGPU *)memory_device_pointer;
    return;
  }
  KERNEL_DATA_ARRAY(KernelData, data)
#  include "kernel/data_arrays.h"
  else {
    std::cerr << "Can't found global/constant memory with name \"" << matched_name << "\"!"
              << std::endl;
    assert(false);
  }
#  undef KERNEL_DATA_ARRAY
}

bool OneapiDevice::enqueue_kernel(KernelContext *kernel_context,
                                  const int kernel,
                                  const size_t global_size,
                                  const size_t local_size,
                                  void **args)
{
  return oneapi_enqueue_kernel(kernel_context,
                               kernel,
                               global_size,
                               local_size,
                               kernel_features,
                               use_hardware_raytracing,
                               args);
}

void OneapiDevice::get_adjusted_global_and_local_sizes(SyclQueue *queue,
                                                       const DeviceKernel kernel,
                                                       size_t &kernel_global_size,
                                                       size_t &kernel_local_size)
{
  assert(queue);
  static const size_t preferred_work_group_size_intersect = 128;
  static const size_t preferred_work_group_size_shading = 256;
  static const size_t preferred_work_group_size_shading_simd8 = 64;
  /* Shader evaluation kernels seems to use some amount of shared memory, so better
   * to avoid usage of maximum work group sizes for them. */
  static const size_t preferred_work_group_size_shader_evaluation = 256;
  /* NOTE(@nsirgien): 1024 currently may lead to issues with cryptomatte kernels, so
   * for now their work-group size is restricted to 512. */
  static const size_t preferred_work_group_size_cryptomatte = 512;
  static const size_t preferred_work_group_size_default = 1024;

  const sycl::device &device = reinterpret_cast<sycl::queue *>(queue)->get_device();
  const size_t max_work_group_size = device.get_info<sycl::info::device::max_work_group_size>();

  size_t preferred_work_group_size = 0;
  switch (kernel) {
    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA:
    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_BAKE:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_DEDICATED_LIGHT:
      preferred_work_group_size = preferred_work_group_size_intersect;
      break;

    case DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEDICATED_LIGHT: {
      const bool device_is_simd8 =
          (device.has(sycl::aspect::ext_intel_gpu_eu_simd_width) &&
           device.get_info<sycl::ext::intel::info::device::gpu_eu_simd_width>() == 8);
      preferred_work_group_size = (device_is_simd8) ? preferred_work_group_size_shading_simd8 :
                                                      preferred_work_group_size_shading;
    } break;

    case DEVICE_KERNEL_CRYPTOMATTE_POSTPROCESS:
      preferred_work_group_size = preferred_work_group_size_cryptomatte;
      break;

    case DEVICE_KERNEL_SHADER_EVAL_DISPLACE:
    case DEVICE_KERNEL_SHADER_EVAL_BACKGROUND:
    case DEVICE_KERNEL_SHADER_EVAL_CURVE_SHADOW_TRANSPARENCY:
      preferred_work_group_size = preferred_work_group_size_shader_evaluation;
      break;

    default:
      /* Do nothing and keep initial zero value. */
      break;
  }

  /* Such order of logic allow us to override Blender default values, if needed,
   * yet respect them otherwise. */
  if (preferred_work_group_size == 0) {
    preferred_work_group_size = oneapi_suggested_gpu_kernel_size((::DeviceKernel)kernel);
  }

  /* If there is no recommendation, then use manual default value. */
  if (preferred_work_group_size == 0) {
    preferred_work_group_size = preferred_work_group_size_default;
  }

  kernel_local_size = std::min(max_work_group_size, preferred_work_group_size);

  /* NOTE(@nsirgien): As for now non-uniform work-groups don't work on most oneAPI devices,
   * we extend work size to fit uniformity requirements. */
  kernel_global_size = round_up(kernel_global_size, kernel_local_size);

#  ifdef WITH_ONEAPI_SYCL_HOST_TASK
  /* Kernels listed below need a specific number of work groups. */
  if (kernel == DEVICE_KERNEL_INTEGRATOR_ACTIVE_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_QUEUED_SHADOW_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_TERMINATED_SHADOW_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_COMPACT_PATHS_ARRAY ||
      kernel == DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_PATHS_ARRAY)
  {
    /* Path array implementation is serial in case of SYCL Host Task execution. */
    kernel_global_size = 1;
    kernel_local_size = 1;
  }
#  endif

  assert(kernel_global_size % kernel_local_size == 0);
}

/* Compute-runtime (ie. NEO) version is what gets returned by sycl/L0 on Windows
 * since Windows driver 101.3268. */
static const int lowest_supported_driver_version_win = 1016554;
#  ifdef _WIN32
/* For Windows driver 101.6557, compute-runtime version is 31896.
 * This information is returned by `ocloc query OCL_DRIVER_VERSION`. */
static const int lowest_supported_driver_version_neo = 31896;
#  else
static const int lowest_supported_driver_version_neo = 31740;
#  endif

int parse_driver_build_version(const sycl::device &device)
{
  const std::string &driver_version = device.get_info<sycl::info::device::driver_version>();
  int driver_build_version = 0;

  size_t second_dot_position = driver_version.find('.', driver_version.find('.') + 1);
  if (second_dot_position != std::string::npos) {
    try {
      size_t third_dot_position = driver_version.find('.', second_dot_position + 1);
      if (third_dot_position != std::string::npos) {
        const std::string &third_number_substr = driver_version.substr(
            second_dot_position + 1, third_dot_position - second_dot_position - 1);
        const std::string &forth_number_substr = driver_version.substr(third_dot_position + 1);
        if (third_number_substr.length() == 3 && forth_number_substr.length() == 4) {
          driver_build_version = std::stoi(third_number_substr) * 10000 +
                                 std::stoi(forth_number_substr);
        }
      }
      else {
        const std::string &third_number_substr = driver_version.substr(second_dot_position + 1);
        driver_build_version = std::stoi(third_number_substr);
      }
    }
    catch (std::invalid_argument &) {
    }
  }

  if (driver_build_version == 0) {
    LOG_WARNING << "Unable to parse unknown Intel GPU driver version. \"" << driver_version
                << "\" does not match xx.xx.xxxxx (Linux), x.x.xxxx (L0),"
                << " xx.xx.xxx.xxxx (Windows) for device \""
                << device.get_info<sycl::info::device::name>() << "\".";
  }

  return driver_build_version;
}

std::vector<sycl::device> available_sycl_devices()
{
  std::vector<sycl::device> available_devices;
  bool allow_all_devices = false;
  if (getenv("CYCLES_ONEAPI_ALL_DEVICES") != nullptr) {
    allow_all_devices = true;
  }

  try {
    const std::vector<sycl::platform> &oneapi_platforms = sycl::platform::get_platforms();

    for (const sycl::platform &platform : oneapi_platforms) {
      /* ignore OpenCL platforms to avoid using the same devices through both Level-Zero and
       * OpenCL.
       */
      if (platform.get_backend() == sycl::backend::opencl) {
        continue;
      }

      const std::vector<sycl::device> &oneapi_devices =
          (allow_all_devices) ? platform.get_devices(sycl::info::device_type::all) :
                                platform.get_devices(sycl::info::device_type::gpu);

      for (const sycl::device &device : oneapi_devices) {
        bool filter_out = false;
        if (!allow_all_devices) {
          /* For now we support all Intel(R) Arc(TM) devices and likely any future GPU,
           * assuming they have either more than 96 Execution Units or not 7 threads per EU.
           * Official support can be broaden to older and smaller GPUs once ready. */
          if (!device.is_gpu() || platform.get_backend() != sycl::backend::ext_oneapi_level_zero) {
            filter_out = true;
          }
          else {
            /* Filtered-out defaults in-case these values aren't available. */
            int number_of_eus = 96;
            int threads_per_eu = 7;
            if (device.has(sycl::aspect::ext_intel_gpu_eu_count)) {
              number_of_eus = device.get_info<sycl::ext::intel::info::device::gpu_eu_count>();
            }
            if (device.has(sycl::aspect::ext_intel_gpu_hw_threads_per_eu)) {
              threads_per_eu =
                  device.get_info<sycl::ext::intel::info::device::gpu_hw_threads_per_eu>();
            }
            /* This filters out all Level-Zero supported GPUs from older generation than Arc. */
            if (number_of_eus <= 96 && threads_per_eu == 7) {
              filter_out = true;
            }
            /* if not already filtered out, check driver version. */
            bool check_driver_version = !filter_out;
            /* We don't know how to check driver version strings for non-Intel GPUs. */
            if (check_driver_version &&
                device.get_info<sycl::info::device::vendor>().find("Intel") == std::string::npos)
            {
              check_driver_version = false;
            }
            /* Because of https://github.com/oneapi-src/unified-runtime/issues/1777, future drivers
             * may break parsing done by a SYCL runtime from before the fix we expect in major
             * version 8. Parsed driver version would start with something different than current
             * "1.3.". To avoid blocking a device by mistake in the case of new driver / old SYCL
             * runtime, we disable driver version check in case LIBSYCL_MAJOR_VERSION is below 8
             * and actual driver version doesn't start with 1.3. */
#  if __LIBSYCL_MAJOR_VERSION < 8
            if (check_driver_version &&
                !string_startswith(device.get_info<sycl::info::device::driver_version>(), "1.3."))
            {
              check_driver_version = false;
            }
#  endif
            if (check_driver_version) {
              int driver_build_version = parse_driver_build_version(device);
              const int lowest_supported_driver_version = (driver_build_version > 100000) ?
                                                              lowest_supported_driver_version_win :
                                                              lowest_supported_driver_version_neo;
              if (driver_build_version < lowest_supported_driver_version) {
                filter_out = true;

                LOG_WARNING << "Driver version for device \""
                            << device.get_info<sycl::info::device::name>()
                            << "\" is too old. Expected \"" << lowest_supported_driver_version
                            << "\" or newer, but got \"" << driver_build_version << "\".";
              }
            }
          }
        }
        if (!filter_out) {
          available_devices.push_back(device);
        }
      }
    }
  }
  catch (sycl::exception &e) {
    LOG_WARNING << "An error has been encountered while enumerating SYCL devices: " << e.what();
  }
  return available_devices;
}

void OneapiDevice::architecture_information(const SyclDevice *device,
                                            string &name,
                                            bool &is_optimized)
{
  const sycl::ext::oneapi::experimental::architecture arch =
      reinterpret_cast<const sycl::device *>(device)
          ->get_info<sycl::ext::oneapi::experimental::info::device::architecture>();

#  define FILL_ARCH_INFO(architecture_code, is_arch_optimised) \
    case sycl::ext::oneapi::experimental::architecture ::architecture_code: \
      name = #architecture_code; \
      is_optimized = is_arch_optimised; \
      break;

  /* List of architectures that have been optimized by Intel and Blender developers.
   *
   * For example, Intel Rocket Lake iGPU (rkl) is not supported and not optimized,
   * while Intel Arc Alchemist dGPU (dg2) was optimized for.
   *
   * Devices can changed from unoptimized to optimized manually, after DPC++ has
   * been upgraded to support the architecture and CYCLES_ONEAPI_INTEL_BINARIES_ARCH
   * in CMake includes the architecture. */
  switch (arch) {
    FILL_ARCH_INFO(intel_gpu_bdw, false)
    FILL_ARCH_INFO(intel_gpu_skl, false)
    FILL_ARCH_INFO(intel_gpu_kbl, false)
    FILL_ARCH_INFO(intel_gpu_cfl, false)
    FILL_ARCH_INFO(intel_gpu_apl, false)
    FILL_ARCH_INFO(intel_gpu_glk, false)
    FILL_ARCH_INFO(intel_gpu_whl, false)
    FILL_ARCH_INFO(intel_gpu_aml, false)
    FILL_ARCH_INFO(intel_gpu_cml, false)
    FILL_ARCH_INFO(intel_gpu_icllp, false)
    FILL_ARCH_INFO(intel_gpu_ehl, false)
    FILL_ARCH_INFO(intel_gpu_tgllp, false)
    FILL_ARCH_INFO(intel_gpu_rkl, false)
    FILL_ARCH_INFO(intel_gpu_adl_s, false)
    FILL_ARCH_INFO(intel_gpu_adl_p, false)
    FILL_ARCH_INFO(intel_gpu_adl_n, false)
    FILL_ARCH_INFO(intel_gpu_dg1, false)
    FILL_ARCH_INFO(intel_gpu_dg2_g10, true)
    FILL_ARCH_INFO(intel_gpu_dg2_g11, true)
    FILL_ARCH_INFO(intel_gpu_dg2_g12, true)
    FILL_ARCH_INFO(intel_gpu_pvc, false)
    FILL_ARCH_INFO(intel_gpu_pvc_vg, false)
    /* intel_gpu_mtl_u == intel_gpu_mtl_s == intel_gpu_arl_u == intel_gpu_arl_s */
    FILL_ARCH_INFO(intel_gpu_mtl_u, true)
    FILL_ARCH_INFO(intel_gpu_mtl_h, true)
    FILL_ARCH_INFO(intel_gpu_bmg_g21, true)
    FILL_ARCH_INFO(intel_gpu_lnl_m, true)

    default:
      name = "unknown";
      is_optimized = false;
      break;
  }
}

char *OneapiDevice::device_capabilities()
{
  std::stringstream capabilities;

  const std::vector<sycl::device> &oneapi_devices = available_sycl_devices();
  for (const sycl::device &device : oneapi_devices) {
#  ifndef WITH_ONEAPI_SYCL_HOST_TASK
    const std::string &name = device.get_info<sycl::info::device::name>();
#  else
    const std::string &name = "SYCL Host Task (Debug)";
#  endif

    capabilities << std::string("\t") << name << "\n";
    capabilities << "\t\tsycl::info::platform::name\t\t\t"
                 << device.get_platform().get_info<sycl::info::platform::name>() << "\n";

    string arch_name;
    bool is_optimised_for_arch;
    architecture_information(
        reinterpret_cast<const SyclDevice *>(&device), arch_name, is_optimised_for_arch);
    capabilities << "\t\tsycl::info::device::architecture\t\t\t";
    capabilities << arch_name << "\n";
    capabilities << "\t\tsycl::info::device::is_cycles_optimized\t\t\t";
    capabilities << is_optimised_for_arch << "\n";

#  define WRITE_ATTR(attribute_name, attribute_variable) \
    capabilities << "\t\tsycl::info::device::" #attribute_name "\t\t\t" << attribute_variable \
                 << "\n";
#  define GET_ATTR(attribute) \
    { \
      capabilities << "\t\tsycl::info::device::" #attribute "\t\t\t" \
                   << device.get_info<sycl::info::device ::attribute>() << "\n"; \
    }
#  define GET_INTEL_ATTR(attribute) \
    { \
      if (device.has(sycl::aspect::ext_intel_##attribute)) { \
        capabilities << "\t\tsycl::ext::intel::info::device::" #attribute "\t\t\t" \
                     << device.get_info<sycl::ext::intel::info::device ::attribute>() << "\n"; \
      } \
    }
#  define GET_ASPECT(aspect_) \
    { \
      capabilities << "\t\tdevice::has(" #aspect_ ")\t\t\t" << device.has(sycl::aspect ::aspect_) \
                   << "\n"; \
    }

    GET_ATTR(vendor)
    GET_ATTR(driver_version)
    GET_ATTR(max_compute_units)
    GET_ATTR(max_clock_frequency)
    GET_ATTR(global_mem_size)
    GET_INTEL_ATTR(pci_address)
    GET_INTEL_ATTR(gpu_eu_simd_width)
    GET_INTEL_ATTR(gpu_eu_count)
    GET_INTEL_ATTR(gpu_slices)
    GET_INTEL_ATTR(gpu_subslices_per_slice)
    GET_INTEL_ATTR(gpu_eu_count_per_subslice)
    GET_INTEL_ATTR(gpu_hw_threads_per_eu)
    GET_INTEL_ATTR(max_mem_bandwidth)
    GET_ATTR(max_work_group_size)
    GET_ATTR(max_work_item_dimensions)
    sycl::id<3> max_work_item_sizes =
        device.get_info<sycl::info::device::max_work_item_sizes<3>>();
    WRITE_ATTR(max_work_item_sizes[0], max_work_item_sizes.get(0))
    WRITE_ATTR(max_work_item_sizes[1], max_work_item_sizes.get(1))
    WRITE_ATTR(max_work_item_sizes[2], max_work_item_sizes.get(2))

    GET_ATTR(max_num_sub_groups)
    for (size_t sub_group_size : device.get_info<sycl::info::device::sub_group_sizes>()) {
      WRITE_ATTR(sub_group_size[], sub_group_size)
    }
    GET_ATTR(sub_group_independent_forward_progress)

    GET_ATTR(preferred_vector_width_char)
    GET_ATTR(preferred_vector_width_short)
    GET_ATTR(preferred_vector_width_int)
    GET_ATTR(preferred_vector_width_long)
    GET_ATTR(preferred_vector_width_float)
    GET_ATTR(preferred_vector_width_double)
    GET_ATTR(preferred_vector_width_half)

    GET_ATTR(address_bits)
    GET_ATTR(max_mem_alloc_size)
    GET_ATTR(mem_base_addr_align)
    GET_ATTR(error_correction_support)
    GET_ATTR(is_available)

    GET_ASPECT(cpu)
    GET_ASPECT(gpu)
    GET_ASPECT(fp16)
    GET_ASPECT(atomic64)
    GET_ASPECT(usm_host_allocations)
    GET_ASPECT(usm_device_allocations)
    GET_ASPECT(usm_shared_allocations)
    GET_ASPECT(usm_system_allocations)

#  ifdef __SYCL_ANY_DEVICE_HAS_ext_oneapi_non_uniform_groups__
    GET_ASPECT(ext_oneapi_non_uniform_groups)
#  endif
#  ifdef __SYCL_ANY_DEVICE_HAS_ext_oneapi_bindless_images__
    GET_ASPECT(ext_oneapi_bindless_images)
#  endif
#  ifdef __SYCL_ANY_DEVICE_HAS_ext_oneapi_interop_semaphore_import__
    GET_ASPECT(ext_oneapi_interop_semaphore_import)
#  endif
#  ifdef __SYCL_ANY_DEVICE_HAS_ext_oneapi_interop_semaphore_export__
    GET_ASPECT(ext_oneapi_interop_semaphore_export)
#  endif

#  undef GET_INTEL_ATTR
#  undef GET_ASPECT
#  undef GET_ATTR
#  undef WRITE_ATTR
    capabilities << "\n";
  }

  return ::strdup(capabilities.str().c_str());
}

void OneapiDevice::iterate_devices(OneAPIDeviceIteratorCallback cb, void *user_ptr)
{
  int num = 0;
  std::vector<sycl::device> devices = available_sycl_devices();
  for (sycl::device &device : devices) {
    const std::string &platform_name =
        device.get_platform().get_info<sycl::info::platform::name>();
#  ifndef WITH_ONEAPI_SYCL_HOST_TASK
    std::string name = device.get_info<sycl::info::device::name>();
#  else
    std::string name = "SYCL Host Task (Debug)";
#  endif
#  ifdef WITH_EMBREE_GPU
    bool hwrt_support = rtcIsSYCLDeviceSupported(device);
#  else
    bool hwrt_support = false;
#  endif
#  if defined(WITH_OPENIMAGEDENOISE) && OIDN_VERSION >= 20300
    bool oidn_support = oidnIsSYCLDeviceSupported(&device);
#  else
    bool oidn_support = false;
#  endif
    std::string id = "ONEAPI_" + platform_name + "_" + name;

    string arch_name;
    bool is_optimised_for_arch;
    architecture_information(
        reinterpret_cast<const SyclDevice *>(&device), arch_name, is_optimised_for_arch);

    if (device.has(sycl::aspect::ext_intel_pci_address)) {
      id.append("_" + device.get_info<sycl::ext::intel::info::device::pci_address>());
    }
    (cb)(id.c_str(),
         name.c_str(),
         num,
         hwrt_support,
         oidn_support,
         is_optimised_for_arch,
         user_ptr);
    num++;
  }
}

size_t OneapiDevice::get_memcapacity()
{
  return reinterpret_cast<sycl::queue *>(device_queue_)
      ->get_device()
      .get_info<sycl::info::device::global_mem_size>();
}

int OneapiDevice::get_num_multiprocessors()
{
  const sycl::device &device = reinterpret_cast<sycl::queue *>(device_queue_)->get_device();
  if (device.has(sycl::aspect::ext_intel_gpu_eu_count)) {
    return device.get_info<sycl::ext::intel::info::device::gpu_eu_count>();
  }
  return device.get_info<sycl::info::device::max_compute_units>();
}

int OneapiDevice::get_max_num_threads_per_multiprocessor()
{
  const sycl::device &device = reinterpret_cast<sycl::queue *>(device_queue_)->get_device();
  if (device.has(sycl::aspect::ext_intel_gpu_eu_simd_width) &&
      device.has(sycl::aspect::ext_intel_gpu_hw_threads_per_eu))
  {
    return device.get_info<sycl::ext::intel::info::device::gpu_eu_simd_width>() *
           device.get_info<sycl::ext::intel::info::device::gpu_hw_threads_per_eu>();
  }
  /* We'd want sycl::info::device::max_threads_per_compute_unit which doesn't exist yet.
   * max_work_group_size is the closest approximation but it can still be several times off. */
  return device.get_info<sycl::info::device::max_work_group_size>();
}

CCL_NAMESPACE_END

#endif
