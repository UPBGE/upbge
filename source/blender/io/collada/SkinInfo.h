/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */

#pragma once

#include <map>
#include <vector>

#include "COLLADAFWNode.h"
#include "COLLADAFWSkinController.h"
#include "COLLADAFWSkinControllerData.h"
#include "COLLADAFWTypes.h"
#include "COLLADAFWUniqueId.h"

#include "BKE_context.h"
#include "DNA_object_types.h"

#include "TransformReader.h"
#include "collada_internal.h"

/**
 * This is used to store data passed in write_controller_data.
 * Arrays from #COLLADAFW::SkinControllerData lose ownership, so do this class members
 * so that arrays don't get freed until we free them explicitly.
 */
class SkinInfo {
 private:
  /* to build armature bones from inverse bind matrices */
  struct JointData {
    float inv_bind_mat[4][4];      /* joint inverse bind matrix */
    COLLADAFW::UniqueId joint_uid; /* joint node UID */
    // Object *ob_arm;            /* armature object */
  };

  float bind_shape_matrix[4][4];

  /* data from COLLADAFW::SkinControllerData, each array should be freed */
  COLLADAFW::UIntValuesArray joints_per_vertex;
  COLLADAFW::UIntValuesArray weight_indices;
  COLLADAFW::IntValuesArray joint_indices;
  /* COLLADAFW::FloatOrDoubleArray weights; */
  std::vector<float> weights;

  std::vector<JointData> joint_data; /* index to this vector is joint index */

  UnitConverter *unit_converter;

  Object *ob_arm;
  COLLADAFW::UniqueId controller_uid;
  Object *parent;

 public:
  SkinInfo();
  SkinInfo(const SkinInfo &skin);
  SkinInfo(UnitConverter *conv);

  /** Nobody owns the data after this, so it should be freed manually with releaseMemory. */
  template<typename T> void transfer_array_data(T &src, T &dest);

  /** When src is const we cannot `src.yieldOwnerShip`, this is used by copy constructor. */
  void transfer_int_array_data_const(const COLLADAFW::IntValuesArray &src,
                                     COLLADAFW::IntValuesArray &dest);

  void transfer_uint_array_data_const(const COLLADAFW::UIntValuesArray &src,
                                      COLLADAFW::UIntValuesArray &dest);

  void borrow_skin_controller_data(const COLLADAFW::SkinControllerData *skin);

  void free();

  /**
   * Using inverse bind matrices to construct armature
   * it is safe to invert them to get the original matrices
   * because if they are inverse matrices, they can be inverted.
   */
  void add_joint(const COLLADABU::Math::Matrix4 &matrix);

  void set_controller(const COLLADAFW::SkinController *co);

  /** Called from write_controller. */
  Object *create_armature(Main *bmain, Scene *scene, ViewLayer *view_layer);

  Object *set_armature(Object *ob_arm);

  bool get_joint_inv_bind_matrix(float inv_bind_mat[4][4], COLLADAFW::Node *node);

  Object *BKE_armature_from_object();

  const COLLADAFW::UniqueId &get_controller_uid();

  /**
   * Check if this skin controller references a joint or any descendant of it
   *
   * some nodes may not be referenced by SkinController,
   * in this case to determine if the node belongs to this armature,
   * we need to search down the tree.
   */
  bool uses_joint_or_descendant(COLLADAFW::Node *node);

  void link_armature(bContext *C,
                     Object *ob,
                     std::map<COLLADAFW::UniqueId, COLLADAFW::Node *> &joint_by_uid,
                     TransformReader *tm);

  bPoseChannel *get_pose_channel_from_node(COLLADAFW::Node *node);

  void set_parent(Object *_parent);

  Object *get_parent();

  void find_root_joints(const std::vector<COLLADAFW::Node *> &root_joints,
                        std::map<COLLADAFW::UniqueId, COLLADAFW::Node *> &joint_by_uid,
                        std::vector<COLLADAFW::Node *> &result);

  bool find_node_in_tree(COLLADAFW::Node *node, COLLADAFW::Node *tree_root);
};
