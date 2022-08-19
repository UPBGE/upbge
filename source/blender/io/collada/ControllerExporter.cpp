/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */

#include "COLLADASWBaseInputElement.h"
#include "COLLADASWInstanceController.h"
#include "COLLADASWPrimitves.h"
#include "COLLADASWSource.h"

#include "DNA_action_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_deform.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"

#include "ED_armature.h"

#include "BLI_listbase.h"

#include "ArmatureExporter.h"
#include "ControllerExporter.h"
#include "GeometryExporter.h"
#include "SceneExporter.h"

#include "collada_utils.h"

bool ControllerExporter::is_skinned_mesh(Object *ob)
{
  return bc_get_assigned_armature(ob) != nullptr;
}

void ControllerExporter::write_bone_URLs(COLLADASW::InstanceController &ins,
                                         Object *ob_arm,
                                         Bone *bone)
{
  if (bc_is_root_bone(bone, this->export_settings.get_deform_bones_only())) {
    std::string node_id = translate_id(id_name(ob_arm) + "_" + bone->name);
    ins.addSkeleton(COLLADABU::URI(COLLADABU::Utils::EMPTY_STRING, node_id));
  }
  else {
    for (Bone *child = (Bone *)bone->childbase.first; child; child = child->next) {
      write_bone_URLs(ins, ob_arm, child);
    }
  }
}

bool ControllerExporter::add_instance_controller(Object *ob)
{
  Object *ob_arm = bc_get_assigned_armature(ob);
  bArmature *arm = (bArmature *)ob_arm->data;

  const std::string &controller_id = get_controller_id(ob_arm, ob);

  COLLADASW::InstanceController ins(mSW);
  ins.setUrl(COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, controller_id));

  Mesh *me = (Mesh *)ob->data;
  if (!me->dvert) {
    return false;
  }

  /* write root bone URLs */
  Bone *bone;
  for (bone = (Bone *)arm->bonebase.first; bone; bone = bone->next) {
    write_bone_URLs(ins, ob_arm, bone);
  }

  InstanceWriter::add_material_bindings(
      ins.getBindMaterial(), ob, this->export_settings.get_active_uv_only());

  ins.add();
  return true;
}

void ControllerExporter::export_controllers()
{
  Scene *sce = blender_context.get_scene();
  openLibrary();

  GeometryFunctor gf;
  gf.forEachMeshObjectInExportSet<ControllerExporter>(
      sce, *this, this->export_settings.get_export_set());

  closeLibrary();
}

void ControllerExporter::operator()(Object *ob)
{
  Object *ob_arm = bc_get_assigned_armature(ob);
  Key *key = BKE_key_from_object(ob);

  if (ob_arm) {
    export_skin_controller(ob, ob_arm);
  }
  if (key && this->export_settings.get_include_shapekeys()) {
    export_morph_controller(ob, key);
  }
}
#if 0

bool ArmatureExporter::already_written(Object *ob_arm)
{
  return std::find(written_armatures.begin(), written_armatures.end(), ob_arm) !=
         written_armatures.end();
}

void ArmatureExporter::wrote(Object *ob_arm)
{
  written_armatures.push_back(ob_arm);
}

void ArmatureExporter::find_objects_using_armature(Object *ob_arm,
                                                   std::vector<Object *> &objects,
                                                   Scene *sce)
{
  objects.clear();

  Base *base = (Base *)sce->base.first;
  while (base) {
    Object *ob = base->object;

    if (ob->type == OB_MESH && get_assigned_armature(ob) == ob_arm) {
      objects.push_back(ob);
    }

    base = base->next;
  }
}
#endif

std::string ControllerExporter::get_controller_id(Object *ob_arm, Object *ob)
{
  return translate_id(id_name(ob_arm)) + "_" + translate_id(id_name(ob)) +
         SKIN_CONTROLLER_ID_SUFFIX;
}

std::string ControllerExporter::get_controller_id(Key *key, Object *ob)
{
  return translate_id(id_name(ob)) + MORPH_CONTROLLER_ID_SUFFIX;
}

void ControllerExporter::export_skin_controller(Object *ob, Object *ob_arm)
{
  /* joint names
   * joint inverse bind matrices
   * vertex weights */

  /* input:
   * joint names: ob -> vertex group names
   * vertex group weights: me->dvert -> groups -> index, weight */

  bool use_instantiation = this->export_settings.get_use_object_instantiation();
  Mesh *me;

  if (((Mesh *)ob->data)->dvert == nullptr) {
    return;
  }

  me = bc_get_mesh_copy(blender_context,
                        ob,
                        this->export_settings.get_export_mesh_type(),
                        this->export_settings.get_apply_modifiers(),
                        this->export_settings.get_triangulate());

  std::string controller_name = id_name(ob_arm);
  std::string controller_id = get_controller_id(ob_arm, ob);

  openSkin(controller_id,
           controller_name,
           COLLADABU::URI(COLLADABU::Utils::EMPTY_STRING, get_geometry_id(ob, use_instantiation)));

  add_bind_shape_mat(ob);

  const ListBase *defbase = BKE_object_defgroup_list(ob);
  std::string joints_source_id = add_joints_source(ob_arm, defbase, controller_id);
  std::string inv_bind_mat_source_id = add_inv_bind_mats_source(ob_arm, defbase, controller_id);

  std::list<int> vcounts;
  std::list<int> joints;
  std::list<float> weights;

  {
    int i, j;

    /* def group index -> joint index */
    std::vector<int> joint_index_by_def_index;
    const bDeformGroup *def;

    for (def = (const bDeformGroup *)defbase->first, i = 0, j = 0; def; def = def->next, i++) {
      if (is_bone_defgroup(ob_arm, def)) {
        joint_index_by_def_index.push_back(j++);
      }
      else {
        joint_index_by_def_index.push_back(-1);
      }
    }

    int oob_counter = 0;
    for (i = 0; i < me->totvert; i++) {
      MDeformVert *vert = &me->dvert[i];
      std::map<int, float> jw;

      /* We're normalizing the weights later */
      float sumw = 0.0f;

      for (j = 0; j < vert->totweight; j++) {
        uint idx = vert->dw[j].def_nr;
        if (idx >= joint_index_by_def_index.size()) {
          /* XXX: Maybe better find out where and
           *      why the Out Of Bound indexes get created? */
          oob_counter += 1;
        }
        else {
          int joint_index = joint_index_by_def_index[idx];
          if (joint_index != -1 && vert->dw[j].weight > 0.0f) {
            jw[joint_index] += vert->dw[j].weight;
            sumw += vert->dw[j].weight;
          }
        }
      }

      if (sumw > 0.0f) {
        float invsumw = 1.0f / sumw;
        vcounts.push_back(jw.size());
        for (auto &index_and_weight : jw) {
          joints.push_back(index_and_weight.first);
          weights.push_back(invsumw * index_and_weight.second);
        }
      }
      else {
        vcounts.push_back(0);
#if 0
        vcounts.push_back(1);
        joints.push_back(-1);
        weights.push_back(1.0f);
#endif
      }
    }

    if (oob_counter > 0) {
      fprintf(stderr,
              "Ignored %d Vertex weights which use index to non existing VGroup %zu.\n",
              oob_counter,
              joint_index_by_def_index.size());
    }
  }

  std::string weights_source_id = add_weights_source(me, controller_id, weights);
  add_joints_element(defbase, joints_source_id, inv_bind_mat_source_id);
  add_vertex_weights_element(weights_source_id, joints_source_id, vcounts, joints);

  BKE_id_free(nullptr, me);

  closeSkin();
  closeController();
}

void ControllerExporter::export_morph_controller(Object *ob, Key *key)
{
  bool use_instantiation = this->export_settings.get_use_object_instantiation();
  Mesh *me;

  me = bc_get_mesh_copy(blender_context,
                        ob,
                        this->export_settings.get_export_mesh_type(),
                        this->export_settings.get_apply_modifiers(),
                        this->export_settings.get_triangulate());

  std::string controller_name = id_name(ob) + "-morph";
  std::string controller_id = get_controller_id(key, ob);

  openMorph(
      controller_id,
      controller_name,
      COLLADABU::URI(COLLADABU::Utils::EMPTY_STRING, get_geometry_id(ob, use_instantiation)));

  std::string targets_id = add_morph_targets(key, ob);
  std::string morph_weights_id = add_morph_weights(key, ob);

  COLLADASW::TargetsElement targets(mSW);

  COLLADASW::InputList &input = targets.getInputList();

  input.push_back(COLLADASW::Input(
      COLLADASW::InputSemantic::MORPH_TARGET, /* constant declared in COLLADASWInputList.h */
      COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, targets_id)));
  input.push_back(
      COLLADASW::Input(COLLADASW::InputSemantic::MORPH_WEIGHT,
                       COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, morph_weights_id)));
  targets.add();

  BKE_id_free(nullptr, me);

  /* support for animations
   * can also try the base element and param alternative */
  add_weight_extras(key);
  closeMorph();
  closeController();
}

std::string ControllerExporter::add_morph_targets(Key *key, Object *ob)
{
  std::string source_id = translate_id(id_name(ob)) + TARGETS_SOURCE_ID_SUFFIX;

  COLLADASW::IdRefSource source(mSW);
  source.setId(source_id);
  source.setArrayId(source_id + ARRAY_ID_SUFFIX);
  source.setAccessorCount(key->totkey - 1);
  source.setAccessorStride(1);

  COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
  param.push_back("IDREF");

  source.prepareToAppendValues();

  KeyBlock *kb = (KeyBlock *)key->block.first;
  /* skip the basis */
  kb = kb->next;
  for (; kb; kb = kb->next) {
    std::string geom_id = get_geometry_id(ob, false) + "_morph_" + translate_id(kb->name);
    source.appendValues(geom_id);
  }

  source.finish();

  return source_id;
}

std::string ControllerExporter::add_morph_weights(Key *key, Object *ob)
{
  std::string source_id = translate_id(id_name(ob)) + WEIGHTS_SOURCE_ID_SUFFIX;

  COLLADASW::FloatSourceF source(mSW);
  source.setId(source_id);
  source.setArrayId(source_id + ARRAY_ID_SUFFIX);
  source.setAccessorCount(key->totkey - 1);
  source.setAccessorStride(1);

  COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
  param.push_back("MORPH_WEIGHT");

  source.prepareToAppendValues();

  KeyBlock *kb = (KeyBlock *)key->block.first;
  /* skip the basis */
  kb = kb->next;
  for (; kb; kb = kb->next) {
    float weight = kb->curval;
    source.appendValues(weight);
  }
  source.finish();

  return source_id;
}

void ControllerExporter::add_weight_extras(Key *key)
{
  /* can also try the base element and param alternative */
  COLLADASW::BaseExtraTechnique extra;

  KeyBlock *kb = (KeyBlock *)key->block.first;
  /* skip the basis */
  kb = kb->next;
  for (; kb; kb = kb->next) {
    /* XXX why is the weight not used here and set to 0.0?
     * float weight = kb->curval; */
    extra.addExtraTechniqueParameter("KHR", "morph_weights", 0.000, "MORPH_WEIGHT_TO_TARGET");
  }
}

void ControllerExporter::add_joints_element(const ListBase *defbase,
                                            const std::string &joints_source_id,
                                            const std::string &inv_bind_mat_source_id)
{
  COLLADASW::JointsElement joints(mSW);
  COLLADASW::InputList &input = joints.getInputList();

  input.push_back(COLLADASW::Input(
      COLLADASW::InputSemantic::JOINT, /* constant declared in COLLADASWInputList.h */
      COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, joints_source_id)));
  input.push_back(
      COLLADASW::Input(COLLADASW::InputSemantic::BINDMATRIX,
                       COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, inv_bind_mat_source_id)));
  joints.add();
}

void ControllerExporter::add_bind_shape_mat(Object *ob)
{
  double bind_mat[4][4];
  float f_obmat[4][4];
  BKE_object_matrix_local_get(ob, f_obmat);

  if (export_settings.get_apply_global_orientation()) {
    /* do nothing, rotation is going to be applied to the Data */
  }
  else {
    bc_add_global_transform(f_obmat, export_settings.get_global_transform());
  }

  // UnitConverter::mat4_to_dae_double(bind_mat, ob->obmat);
  UnitConverter::mat4_to_dae_double(bind_mat, f_obmat);
  if (this->export_settings.get_limit_precision()) {
    BCMatrix::sanitize(bind_mat, LIMITTED_PRECISION);
  }

  addBindShapeTransform(bind_mat);
}

std::string ControllerExporter::add_joints_source(Object *ob_arm,
                                                  const ListBase *defbase,
                                                  const std::string &controller_id)
{
  std::string source_id = controller_id + JOINTS_SOURCE_ID_SUFFIX;

  int totjoint = 0;
  bDeformGroup *def;
  for (def = (bDeformGroup *)defbase->first; def; def = def->next) {
    if (is_bone_defgroup(ob_arm, def)) {
      totjoint++;
    }
  }

  COLLADASW::NameSource source(mSW);
  source.setId(source_id);
  source.setArrayId(source_id + ARRAY_ID_SUFFIX);
  source.setAccessorCount(totjoint);
  source.setAccessorStride(1);

  COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
  param.push_back("JOINT");

  source.prepareToAppendValues();

  for (def = (bDeformGroup *)defbase->first; def; def = def->next) {
    Bone *bone = get_bone_from_defgroup(ob_arm, def);
    if (bone) {
      source.appendValues(get_joint_sid(bone));
    }
  }

  source.finish();

  return source_id;
}

std::string ControllerExporter::add_inv_bind_mats_source(Object *ob_arm,
                                                         const ListBase *defbase,
                                                         const std::string &controller_id)
{
  std::string source_id = controller_id + BIND_POSES_SOURCE_ID_SUFFIX;

  int totjoint = 0;
  for (bDeformGroup *def = (bDeformGroup *)defbase->first; def; def = def->next) {
    if (is_bone_defgroup(ob_arm, def)) {
      totjoint++;
    }
  }

  COLLADASW::FloatSourceF source(mSW);
  source.setId(source_id);
  source.setArrayId(source_id + ARRAY_ID_SUFFIX);
  source.setAccessorCount(totjoint);  // BLI_listbase_count(defbase));
  source.setAccessorStride(16);

  source.setParameterTypeName(&COLLADASW::CSWC::CSW_VALUE_TYPE_FLOAT4x4);
  COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
  param.push_back("TRANSFORM");

  source.prepareToAppendValues();

  bPose *pose = ob_arm->pose;
  bArmature *arm = (bArmature *)ob_arm->data;

  int flag = arm->flag;

  /* put armature in rest position */
  if (!(arm->flag & ARM_RESTPOS)) {
    Depsgraph *depsgraph = blender_context.get_depsgraph();
    Scene *scene = blender_context.get_scene();

    arm->flag |= ARM_RESTPOS;
    BKE_pose_where_is(depsgraph, scene, ob_arm);
  }

  for (bDeformGroup *def = (bDeformGroup *)defbase->first; def; def = def->next) {
    if (is_bone_defgroup(ob_arm, def)) {
      bPoseChannel *pchan = BKE_pose_channel_find_name(pose, def->name);

      float mat[4][4];
      float world[4][4];
      float inv_bind_mat[4][4];

      float bind_mat[4][4]; /* derived from bone->arm_mat */

      bool has_bindmat = bc_get_property_matrix(pchan->bone, "bind_mat", bind_mat);

      if (!has_bindmat) {

        /* Have no bind matrix stored, try old style <= Blender 2.78 */

        bc_create_restpose_mat(
            this->export_settings, pchan->bone, bind_mat, pchan->bone->arm_mat, true);

        /* SL/OPEN_SIM COMPATIBILITY */
        if (export_settings.get_open_sim()) {
          float loc[3];
          float rot[3] = {0, 0, 0};
          float scale[3];
          bc_decompose(bind_mat, loc, nullptr, nullptr, scale);

          /* Only translations, no rotation vs armature */
          loc_eulO_size_to_mat4(bind_mat, loc, rot, scale, 6);
        }
      }

      /* make world-space matrix (bind_mat is armature-space) */
      mul_m4_m4m4(world, ob_arm->obmat, bind_mat);

      if (!has_bindmat) {
        if (export_settings.get_apply_global_orientation()) {
          bc_apply_global_transform(world, export_settings.get_global_transform());
        }
      }

      invert_m4_m4(mat, world);
      UnitConverter::mat4_to_dae(inv_bind_mat, mat);
      if (this->export_settings.get_limit_precision()) {
        BCMatrix::sanitize(inv_bind_mat, LIMITTED_PRECISION);
      }
      source.appendValues(inv_bind_mat);
    }
  }

  /* back from rest position */
  if (!(flag & ARM_RESTPOS)) {
    Depsgraph *depsgraph = blender_context.get_depsgraph();
    Scene *scene = blender_context.get_scene();
    arm->flag = flag;
    BKE_pose_where_is(depsgraph, scene, ob_arm);
  }

  source.finish();

  return source_id;
}

Bone *ControllerExporter::get_bone_from_defgroup(Object *ob_arm, const bDeformGroup *def)
{
  bPoseChannel *pchan = BKE_pose_channel_find_name(ob_arm->pose, def->name);
  return pchan ? pchan->bone : nullptr;
}

bool ControllerExporter::is_bone_defgroup(Object *ob_arm, const bDeformGroup *def)
{
  return get_bone_from_defgroup(ob_arm, def) != nullptr;
}

std::string ControllerExporter::add_weights_source(Mesh *me,
                                                   const std::string &controller_id,
                                                   const std::list<float> &weights)
{
  std::string source_id = controller_id + WEIGHTS_SOURCE_ID_SUFFIX;

  COLLADASW::FloatSourceF source(mSW);
  source.setId(source_id);
  source.setArrayId(source_id + ARRAY_ID_SUFFIX);
  source.setAccessorCount(weights.size());
  source.setAccessorStride(1);

  COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
  param.push_back("WEIGHT");

  source.prepareToAppendValues();

  for (float weight : weights) {
    source.appendValues(weight);
  }

  source.finish();

  return source_id;
}

void ControllerExporter::add_vertex_weights_element(const std::string &weights_source_id,
                                                    const std::string &joints_source_id,
                                                    const std::list<int> &vcounts,
                                                    const std::list<int> &joints)
{
  COLLADASW::VertexWeightsElement weightselem(mSW);
  COLLADASW::InputList &input = weightselem.getInputList();

  int offset = 0;
  input.push_back(COLLADASW::Input(
      COLLADASW::InputSemantic::JOINT, /* constant declared in COLLADASWInputList.h */
      COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, joints_source_id),
      offset++));
  input.push_back(
      COLLADASW::Input(COLLADASW::InputSemantic::WEIGHT,
                       COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, weights_source_id),
                       offset++));

  weightselem.setCount(vcounts.size());

  /* write number of deformers per vertex */
  COLLADASW::PrimitivesBase::VCountList vcountlist;

  vcountlist.resize(vcounts.size());
  std::copy(vcounts.begin(), vcounts.end(), vcountlist.begin());

  weightselem.prepareToAppendVCountValues();
  weightselem.appendVertexCount(vcountlist);

  weightselem.CloseVCountAndOpenVElement();

  /* write deformer index - weight index pairs */
  int weight_index = 0;
  for (int joint_index : joints) {
    weightselem.appendValues(joint_index, weight_index++);
  }

  weightselem.finish();
}
