/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_game_persistence.hh"

#include "BKE_collection.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "BLI_uuid.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "testing/testing.h"

namespace blender::bke::tests {

class GamePersistenceIdentityTest : public BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  Object *object = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    scene = BKE_scene_add(bmain, "Persistence Scene");
    object = BKE_object_add_only_object(bmain, OB_EMPTY, "Persistent Object");
    BKE_collection_object_add(bmain, scene->master_collection, object);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(GamePersistenceIdentityTest, ExplicitAuthoringCreatesStableNamespacedIdentities)
{
  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(scene->id, uuid),
            BKE_GamePersistenceIdentityState::Missing);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));
  EXPECT_EQ(BKE_game_persistence_identity_read(object->id, uuid),
            BKE_GamePersistenceIdentityState::Missing);

  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID scene_uuid;
  bUUID object_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(scene->id, scene_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, object_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_is_nil(scene_uuid));
  EXPECT_FALSE(BLI_uuid_is_nil(object_uuid));
  EXPECT_FALSE(BLI_uuid_equal(scene_uuid, object_uuid));

  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID repeated_scene_uuid;
  bUUID repeated_object_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(scene->id, repeated_scene_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, repeated_object_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_TRUE(BLI_uuid_equal(scene_uuid, repeated_scene_uuid));
  EXPECT_TRUE(BLI_uuid_equal(object_uuid, repeated_object_uuid));

  const IDProperty *upbge = IDP_GetPropertyTypeFromGroup(
      object->id.system_properties, "upbge", IDP_GROUP);
  ASSERT_NE(upbge, nullptr);
  EXPECT_NE(upbge->flag & IDP_FLAG_STATIC_TYPE, 0);
  const IDProperty *persistence = IDP_GetPropertyTypeFromGroup(
      upbge, "game_persistence", IDP_GROUP);
  ASSERT_NE(persistence, nullptr);
  EXPECT_NE(persistence->flag & IDP_FLAG_STATIC_TYPE, 0);
  const IDProperty *stored_uuid = IDP_GetPropertyTypeFromGroup(persistence, "uuid", IDP_STRING);
  ASSERT_NE(stored_uuid, nullptr);
  EXPECT_EQ(stored_uuid->len, UUID_STRING_SIZE);
  EXPECT_NE(stored_uuid->flag & IDP_FLAG_STATIC_TYPE, 0);
}

TEST_F(GamePersistenceIdentityTest, AuthoredCopiesRenewAndInternalCopiesPreserveIdentity)
{
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID source_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, source_uuid),
            BKE_GamePersistenceIdentityState::Valid);

  ID *authored_id = BKE_id_copy_ex(bmain, &object->id, nullptr, 0);
  ASSERT_NE(authored_id, nullptr);
  bUUID authored_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(*authored_id, authored_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_equal(source_uuid, authored_uuid));

  ID *internal_id = BKE_id_copy_ex(
      nullptr, &object->id, nullptr, LIB_ID_CREATE_NO_MAIN | LIB_ID_CREATE_NO_USER_REFCOUNT);
  ASSERT_NE(internal_id, nullptr);
  bUUID internal_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(*internal_id, internal_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_TRUE(BLI_uuid_equal(source_uuid, internal_uuid));

  ASSERT_TRUE(BKE_game_persistence_identity_copy(object->id, *authored_id));
  ASSERT_EQ(BKE_game_persistence_identity_read(*authored_id, authored_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_TRUE(BLI_uuid_equal(source_uuid, authored_uuid));

  BKE_id_free(nullptr, internal_id);
}

TEST_F(GamePersistenceIdentityTest, NonPersistentCopiesRemainUnallocated)
{
  ASSERT_EQ(object->id.system_properties, nullptr);
  ID *copy_id = BKE_id_copy_ex(bmain, &object->id, nullptr, 0);
  ASSERT_NE(copy_id, nullptr);
  EXPECT_EQ(copy_id->system_properties, nullptr);

  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(*copy_id, uuid),
            BKE_GamePersistenceIdentityState::Missing);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));
}

TEST_F(GamePersistenceIdentityTest, SceneCopiesReceiveDistinctCheckpointNamespaces)
{
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID source_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(scene->id, source_uuid),
            BKE_GamePersistenceIdentityState::Valid);

  ID *copy_id = BKE_id_copy_ex(bmain, &scene->id, nullptr, 0);
  ASSERT_NE(copy_id, nullptr);
  bUUID copy_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(*copy_id, copy_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_equal(source_uuid, copy_uuid));
}

TEST_F(GamePersistenceIdentityTest, EmptySceneCopiesReceiveDistinctCheckpointNamespaces)
{
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID source_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(scene->id, source_uuid),
            BKE_GamePersistenceIdentityState::Valid);

  Scene *copy = BKE_scene_duplicate(bmain, scene, SCE_COPY_EMPTY, eDupli_ID_Flags(0), 0);
  ASSERT_NE(copy, nullptr);
  bUUID copy_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(copy->id, copy_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_equal(source_uuid, copy_uuid));
}

TEST_F(GamePersistenceIdentityTest, LibraryOverrideTransfersPreserveIdentity)
{
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID source_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, source_uuid),
            BKE_GamePersistenceIdentityState::Valid);

  ID *transfer_id = BKE_id_copy_ex(
      bmain, &object->id, nullptr, LIB_ID_COPY_NO_LIB_OVERRIDE_LOCAL_DATA_FLAG);
  ASSERT_NE(transfer_id, nullptr);
  bUUID transfer_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(*transfer_id, transfer_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_TRUE(BLI_uuid_equal(source_uuid, transfer_uuid));
}

TEST_F(GamePersistenceIdentityTest, ClipboardPasteRenewsOnlyNewPersistentIdentities)
{
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID source_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, source_uuid),
            BKE_GamePersistenceIdentityState::Valid);

  ID *pasted_id = BKE_id_copy_ex(bmain, &object->id, nullptr, 0);
  ASSERT_NE(pasted_id, nullptr);
  /* Simulate the clipboard blend-file round-trip, which preserves system properties but bypasses
   * Object's regular copy callback while appending the pasted ID. */
  ASSERT_TRUE(BKE_game_persistence_identity_copy(object->id, *pasted_id));
  object->id.tag |= ID_TAG_PRE_EXISTING;
  scene->id.tag |= ID_TAG_PRE_EXISTING;

  Object *unmarked = BKE_object_add_only_object(bmain, OB_EMPTY, "Unmarked Paste");
  ASSERT_EQ(unmarked->id.system_properties, nullptr);
  BKE_game_persistence_identities_on_copybuffer_paste(*bmain);

  bUUID pasted_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(*pasted_id, pasted_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_equal(source_uuid, pasted_uuid));
  bUUID unchanged_uuid;
  ASSERT_EQ(BKE_game_persistence_identity_read(object->id, unchanged_uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_TRUE(BLI_uuid_equal(source_uuid, unchanged_uuid));
  EXPECT_EQ(unmarked->id.system_properties, nullptr);
}

TEST_F(GamePersistenceIdentityTest, AuthoringUsesTheLocalOwningScene)
{
  Scene *unrelated_scene = BKE_scene_add(bmain, "Unrelated Scene");
  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, unrelated_scene, *object));

  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(scene->id, uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_EQ(BKE_game_persistence_identity_read(unrelated_scene->id, uuid),
            BKE_GamePersistenceIdentityState::Missing);
}

TEST_F(GamePersistenceIdentityTest, MalformedMetadataFailsClosed)
{
  IDProperty *root = IDP_ID_system_properties_ensure(&object->id);
  ASSERT_TRUE(IDP_AddToGroup(root, bke::idprop::create("upbge", 17).release()));

  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(object->id, uuid),
            BKE_GamePersistenceIdentityState::Invalid);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));
  EXPECT_FALSE(BKE_game_persistence_ensure(*bmain, scene, *object));

  EXPECT_EQ(BKE_game_persistence_identity_read(scene->id, uuid),
            BKE_GamePersistenceIdentityState::Missing);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));

  const IDProperty *upbge = IDP_GetPropertyFromGroup(object->id.system_properties, "upbge");
  ASSERT_NE(upbge, nullptr);
  EXPECT_EQ(upbge->type, IDP_INT);
  EXPECT_EQ(IDP_int_get(upbge), 17);
}

TEST_F(GamePersistenceIdentityTest, UserPropertiesCannotShadowSystemIdentity)
{
  IDProperty *user_properties = IDP_EnsureProperties(&object->id);
  ASSERT_TRUE(IDP_AddToGroup(user_properties, bke::idprop::create("upbge", 23).release()));

  ASSERT_TRUE(BKE_game_persistence_ensure(*bmain, scene, *object));
  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(object->id, uuid),
            BKE_GamePersistenceIdentityState::Valid);
  EXPECT_FALSE(BLI_uuid_is_nil(uuid));

  const IDProperty *user_upbge = IDP_GetPropertyFromGroup(object->id.properties, "upbge");
  ASSERT_NE(user_upbge, nullptr);
  EXPECT_EQ(user_upbge->type, IDP_INT);
  EXPECT_EQ(IDP_int_get(user_upbge), 23);
}

TEST_F(GamePersistenceIdentityTest, NonCanonicalIdentityFailsClosed)
{
  IDProperty *root = IDP_ID_system_properties_ensure(&object->id);
  IDProperty *upbge = bke::idprop::create_group("upbge").release();
  IDProperty *persistence = bke::idprop::create_group("game_persistence").release();
  ASSERT_TRUE(IDP_AddToGroup(
      persistence,
      IDP_NewString("123E4567-E89B-12D3-A456-426614174000", "uuid", IDP_FLAG_STATIC_TYPE)));
  ASSERT_TRUE(IDP_AddToGroup(upbge, persistence));
  ASSERT_TRUE(IDP_AddToGroup(root, upbge));

  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(object->id, uuid),
            BKE_GamePersistenceIdentityState::Invalid);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));
  EXPECT_FALSE(BKE_game_persistence_ensure(*bmain, scene, *object));
}

TEST_F(GamePersistenceIdentityTest, LinkedObjectsCannotAcquireAuthoredIdentity)
{
  Library *library = BKE_id_new<Library>(bmain, "Persistence Library");
  Object *linked_object = BKE_id_new_in_lib<Object>(bmain, library, "Linked Object");
  EXPECT_FALSE(BKE_game_persistence_ensure(*bmain, scene, *linked_object));
  EXPECT_FALSE(BKE_game_persistence_identity_renew(linked_object->id));

  bUUID uuid;
  EXPECT_EQ(BKE_game_persistence_identity_read(linked_object->id, uuid),
            BKE_GamePersistenceIdentityState::Missing);
  EXPECT_TRUE(BLI_uuid_is_nil(uuid));
}

}  // namespace blender::bke::tests
