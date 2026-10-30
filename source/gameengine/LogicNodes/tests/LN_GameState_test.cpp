/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include "EXP_FloatValue.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "LN_GameState.h"
#include "LN_GameStateInternal.h"
#include "LN_PersistenceCapability.h"
#include "SCA_TimeEventManager.h"

namespace {

class RuntimePathScope {
 public:
  RuntimePathScope() : m_main_path(KX_GetMainPath()), m_original_path(KX_GetOrigPath()) {}

  ~RuntimePathScope()
  {
    KX_SetMainPath(m_main_path);
    KX_SetOrigPath(m_original_path);
  }

 private:
  std::string m_main_path;
  std::string m_original_path;
};

std::filesystem::path TestProjectFile()
{
  return std::filesystem::temp_directory_path() / "upbge_logic_nodes_path_test" / "game.blend";
}

constexpr char kSceneUuid[] = "123e4567-e89b-12d3-a456-426614174000";

LN_PersistenceCandidateDescriptor SupportedCandidate()
{
  LN_PersistenceCandidateDescriptor descriptor;
  descriptor.object_kind = LN_PersistenceObjectKind::Mesh;
  descriptor.runtime_role = LN_PersistenceRuntimeRole::Ordinary;
  descriptor.physics_kind = LN_PersistencePhysicsKind::JoltRigid;
  descriptor.scene_identity_valid = true;
  descriptor.object_identity_valid = true;
  descriptor.runtime_class_supported = true;
  descriptor.expects_physics_body = true;
  descriptor.physics_body_valid = true;
  descriptor.reconstruction_state_valid = true;
  descriptor.reconstruction_render_data_valid = true;
  descriptor.reconstruction_properties_supported = true;
  return descriptor;
}

upbge::logic_nodes::game_state::ObjectState StructuralObject(const char *uuid,
                                                             const char *name)
{
  using namespace upbge::logic_nodes::game_state;
  ObjectState object;
  object.persistence_id = uuid;
  object.name = name;
  object.has_existence = true;
  object.exists = true;
  object.has_transform = true;
  object.has_material_state = true;
  object.has_runtime_structure_state = true;
  object.has_hierarchy_state = true;
  object.has_rigid_body_state = true;
  return object;
}

}  // namespace

TEST(LN_GameState, ResolvesDefaultAndRelativeDirectoriesAgainstRuntimeProject)
{
  RuntimePathScope restore_paths;
  const std::filesystem::path project_file = TestProjectFile();
  KX_SetMainPath(project_file.string());
  KX_SetOrigPath(std::string());

  std::string filepath;
  std::string error;
  const std::filesystem::path user_data = project_file.parent_path() / "user-data";
  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      4, std::string(), kSceneUuid, user_data.string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath),
            user_data / "upbge/savegames" / kSceneUuid / "save4.json");

  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      2, "profiles.v2", kSceneUuid, user_data.string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath),
            project_file.parent_path() / "profiles.v2/save2.json");

  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      7, "//checkpoints", kSceneUuid, user_data.string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath),
            project_file.parent_path() / "checkpoints/save7.json");
}

TEST(LN_GameState, PreservesExplicitSaveFilesAndAbsolutePaths)
{
  RuntimePathScope restore_paths;
  const std::filesystem::path project_file = TestProjectFile();
  KX_SetMainPath(project_file.string());
  KX_SetOrigPath(std::string());

  std::string filepath;
  std::string error;
  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      9, "manual.JSON", kSceneUuid, std::string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath), project_file.parent_path() / "manual.JSON");

  const std::filesystem::path absolute_file = std::filesystem::temp_directory_path() /
                                              "upbge_explicit_save_game.SAV";
  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      1, absolute_file.string(), kSceneUuid, std::string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath), absolute_file);
}

TEST(LN_GameState, UsesOriginalRuntimePathWhenMainPathIsUnavailable)
{
  RuntimePathScope restore_paths;
  const std::filesystem::path project_file = TestProjectFile();
  KX_SetMainPath(std::string());
  KX_SetOrigPath(project_file.string());

  std::string filepath;
  std::string error;
  EXPECT_TRUE(LN_GameStateService::ResolveFilePath(
      3, "slot.json", kSceneUuid, std::string(), filepath, error));
  EXPECT_EQ(std::filesystem::path(filepath), project_file.parent_path() / "slot.json");
}

TEST(LN_GameState, RejectsInvalidSlotAndEmbeddedNullPath)
{
  RuntimePathScope restore_paths;
  KX_SetMainPath(TestProjectFile().string());

  std::string filepath;
  std::string error;
  EXPECT_FALSE(LN_GameStateService::ResolveFilePath(-1,
                                                    std::string(),
                                                    kSceneUuid,
                                                    std::filesystem::temp_directory_path().string(),
                                                    filepath,
                                                    error));
  EXPECT_NE(error.find("slot"), std::string::npos);

  const std::string embedded_null("save\0.json", 10);
  EXPECT_FALSE(LN_GameStateService::ResolveFilePath(
      0, embedded_null, kSceneUuid, std::string(), filepath, error));
  EXPECT_NE(error.find("null"), std::string::npos);
}

TEST(LN_GameState, DefaultPathRequiresCanonicalSceneIdentity)
{
  std::string filepath;
  std::string error;
  EXPECT_FALSE(LN_GameStateService::ResolveFilePath(0,
                                                    std::string(),
                                                    "not-a-uuid",
                                                    std::filesystem::temp_directory_path().string(),
                                                    filepath,
                                                    error));
  EXPECT_NE(error.find("UUID"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointGoldenDocumentIsStableAndReadable)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  checkpoint.scene_name = "Golden Scene";

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  constexpr char expected[] =
      "{\"format\":\"UPBGE_CXX_LOGIC_NODES_CHECKPOINT\","
      "\"scene_uuid\":\"123e4567-e89b-12d3-a456-426614174000\","
      "\"scene_name\":\"Golden Scene\",\"authored_objects\":[],"
      "\"spawned_instances\":[],\"shared_materials\":[],"
      "\"runtime_node_trees\":[],"
      "\"compositor_state\":{\"has_node_tree\":false},"
      "\"checksum\":\"96ca7dfcea7a4fbd960c9dfe411f7822\"}";
  EXPECT_EQ(contents, expected);

  Checkpoint decoded;
  ASSERT_EQ(decode_checkpoint(expected, decoded, error), LN_GameStateResult::Success) << error;
  EXPECT_EQ(decoded.scene_uuid, checkpoint.scene_uuid);
  EXPECT_EQ(decoded.scene_name, checkpoint.scene_name);
  EXPECT_TRUE(decoded.authored_objects.empty());
  EXPECT_TRUE(decoded.spawned_instances.empty());
  EXPECT_TRUE(decoded.shared_materials.empty());
  EXPECT_TRUE(decoded.runtime_node_trees.empty());
  EXPECT_FALSE(decoded.compositor.has_node_tree);

  std::string rewritten;
  ASSERT_EQ(encode_checkpoint(decoded, rewritten, error), LN_GameStateResult::Success) << error;
  EXPECT_EQ(rewritten, expected);
}

TEST(LN_GameState, CurrentCheckpointRoundTripsUnifiedObjectState)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  checkpoint.scene_name = "Unified State";

  ObjectState parent = StructuralObject("11111111-1111-4111-8111-111111111111", "Parent");
  parent.world_position = MT_Vector3(1.0f, 2.0f, 3.0f);

  ObjectState child = StructuralObject("22222222-2222-4222-8222-222222222222", "Child");
  child.world_position = MT_Vector3(4.0f, 5.0f, 6.0f);
  child.material_slots.push_back(MaterialSlotState{true, "Shared Material", std::string()});
  child.material_parameters.push_back(
      MaterialParameterState{"_LNMP_01234567_89abcdef", {0.125f, 0.25f, 0.5f, 1.0f}});
  child.logic_trees.push_back(LogicTreeAttachmentState{"Gameplay", "", 0, true});
  child.logic_trees.push_back(
      LogicTreeAttachmentState{"Damage Reactions", "//libraries/gameplay.blend", 3, false});
  child.non_geometry_modifiers.push_back(
      NonGeometryModifierState{"Armature", 8, 1001, 5});
  GeometryNodesModifierState geometry_nodes;
  geometry_nodes.name = "Runtime Appearance";
  geometry_nodes.persistent_uid = 1002;
  geometry_nodes.stack_index = 1;
  geometry_nodes.mode = 1;
  geometry_nodes.node_group_name = "Character Appearance";
  geometry_nodes.node_group_fingerprint = "gnv1-0123456789abcdef";
  GeometryNodesInputState density;
  density.socket_identifier = "Socket_2";
  density.mode = GeometryNodesInputMode::Value;
  density.value.socket_identifier = density.socket_identifier;
  density.value.type = MaterialSocketValueType::Float;
  density.value.float_values[0] = 0.75f;
  geometry_nodes.inputs.push_back(std::move(density));
  GeometryNodesInputState selection;
  selection.socket_identifier = "Socket_4";
  selection.mode = GeometryNodesInputMode::Attribute;
  selection.attribute_name = "saved_selection";
  geometry_nodes.inputs.push_back(std::move(selection));
  geometry_nodes.outputs.push_back(
      GeometryNodesOutputState{"Socket_7", "saved_weight"});
  child.geometry_nodes_modifiers.push_back(std::move(geometry_nodes));
  child.hierarchy.relation = HierarchyRelation::Normal;
  child.hierarchy.parent_id = parent.persistence_id;
  child.has_rigid_body_state = true;
  child.rigid_body.dynamic_body = true;
  child.rigid_body.mass = 3.25f;
  child.rigid_body.awake = true;
  child.rigid_body.rotation_enabled = true;
  child.rigid_body.ccd_enabled = true;
  child.rigid_body.dynamic_ccd_enabled = true;
  child.rigid_body.linear_velocity = MT_Vector3(1.0f, 2.0f, 3.0f);
  child.rigid_body.collision_group = 7;
  child.rigid_body.collision_mask = 0x55;
  PropertyState score;
  score.name = "score";
  score.type = PropertyType::Int;
  score.int_value = 42;
  child.properties.push_back(std::move(score));

  checkpoint.authored_objects.push_back(std::move(parent));
  checkpoint.authored_objects.push_back(std::move(child));

  SharedMaterialState material;
  material.material = MaterialSlotState{true, "Shared Material", std::string()};
  SharedMaterialNodeState node;
  node.node_identifier = 17;
  node.node_type = "ShaderNodeBsdfPrincipled";
  MaterialSocketValueState roughness;
  roughness.socket_identifier = "Roughness";
  roughness.type = MaterialSocketValueType::Float;
  roughness.float_values[0] = 0.375f;
  node.inputs.push_back(std::move(roughness));
  material.nodes.push_back(std::move(node));
  checkpoint.shared_materials.push_back(std::move(material));

  RuntimeNodeTreeState geometry_tree;
  geometry_tree.type = RuntimeNodeTreeType::Geometry;
  geometry_tree.name = "Character Appearance";
  geometry_tree.structural_fingerprint = "nt-0123456789abcdef";
  RuntimeNodeState geometry_node;
  geometry_node.node_identifier = 8;
  geometry_node.node_type = "GeometryNodeMeshIcoSphere";
  geometry_node.muted = true;
  MaterialSocketValueState radius;
  radius.socket_identifier = "Radius";
  radius.type = MaterialSocketValueType::Float;
  radius.float_values[0] = 0.2f;
  geometry_node.inputs.push_back(std::move(radius));
  geometry_tree.nodes.push_back(std::move(geometry_node));
  checkpoint.runtime_node_trees.push_back(std::move(geometry_tree));

  RuntimeNodeTreeState compositor_tree;
  compositor_tree.type = RuntimeNodeTreeType::Compositor;
  compositor_tree.name = "Runtime Compositor";
  compositor_tree.structural_fingerprint = "nt-fedcba9876543210";
  RuntimeNodeState compositor_node;
  compositor_node.node_identifier = 12;
  compositor_node.node_type = "CompositorNodeBlur";
  MaterialSocketValueState blur_size;
  blur_size.socket_identifier = "Size";
  blur_size.type = MaterialSocketValueType::Float;
  blur_size.float_values[0] = 4.0f;
  compositor_node.inputs.push_back(std::move(blur_size));
  compositor_tree.nodes.push_back(std::move(compositor_node));
  checkpoint.runtime_node_trees.push_back(std::move(compositor_tree));
  checkpoint.compositor.has_node_tree = true;
  checkpoint.compositor.node_tree_name = "Runtime Compositor";
  checkpoint.compositor.node_tree_structural_fingerprint = "nt-fedcba9876543210";

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  EXPECT_NE(contents.find("\"hierarchy\""), std::string::npos);
  EXPECT_NE(contents.find("\"physics\""), std::string::npos);
  EXPECT_NE(contents.find("\"logic_trees\""), std::string::npos);
  EXPECT_NE(contents.find("\"modifier_state\""), std::string::npos);
  EXPECT_NE(contents.find("\"runtime_node_trees\""), std::string::npos);
  EXPECT_NE(contents.find("\"compositor_state\""), std::string::npos);

  Checkpoint decoded;
  ASSERT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success) << error;
  ASSERT_EQ(decoded.authored_objects.size(), 2u);
  const ObjectState &decoded_child = decoded.authored_objects[1];
  EXPECT_EQ(decoded_child.hierarchy.relation, HierarchyRelation::Normal);
  EXPECT_EQ(decoded_child.hierarchy.parent_id,
            "11111111-1111-4111-8111-111111111111");
  EXPECT_TRUE(decoded_child.has_rigid_body_state);
  EXPECT_FLOAT_EQ(decoded_child.rigid_body.mass, 3.25f);
  EXPECT_EQ(decoded_child.rigid_body.collision_group, 7);
  EXPECT_EQ(decoded_child.rigid_body.collision_mask, 0x55);
  EXPECT_TRUE(decoded_child.rigid_body.ccd_enabled);
  EXPECT_TRUE(decoded_child.rigid_body.dynamic_ccd_enabled);
  ASSERT_EQ(decoded_child.logic_trees.size(), 2u);
  EXPECT_EQ(decoded_child.logic_trees[1].tree_name, "Damage Reactions");
  EXPECT_EQ(decoded_child.logic_trees[1].applied_tree_index, 3u);
  EXPECT_FALSE(decoded_child.logic_trees[1].enabled);
  ASSERT_EQ(decoded_child.non_geometry_modifiers.size(), 1u);
  EXPECT_EQ(decoded_child.non_geometry_modifiers[0].persistent_uid, 1001);
  ASSERT_EQ(decoded_child.geometry_nodes_modifiers.size(), 1u);
  const GeometryNodesModifierState &decoded_geometry_nodes =
      decoded_child.geometry_nodes_modifiers[0];
  EXPECT_EQ(decoded_geometry_nodes.node_group_name, "Character Appearance");
  ASSERT_EQ(decoded_geometry_nodes.inputs.size(), 2u);
  EXPECT_FLOAT_EQ(decoded_geometry_nodes.inputs[0].value.float_values[0], 0.75f);
  EXPECT_EQ(decoded_geometry_nodes.inputs[1].attribute_name, "saved_selection");
  ASSERT_EQ(decoded_geometry_nodes.outputs.size(), 1u);
  EXPECT_EQ(decoded_geometry_nodes.outputs[0].socket_identifier, "Socket_7");
  EXPECT_EQ(decoded_geometry_nodes.outputs[0].attribute_name, "saved_weight");
  ASSERT_EQ(decoded.shared_materials.size(), 1u);
  EXPECT_FLOAT_EQ(decoded.shared_materials[0].nodes[0].inputs[0].float_values[0], 0.375f);
  ASSERT_EQ(decoded.runtime_node_trees.size(), 2u);
  EXPECT_EQ(decoded.runtime_node_trees[0].type, RuntimeNodeTreeType::Geometry);
  ASSERT_EQ(decoded.runtime_node_trees[0].nodes.size(), 1u);
  EXPECT_TRUE(decoded.runtime_node_trees[0].nodes[0].muted);
  ASSERT_EQ(decoded.runtime_node_trees[0].nodes[0].inputs.size(), 1u);
  EXPECT_FLOAT_EQ(
      decoded.runtime_node_trees[0].nodes[0].inputs[0].float_values[0], 0.2f);
  EXPECT_EQ(decoded.runtime_node_trees[1].type, RuntimeNodeTreeType::Compositor);
  EXPECT_TRUE(decoded.compositor.has_node_tree);
  EXPECT_EQ(decoded.compositor.node_tree_name, "Runtime Compositor");

  std::string rewritten;
  ASSERT_EQ(encode_checkpoint(decoded, rewritten, error), LN_GameStateResult::Success) << error;
  EXPECT_EQ(rewritten, contents);
}

/* Set Collection Member's changes are part of Save Object State, in their order, for authored
 * objects and Add Object copies. Objects without changes write no "collections" field. */
TEST(LN_GameState, CurrentCheckpointRoundTripsCollectionMemberships)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  ObjectState marker = StructuralObject("11111111-1111-4111-8111-111111111111", "Marker");
  marker.collection_memberships.push_back({"Minimap", "", true});
  marker.collection_memberships.push_back({"Enemies", "//libraries/enemies.blend", false});
  ObjectState plain = StructuralObject("22222222-2222-4222-8222-222222222222", "Plain");
  ObjectState copy = StructuralObject("33333333-3333-4333-8333-333333333333", "Marker Copy");
  copy.spawned = true;
  copy.source_id = marker.persistence_id;
  copy.collection_memberships.push_back({"Minimap", "", false});
  checkpoint.authored_objects.push_back(std::move(marker));
  checkpoint.authored_objects.push_back(std::move(plain));
  checkpoint.spawned_instances.push_back(std::move(copy));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  size_t fields = 0;
  for (size_t at = contents.find("\"collections\""); at != std::string::npos;
       at = contents.find("\"collections\"", at + 1))
  {
    fields++;
  }
  EXPECT_EQ(fields, 2u);

  Checkpoint decoded;
  ASSERT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success) << error;
  ASSERT_EQ(decoded.authored_objects.size(), 2u);
  ASSERT_EQ(decoded.spawned_instances.size(), 1u);
  const std::vector<CollectionMembershipState> &saved =
      decoded.authored_objects[0].collection_memberships;
  ASSERT_EQ(saved.size(), 2u);
  EXPECT_EQ(saved[0].collection_name, "Minimap");
  EXPECT_EQ(saved[0].library_path, "");
  EXPECT_TRUE(saved[0].member);
  EXPECT_EQ(saved[1].collection_name, "Enemies");
  EXPECT_EQ(saved[1].library_path, "//libraries/enemies.blend");
  EXPECT_FALSE(saved[1].member);
  EXPECT_TRUE(decoded.authored_objects[1].collection_memberships.empty());
  ASSERT_EQ(decoded.spawned_instances[0].collection_memberships.size(), 1u);
  EXPECT_FALSE(decoded.spawned_instances[0].collection_memberships[0].member);

  std::string rewritten;
  ASSERT_EQ(encode_checkpoint(decoded, rewritten, error), LN_GameStateResult::Success) << error;
  EXPECT_EQ(rewritten, contents);
}

TEST(LN_GameState, CurrentCheckpointRejectsInvalidCollectionMemberships)
{
  using namespace upbge::logic_nodes::game_state;

  const auto decode_with = [](std::vector<CollectionMembershipState> memberships,
                              std::string &r_error) {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object = StructuralObject("11111111-1111-4111-8111-111111111111", "Marker");
    object.collection_memberships = std::move(memberships);
    checkpoint.authored_objects.push_back(std::move(object));
    std::string contents;
    if (encode_checkpoint(checkpoint, contents, r_error) != LN_GameStateResult::Success) {
      return LN_GameStateResult::InternalError;
    }
    Checkpoint decoded;
    return decode_checkpoint(std::move(contents), decoded, r_error);
  };

  std::string error;
  /* One entry per collection, as the object keeps them. */
  EXPECT_EQ(decode_with({{"Minimap", "", true}, {"Minimap", "", false}}, error),
            LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("twice"), std::string::npos) << error;
  /* The same name in another library is another collection. */
  EXPECT_EQ(decode_with({{"Minimap", "", true}, {"Minimap", "//lib.blend", false}}, error),
            LN_GameStateResult::Success)
      << error;
  EXPECT_EQ(decode_with({{"", "", true}}, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("invalid collection membership"), std::string::npos) << error;
  EXPECT_NE(decode_with({{std::string("Mini\0map", 8), "", true}}, error),
            LN_GameStateResult::Success);
  std::vector<CollectionMembershipState> too_many;
  for (size_t index = 0; index <= kMaxCollectionMembershipsPerObject; index++) {
    too_many.push_back({"Collection " + std::to_string(index), "", true});
  }
  EXPECT_EQ(decode_with(std::move(too_many), error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("oversized"), std::string::npos) << error;
}

TEST(LN_GameState, CurrentCheckpointRejectsAmbiguousRuntimeStructure)
{
  using namespace upbge::logic_nodes::game_state;

  auto decode_model = [](Checkpoint checkpoint, std::string &r_error) {
    std::string contents;
    if (encode_checkpoint(checkpoint, contents, r_error) != LN_GameStateResult::Success) {
      return LN_GameStateResult::InternalError;
    }
    Checkpoint decoded;
    return decode_checkpoint(std::move(contents), decoded, r_error);
  };

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object =
        StructuralObject("11111111-1111-4111-8111-111111111111", "Duplicate Trees");
    object.logic_trees.push_back(LogicTreeAttachmentState{"First", "", 2, true});
    object.logic_trees.push_back(LogicTreeAttachmentState{"Second", "", 2, true});
    checkpoint.authored_objects.push_back(std::move(object));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("duplicate C++ Logic Node"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object =
        StructuralObject("11111111-1111-4111-8111-111111111111", "Unordered Trees");
    object.logic_trees.push_back(LogicTreeAttachmentState{"Second", "", 2, true});
    object.logic_trees.push_back(LogicTreeAttachmentState{"First", "", 1, true});
    checkpoint.authored_objects.push_back(std::move(object));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("stable applied-index order"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object =
        StructuralObject("11111111-1111-4111-8111-111111111111", "Invalid Modifier Stack");
    object.non_geometry_modifiers.push_back(
        NonGeometryModifierState{"Subdivision", 1, 41, 1});
    GeometryNodesModifierState modifier;
    modifier.name = "Geometry";
    modifier.persistent_uid = 42;
    modifier.stack_index = 2;
    modifier.node_group_name = "Authored Geometry";
    modifier.node_group_fingerprint = "gnv1-0123456789abcdef";
    object.geometry_nodes_modifiers.push_back(std::move(modifier));
    checkpoint.authored_objects.push_back(std::move(object));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("invalid or duplicate Geometry Nodes modifier"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object =
        StructuralObject("11111111-1111-4111-8111-111111111111", "Invalid Modifier Mode");
    object.non_geometry_modifiers.push_back(
        NonGeometryModifierState{"Subdivision", 1, 41, uint32_t(1u << 30)});
    checkpoint.authored_objects.push_back(std::move(object));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("invalid or duplicate authored modifier"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    ObjectState object =
        StructuralObject("11111111-1111-4111-8111-111111111111", "Invalid Modifier Type");
    object.non_geometry_modifiers.push_back(
        NonGeometryModifierState{"Unknown", int32_t(blender::NUM_MODIFIER_TYPES), 41, 1});
    checkpoint.authored_objects.push_back(std::move(object));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("invalid or duplicate authored modifier"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    RuntimeNodeTreeState second;
    second.type = RuntimeNodeTreeType::Geometry;
    second.name = "Second";
    second.structural_fingerprint = "nt-2222222222222222";
    RuntimeNodeTreeState first;
    first.type = RuntimeNodeTreeType::Geometry;
    first.name = "First";
    first.structural_fingerprint = "nt-1111111111111111";
    checkpoint.runtime_node_trees.push_back(std::move(second));
    checkpoint.runtime_node_trees.push_back(std::move(first));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("not canonical"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    checkpoint.compositor.has_node_tree = true;
    checkpoint.compositor.node_tree_name = "Missing Compositor";
    checkpoint.compositor.node_tree_structural_fingerprint = "nt-3333333333333333";
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("no matching runtime node-tree state"), std::string::npos);
  }

  {
    Checkpoint checkpoint;
    checkpoint.scene_uuid = kSceneUuid;
    RuntimeNodeTreeState compositor;
    compositor.type = RuntimeNodeTreeType::Compositor;
    compositor.name = "Unreferenced Compositor";
    compositor.structural_fingerprint = "nt-4444444444444444";
    checkpoint.runtime_node_trees.push_back(std::move(compositor));
    std::string error;
    EXPECT_EQ(decode_model(std::move(checkpoint), error), LN_GameStateResult::CorruptData);
    EXPECT_NE(error.find("has no saved compositor root"), std::string::npos);
  }
}

/** A checkpoint whose one object holds \a property. */
upbge::logic_nodes::game_state::Checkpoint StructuredPropertyCheckpoint(
    upbge::logic_nodes::game_state::PropertyState property)
{
  using namespace upbge::logic_nodes::game_state;
  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  checkpoint.scene_name = "Properties";
  ObjectState object = StructuralObject("33333333-3333-4333-8333-333333333333", "Chest");
  object.properties.push_back(std::move(property));
  checkpoint.authored_objects.push_back(std::move(object));
  return checkpoint;
}

TEST(LN_GameState, CurrentCheckpointRoundTripsStructuredProperties)
{
  using namespace upbge::logic_nodes::game_state;
  using Kind = PropertyData::Kind;
  const auto plain = [](const Kind kind) {
    PropertyData data;
    data.kind = kind;
    return data;
  };

  /* A Table row: {count: 5, item: "Apple", owner: <saved object>}, then a named object. */
  PropertyData count = plain(Kind::Int);
  count.int_value = 5;
  PropertyData item = plain(Kind::String);
  item.text = "Apple";
  PropertyData owner = plain(Kind::Object);
  owner.text = "44444444-4444-4444-8444-444444444444";
  PropertyData row = plain(Kind::Dict);
  row.entries = {{"count", count}, {"item", item}, {"owner", owner}};
  PropertyData named = plain(Kind::Object);
  named.text = "Library Apple";
  named.object_by_name = true;
  PropertyData empty = plain(Kind::Object);
  PropertyData weight = plain(Kind::Float);
  weight.float_value = 2.5;
  PropertyData direction = plain(Kind::Vector3);
  direction.vector[0] = 1.0f;
  direction.vector[1] = -2.0f;
  direction.vector[2] = 0.5f;
  PropertyData flag = plain(Kind::Bool);
  flag.bool_value = true;

  PropertyState items;
  items.name = "items";
  items.type = PropertyType::List;
  items.data = plain(Kind::List);
  items.data.items = {row, named, empty, weight, direction, flag};

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(StructuredPropertyCheckpoint(items), contents, error),
            LN_GameStateResult::Success)
      << error;
  EXPECT_NE(contents.find("{\"object_name\":\"Library Apple\"}"), std::string::npos);
  EXPECT_NE(contents.find("{\"vector3\":[1.0,-2.0,0.5]}"), std::string::npos) << contents;

  Checkpoint decoded;
  ASSERT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success) << error;
  ASSERT_EQ(decoded.authored_objects.size(), 1u);
  ASSERT_EQ(decoded.authored_objects[0].properties.size(), 1u);
  const PropertyState &restored = decoded.authored_objects[0].properties[0];
  EXPECT_EQ(restored.type, PropertyType::List);
  ASSERT_EQ(restored.data.items.size(), 6u);
  const PropertyData &restored_row = restored.data.items[0];
  ASSERT_EQ(restored_row.kind, Kind::Dict);
  ASSERT_EQ(restored_row.entries.size(), 3u);
  EXPECT_EQ(restored_row.entries[0].second.int_value, 5);
  EXPECT_EQ(restored_row.entries[1].second.text, "Apple");
  EXPECT_EQ(restored_row.entries[2].second.kind, Kind::Object);
  EXPECT_EQ(restored_row.entries[2].second.text, owner.text);
  EXPECT_FALSE(restored_row.entries[2].second.object_by_name);
  EXPECT_TRUE(restored.data.items[1].object_by_name);
  EXPECT_EQ(restored.data.items[1].text, "Library Apple");
  EXPECT_EQ(restored.data.items[2].kind, Kind::Object);
  EXPECT_TRUE(restored.data.items[2].text.empty());
  EXPECT_EQ(restored.data.items[3].kind, Kind::Float);
  EXPECT_EQ(restored.data.items[3].float_value, 2.5);
  EXPECT_EQ(restored.data.items[4].kind, Kind::Vector3);
  EXPECT_EQ(restored.data.items[4].vector[1], -2.0f);
  EXPECT_EQ(restored.data.items[5].kind, Kind::Bool);

  std::string rewritten;
  ASSERT_EQ(encode_checkpoint(decoded, rewritten, error), LN_GameStateResult::Success) << error;
  EXPECT_EQ(rewritten, contents);
}

TEST(LN_GameState, CurrentCheckpointRejectsMalformedStructuredProperties)
{
  using namespace upbge::logic_nodes::game_state;

  /* A value that is not of the declared property type. */
  PropertyState mismatched;
  mismatched.name = "spawn";
  mismatched.type = PropertyType::Vector3;
  mismatched.data.kind = PropertyData::Kind::List;
  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(StructuredPropertyCheckpoint(mismatched), contents, error),
            LN_GameStateResult::Success)
      << error;
  Checkpoint decoded;
  EXPECT_NE(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success);
  EXPECT_NE(error.find("does not match"), std::string::npos) << error;

  /* Lists as deep as a save may hold load back; deeper ones are refused. */
  const auto nested_lists = [](const int depth) {
    PropertyState nested;
    nested.name = "nested";
    nested.type = PropertyType::List;
    nested.data.kind = PropertyData::Kind::List;
    PropertyData *level = &nested.data;
    for (int index = 0; index < depth; index++) {
      level->items.emplace_back();
      level = &level->items.back();
      level->kind = PropertyData::Kind::List;
    }
    return StructuredPropertyCheckpoint(nested);
  };
  ASSERT_EQ(encode_checkpoint(nested_lists(kMaxPropertyDataDepth - 2), contents, error),
            LN_GameStateResult::Success)
      << error;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success) << error;
  ASSERT_EQ(encode_checkpoint(nested_lists(30), contents, error), LN_GameStateResult::Success)
      << error;
  error.clear();
  EXPECT_NE(decode_checkpoint(contents, decoded, error), LN_GameStateResult::Success);
  /* The file reader's own nesting limit may refuse it first. */
  EXPECT_NE(error.find("dep"), std::string::npos) << error;
}

TEST(LN_GameState, CurrentCheckpointRejectsChecksumMismatch)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  checkpoint.scene_name = "Golden Scene";
  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  const size_t scene_name = contents.find("Golden Scene");
  ASSERT_NE(scene_name, std::string::npos);
  contents.replace(scene_name, std::string("Golden Scene").size(), "Broken Scene");

  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(std::move(contents), decoded, error),
            LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("checksum"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointRejectsIncompleteHierarchyGraph)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  ObjectState child = StructuralObject("22222222-2222-4222-8222-222222222222", "Orphan");
  child.hierarchy.relation = HierarchyRelation::Normal;
  child.hierarchy.parent_id = "11111111-1111-4111-8111-111111111111";
  checkpoint.authored_objects.push_back(std::move(child));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("parent"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointRejectsHierarchyCycle)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  ObjectState first = StructuralObject("11111111-1111-4111-8111-111111111111", "First");
  ObjectState second = StructuralObject("22222222-2222-4222-8222-222222222222", "Second");
  first.hierarchy.relation = HierarchyRelation::Normal;
  first.hierarchy.parent_id = second.persistence_id;
  second.hierarchy.relation = HierarchyRelation::Normal;
  second.hierarchy.parent_id = first.persistence_id;
  checkpoint.authored_objects.push_back(std::move(first));
  checkpoint.authored_objects.push_back(std::move(second));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("cycle"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointRejectsInvalidRigidBodyState)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  ObjectState object = StructuralObject("11111111-1111-4111-8111-111111111111",
                                        "Invalid Body");
  object.has_rigid_body_state = true;
  object.rigid_body.dynamic_body = true;
  object.rigid_body.mass = 0.0f;
  object.rigid_body.rotation_enabled = true;
  checkpoint.authored_objects.push_back(std::move(object));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("rigid-body"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointRejectsSingularObjectTransform)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  ObjectState object = StructuralObject("11111111-1111-4111-8111-111111111111",
                                        "Singular Object");
  object.world_scale = MT_Vector3(1.0f, 0.0f, 1.0f);
  checkpoint.authored_objects.push_back(std::move(object));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("singular"), std::string::npos);
}

TEST(LN_GameState, CurrentCheckpointRejectsUnreferencedSharedMaterial)
{
  using namespace upbge::logic_nodes::game_state;

  Checkpoint checkpoint;
  checkpoint.scene_uuid = kSceneUuid;
  SharedMaterialState material;
  material.material = MaterialSlotState{true, "Unreferenced", std::string()};
  checkpoint.shared_materials.push_back(std::move(material));

  std::string contents;
  std::string error;
  ASSERT_EQ(encode_checkpoint(checkpoint, contents, error), LN_GameStateResult::Success) << error;
  Checkpoint decoded;
  EXPECT_EQ(decode_checkpoint(contents, decoded, error), LN_GameStateResult::CorruptData);
  EXPECT_NE(error.find("shared material manifest"), std::string::npos);
}

TEST(LN_GameState, PersistenceCapabilityReportsEverySupportedBoundary)
{
  EXPECT_EQ(LN_ValidatePersistenceCandidate(SupportedCandidate()),
            LN_PersistenceCapabilityIssue::None);

#define EXPECT_BOOL_ISSUE(member, issue) \
  { \
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate(); \
    descriptor.member = false; \
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor), \
              LN_PersistenceCapabilityIssue::issue); \
  }
  EXPECT_BOOL_ISSUE(scene_identity_valid, MissingSceneIdentity);
  EXPECT_BOOL_ISSUE(object_identity_valid, MissingObjectIdentity);
  EXPECT_BOOL_ISSUE(runtime_class_supported, UnsupportedRuntimeClass);
  EXPECT_BOOL_ISSUE(physics_body_valid, MissingPhysicsBody);
  EXPECT_BOOL_ISSUE(reconstruction_state_valid, InvalidReconstructionState);
  EXPECT_BOOL_ISSUE(reconstruction_render_data_valid, InvalidRenderData);
  EXPECT_BOOL_ISSUE(reconstruction_properties_supported, UnsupportedReconstructionProperty);
#undef EXPECT_BOOL_ISSUE

#define EXPECT_TRUE_ISSUE(member, issue) \
  { \
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate(); \
    descriptor.member = true; \
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor), \
              LN_PersistenceCapabilityIssue::issue); \
  }
  EXPECT_TRUE_ISSUE(has_authored_parent, AuthoredParent);
  EXPECT_TRUE_ISSUE(has_runtime_parent, RuntimeParent);
  EXPECT_TRUE_ISSUE(has_authored_children, AuthoredChildren);
  EXPECT_TRUE_ISSUE(has_runtime_children, RuntimeChildren);
  EXPECT_TRUE_ISSUE(has_collection_instance, CollectionInstance);
  EXPECT_TRUE_ISSUE(has_constraints, ConstraintGraph);
  EXPECT_TRUE_ISSUE(has_python_runtime, PythonRuntime);
  EXPECT_TRUE_ISSUE(has_logic_bricks, LogicBricks);
  EXPECT_TRUE_ISSUE(is_compound, CompoundPhysics);
  EXPECT_TRUE_ISSUE(has_lod, LevelOfDetail);
  EXPECT_TRUE_ISSUE(has_obstacle, ObstacleSimulation);
  EXPECT_TRUE_ISSUE(has_scene_graph_controllers, SceneGraphController);
  EXPECT_TRUE_ISSUE(has_soft_body_pin_dependency, SoftBodyPinDependency);
#undef EXPECT_TRUE_ISSUE

  {
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate();
    descriptor.object_kind = LN_PersistenceObjectKind::Unsupported;
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor),
              LN_PersistenceCapabilityIssue::UnsupportedObjectKind);
  }
  {
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate();
    descriptor.runtime_role = LN_PersistenceRuntimeRole::Vehicle;
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor),
              LN_PersistenceCapabilityIssue::UnsupportedRuntimeRole);
  }
  {
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate();
    descriptor.physics_kind = LN_PersistencePhysicsKind::Unsupported;
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor),
              LN_PersistenceCapabilityIssue::UnsupportedPhysicsBackend);
  }
  {
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate();
    descriptor.object_kind = LN_PersistenceObjectKind::Empty;
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor),
              LN_PersistenceCapabilityIssue::PhysicsRequiresMesh);
  }
  {
    LN_PersistenceCandidateDescriptor descriptor = SupportedCandidate();
    descriptor.has_authored_parent = true;
    descriptor.has_runtime_parent = true;
    descriptor.has_authored_children = true;
    descriptor.has_runtime_children = true;
    LN_PersistenceCapabilityRequirements requirements;
    requirements.allow_normal_hierarchy = true;
    EXPECT_EQ(LN_ValidatePersistenceCandidate(descriptor, requirements),
              LN_PersistenceCapabilityIssue::None);
  }
}

TEST(LN_GameState, RuntimeMetadataUsesOneObjectStateFlag)
{
  KX_GameObject object;
  EXPECT_TRUE(blender::BLI_uuid_is_nil(object.GetPersistenceSourceUuid()));
  EXPECT_TRUE(blender::BLI_uuid_is_nil(object.GetPersistenceInstanceUuid()));
  EXPECT_FALSE(object.GetSaveGameObjectState());
  EXPECT_FALSE(object.HasSaveGameProperties());

  object.SetSaveGameObjectState(true);
  object.AddSaveGameProperty("score");
  object.AddSaveGameProperty("score");
  EXPECT_TRUE(object.GetSaveGameObjectState());
  EXPECT_TRUE(object.HasSaveGameProperties());
  EXPECT_TRUE(object.IsSaveGameProperty("score"));
  EXPECT_FALSE(object.IsSaveGameProperty("transient"));
  EXPECT_EQ(object.GetSaveGameProperties().size(), 1u);
}

TEST(LN_GameState, TimerPersistenceHooksExposeAndPreventDuplicateMembership)
{
  SCA_TimeEventManager manager(nullptr);
  EXP_FloatValue *timer = new EXP_FloatValue(0.0f);

  manager.AddTimeProperty(timer);
  manager.AddTimeProperty(timer);
  EXPECT_EQ(std::count(manager.PersistenceTimeProperties().begin(),
                       manager.PersistenceTimeProperties().end(),
                       timer),
            2);
  EXPECT_TRUE(manager.PersistenceDetachTimeProperty(timer));
  EXPECT_TRUE(manager.PersistenceDetachTimeProperty(timer));
  EXPECT_FALSE(manager.PersistenceDetachTimeProperty(timer));

  ASSERT_TRUE(manager.PersistenceReserveTimeProperties(1));
  EXPECT_TRUE(manager.PersistenceAttachPreparedTimeProperty(timer));
  EXPECT_FALSE(manager.PersistenceAttachPreparedTimeProperty(timer));
  EXPECT_TRUE(manager.PersistenceDetachTimeProperty(timer));
  timer->Release();
}
