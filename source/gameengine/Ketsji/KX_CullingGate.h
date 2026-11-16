/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_CullingGate.h
 *  \ingroup ketsji
 *
 * CPU visibility gate in front of the depsgraph and the draw manager.
 *
 * The depsgraph cannot be skipped, but it only evaluates what is tagged, and
 * the draw manager only syncs what is visible. The gate classifies the scene
 * once per render pass, before anything is tagged:
 *
 *   EVAL  the object is tagged and evaluated by the depsgraph this pass.
 *   DRAW  the object is synced by the draw manager this pass.
 *
 * An object is relevant to a view when its world AABB touches
 *   - the camera frustum, or
 *   - the caster volume of a shadowed sun (frustum swept toward the light), or
 *   - the influence sphere of a visible shadowed local light, clipped by the
 *     frustum planes that have the light on their inner side.
 * Local lights are culled by their EEVEE influence radius, suns never.
 *
 * Objects that are not relevant keep their SG_Node::DIRTY_RENDER flag and
 * their deferred depsgraph tags, so they are brought up to date on the first
 * pass where they become relevant again. They are hidden from the draw
 * manager by setting OB_HIDE_VIEWPORT on their evaluated copy only: the
 * original DNA is never written and no relations rebuild is ever triggered.
 *
 * Meshes skinned by a game armature use a bound built from the posed bones,
 * so the armature evaluation (CPU skinning) is deferred while all of its
 * skinned meshes are irrelevant.
 *
 * Occlusion (KX_CullingGateOcclusion.cpp): after the active camera pass the
 * gate boxes are tested on the GPU against a Hi-Z pyramid of that pass depth
 * and read back without stalling, so the results are two frames old when
 * they are used. Objects only relevant to the camera (not as shadow casters)
 * that were occluded in two consecutive results are culled, unless the
 * camera moved too much since the depth was rendered.
 *
 * Data is kept as flat arrays indexed by slot (swap-remove on unregister) so
 * the per pass classification is a handful of branch-free, auto-vectorized
 * loops over contiguous floats.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "BLI_map.hh"

namespace blender {
namespace gpu {
class Shader;
class StorageBuf;
class Texture;
}  // namespace gpu
struct Depsgraph;
struct ID;
struct Light;
struct Object;
struct Scene;
}  // namespace blender

class BL_ArmatureObject;
class KX_Camera;
class KX_GameObject;

class KX_CullingGate {
 public:
  enum Table : uint8_t { TABLE_OBJECT = 0, TABLE_LIGHT = 1, TABLE_ARMATURE = 2, TABLE_COUNT = 3 };
  enum State : uint8_t { STATE_DRAW = 1 << 0, STATE_EVAL = 1 << 1 };

  /* KX_GameObject culling slot values that are not a table index. */
  static constexpr int SLOT_UNREGISTERED = -1;
  static constexpr int SLOT_NEVER = -2;

  struct Stats {
    int objects = 0;
    int objects_drawn = 0;
    int objects_evaluated = 0;
    int lights = 0;
    int lights_drawn = 0;
    int skinned = 0;
    int armatures_deferred = 0;
    int objects_occluded = 0;
  };

  KX_CullingGate();
  ~KX_CullingGate();

  /* -------------------------------------------------------------------- */
  /* Registration. */

  /* Decide eligibility and insert the object in its table. Called lazily
   * after a depsgraph update, so the evaluated copy and its bounds exist. */
  void Register(KX_GameObject *gameobj, blender::Depsgraph *depsgraph, const blender::Scene *scene);
  void Unregister(KX_GameObject *gameobj);

  /* Cached evaluated pointers must be looked up again after the depsgraph
   * relations are rebuilt (collection remap). */
  void InvalidateEvalPointers()
  {
    m_evalPointersStale = true;
  }

  /* -------------------------------------------------------------------- */
  /* Hooks called from KX_GameObject / KX_Scene. */

  /* World transform changed (SG update callback, serialized by the SG
   * transform mutex). */
  void MarkMoved(int slot, uint8_t table)
  {
    if (table == TABLE_OBJECT) {
      m_obMoved[slot] = 1;
    }
  }
  /* Geometry of the object may change at runtime (mesh replacement). */
  void MarkBoundsDynamic(int slot, uint8_t table);
  /* KX_GameObject::SetVisible() of a registered object: hidden by the gate,
   * not evaluated while hidden. Takes effect on the next render pass. */
  void SetUserVisible(int slot, uint8_t table, bool visible);

  /* -------------------------------------------------------------------- */
  /* Per render pass. */

  /* Classify the scene for this pass. cam is the culling camera of the pass,
   * aux_cams are the image render cameras: what they see is evaluated but
   * not drawn in this pass. enabled == false makes everything relevant.
   * use_occlusion: cam is the camera whose depth SubmitOcclusion() tests. */
  void BeginPass(KX_Camera *cam,
                 const std::vector<KX_Camera *> &aux_cams,
                 const blender::Scene *scene,
                 bool enabled,
                 bool use_occlusion);

  /* After the draw loop of the active camera pass: test the gate boxes
   * against depth_tx on the GPU and collect the results of an earlier
   * submission if the GPU is done with them. */
  void SubmitOcclusion(KX_Camera *cam, blender::gpu::Texture *depth_tx);

  bool ShouldEvaluate(int slot, uint8_t table) const
  {
    return slot < 0 || (m_state[table][slot] & STATE_EVAL);
  }

  /* Absorb a depsgraph tag for an ID whose object is currently not
   * evaluated. Returns true when the tag was deferred. */
  bool DeferIdTag(blender::ID *id, int recalc);

  /* Called right before an evaluated object is tagged: flushes the tags
   * deferred while it was culled. */
  void FlushDeferredTags(int slot, uint8_t table);

  /* Bookkeeping to clear DIRTY_RENDER only when the object really got tagged
   * in one of the passes of the frame. */
  void MarkTagged(int slot, uint8_t table)
  {
    if (slot >= 0) {
      TagFrame(slot, table) = m_frame;
    }
  }
  bool TaggedThisFrame(int slot, uint8_t table)
  {
    return slot >= 0 && TagFrame(slot, table) == m_frame;
  }

  /* After the depsgraph update of a pass: refresh cached evaluated pointers
   * and write the draw visibility of this pass on the evaluated objects. */
  void ApplyDrawState(blender::Depsgraph *depsgraph);

  /* Image render path: classify for cam and write its draw visibility.
   * Objects that become visible get their evaluated transform refreshed
   * from the SG since they were possibly skipped by the last main pass. */
  void ApplyAuxView(KX_Camera *cam, const blender::Scene *scene, blender::Depsgraph *depsgraph);

  void EndFrame()
  {
    m_frame++;
  }

  /* Game exit: push pending transforms and make every object visible again
   * in the evaluated depsgraph that outlives the game. */
  void RestoreAll(blender::Depsgraph *depsgraph);

  const Stats &GetStats() const
  {
    return m_stats;
  }

 private:
  /* A convex region: planes (n.x + w >= 0 inside) plus an optional sphere.
   * At most the 6 frustum faces and 12 silhouette edges. */
  struct Volume {
    float planes[18][4];
    int plane_count;
    float sphere[4]; /* xyz center, w radius squared, < 0 when unused. */
  };

  struct LightRec {
    KX_GameObject *owner;
    blender::Object *ob_orig;
    blender::Object *ob_eval;
    /* Refreshed once per frame. */
    float pos[3];
    float dir[3];      /* Direction of travel of the emitted light (world -Z). */
    float bound[4];    /* Influence bounding sphere: xyz center, w radius. */
    uint8_t is_sun;
    uint8_t shadow;
    uint8_t applied_hidden;
    uint8_t user_mask; /* 0xFF visible, 0 hidden by the user. */
    int pending_recalc;
    uint32_t tag_frame;
  };

  struct ArmRec {
    BL_ArmatureObject *owner;
    /* Posed bone box in armature space. */
    float bmin[3];
    float bmax[3];
    double pose_frame;
    int skinned_count;
    int pending_recalc;
    uint32_t tag_frame;
  };

  struct Ref {
    uint8_t table;
    int index;
  };

  /* Camera of an occlusion submission, to validate its results later. */
  struct OcclusionView {
    uint32_t frame;
    float pos[3];
    float fwd[3];
    float proj[16];
    float margin; /* World margin the boxes were expanded by. */
    int count;    /* Object slots tested. */
    bool valid;
  };

  enum Mask : uint8_t { MASK_CAMERA = 1 << 0, MASK_CASTER = 1 << 1 };

  bool OcclusionUsable(KX_Camera *cam) const;
  void ConsumeOcclusion(const uint32_t *results);
  void FreeGPUResources();

  enum ObFlag : uint8_t {
    OB_FLAG_DYNAMIC_BOUNDS = 1 << 0,
    OB_FLAG_APPLIED_HIDDEN = 1 << 1,
  };

  uint32_t &TagFrame(int slot, uint8_t table)
  {
    switch (table) {
      case TABLE_LIGHT:
        return m_lights[slot].tag_frame;
      case TABLE_ARMATURE:
        return m_arms[slot].tag_frame;
      default:
        return m_obTagFrame[slot];
    }
  }

  void RegisterObject(KX_GameObject *gameobj,
                      blender::Object *ob_orig,
                      blender::Object *ob_eval);
  void RegisterLight(KX_GameObject *gameobj, blender::Object *ob_orig, blender::Object *ob_eval);
  int RegisterArmature(BL_ArmatureObject *arm);
  void UnregisterObject(int slot);
  void UnregisterLight(int slot);
  void UnregisterArmature(int slot);

  void Refresh(const blender::Scene *scene);
  void RefreshEvalPointers(blender::Depsgraph *depsgraph);
  void RefreshObjectBounds();
  void RefreshArmatures();
  void RefreshLights(const blender::Scene *scene);
  void UpdateWorldBox(int slot, const float mat[4][4]);
  void UpdateSkinnedBox(int slot);

  /* Relevance of every object and light to the view of cam, written or
   * OR-ed into the masks. Objects: MASK_CAMERA and/or MASK_CASTER bits,
   * lights: 1/0. */
  void ClassifyView(KX_Camera *cam, uint8_t *ob_mask, uint8_t *light_mask, bool accumulate);

  /* Object table: SoA, every vector has the same length. */
  std::vector<KX_GameObject *> m_obOwner;
  std::vector<blender::Object *> m_obOrig;
  std::vector<blender::Object *> m_obEval;
  std::vector<float> m_cx, m_cy, m_cz; /* World AABB center. */
  std::vector<float> m_ex, m_ey, m_ez; /* World AABB half size. */
  std::vector<float> m_casterBias;     /* 0 for shadow casters, very negative otherwise. */
  std::vector<float> m_localBox;       /* 6 floats per slot: local center, local half size. */
  std::vector<uint8_t> m_obFlags;
  std::vector<uint8_t> m_obUserMask;   /* 0xFF visible, 0 hidden by the user. */
  std::vector<uint8_t> m_obMoved;
  std::vector<int> m_obArm;            /* Armature record of a skinned mesh, -1 otherwise. */
  std::vector<float> m_obMargin;       /* Skin margin around the posed bone box. */
  std::vector<int> m_obPending;
  std::vector<uint32_t> m_obTagFrame;
  std::vector<uint8_t> m_obOccStreak; /* Consecutive occluded results. */

  std::vector<LightRec> m_lights;
  std::vector<ArmRec> m_arms;

  std::vector<uint8_t> m_state[TABLE_COUNT];

  /* Scratch, reused every pass. */
  std::vector<Volume> m_volumes;
  std::vector<uint8_t> m_obMaskMain, m_obMaskAux;
  std::vector<uint8_t> m_lightMaskMain, m_lightMaskAux;

  /* Deferred tag lookup: ID of the original object -> table slot. */
  blender::Map<const blender::ID *, Ref> m_idToRef;

  /* Occlusion. Results arrive for the layout of their submission: the slot
   * moves of later unregistrations are logged and replayed on them. */
  OcclusionView m_occInflight = {};
  OcclusionView m_occLatest = {};
  std::vector<std::pair<int, int>> m_occMoveLog;
  std::vector<float> m_occStaging;
  std::vector<uint32_t> m_occReadback;
  float m_occLastCamPos[3] = {0.0f, 0.0f, 0.0f};
  int m_occCapacity = 0;
  int m_occHizSize[2] = {0, 0};
  int m_occDepthSize[2] = {0, 0};
  int m_occLevels = 0;
  int m_occSupported = -1; /* -1 unknown, 0 no, 1 yes. */
  blender::gpu::Shader *m_occHizShader = nullptr;
  blender::gpu::Shader *m_occTestShader = nullptr;
  blender::gpu::StorageBuf *m_occBoundsBuf = nullptr;
  blender::gpu::StorageBuf *m_occResultBuf = nullptr;
  blender::gpu::Texture *m_occHizTex = nullptr;

  uint32_t m_frame = 1;
  uint32_t m_refreshedFrame = 0;
  bool m_evalPointersStale = false;
  Stats m_stats;
};
