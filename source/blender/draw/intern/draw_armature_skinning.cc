/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_armature_skinning.hh"

#include <map>

#include "BLI_vector.hh"
#include "BLI_map.hh"
#include "BLI_threads.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

#include "BKE_deform.hh"
#include "BKE_object.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "GPU_storage_buffer.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_compute.hh"
#include "GPU_capabilities.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_extract.hh"
#include "draw_cache_impl.hh"

#include "DNA_mesh_types.h"
#include <cstring>

using namespace blender::draw;

struct blender::draw::ArmatureSkinningManager::Impl {
 int ref_count =0;

 /* Static CPU-side buffers (kept per original mesh pointer key). */
 struct MeshStaticData {
 std::vector<int> in_indices; /* size = verts *4 */
 std::vector<float> in_weights; /* size = verts *4 */
 std::vector<float> rest_positions; /* float4 per vert (flattened) */
 int verts_num =0;

 /* GPU SSBOs (created in GL context when needed) */
 blender::gpu::StorageBuf *ssbo_in_idx = nullptr;
 blender::gpu::StorageBuf *ssbo_in_wgt = nullptr;
 blender::gpu::StorageBuf *ssbo_rest_pos = nullptr;
 blender::gpu::StorageBuf *ssbo_skinned_pos = nullptr;
 /* Per-mesh small SSBO for premat (1 mat4). */
 blender::gpu::StorageBuf *ssbo_premat = nullptr;
 blender::gpu::StorageBuf *ssbo_postmat = nullptr;
 /* Armature object and deformed object pointers used to compute premat/postmat. */
 Object *arm = nullptr;
 Object *deformed = nullptr;

 /* Pending GPU setup: if true, we will set orig_mesh->is_using_gpu_deform =1 on the first
 * GL pass and wait one frame for the draw cache to be populated (which requires a DrwContext).
 * This implements the "pending + retry next frames" strategy without hooking the draw manager. */
 bool pending_gpu_setup = false;
 int gpu_setup_attempts =0;
 };

 Map<Mesh *, MeshStaticData> static_map;

 /* Compute shader used to skin vertices. Created on demand in GL context. */
 blender::gpu::Shader *compute_shader = nullptr;

 struct ArmatureData {
 int refcount =0;
 int bones =0;
 int last_update_frame = -1;
 blender::gpu::StorageBuf *ssbo_bone_pose = nullptr; /* mat4[] */
 };

 Map<Object *, ArmatureData> arm_map;
};

static const char *skin_compute_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
#define CONTRIB_THRESHOLD 1e-4
#endif

vec4 skin_pos_object(int v_idx) {
  vec4 rest_pos_object = premat[0] * rest_positions[v_idx];
  vec4 acc = vec4(0.0);
  float tw =0.0;
  for (int i =0; i <4; ++i) {
 int b = in_idx[v_idx][i];
 float w = in_wgt[v_idx][i];
 if (w >0.0) {
 acc += (bone_pose_mat[b] * rest_pos_object) * w;
 tw += w;
 }
 }
 return (tw <= CONTRIB_THRESHOLD) ? rest_pos_object : (acc + rest_pos_object * (1.0 - tw));
}

void main() {
 uint v = gl_GlobalInvocationID.x;
 if (v >= skinned_vert_positions.length()) {
 return;
 }
 skinned_vert_positions[v] = skin_pos_object(int(v));
}
)GLSL";

ArmatureSkinningManager &ArmatureSkinningManager::instance()
{
 static ArmatureSkinningManager manager;
 return manager;
}

ArmatureSkinningManager::ArmatureSkinningManager() : impl_(new Impl()) {}
ArmatureSkinningManager::~ArmatureSkinningManager() { free_all(); }

void ArmatureSkinningManager::ensure_static_resources(Object *arm_ob,
 Object *deformed_ob,
 Mesh *orig_mesh)
{
 (void)deformed_ob;
 if (!orig_mesh) {
 return;
 }

 Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);

 if (msd.verts_num == orig_mesh->verts_num) {
 /* Already prepared */
 return;
 }

 const int verts_num = orig_mesh->verts_num;
 msd.verts_num = verts_num;
 msd.in_indices.clear();
 msd.in_weights.clear();
 msd.rest_positions.clear();

 msd.in_indices.resize(verts_num *4,0);
 msd.in_weights.resize(verts_num *4,0.0f);
 msd.rest_positions.resize(verts_num *4,0.0f); /* float4 */

 /* Build group name -> bone index map from armature pose. */
 Map<std::string, int> bone_name_to_index;
 if (arm_ob && arm_ob->pose) {
 int idx =0;
 for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
 pchan = pchan->next) {
 if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
 bone_name_to_index.add_new(pchan->name, idx++);
 }
 }
 }

 /* Vertex group names/order from original mesh */
 std::vector<std::string> group_names;
 const ListBase *defbase = BKE_id_defgroup_list_get(&orig_mesh->id);
 if (defbase) {
 for (const bDeformGroup *dg = (const bDeformGroup *)defbase->first; dg; dg = dg->next) {
 group_names.push_back(dg->name);
 }
 }

 /* Fill influences from deform verts if present */
 blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
 constexpr float kContribThreshold =1e-4f;

 for (int v =0; v < verts_num; ++v) {
 const MDeformVert &dvert = dverts[v];
 std::map<int, float> bone_weight_map;
 for (int j =0; j < dvert.totweight; ++j) {
 const int def_nr = dvert.dw[j].def_nr;
 if (def_nr >=0 && def_nr < (int)group_names.size()) {
 const std::string &group_name = group_names[def_nr];
 if (auto *it = bone_name_to_index.lookup_ptr(group_name)) {
 bone_weight_map[*it] += dvert.dw[j].weight;
 }
 }
 }

 struct Influence {
 int bone_idx;
 float weight;
 };
 std::vector<Influence> influences;
 float total_raw =0.0f;
 for (const auto &kv : bone_weight_map) {
 influences.push_back({kv.first, kv.second});
 total_raw += kv.second;
 }

 if (total_raw <= kContribThreshold || influences.empty()) {
 for (int j =0; j <4; ++j) {
 msd.in_indices[v *4 + j] =0;
 msd.in_weights[v *4 + j] =0.0f;
 }
 }
 else {
 std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b) {
 return a.weight > b.weight;
 });
 float total =0.0f;
 for (const auto &inf : influences) {
 total += inf.weight;
 }
 if (total >0.0f) {
 for (auto &inf : influences) {
 inf.weight /= total;
 }
 }
 for (int j =0; j <4; ++j) {
 if (j < (int)influences.size()) {
 msd.in_indices[v *4 + j] = influences[j].bone_idx;
 msd.in_weights[v *4 + j] = influences[j].weight;
 }
 else {
 msd.in_indices[v *4 + j] =0;
 msd.in_weights[v *4 + j] =0.0f;
 }
 }
 }
 }

 /* Rest positions (float4) from orig_mesh vert positions */
 blender::Span<blender::float3> vert_positions = orig_mesh->vert_positions();
 for (int i =0; i < verts_num; ++i) {
 const blender::float3 &p = vert_positions[i];
 msd.rest_positions[i *4 +0] = p.x;
 msd.rest_positions[i *4 +1] = p.y;
 msd.rest_positions[i *4 +2] = p.z;
 msd.rest_positions[i *4 +3] =1.0f;
 }

 /* Remember armature/deformed pointers so dispatch can compute premat/postmat. */
 msd.arm = arm_ob;
 msd.deformed = deformed_ob;

 /* Mark as pending GPU setup: the draw cache must be populated while `is_using_gpu_deform==1` to
 * produce float4 vertex positions. We cannot call `draw_cache_populate` here (no DrwContext),
 * so we set a pending flag and the first GL pass will set `is_using_gpu_deform` and return,
 * allowing the draw manager to populate the cache on the next frame. */
 msd.pending_gpu_setup = true;
 msd.gpu_setup_attempts =0;

 /* GPU SSBO creation/upload will be deferred until in GL context (update_per_frame or dispatch). */
}

void ArmatureSkinningManager::update_per_frame(Object *arm_ob, Object *deformed_ob)
{
 (void)deformed_ob;

 /* Update armature bone matrices (one SSBO per armature). */
 if (!arm_ob || arm_ob->pose == nullptr) {
 return;
 }

 /* Get or create ArmatureData for this armature. */
 Impl::ArmatureData &ad = impl_->arm_map.lookup_or_add_default(arm_ob);

 /* Count deforming bones in the same order as ensure_static_resources (skip BONE_NO_DEFORM). */
 int bones_count =0;
 for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
 pchan = pchan->next) {
 if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
 bones_count++;
 }
 }

 /* Recreate SSBO if bone count changed. */
 if (ad.bones != bones_count) {
 if (ad.ssbo_bone_pose) {
 GPU_storagebuf_free(ad.ssbo_bone_pose);
 ad.ssbo_bone_pose = nullptr;
 }
 if (bones_count >0) {
 ad.ssbo_bone_pose = GPU_storagebuf_create(sizeof(float) *16 * bones_count);
 ad.bones = bones_count;
 }
 }

 if (ad.ssbo_bone_pose == nullptr || bones_count ==0) {
 return;
 }

 /* Fill an array of float[16] using pchan->chan_mat in the same order used by the index map. */
 std::vector<float> mats;
 mats.resize(size_t(bones_count) *16);
 int bi =0;
 for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
 pchan = pchan->next) {
 if (pchan->bone->flag & BONE_NO_DEFORM) {
 continue;
 }
 /* pchan->chan_mat is a4x4 float (row-major in memory). Copy directly. */
 memcpy(&mats[bi *16], pchan->chan_mat, sizeof(float) *16);
 bi++;
 }

 GPU_storagebuf_update(ad.ssbo_bone_pose, mats.data());
}

bool ArmatureSkinningManager::dispatch_skinning(Depsgraph *depsgraph,
 Object *deformed_eval,
 MeshBatchCache *cache,
 blender::gpu::VertBuf *vbo_pos,
 blender::gpu::VertBuf *vbo_nor)
{
 (void)vbo_pos;
 (void)vbo_nor;
 (void)depsgraph;
 (void)deformed_eval;
 
 /* Ensure armature SSBOs are updated for this frame. We call update_per_frame with current arm/deformed objects. */
 /* Note: this may early-return if no armature present. */
 update_per_frame(nullptr, nullptr);

 /* Find static data for mesh_owner if present */
 Mesh *mesh_owner = nullptr;
 if (cache && cache->mesh_owner) {
 mesh_owner = cache->mesh_owner;
 }
 if (!mesh_owner) {
 return false;
 }

 auto *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
 if (!msd_ptr) {
 return false;
 }
 Impl::MeshStaticData &msd = *msd_ptr;

 /* If we previously requested GPU-deform setup, ensure we let the draw cache populate while
 * `is_using_gpu_deform ==1`. We cannot call draw_cache_populate here; instead, we set the flag
 * on the first GL pass and return false so the draw manager has one frame to populate caches.
 */
 const int MAX_ATTEMPTS =3;
 if (msd.pending_gpu_setup) {
 if (msd.gpu_setup_attempts ==0) {
 /* First GL pass: enable flag so draw extraction will emit float4 positions next frame. */
 mesh_owner->is_using_gpu_deform =1;
 msd.gpu_setup_attempts =1;
 return false; /* retry next frame */
 }
 else if (msd.gpu_setup_attempts >= MAX_ATTEMPTS) {
 /* Too many attempts: give up and fallback to CPU. Clean up flag and pending state. */
 mesh_owner->is_using_gpu_deform =0;
 msd.pending_gpu_setup = false;
 msd.gpu_setup_attempts =0;
 return false;
 }
 /* If attempts >0, fall through and try to proceed; if caches are still not populated, we'll fail
 * later and can increment attempts or clear the pending flag. */
 }

 /* Ensure SSBOs exist and are uploaded via BKE helpers (keyed, refcounted). */
 MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
 if (!ires) {
 return false;
 }

 const std::string key_in_idx = "armature_in_idx";
 const std::string key_in_wgt = "armature_in_wgt";
 const std::string key_rest_pos = "armature_rest_pos";
 const std::string key_skinned_pos = "armature_skinned_pos";
 const std::string key_premat = "armature_premat";
 const std::string key_postmat = "armature_postmat";

 if (!msd.ssbo_in_idx) {
 msd.ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_in_idx, sizeof(int) * msd.verts_num *4);
 if (!msd.ssbo_in_idx) {
 return false;
 }
 BKE_mesh_gpu_internal_ssbo_update(mesh_owner, key_in_idx, msd.in_indices.data());
 }
 if (!msd.ssbo_in_wgt) {
 msd.ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_in_wgt, sizeof(float) * msd.verts_num *4);
 if (!msd.ssbo_in_wgt) {
 return false;
 }
 BKE_mesh_gpu_internal_ssbo_update(mesh_owner, key_in_wgt, msd.in_weights.data());
 }
 if (!msd.ssbo_rest_pos) {
 msd.ssbo_rest_pos = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_rest_pos, sizeof(float) * msd.verts_num *4);
 if (!msd.ssbo_rest_pos) {
 return false;
 }
 BKE_mesh_gpu_internal_ssbo_update(mesh_owner, key_rest_pos, msd.rest_positions.data());
 }
 if (!msd.ssbo_skinned_pos) {
 msd.ssbo_skinned_pos = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_skinned_pos, sizeof(float) * msd.verts_num *4);
 if (!msd.ssbo_skinned_pos) {
 return false;
 }
 }
 if (!msd.ssbo_premat) {
 msd.ssbo_premat = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_premat, sizeof(float) *16);
 if (!msd.ssbo_premat) {
 return false;
 }
 }
 if (!msd.ssbo_postmat) {
 msd.ssbo_postmat = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_postmat, sizeof(float) *16);
 if (!msd.ssbo_postmat) {
 return false;
 }
 }
 

 /* Ensure compute shader via BKE helper to avoid duplicates. Use a mesh-scoped key. */
 blender::gpu::Shader *compute_sh = nullptr;
 using namespace blender::gpu::shader;
 ShaderCreateInfo info("BGE_Armature_Skin_Vertices_Pass");
 info.local_group_size(256,1,1);
 info.compute_source_generated = skin_compute_src;
 info.compute_source("draw_colormanagement_lib.glsl");
 info.storage_buf(0, Qualifier::write, "vec4", "skinned_vert_positions[]");
 info.storage_buf(1, Qualifier::read, "ivec4", "in_idx[]");
 info.storage_buf(2, Qualifier::read, "vec4", "in_wgt[]");
 info.storage_buf(3, Qualifier::read, "mat4", "bone_pose_mat[]");
 info.storage_buf(4, Qualifier::read, "mat4", "premat[]");
 info.storage_buf(5, Qualifier::read, "vec4", "rest_positions[]");

 const std::string shader_key = "armature_skinning_compute";
 compute_sh = BKE_mesh_gpu_internal_shader_ensure(mesh_owner, shader_key, info);
 if (!compute_sh) {
 return false;
 }

 GPU_shader_bind(compute_sh);
 GPU_storagebuf_bind(msd.ssbo_skinned_pos,0);
 GPU_storagebuf_bind(msd.ssbo_in_idx,1);
 GPU_storagebuf_bind(msd.ssbo_in_wgt,2);
 /* Bind rest positions SSBO as declared in the compute shader (binding5). */
 if (msd.ssbo_rest_pos) {
 GPU_storagebuf_bind(msd.ssbo_rest_pos,5);
 }
 /* Bind bone pose SSBO from armature data if available. */
 if (msd.arm) {
 if (auto *ad_ptr = impl_->arm_map.lookup_ptr(msd.arm)) {
 Impl::ArmatureData &ad = *ad_ptr;
 if (ad.ssbo_bone_pose) {
 GPU_storagebuf_bind(ad.ssbo_bone_pose,3);
 }
 }
 }
 /* Bind per-mesh premat (slot4) */
 if (msd.ssbo_premat) {
 GPU_storagebuf_bind(msd.ssbo_premat,4);
 }

 const int group_size =256;
 int num_groups = (msd.verts_num + group_size -1) / group_size;
 GPU_compute_dispatch(compute_sh, num_groups,1,1);
 GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

 GPU_shader_unbind();

 /* Scatter skinned positions to mesh corners and compute normals. */

 if (deformed_eval && cache && vbo_pos && vbo_nor) {
 std::vector<blender::bke::GpuMeshComputeBinding> caller_bindings;
 caller_bindings.reserve(4);

 {
 blender::bke::GpuMeshComputeBinding b = {};
 b.binding =0;
 b.qualifiers = blender::gpu::shader::Qualifier::read_write;
 b.type_name = "vec4";
 b.bind_name = "positions_out[]";
 b.buffer = vbo_pos;
 caller_bindings.push_back(b);
 }
 {
 blender::bke::GpuMeshComputeBinding b = {};
 b.binding =1;
 b.qualifiers = blender::gpu::shader::Qualifier::write;
 b.type_name = "uint";
 b.bind_name = "normals_out[]";
 b.buffer = vbo_nor;
 caller_bindings.push_back(b);
 }
 {
 blender::bke::GpuMeshComputeBinding b = {};
 b.binding =2;
 b.qualifiers = blender::gpu::shader::Qualifier::read;
 b.type_name = "vec4";
 b.bind_name = "positions_in[]";
 b.buffer = msd.ssbo_skinned_pos;
 caller_bindings.push_back(b);
 }
 {
 blender::bke::GpuMeshComputeBinding b = {};
 b.binding =3;
 b.qualifiers = blender::gpu::shader::Qualifier::read;
 b.type_name = "mat4";
 b.bind_name = "transform_mat[]";
 b.buffer = msd.ssbo_postmat; /* provide postmat to scatter stage */
 caller_bindings.push_back(b);
 }

 auto post_bind_fn = [](blender::gpu::Shader *sh) {};
 auto config_fn = [](blender::gpu::shader::ShaderCreateInfo &info) {};

 Mesh *mesh_eval = BKE_object_get_evaluated_mesh(deformed_eval);
 if (!mesh_eval) {
 return false;
 }
 /* If we reached here and pending_gpu_setup was set, assume draw cache was populated with float4
 * positions and mark setup complete. */
 if (msd.pending_gpu_setup) {
 msd.pending_gpu_setup = false;
 msd.gpu_setup_attempts =0;
 }

 BKE_mesh_gpu_scatter_to_corners(depsgraph,
 deformed_eval,
 caller_bindings,
 config_fn,
 post_bind_fn,
 mesh_eval->corners_num);
 }

 return true;
}

void ArmatureSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
 if (!mesh) {
 return;
 }
 if (auto *msd_ptr = impl_->static_map.lookup_ptr(mesh)) {
 Impl::MeshStaticData &msd = *msd_ptr;
 /* Release our references to keyed internal resources. The actual free is centralized in BKE. */
 const std::string key_in_idx = "armature_in_idx";
 const std::string key_in_wgt = "armature_in_wgt";
 const std::string key_rest_pos = "armature_rest_pos";
 const std::string key_skinned_pos = "armature_skinned_pos";
 const std::string key_premat = "armature_premat";
 const std::string key_postmat = "armature_postmat";
 const std::string shader_key = "armature_skinning_compute";
 if (msd.ssbo_in_idx) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_in_idx);
 }
 if (msd.ssbo_in_wgt) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_in_wgt);
 }
 if (msd.ssbo_rest_pos) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_rest_pos);
 }
 if (msd.ssbo_skinned_pos) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_skinned_pos);
 }
 if (msd.ssbo_premat) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_premat);
 }
 if (msd.ssbo_postmat) {
 BKE_mesh_gpu_internal_ssbo_release(mesh, key_postmat);
 }
 /* Release shader reference */
 BKE_mesh_gpu_internal_shader_release(mesh, shader_key);
 msd.ssbo_in_idx = nullptr;
 msd.ssbo_in_wgt = nullptr;
 msd.ssbo_rest_pos = nullptr;
 msd.ssbo_skinned_pos = nullptr;
 msd.ssbo_premat = nullptr;
 msd.ssbo_postmat = nullptr;
 /* Decrement armature refcount and free arm data if unused. */
 if (msd.arm) {
 if (auto *ad_ptr = impl_->arm_map.lookup_ptr(msd.arm)) {
 Impl::ArmatureData &ad = *ad_ptr;
 ad.refcount -=1;
 if (ad.refcount <=0) {
 if (ad.ssbo_bone_pose) {
 GPU_storagebuf_free(ad.ssbo_bone_pose);
 ad.ssbo_bone_pose = nullptr;
 }
 impl_->arm_map.remove(msd.arm);
 }
 }
 }
 impl_->static_map.remove(mesh);
 }
}

void ArmatureSkinningManager::free_all()
{
 /* SSBOs are owned by BKE mesh internal resources and will be freed by BKE_mesh_gpu_free_all_caches().
 * Only clear static CPU data and let BKE handle GPU resources. */
 impl_->static_map.clear();
 /* Free armature data */
 for (auto item : impl_->arm_map.items()) {
 Impl::ArmatureData &ad = item.value;
 if (ad.ssbo_bone_pose) {
 GPU_storagebuf_free(ad.ssbo_bone_pose);
 ad.ssbo_bone_pose = nullptr;
 }
 }
 impl_->arm_map.clear();

 if (impl_->compute_shader) {
 GPU_shader_free(impl_->compute_shader);
 impl_->compute_shader = nullptr;
 }
}
