/* SPDX-FileCopyrightText: 2018 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Author: Sergey Sharybin. */

#include <cassert>

#ifdef _MSC_VER
#  include <iso646.h>
#endif

#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/osd/mesh.h>
#include <opensubdiv/osd/types.h>
#include <opensubdiv/version.h>

#include "internal/evaluator/eval_output_cpu.h"
#include "internal/evaluator/eval_output_gpu.h"
#include "internal/evaluator/evaluator_cache_impl.h"
#include "internal/evaluator/patch_map.h"
#include "opensubdiv_evaluator.hh"
#include "opensubdiv_evaluator_capi.hh"
#include "opensubdiv_topology_refiner.hh"

using OpenSubdiv::Far::PatchTable;
using OpenSubdiv::Far::PatchTableFactory;
using OpenSubdiv::Far::StencilTable;
using OpenSubdiv::Far::StencilTableFactory;
using OpenSubdiv::Far::StencilTableReal;
using OpenSubdiv::Far::TopologyRefiner;
using OpenSubdiv::Osd::PatchArray;
using OpenSubdiv::Osd::PatchCoord;

namespace blender::opensubdiv {

// Array implementation which stores small data on stack (or, rather, in the class itself).
template<typename T, int kNumMaxElementsOnStack> class StackOrHeapArray {
 public:
  StackOrHeapArray()
      : num_elements_(0),
        heap_elements_(nullptr),
        num_heap_elements_(0),
        effective_elements_(nullptr)
  {
  }

  explicit StackOrHeapArray(int size) : StackOrHeapArray()
  {
    resize(size);
  }

  ~StackOrHeapArray()
  {
    delete[] heap_elements_;
  }

  int size() const
  {
    return num_elements_;
  };

  T *data()
  {
    return effective_elements_;
  }

  void resize(int num_elements)
  {
    const int old_num_elements = num_elements_;
    num_elements_ = num_elements;
    // Early output if allcoation size did not change, or allocation size is smaller.
    // We never re-allocate, sacrificing some memory over performance.
    if (old_num_elements >= num_elements) {
      return;
    }
    // Simple case: no previously allocated buffer, can simply do one allocation.
    if (effective_elements_ == nullptr) {
      effective_elements_ = allocate(num_elements);
      return;
    }
    // Make new allocation, and copy elements if needed.
    T *old_buffer = effective_elements_;
    effective_elements_ = allocate(num_elements);
    if (old_buffer != effective_elements_) {
      memcpy(
          effective_elements_, old_buffer, sizeof(T) * std::min(old_num_elements, num_elements));
    }
    if (old_buffer != stack_elements_) {
      delete[] old_buffer;
    }
  }

 protected:
  T *allocate(int num_elements)
  {
    if (num_elements < kNumMaxElementsOnStack) {
      return stack_elements_;
    }
    heap_elements_ = new T[num_elements];
    return heap_elements_;
  }

  // Number of elements in the buffer.
  int num_elements_;

  // Elements which are allocated on a stack (or, rather, in the same allocation as the buffer
  // itself).
  // Is used as long as buffer is smaller than kNumMaxElementsOnStack.
  T stack_elements_[kNumMaxElementsOnStack];

  // Heap storage for buffer larger than kNumMaxElementsOnStack.
  T *heap_elements_;
  int num_heap_elements_;

  // Depending on the current buffer size points to rither stack_elements_ or heap_elements_.
  T *effective_elements_;
};

// 32 is a number of inner vertices along the patch size at subdivision level 6.
using StackOrHeapPatchCoordArray = StackOrHeapArray<PatchCoord, 32 * 32>;

static void convertPatchCoordsToArray(const OpenSubdiv_PatchCoord *patch_coords,
                                      const int num_patch_coords,
                                      const PatchMap *patch_map,
                                      StackOrHeapPatchCoordArray *array)
{
  array->resize(num_patch_coords);
  for (int i = 0; i < num_patch_coords; ++i) {
    const PatchTable::PatchHandle *handle = patch_map->FindPatch(
        patch_coords[i].ptex_face, patch_coords[i].u, patch_coords[i].v);
    (array->data())[i] = PatchCoord(*handle, patch_coords[i].u, patch_coords[i].v);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Evaluator wrapper for anonymous API.

EvalOutputAPI::EvalOutputAPI(EvalOutput *implementation, PatchMap *patch_map)
    : patch_map_(patch_map), implementation_(implementation)
{
}

EvalOutputAPI::~EvalOutputAPI()
{
  delete implementation_;
}

void EvalOutputAPI::setSettings(const OpenSubdiv_EvaluatorSettings *settings)
{
  implementation_->updateSettings(settings);
}

void EvalOutputAPI::setCoarsePositions(const float *positions,
                                       const int start_vertex_index,
                                       const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateData(positions, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setVaryingData(const float *varying_data,
                                   const int start_vertex_index,
                                   const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateVaryingData(varying_data, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setVertexData(const float *vertex_data,
                                  const int start_vertex_index,
                                  const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateVertexData(vertex_data, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setFaceVaryingData(const int face_varying_channel,
                                       const float *face_varying_data,
                                       const int start_vertex_index,
                                       const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  implementation_->updateFaceVaryingData(
      face_varying_channel, face_varying_data, start_vertex_index, num_vertices);
}

void EvalOutputAPI::setCoarsePositionsFromBuffer(const void *buffer,
                                                 const int start_offset,
                                                 const int stride,
                                                 const int start_vertex_index,
                                                 const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateData(
        reinterpret_cast<const float *>(current_buffer), current_vertex_index, 1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::setVaryingDataFromBuffer(const void *buffer,
                                             const int start_offset,
                                             const int stride,
                                             const int start_vertex_index,
                                             const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateVaryingData(
        reinterpret_cast<const float *>(current_buffer), current_vertex_index, 1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::setFaceVaryingDataFromBuffer(const int face_varying_channel,
                                                 const void *buffer,
                                                 const int start_offset,
                                                 const int stride,
                                                 const int start_vertex_index,
                                                 const int num_vertices)
{
  // TODO(sergey): Add sanity check on indices.
  const unsigned char *current_buffer = (unsigned char *)buffer;
  current_buffer += start_offset;
  for (int i = 0; i < num_vertices; ++i) {
    const int current_vertex_index = start_vertex_index + i;
    implementation_->updateFaceVaryingData(face_varying_channel,
                                           reinterpret_cast<const float *>(current_buffer),
                                           current_vertex_index,
                                           1);
    current_buffer += stride;
  }
}

void EvalOutputAPI::refine()
{
  implementation_->refine();
}

void EvalOutputAPI::evaluateLimit(const int ptex_face_index,
                                  float face_u,
                                  float face_v,
                                  float P[3],
                                  float dPdu[3],
                                  float dPdv[3])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  if (dPdu != nullptr || dPdv != nullptr) {
    implementation_->evalPatchesWithDerivatives(&patch_coord, 1, P, dPdu, dPdv);
  }
  else {
    implementation_->evalPatches(&patch_coord, 1, P);
  }
}

void EvalOutputAPI::evaluateVarying(const int ptex_face_index,
                                    float face_u,
                                    float face_v,
                                    float varying[3])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  implementation_->evalPatchesVarying(&patch_coord, 1, varying);
}

void EvalOutputAPI::evaluateVertexData(const int ptex_face_index,
                                       float face_u,
                                       float face_v,
                                       float vertex_data[])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  implementation_->evalPatchesVertexData(&patch_coord, 1, vertex_data);
}

void EvalOutputAPI::evaluateFaceVarying(const int face_varying_channel,
                                        const int ptex_face_index,
                                        float face_u,
                                        float face_v,
                                        float face_varying[2])
{
  assert(face_u >= 0.0f);
  assert(face_u <= 1.0f);
  assert(face_v >= 0.0f);
  assert(face_v <= 1.0f);
  const PatchTable::PatchHandle *handle = patch_map_->FindPatch(ptex_face_index, face_u, face_v);
  PatchCoord patch_coord(*handle, face_u, face_v);
  implementation_->evalPatchesFaceVarying(face_varying_channel, &patch_coord, 1, face_varying);
}

void EvalOutputAPI::evaluatePatchesLimit(const OpenSubdiv_PatchCoord *patch_coords,
                                         const int num_patch_coords,
                                         float *P,
                                         float *dPdu,
                                         float *dPdv)
{
  StackOrHeapPatchCoordArray patch_coords_array;
  convertPatchCoordsToArray(patch_coords, num_patch_coords, patch_map_, &patch_coords_array);
  if (dPdu != nullptr || dPdv != nullptr) {
    implementation_->evalPatchesWithDerivatives(
        patch_coords_array.data(), num_patch_coords, P, dPdu, dPdv);
  }
  else {
    implementation_->evalPatches(patch_coords_array.data(), num_patch_coords, P);
  }
}

void EvalOutputAPI::getPatchMap(blender::gpu::VertBuf *patch_map_handles,
                                blender::gpu::VertBuf *patch_map_quadtree,
                                int *min_patch_face,
                                int *max_patch_face,
                                int *max_depth,
                                int *patches_are_triangular)
{
  *min_patch_face = patch_map_->getMinPatchFace();
  *max_patch_face = patch_map_->getMaxPatchFace();
  *max_depth = patch_map_->getMaxDepth();
  *patches_are_triangular = patch_map_->getPatchesAreTriangular();

  const std::vector<PatchTable::PatchHandle> &handles = patch_map_->getHandles();
  // TODO(jbakker): should these be SSBO's they are never bound as vertex buffers.
  GPU_vertbuf_data_alloc(*patch_map_handles, handles.size());
  MutableSpan<PatchTable::PatchHandle> buffer_handles =
      patch_map_handles->data<PatchTable::PatchHandle>();
  memcpy(buffer_handles.data(), handles.data(), sizeof(PatchTable::PatchHandle) * handles.size());

  const std::vector<PatchMap::QuadNode> &quadtree = patch_map_->nodes();
  GPU_vertbuf_data_alloc(*patch_map_quadtree, quadtree.size());
  MutableSpan<PatchMap::QuadNode> buffer_nodes = patch_map_quadtree->data<PatchMap::QuadNode>();
  memcpy(buffer_nodes.data(), quadtree.data(), sizeof(PatchMap::QuadNode) * quadtree.size());
}

gpu::StorageBuf *EvalOutputAPI::create_patch_arrays_buf()
{
  return implementation_->create_patch_arrays_buf();
}

gpu::StorageBuf *EvalOutputAPI::get_patch_index_buf()
{
  return implementation_->get_patch_index_buf();
}

gpu::StorageBuf *EvalOutputAPI::get_patch_param_buf()
{
  return implementation_->get_patch_param_buf();
}

gpu::VertBuf *EvalOutputAPI::get_source_buf()
{
  return implementation_->get_source_buf();
}

gpu::VertBuf *EvalOutputAPI::get_source_data_buf()
{
  return implementation_->get_source_data_buf();
}

gpu::StorageBuf *EvalOutputAPI::create_face_varying_patch_array_buf(const int face_varying_channel)
{
  return implementation_->create_face_varying_patch_array_buf(face_varying_channel);
}

gpu::StorageBuf *EvalOutputAPI::get_face_varying_patch_index_buf(const int face_varying_channel)
{
  return implementation_->get_face_varying_patch_index_buf(face_varying_channel);
}

gpu::StorageBuf *EvalOutputAPI::get_face_varying_patch_param_buf(const int face_varying_channel)
{
  return implementation_->get_face_varying_patch_param_buf(face_varying_channel);
}

gpu::VertBuf *EvalOutputAPI::get_face_varying_source_buf(const int face_varying_channel)
{
  return implementation_->get_face_varying_source_buf(face_varying_channel);
}

int EvalOutputAPI::get_face_varying_source_offset(const int face_varying_channel) const
{
  return implementation_->get_face_varying_source_offset(face_varying_channel);
}

bool EvalOutputAPI::hasVertexData() const
{
  return implementation_->hasVertexData();
}

}  // namespace blender::opensubdiv

OpenSubdiv_Evaluator::OpenSubdiv_Evaluator()
    : eval_output(nullptr), patch_map(nullptr), patch_table(nullptr)
{
}

OpenSubdiv_Evaluator::~OpenSubdiv_Evaluator()
{
  delete eval_output;
  delete patch_map;
  delete patch_table;
}

OpenSubdiv_Evaluator *openSubdiv_createEvaluatorFromTopologyRefiner(
    blender::opensubdiv::TopologyRefinerImpl *topology_refiner,
    eOpenSubdivEvaluator evaluator_type,
    OpenSubdiv_EvaluatorCache *evaluator_cache_descr)
{
  TopologyRefiner *refiner = topology_refiner->topology_refiner;
  if (refiner == nullptr) {
    // Happens on bad topology.
    return nullptr;
  }
  // TODO(sergey): Base this on actual topology.
  const bool has_varying_data = false;
  const int num_face_varying_channels = refiner->GetNumFVarChannels();
  const bool has_face_varying_data = (num_face_varying_channels != 0);
  const int level = topology_refiner->settings.level;
  const bool is_adaptive = topology_refiner->settings.is_adaptive;
  // Common settings for stencils and patches.
  const bool stencil_generate_intermediate_levels = is_adaptive;
  const bool stencil_generate_offsets = true;
  const bool use_inf_sharp_patch = true;
  // Refine the topology with given settings.
  // TODO(sergey): What if topology is already refined?
  if (is_adaptive) {
    TopologyRefiner::AdaptiveOptions options(level);
    options.considerFVarChannels = has_face_varying_data;
    options.useInfSharpPatch = use_inf_sharp_patch;
    refiner->RefineAdaptive(options);
  }
  else {
    TopologyRefiner::UniformOptions options(level);
    refiner->RefineUniform(options);
  }

  // Work around ASAN warnings, due to OpenSubdiv pretending to have an actual StencilTable
  // instance while it's really its base class.
  auto delete_stencil_table = [](const StencilTable *table) {
    static_assert(std::is_base_of_v<StencilTableReal<float>, StencilTable>);
    delete reinterpret_cast<const StencilTableReal<float> *>(table);
  };

  // Generate stencil table to update the bi-cubic patches control vertices
  // after they have been re-posed (both for vertex & varying interpolation).
  //
  // Vertex stencils.
  StencilTableFactory::Options vertex_stencil_options;
  vertex_stencil_options.generateOffsets = stencil_generate_offsets;
  vertex_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
  const StencilTable *vertex_stencils = StencilTableFactory::Create(*refiner,
                                                                    vertex_stencil_options);
  // Varying stencils.
  //
  // TODO(sergey): Seems currently varying stencils are always required in
  // OpenSubdiv itself.
  const StencilTable *varying_stencils = nullptr;
  if (has_varying_data) {
    StencilTableFactory::Options varying_stencil_options;
    varying_stencil_options.generateOffsets = stencil_generate_offsets;
    varying_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
    varying_stencil_options.interpolationMode = StencilTableFactory::INTERPOLATE_VARYING;
    varying_stencils = StencilTableFactory::Create(*refiner, varying_stencil_options);
  }
  // Face warying stencil.
  std::vector<const StencilTable *> all_face_varying_stencils;
  all_face_varying_stencils.reserve(num_face_varying_channels);
  for (int face_varying_channel = 0; face_varying_channel < num_face_varying_channels;
       ++face_varying_channel)
  {
    StencilTableFactory::Options face_varying_stencil_options;
    face_varying_stencil_options.generateOffsets = stencil_generate_offsets;
    face_varying_stencil_options.generateIntermediateLevels = stencil_generate_intermediate_levels;
    face_varying_stencil_options.interpolationMode = StencilTableFactory::INTERPOLATE_FACE_VARYING;
    face_varying_stencil_options.fvarChannel = face_varying_channel;
    all_face_varying_stencils.push_back(
        StencilTableFactory::Create(*refiner, face_varying_stencil_options));
  }
  // Generate bi-cubic patch table for the limit surface.
  PatchTableFactory::Options patch_options(level);
  patch_options.SetEndCapType(PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
  patch_options.useInfSharpPatch = use_inf_sharp_patch;
  patch_options.generateFVarTables = has_face_varying_data;
  patch_options.generateFVarLegacyLinearPatches = false;
  const PatchTable *patch_table = PatchTableFactory::Create(*refiner, patch_options);
  // Append local points stencils.
  // Point stencils.
  const StencilTable *local_point_stencil_table = patch_table->GetLocalPointStencilTable();
  if (local_point_stencil_table != nullptr) {
    const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTable(
        *refiner, vertex_stencils, local_point_stencil_table);
    delete_stencil_table(vertex_stencils);
    vertex_stencils = table;
  }
  // Varying stencils.
  if (has_varying_data) {
    const StencilTable *local_point_varying_stencil_table =
        patch_table->GetLocalPointVaryingStencilTable();
    if (local_point_varying_stencil_table != nullptr) {
      const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTable(
          *refiner, varying_stencils, local_point_varying_stencil_table);
      delete_stencil_table(varying_stencils);
      varying_stencils = table;
    }
  }
  for (int face_varying_channel = 0; face_varying_channel < num_face_varying_channels;
       ++face_varying_channel)
  {
    const StencilTable *table = StencilTableFactory::AppendLocalPointStencilTableFaceVarying(
        *refiner,
        all_face_varying_stencils[face_varying_channel],
        patch_table->GetLocalPointFaceVaryingStencilTable(face_varying_channel),
        face_varying_channel);
    if (table != nullptr) {
      delete_stencil_table(all_face_varying_stencils[face_varying_channel]);
      all_face_varying_stencils[face_varying_channel] = table;
    }
  }
  // Create OpenSubdiv's CPU side evaluator.
  blender::opensubdiv::EvalOutputAPI::EvalOutput *eval_output = nullptr;

  const bool use_gpu_evaluator = evaluator_type == OPENSUBDIV_EVALUATOR_GPU;
  if (use_gpu_evaluator) {
    blender::opensubdiv::GpuEvalOutput::EvaluatorCache *evaluator_cache = nullptr;
    if (evaluator_cache_descr) {
      evaluator_cache = static_cast<blender::opensubdiv::GpuEvalOutput::EvaluatorCache *>(
          evaluator_cache_descr->impl->eval_cache);
    }

    eval_output = new blender::opensubdiv::GpuEvalOutput(vertex_stencils,
                                                         varying_stencils,
                                                         all_face_varying_stencils,
                                                         2,
                                                         patch_table,
                                                         evaluator_cache);
  }
  else {
    eval_output = new blender::opensubdiv::CpuEvalOutput(
        vertex_stencils, varying_stencils, all_face_varying_stencils, 2, patch_table);
  }

  blender::opensubdiv::PatchMap *patch_map = new blender::opensubdiv::PatchMap(*patch_table);
  // Wrap everything we need into an object which we control from our side.
  OpenSubdiv_Evaluator *evaluator = new OpenSubdiv_Evaluator();
  evaluator->type = evaluator_type;

  evaluator->eval_output = new blender::opensubdiv::EvalOutputAPI(eval_output, patch_map);
  evaluator->patch_map = patch_map;
  evaluator->patch_table = patch_table;
  // TODO(sergey): Look into whether we've got duplicated stencils arrays.
  delete_stencil_table(vertex_stencils);
  delete_stencil_table(varying_stencils);
  for (const StencilTable *table : all_face_varying_stencils) {
    delete_stencil_table(table);
  }

  return evaluator;
}
