/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */

/* TODO:
 * * name imported objects
 * * import object rotation as euler */

#include <algorithm> /* sort() */
#include <map>
#include <string>

#include "COLLADAFWArrayPrimitiveType.h"
#include "COLLADAFWCamera.h"
#include "COLLADAFWColorOrTexture.h"
#include "COLLADAFWIndexList.h"
#include "COLLADAFWLibraryNodes.h"
#include "COLLADAFWLight.h"
#include "COLLADAFWMeshPrimitiveWithFaceVertexCount.h"
#include "COLLADAFWPolygons.h"
#include "COLLADAFWRoot.h"
#include "COLLADAFWSampler.h"
#include "COLLADAFWStableHeaders.h"
#include "COLLADAFWTypes.h"
#include "COLLADAFWVisualScene.h"

#include "COLLADASaxFWLIExtraDataCallbackHandler.h"
#include "COLLADASaxFWLLoader.h"

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_camera.h"
#include "BKE_collection.h"
#include "BKE_fcurve.h"
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_light.h"
#include "BKE_material.h"
#include "BKE_scene.h"

#include "BLI_path_util.h"

#include "DNA_camera_types.h"
#include "DNA_light_types.h"

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "DocumentImporter.h"
#include "ErrorHandler.h"
#include "ExtraHandler.h"
#include "TransformReader.h"

#include "Materials.h"
#include "collada_internal.h"
#include "collada_utils.h"

/*
 * COLLADA Importer limitations:
 * - no multiple scene import, all objects are added to active scene
 */

// #define COLLADA_DEBUG
/* creates empties for each imported bone on layer 2, for debugging */
// #define ARMATURE_TEST

DocumentImporter::DocumentImporter(bContext *C, const ImportSettings *import_settings)
    : import_settings(import_settings),
      mImportStage(Fetching_Scene_data),
      mContext(C),
      view_layer(CTX_data_view_layer(mContext)),
      armature_importer(&unit_converter,
                        &mesh_importer,
                        CTX_data_main(C),
                        CTX_data_scene(C),
                        view_layer,
                        import_settings),
      mesh_importer(
          &unit_converter, &armature_importer, CTX_data_main(C), CTX_data_scene(C), view_layer),
      anim_importer(C, &unit_converter, &armature_importer, CTX_data_scene(C))
{
}

DocumentImporter::~DocumentImporter()
{
  TagsMap::iterator etit;
  etit = uid_tags_map.begin();
  while (etit != uid_tags_map.end()) {
    delete etit->second;
    etit++;
  }
}

bool DocumentImporter::import()
{
  ErrorHandler errorHandler;
  COLLADASaxFWL::Loader loader(&errorHandler);
  COLLADAFW::Root root(&loader, this);
  ExtraHandler *ehandler = new ExtraHandler(this, &(this->anim_importer));

  loader.registerExtraDataCallbackHandler(ehandler);

  /* deselect all to select new objects */
  BKE_view_layer_base_deselect_all(view_layer);

  std::string mFilename = std::string(this->import_settings->filepath);
  const std::string encodedFilename = bc_url_encode(mFilename);
  if (!root.loadDocument(encodedFilename)) {
    fprintf(stderr, "COLLADAFW::Root::loadDocument() returned false on 1st pass\n");
    delete ehandler;
    return false;
  }

  if (errorHandler.hasError()) {
    delete ehandler;
    return false;
  }

  /** TODO: set up scene graph and such here. */
  mImportStage = Fetching_Controller_data;
  COLLADASaxFWL::Loader loader2;
  COLLADAFW::Root root2(&loader2, this);

  if (!root2.loadDocument(encodedFilename)) {
    fprintf(stderr, "COLLADAFW::Root::loadDocument() returned false on 2nd pass\n");
    delete ehandler;
    return false;
  }

  delete ehandler;

  return true;
}

void DocumentImporter::cancel(const COLLADAFW::String &errorMessage)
{
  /* TODO: if possible show error info
   *
   * Should we get rid of invisible Meshes that were created so far
   * or maybe create objects at coordinate space origin?
   *
   * The latter sounds better. */
}

void DocumentImporter::start()
{
}

void DocumentImporter::finish()
{
  if (mImportStage == Fetching_Controller_data) {
    return;
  }

  Main *bmain = CTX_data_main(mContext);
  /* TODO: create a new scene except the selected <visual_scene> -
   * use current blender scene for it */
  Scene *sce = CTX_data_scene(mContext);
  unit_converter.calculate_scale(*sce);

  std::vector<Object *> *objects_to_scale = new std::vector<Object *>();

  /** TODO: Break up and put into 2-pass parsing of DAE. */
  std::vector<const COLLADAFW::VisualScene *>::iterator sit;
  for (sit = vscenes.begin(); sit != vscenes.end(); sit++) {
    PointerRNA sceneptr, unit_settings;
    PropertyRNA *system, *scale;

    /* for scene unit settings: system, scale_length */

    RNA_id_pointer_create(&sce->id, &sceneptr);
    unit_settings = RNA_pointer_get(&sceneptr, "unit_settings");
    system = RNA_struct_find_property(&unit_settings, "system");
    scale = RNA_struct_find_property(&unit_settings, "scale_length");

    if (this->import_settings->import_units) {

      switch (unit_converter.isMetricSystem()) {
        case UnitConverter::Metric:
          RNA_property_enum_set(&unit_settings, system, USER_UNIT_METRIC);
          break;
        case UnitConverter::Imperial:
          RNA_property_enum_set(&unit_settings, system, USER_UNIT_IMPERIAL);
          break;
        default:
          RNA_property_enum_set(&unit_settings, system, USER_UNIT_NONE);
          break;
      }
      float unit_factor = unit_converter.getLinearMeter();
      RNA_property_float_set(&unit_settings, scale, unit_factor);
      fprintf(stdout, "Collada: Adjusting Blender units to Importset units: %f.\n", unit_factor);
    }

    /* Write nodes to scene */
    fprintf(stderr, "+-- Import Scene --------\n");
    const COLLADAFW::NodePointerArray &roots = (*sit)->getRootNodes();
    for (unsigned int i = 0; i < roots.getCount(); i++) {
      std::vector<Object *> *objects_done = write_node(roots[i], nullptr, sce, nullptr, false);
      objects_to_scale->insert(
          objects_to_scale->end(), objects_done->begin(), objects_done->end());
      delete objects_done;
    }
  }

  mesh_importer.optimize_material_assignements();

  armature_importer.set_tags_map(this->uid_tags_map);
  armature_importer.make_armatures(mContext, *objects_to_scale);
  armature_importer.make_shape_keys(mContext);

#if 0
  armature_importer.fix_animation();
#endif

  for (const COLLADAFW::VisualScene *vscene : vscenes) {
    const COLLADAFW::NodePointerArray &roots = vscene->getRootNodes();

    for (unsigned int i = 0; i < roots.getCount(); i++) {
      translate_anim_recursive(roots[i], nullptr, nullptr);
    }
  }

  if (!libnode_ob.empty()) {

    fprintf(stderr, "| Cleanup: free %d library nodes\n", (int)libnode_ob.size());
    /* free all library_nodes */
    std::vector<Object *>::iterator it;
    for (it = libnode_ob.begin(); it != libnode_ob.end(); it++) {
      Object *ob = *it;
      BKE_scene_collections_object_remove(bmain, sce, ob, true);
    }
    libnode_ob.clear();
  }

  bc_match_scale(objects_to_scale, unit_converter, !this->import_settings->import_units);

  delete objects_to_scale;

  /* update scene */
  DEG_id_tag_update(&sce->id, ID_RECALC_COPY_ON_WRITE);
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(mContext, NC_OBJECT | ND_TRANSFORM, nullptr);
}

void DocumentImporter::translate_anim_recursive(COLLADAFW::Node *node,
                                                COLLADAFW::Node *par = nullptr,
                                                Object *parob = nullptr)
{
  /* The split in T29246, root_map must point at actual root when
   * calculating bones in apply_curves_as_matrix. - actual root is the root node.
   * This has to do with inverse bind poses being world space
   * (the sources for skinned bones' rest-poses) and the way
   * non-skinning nodes have their "rest-pose" recursively calculated.
   * XXX TODO: design issue, how to support unrelated joints taking
   * part in skinning. */
  if (par) {  // && par->getType() == COLLADAFW::Node::JOINT) {
    /* If par is root if there's no corresponding key in root_map. */
    if (root_map.find(par->getUniqueId()) == root_map.end()) {
      root_map[node->getUniqueId()] = node;
    }
    else {
      root_map[node->getUniqueId()] = root_map[par->getUniqueId()];
    }
  }

#if 0
  COLLADAFW::Transformation::TransformationType types[] = {
      COLLADAFW::Transformation::ROTATE,
      COLLADAFW::Transformation::SCALE,
      COLLADAFW::Transformation::TRANSLATE,
      COLLADAFW::Transformation::MATRIX,
  };

  Object *ob;
#endif
  unsigned int i;

  if (node->getType() == COLLADAFW::Node::JOINT && par == nullptr) {
    /* For Skeletons without root node we have to simulate the
     * root node here and recursively enter the same function
     * XXX: maybe this can be made more elegant. */
    translate_anim_recursive(node, node, parob);
  }
  else {
    anim_importer.translate_Animations(
        node, root_map, object_map, FW_object_map, uid_material_map);
    COLLADAFW::NodePointerArray &children = node->getChildNodes();
    for (i = 0; i < children.getCount(); i++) {
      translate_anim_recursive(children[i], node, nullptr);
    }
  }
}

std::string DocumentImporter::get_import_version(const COLLADAFW::FileInfo *asset)
{
  const char AUTORING_TOOL[] = "authoring_tool";
  const std::string BLENDER("Blender ");
  const COLLADAFW::FileInfo::ValuePairPointerArray &valuePairs = asset->getValuePairArray();
  for (size_t i = 0, count = valuePairs.getCount(); i < count; i++) {
    const COLLADAFW::FileInfo::ValuePair *valuePair = valuePairs[i];
    const COLLADAFW::String &key = valuePair->first;
    const COLLADAFW::String &value = valuePair->second;
    if (key == AUTORING_TOOL) {
      if (value.compare(0, BLENDER.length(), BLENDER) == 0) {
        /* Was made with Blender, now get version string */
        std::string v = value.substr(BLENDER.length());
        std::string::size_type n = v.find(" ");
        if (n > 0) {
          return v.substr(0, n);
        }
      }
    }
  }
  return "";
}

bool DocumentImporter::writeGlobalAsset(const COLLADAFW::FileInfo *asset)
{
  unit_converter.read_asset(asset);
  import_from_version = get_import_version(asset);
  anim_importer.set_import_from_version(import_from_version);
  return true;
}

bool DocumentImporter::writeScene(const COLLADAFW::Scene *scene)
{
  /* XXX could store the scene id, but do nothing for now */
  return true;
}
Object *DocumentImporter::create_camera_object(COLLADAFW::InstanceCamera *camera, Scene *sce)
{
  const COLLADAFW::UniqueId &cam_uid = camera->getInstanciatedObjectId();
  if (uid_camera_map.find(cam_uid) == uid_camera_map.end()) {
    // fprintf(stderr, "Couldn't find camera by UID.\n");
    return nullptr;
  }

  Main *bmain = CTX_data_main(mContext);
  Object *ob = bc_add_object(bmain, sce, view_layer, OB_CAMERA, nullptr);
  Camera *cam = uid_camera_map[cam_uid];
  Camera *old_cam = (Camera *)ob->data;
  ob->data = cam;
  BKE_id_free_us(bmain, old_cam);
  return ob;
}

Object *DocumentImporter::create_light_object(COLLADAFW::InstanceLight *lamp, Scene *sce)
{
  const COLLADAFW::UniqueId &lamp_uid = lamp->getInstanciatedObjectId();
  if (uid_light_map.find(lamp_uid) == uid_light_map.end()) {
    fprintf(stderr, "Couldn't find light by UID.\n");
    return nullptr;
  }

  Main *bmain = CTX_data_main(mContext);
  Object *ob = bc_add_object(bmain, sce, view_layer, OB_LAMP, nullptr);
  Light *la = uid_light_map[lamp_uid];
  Light *old_light = (Light *)ob->data;
  ob->data = la;
  BKE_id_free_us(bmain, old_light);
  return ob;
}

Object *DocumentImporter::create_instance_node(Object *source_ob,
                                               COLLADAFW::Node *source_node,
                                               COLLADAFW::Node *instance_node,
                                               Scene *sce,
                                               bool is_library_node)
{
  // fprintf(stderr, "create <instance_node> under node id=%s from node id=%s\n", instance_node ?
  // instance_node->getOriginalId().c_str() : NULL, source_node ?
  // source_node->getOriginalId().c_str() : NULL);

  Main *bmain = CTX_data_main(mContext);
  Object *obn = (Object *)BKE_id_copy(bmain, &source_ob->id);
  id_us_min(&obn->id);
  DEG_id_tag_update(&obn->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
  BKE_collection_object_add_from(bmain, sce, source_ob, obn);

  if (instance_node) {
    anim_importer.read_node_transform(instance_node, obn);
    /* if we also have a source_node (always ;), take its
     * transformation matrix and apply it to the newly instantiated
     * object to account for node hierarchy transforms in `.dae`. */
    if (source_node) {
      COLLADABU::Math::Matrix4 mat4 = source_node->getTransformationMatrix();
      COLLADABU::Math::Matrix4 bmat4 =
          mat4.transpose(); /* transpose to get blender row-major order */
      float mat[4][4];
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          mat[i][j] = bmat4[i][j];
        }
      }
      /* calc new matrix and apply */
      mul_m4_m4m4(obn->obmat, obn->obmat, mat);
      BKE_object_apply_mat4(obn, obn->obmat, false, false);
    }
  }
  else {
    anim_importer.read_node_transform(source_node, obn);
  }

  /*DAG_relations_tag_update(CTX_data_main(mContext));*/

  COLLADAFW::NodePointerArray &children = source_node->getChildNodes();
  if (children.getCount()) {
    for (unsigned int i = 0; i < children.getCount(); i++) {
      COLLADAFW::Node *child_node = children[i];
      const COLLADAFW::UniqueId &child_id = child_node->getUniqueId();
      if (object_map.find(child_id) == object_map.end()) {
        continue;
      }
      COLLADAFW::InstanceNodePointerArray &inodes = child_node->getInstanceNodes();
      Object *new_child = nullptr;
      if (inodes.getCount()) { /* \todo loop through instance nodes */
        const COLLADAFW::UniqueId &id = inodes[0]->getInstanciatedObjectId();
        fprintf(stderr, "Doing %d child nodes\n", (int)node_map.count(id));
        new_child = create_instance_node(
            object_map.find(id)->second, node_map[id], child_node, sce, is_library_node);
      }
      else {
        new_child = create_instance_node(
            object_map.find(child_id)->second, child_node, nullptr, sce, is_library_node);
      }
      bc_set_parent(new_child, obn, mContext, true);

      if (is_library_node) {
        libnode_ob.push_back(new_child);
      }
    }
  }

  return obn;
}

void DocumentImporter::create_constraints(ExtraTags *et, Object *ob)
{
  if (et && et->isProfile("blender")) {
    std::string name;
    short type = 0;
    et->setData("type", &type);
    BKE_constraint_add_for_object(ob, "Test_con", type);
  }
}

void DocumentImporter::report_unknown_reference(const COLLADAFW::Node &node,
                                                const std::string object_type)
{
  std::string id = node.getOriginalId();
  std::string name = node.getName();
  fprintf(stderr,
          "error: node id=\"%s\", name=\"%s\" refers to an undefined %s.\n",
          id.c_str(),
          name.c_str(),
          object_type.c_str());
}

std::vector<Object *> *DocumentImporter::write_node(COLLADAFW::Node *node,
                                                    COLLADAFW::Node *parent_node,
                                                    Scene *sce,
                                                    Object *par,
                                                    bool is_library_node)
{
  Main *bmain = CTX_data_main(mContext);
  Object *ob = nullptr;
  bool is_joint = node->getType() == COLLADAFW::Node::JOINT;
  bool read_transform = true;
  std::string id = node->getOriginalId();
  std::string name = node->getName();

  /* if node has child nodes write them */
  COLLADAFW::NodePointerArray &child_nodes = node->getChildNodes();

  std::vector<Object *> *objects_done = new std::vector<Object *>();
  std::vector<Object *> *root_objects = new std::vector<Object *>();

  fprintf(
      stderr, "| %s id='%s', name='%s'\n", is_joint ? "JOINT" : "NODE ", id.c_str(), name.c_str());

  if (is_joint) {
    if (parent_node == nullptr && !is_library_node) {
      /* A Joint on root level is a skeleton without root node.
       * Here we add the armature "on the fly": */
      par = bc_add_object(bmain, sce, view_layer, OB_ARMATURE, std::string("Armature").c_str());
      objects_done->push_back(par);
      root_objects->push_back(par);
      object_map.insert(std::pair<COLLADAFW::UniqueId, Object *>(node->getUniqueId(), par));
      node_map[node->getUniqueId()] = node;
    }
    if (parent_node == nullptr || parent_node->getType() != COLLADAFW::Node::JOINT) {
      armature_importer.add_root_joint(node, par);
    }

    if (parent_node == nullptr) {
      /* for skeletons without root node all has been done above.
       * Skeletons with root node are handled further down. */
      goto finally;
    }
  }
  else {
    COLLADAFW::InstanceGeometryPointerArray &geom = node->getInstanceGeometries();
    COLLADAFW::InstanceCameraPointerArray &camera = node->getInstanceCameras();
    COLLADAFW::InstanceLightPointerArray &lamp = node->getInstanceLights();
    COLLADAFW::InstanceControllerPointerArray &controller = node->getInstanceControllers();
    COLLADAFW::InstanceNodePointerArray &inst_node = node->getInstanceNodes();
    size_t geom_done = 0;
    size_t camera_done = 0;
    size_t lamp_done = 0;
    size_t controller_done = 0;
    size_t inst_done = 0;

    /* XXX linking object with the first <instance_geometry>, though a node may have more of
     * them... maybe join multiple <instance_...> meshes into 1, and link object with it? not
     * sure... <instance_geometry> */
    while (geom_done < geom.getCount()) {
      ob = mesh_importer.create_mesh_object(node, geom[geom_done], false, uid_material_map);
      if (ob == nullptr) {
        report_unknown_reference(*node, "instance_mesh");
      }
      else {
        objects_done->push_back(ob);
        if (parent_node == nullptr) {
          root_objects->push_back(ob);
        }
      }
      geom_done++;
    }
    while (camera_done < camera.getCount()) {
      ob = create_camera_object(camera[camera_done], sce);
      if (ob == nullptr) {
        report_unknown_reference(*node, "instance_camera");
      }
      else {
        objects_done->push_back(ob);
        if (parent_node == nullptr) {
          root_objects->push_back(ob);
        }
      }
      camera_done++;
    }
    while (lamp_done < lamp.getCount()) {
      ob = create_light_object(lamp[lamp_done], sce);
      if (ob == nullptr) {
        report_unknown_reference(*node, "instance_light");
      }
      else {
        objects_done->push_back(ob);
        if (parent_node == nullptr) {
          root_objects->push_back(ob);
        }
      }
      lamp_done++;
    }
    while (controller_done < controller.getCount()) {
      COLLADAFW::InstanceGeometry *geometry = (COLLADAFW::InstanceGeometry *)
          controller[controller_done];
      ob = mesh_importer.create_mesh_object(node, geometry, true, uid_material_map);
      if (ob == nullptr) {
        report_unknown_reference(*node, "instance_controller");
      }
      else {
        objects_done->push_back(ob);
        if (parent_node == nullptr) {
          root_objects->push_back(ob);
        }
      }
      controller_done++;
    }
    /* XXX instance_node is not supported yet */
    while (inst_done < inst_node.getCount()) {
      const COLLADAFW::UniqueId &node_id = inst_node[inst_done]->getInstanciatedObjectId();
      if (object_map.find(node_id) == object_map.end()) {
        fprintf(stderr,
                "Cannot find object for node referenced by <instance_node name=\"%s\">.\n",
                inst_node[inst_done]->getName().c_str());
        ob = nullptr;
      }
      else {
        std::pair<std::multimap<COLLADAFW::UniqueId, Object *>::iterator,
                  std::multimap<COLLADAFW::UniqueId, Object *>::iterator>
            pair_iter = object_map.equal_range(node_id);
        for (std::multimap<COLLADAFW::UniqueId, Object *>::iterator it2 = pair_iter.first;
             it2 != pair_iter.second;
             it2++) {
          Object *source_ob = (Object *)it2->second;
          COLLADAFW::Node *source_node = node_map[node_id];
          ob = create_instance_node(source_ob, source_node, node, sce, is_library_node);
          objects_done->push_back(ob);
          if (parent_node == nullptr) {
            root_objects->push_back(ob);
          }
        }
      }
      inst_done++;

      read_transform = false;
    }

    /* if node is empty - create empty object
     * XXX empty node may not mean it is empty object, not sure about this */
    if ((geom_done + camera_done + lamp_done + controller_done + inst_done) < 1) {
      /* Check if Object is armature, by checking if immediate child is a JOINT node. */
      if (is_armature(node)) {
        ob = bc_add_object(bmain, sce, view_layer, OB_ARMATURE, name.c_str());
      }
      else {
        ob = bc_add_object(bmain, sce, view_layer, OB_EMPTY, nullptr);
      }
      objects_done->push_back(ob);
      if (parent_node == nullptr) {
        root_objects->push_back(ob);
      }
    }

    /* XXX: if there are multiple instances, only one is stored. */

    if (!ob) {
      goto finally;
    }

    for (Object *ob : *objects_done) {
      std::string nodename = node->getName().empty() ? node->getOriginalId() : node->getName();
      BKE_libblock_rename(bmain, &ob->id, (char *)nodename.c_str());
      object_map.insert(std::pair<COLLADAFW::UniqueId, Object *>(node->getUniqueId(), ob));
      node_map[node->getUniqueId()] = node;

      if (is_library_node) {
        libnode_ob.push_back(ob);
      }
    }

    /* create_constraints(et,ob); */
  }

  for (Object *ob : *objects_done) {
    if (read_transform) {
      anim_importer.read_node_transform(node, ob); /* overwrites location set earlier */
    }

    if (!is_joint) {
      if (par && ob) {
        ob->parent = par;
        ob->partype = PAROBJECT;
        ob->parsubstr[0] = 0;

        // bc_set_parent(ob, par, mContext, false);
      }
    }
  }

  if (objects_done->empty()) {
    ob = nullptr;
  }
  else {
    ob = *objects_done->begin();
  }

  for (unsigned int i = 0; i < child_nodes.getCount(); i++) {
    std::vector<Object *> *child_objects;
    child_objects = write_node(child_nodes[i], node, sce, ob, is_library_node);
    delete child_objects;
  }

finally:
  delete objects_done;

  return root_objects;
}

bool DocumentImporter::writeVisualScene(const COLLADAFW::VisualScene *visualScene)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  /* This method called on post process after writeGeometry, writeMaterial, etc. */

  /* For each <node> in <visual_scene>:
   * create an Object
   * if Mesh (previously created in writeGeometry) to which <node> corresponds exists,
   * link Object with that mesh.
   *
   * Update: since we cannot link a Mesh with Object in
   * writeGeometry because <geometry> does not reference <node>,
   * we link Objects with Meshes here.
   */
  vscenes.push_back(visualScene);

  return true;
}

bool DocumentImporter::writeLibraryNodes(const COLLADAFW::LibraryNodes *libraryNodes)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  Scene *sce = CTX_data_scene(mContext);

  const COLLADAFW::NodePointerArray &nodes = libraryNodes->getNodes();

  fprintf(stderr, "+-- Read Library nodes ----------\n");
  for (unsigned int i = 0; i < nodes.getCount(); i++) {
    std::vector<Object *> *child_objects;
    child_objects = write_node(nodes[i], nullptr, sce, nullptr, true);
    delete child_objects;
  }
  return true;
}

bool DocumentImporter::writeGeometry(const COLLADAFW::Geometry *geom)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  return mesh_importer.write_geometry(geom);
}

bool DocumentImporter::writeMaterial(const COLLADAFW::Material *cmat)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  Main *bmain = CTX_data_main(mContext);
  const std::string &str_mat_id = cmat->getName().empty() ? cmat->getOriginalId() :
                                                            cmat->getName();
  Material *ma = BKE_material_add(bmain, (char *)str_mat_id.c_str());

  this->uid_effect_map[cmat->getInstantiatedEffect()] = ma;
  this->uid_material_map[cmat->getUniqueId()] = ma;

  return true;
}

void DocumentImporter::write_profile_COMMON(COLLADAFW::EffectCommon *ef, Material *ma)
{
  MaterialNode matNode = MaterialNode(mContext, ef, ma, uid_image_map);

  /* Direct mapping to principled BSDF Shader */
  matNode.set_diffuse(ef->getDiffuse());
  matNode.set_emission(ef->getEmission());
  matNode.set_ior(ef->getIndexOfRefraction());
  matNode.set_alpha(ef->getOpaqueMode(), ef->getTransparent(), ef->getTransparency());

  /* following mapping still needs to be verified */
#if 0
  /* needs rework to be done for 2.81 */
  matNode.set_shininess(ef->getShininess());
#endif
  matNode.set_reflectivity(ef->getReflectivity());

  /* not supported by principled BSDF */
  matNode.set_ambient(ef->getAmbient());
  matNode.set_specular(ef->getSpecular());
  matNode.set_reflective(ef->getReflective());

  matNode.update_material_nodetree();
}

bool DocumentImporter::writeEffect(const COLLADAFW::Effect *effect)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  const COLLADAFW::UniqueId &uid = effect->getUniqueId();

  if (uid_effect_map.find(uid) == uid_effect_map.end()) {
    fprintf(stderr, "Couldn't find a material by UID.\n");
    return true;
  }

  Material *ma = uid_effect_map[uid];
  std::map<COLLADAFW::UniqueId, Material *>::iterator iter;
  for (iter = uid_material_map.begin(); iter != uid_material_map.end(); iter++) {
    if (iter->second == ma) {
      this->FW_object_map[iter->first] = effect;
      break;
    }
  }
  COLLADAFW::CommonEffectPointerArray common_efs = effect->getCommonEffects();
  if (common_efs.getCount() < 1) {
    fprintf(stderr, "Couldn't find <profile_COMMON>.\n");
    return true;
  }
  /* XXX TODO: Take all <profile_common>s
   * Currently only first <profile_common> is supported */
  COLLADAFW::EffectCommon *ef = common_efs[0];
  write_profile_COMMON(ef, ma);
  this->FW_object_map[effect->getUniqueId()] = effect;

  return true;
}

bool DocumentImporter::writeCamera(const COLLADAFW::Camera *camera)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  Main *bmain = CTX_data_main(mContext);
  Camera *cam = nullptr;
  std::string cam_id, cam_name;

  ExtraTags *et = getExtraTags(camera->getUniqueId());
  cam_id = camera->getOriginalId();
  cam_name = camera->getName();
  if (cam_name.empty()) {
    cam = (Camera *)BKE_camera_add(bmain, (char *)cam_id.c_str());
  }
  else {
    cam = (Camera *)BKE_camera_add(bmain, (char *)cam_name.c_str());
  }

  if (!cam) {
    fprintf(stderr, "Cannot create camera.\n");
    return true;
  }

  if (et && et->isProfile("blender")) {
    et->setData("shiftx", &(cam->shiftx));
    et->setData("shifty", &(cam->shifty));
    et->setData("dof_distance", &(cam->dof.focus_distance));
  }
  cam->clip_start = camera->getNearClippingPlane().getValue();
  cam->clip_end = camera->getFarClippingPlane().getValue();

  COLLADAFW::Camera::CameraType type = camera->getCameraType();
  switch (type) {
    case COLLADAFW::Camera::ORTHOGRAPHIC: {
      cam->type = CAM_ORTHO;
    } break;
    case COLLADAFW::Camera::PERSPECTIVE: {
      cam->type = CAM_PERSP;
    } break;
    case COLLADAFW::Camera::UNDEFINED_CAMERATYPE: {
      fprintf(stderr, "Current camera type is not supported.\n");
      cam->type = CAM_PERSP;
    } break;
  }

  switch (camera->getDescriptionType()) {
    case COLLADAFW::Camera::ASPECTRATIO_AND_Y: {
      switch (cam->type) {
        case CAM_ORTHO: {
          double ymag = 2 * camera->getYMag().getValue();
          double aspect = camera->getAspectRatio().getValue();
          double xmag = aspect * ymag;
          cam->ortho_scale = (float)xmag;
        } break;
        case CAM_PERSP:
        default: {
          double yfov = camera->getYFov().getValue();
          double aspect = camera->getAspectRatio().getValue();

          /* NOTE: Needs more testing (As we currently have no official test data for this) */

          double xfov = 2.0f * atanf(aspect * tanf(DEG2RADF(yfov) * 0.5f));
          cam->lens = fov_to_focallength(xfov, cam->sensor_x);
        } break;
      }
    } break;
    /* XXX correct way to do following four is probably to get also render
     * size and determine proper settings from that somehow */
    case COLLADAFW::Camera::ASPECTRATIO_AND_X:
    case COLLADAFW::Camera::SINGLE_X:
    case COLLADAFW::Camera::X_AND_Y: {
      switch (cam->type) {
        case CAM_ORTHO:
          cam->ortho_scale = (float)camera->getXMag().getValue() * 2;
          break;
        case CAM_PERSP:
        default: {
          double x = camera->getXFov().getValue();
          /* X is in degrees, cam->lens is in millimeters. */
          cam->lens = fov_to_focallength(DEG2RADF(x), cam->sensor_x);
        } break;
      }
    } break;
    case COLLADAFW::Camera::SINGLE_Y: {
      switch (cam->type) {
        case CAM_ORTHO:
          cam->ortho_scale = (float)camera->getYMag().getValue();
          break;
        case CAM_PERSP:
        default: {
          double yfov = camera->getYFov().getValue();
          /* yfov is in degrees, cam->lens is in millimeters. */
          cam->lens = fov_to_focallength(DEG2RADF(yfov), cam->sensor_x);
        } break;
      }
    } break;
    case COLLADAFW::Camera::UNDEFINED:
      /* read nothing, use blender defaults. */
      break;
  }

  this->uid_camera_map[camera->getUniqueId()] = cam;
  this->FW_object_map[camera->getUniqueId()] = camera;
  /* XXX import camera options */
  return true;
}

bool DocumentImporter::writeImage(const COLLADAFW::Image *image)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  const std::string &imagepath = image->getImageURI().toNativePath();

  char dir[FILE_MAX];
  char absolute_path[FILE_MAX];
  const char *workpath;

  BLI_split_dir_part(this->import_settings->filepath, dir, sizeof(dir));
  BLI_join_dirfile(absolute_path, sizeof(absolute_path), dir, imagepath.c_str());
  if (BLI_exists(absolute_path)) {
    workpath = absolute_path;
  }
  else {
    /* Maybe imagepath was already absolute ? */
    if (!BLI_exists(imagepath.c_str())) {
      fprintf(stderr, "|! Image not found: %s\n", imagepath.c_str());
      return true;
    }
    workpath = imagepath.c_str();
  }

  Image *ima = BKE_image_load_exists(CTX_data_main(mContext), workpath);
  if (!ima) {
    fprintf(stderr, "|! Cannot create image: %s\n", workpath);
    return true;
  }
  this->uid_image_map[image->getUniqueId()] = ima;
  fprintf(stderr, "| import Image: %s\n", workpath);
  return true;
}

bool DocumentImporter::writeLight(const COLLADAFW::Light *light)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  Main *bmain = CTX_data_main(mContext);
  Light *lamp = nullptr;
  std::string la_id, la_name;

  ExtraTags *et = getExtraTags(light->getUniqueId());
#if 0
  TagsMap::iterator etit;
  ExtraTags *et = 0;
  etit = uid_tags_map.find(light->getUniqueId().toAscii());
  if (etit != uid_tags_map.end()) {
    et = etit->second;
  }
#endif

  la_id = light->getOriginalId();
  la_name = light->getName();
  if (la_name.empty()) {
    lamp = (Light *)BKE_light_add(bmain, (char *)la_id.c_str());
  }
  else {
    lamp = (Light *)BKE_light_add(bmain, (char *)la_name.c_str());
  }

  if (!lamp) {
    fprintf(stderr, "Cannot create light.\n");
    return true;
  }

  /* if we find an ExtraTags for this, use that instead. */
  if (et && et->isProfile("blender")) {
    et->setData("type", &(lamp->type));
    et->setData("flag", &(lamp->flag));
    et->setData("mode", &(lamp->mode));
    et->setData("gamma", &(lamp->k));
    et->setData("red", &(lamp->r));
    et->setData("green", &(lamp->g));
    et->setData("blue", &(lamp->b));
    et->setData("shadow_r", &(lamp->shdwr));
    et->setData("shadow_g", &(lamp->shdwg));
    et->setData("shadow_b", &(lamp->shdwb));
    et->setData("energy", &(lamp->energy));
    et->setData("dist", &(lamp->dist));
    et->setData("spotsize", &(lamp->spotsize));
    lamp->spotsize = DEG2RADF(lamp->spotsize);
    et->setData("spotblend", &(lamp->spotblend));
    et->setData("att1", &(lamp->att1));
    et->setData("att2", &(lamp->att2));
    et->setData("falloff_type", &(lamp->falloff_type));
    et->setData("clipsta", &(lamp->clipsta));
    et->setData("clipend", &(lamp->clipend));
    et->setData("bias", &(lamp->bias));
    et->setData("soft", &(lamp->soft));
    et->setData("bufsize", &(lamp->bufsize));
    et->setData("buffers", &(lamp->buffers));
    et->setData("area_shape", &(lamp->area_shape));
    et->setData("area_size", &(lamp->area_size));
    et->setData("area_sizey", &(lamp->area_sizey));
    et->setData("area_sizez", &(lamp->area_sizez));
  }
  else {
    float constatt = light->getConstantAttenuation().getValue();
    float linatt = light->getLinearAttenuation().getValue();
    float quadatt = light->getQuadraticAttenuation().getValue();
    float d = 25.0f;
    float att1 = 0.0f;
    float att2 = 0.0f;
    float e = 1.0f;

    if (light->getColor().isValid()) {
      COLLADAFW::Color col = light->getColor();
      lamp->r = col.getRed();
      lamp->g = col.getGreen();
      lamp->b = col.getBlue();
    }

    if (IS_EQ(linatt, 0.0f) && quadatt > 0.0f) {
      att2 = quadatt;
      d = sqrt(1.0f / quadatt);
    }
    /* linear light */
    else if (IS_EQ(quadatt, 0.0f) && linatt > 0.0f) {
      att1 = linatt;
      d = (1.0f / linatt);
    }
    else if (IS_EQ(constatt, 1.0f)) {
      att1 = 1.0f;
    }
    else {
      /* assuming point light (const att = 1.0); */
      att1 = 1.0f;
    }

    d *= (1.0f / unit_converter.getLinearMeter());

    lamp->energy = e;
    lamp->dist = d;

    switch (light->getLightType()) {
      case COLLADAFW::Light::AMBIENT_LIGHT: {
        lamp->type = LA_SUN; /* TODO: needs more thoughts. */
      } break;
      case COLLADAFW::Light::SPOT_LIGHT: {
        lamp->type = LA_SPOT;
        lamp->att1 = att1;
        lamp->att2 = att2;
        if (IS_EQ(att1, 0.0f) && att2 > 0) {
          lamp->falloff_type = LA_FALLOFF_INVSQUARE;
        }
        if (IS_EQ(att2, 0.0f) && att1 > 0) {
          lamp->falloff_type = LA_FALLOFF_INVLINEAR;
        }
        lamp->spotsize = DEG2RADF(light->getFallOffAngle().getValue());
        lamp->spotblend = light->getFallOffExponent().getValue();
      } break;
      case COLLADAFW::Light::DIRECTIONAL_LIGHT: {
        /* our sun is very strong, so pick a smaller energy level */
        lamp->type = LA_SUN;
      } break;
      case COLLADAFW::Light::POINT_LIGHT: {
        lamp->type = LA_LOCAL;
        lamp->att1 = att1;
        lamp->att2 = att2;
        if (IS_EQ(att1, 0.0f) && att2 > 0) {
          lamp->falloff_type = LA_FALLOFF_INVSQUARE;
        }
        if (IS_EQ(att2, 0.0f) && att1 > 0) {
          lamp->falloff_type = LA_FALLOFF_INVLINEAR;
        }
      } break;
      case COLLADAFW::Light::UNDEFINED: {
        fprintf(stderr, "Current light type is not supported.\n");
        lamp->type = LA_LOCAL;
      } break;
    }
  }

  this->uid_light_map[light->getUniqueId()] = lamp;
  this->FW_object_map[light->getUniqueId()] = light;
  return true;
}

bool DocumentImporter::writeAnimation(const COLLADAFW::Animation *anim)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  return anim_importer.write_animation(anim);
}

bool DocumentImporter::writeAnimationList(const COLLADAFW::AnimationList *animationList)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  /* return true; */
  return anim_importer.write_animation_list(animationList);
}

#if WITH_OPENCOLLADA_ANIMATION_CLIP
bool DocumentImporter::writeAnimationClip(const COLLADAFW::AnimationClip *animationClip)
{
  /* Since opencollada 1.6.68: called on post-process stage after writeVisualScenes. */

  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  return true;
  /* TODO: implement import of AnimationClips */
  // return animation_clip_importer.write_animation_clip(animationClip);
}
#endif

bool DocumentImporter::writeSkinControllerData(const COLLADAFW::SkinControllerData *skin)
{
  return armature_importer.write_skin_controller_data(skin);
}

bool DocumentImporter::writeController(const COLLADAFW::Controller *controller)
{
  if (mImportStage == Fetching_Controller_data) {
    return true;
  }

  return armature_importer.write_controller(controller);
}

bool DocumentImporter::writeFormulas(const COLLADAFW::Formulas *formulas)
{
  return true;
}

bool DocumentImporter::writeKinematicsScene(const COLLADAFW::KinematicsScene *kinematicsScene)
{
  return true;
}

ExtraTags *DocumentImporter::getExtraTags(const COLLADAFW::UniqueId &uid)
{
  if (uid_tags_map.find(uid.toAscii()) == uid_tags_map.end()) {
    return nullptr;
  }
  return uid_tags_map[uid.toAscii()];
}

bool DocumentImporter::addExtraTags(const COLLADAFW::UniqueId &uid, ExtraTags *extra_tags)
{
  uid_tags_map[uid.toAscii()] = extra_tags;
  return true;
}

bool DocumentImporter::is_armature(COLLADAFW::Node *node)
{
  COLLADAFW::NodePointerArray &child_nodes = node->getChildNodes();
  for (unsigned int i = 0; i < child_nodes.getCount(); i++) {
    if (child_nodes[i]->getType() == COLLADAFW::Node::JOINT) {
      return true;
    }
  }

  /* no child is JOINT */
  return false;
}
