/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_game_persistence.hh"

#include <cstring>

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_assert.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_uuid.h"

#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

using namespace blender;

namespace {

constexpr char kNamespaceProperty[] = "upbge";
constexpr char kPersistenceProperty[] = "game_persistence";
constexpr char kUuidProperty[] = "uuid";

IDProperty *group_ensure(IDProperty &parent, const char *name)
{
  if (IDProperty *property = IDP_GetPropertyFromGroup(&parent, name)) {
    return property->type == IDP_GROUP ? property : nullptr;
  }

  IDProperty *group = bke::idprop::create_group(name, IDP_FLAG_STATIC_TYPE).release();
  if (!IDP_AddToGroup(&parent, group)) {
    IDP_FreeProperty(group);
    return nullptr;
  }
  return group;
}

bool is_editable_local_identity(const ID &id)
{
  return id.lib == nullptr && id.override_library == nullptr;
}

bool identity_store(ID &id, const bUUID &uuid)
{
  if (BLI_uuid_is_nil(uuid)) {
    return false;
  }

  IDProperty *root = IDP_ID_system_properties_ensure(&id);
  if (root == nullptr || root->type != IDP_GROUP) {
    return false;
  }
  IDProperty *namespace_group = group_ensure(*root, kNamespaceProperty);
  IDProperty *persistence_group = namespace_group != nullptr ?
                                      group_ensure(*namespace_group, kPersistenceProperty) :
                                      nullptr;
  if (persistence_group == nullptr) {
    return false;
  }

  char text[UUID_STRING_SIZE];
  BLI_uuid_format(text, uuid);
  IDP_ReplaceInGroup(
      persistence_group, IDP_NewString(text, kUuidProperty, IDP_FLAG_STATIC_TYPE));
  return true;
}

bool identity_ensure(ID &id)
{
  bUUID uuid;
  switch (BKE_game_persistence_identity_read(id, uuid)) {
    case BKE_GamePersistenceIdentityState::Valid:
      return true;
    case BKE_GamePersistenceIdentityState::Missing:
      return identity_store(id, BLI_uuid_generate_random());
    case BKE_GamePersistenceIdentityState::Invalid:
      return false;
  }
  BLI_assert_unreachable();
  return false;
}

Scene *find_checkpoint_scene(Main &bmain, Scene *selected_scene, Object &object)
{
  if (selected_scene != nullptr && is_editable_local_identity(selected_scene->id) &&
      BKE_scene_has_object(bmain, selected_scene, &object))
  {
    return selected_scene;
  }

  for (Scene &scene : bmain.scenes) {
    if (is_editable_local_identity(scene.id) && BKE_scene_has_object(bmain, &scene, &object)) {
      return &scene;
    }
  }
  return nullptr;
}

}  // namespace

BKE_GamePersistenceIdentityState BKE_game_persistence_identity_read(const ID &id, bUUID &r_uuid)
{
  r_uuid = BLI_uuid_nil();
  const IDProperty *root = id.system_properties;
  if (root == nullptr) {
    return BKE_GamePersistenceIdentityState::Missing;
  }
  if (root->type != IDP_GROUP) {
    return BKE_GamePersistenceIdentityState::Invalid;
  }

  const IDProperty *namespace_property = IDP_GetPropertyFromGroup(root, kNamespaceProperty);
  if (namespace_property == nullptr) {
    return BKE_GamePersistenceIdentityState::Missing;
  }
  if (namespace_property->type != IDP_GROUP) {
    return BKE_GamePersistenceIdentityState::Invalid;
  }
  const IDProperty *persistence_property = IDP_GetPropertyFromGroup(namespace_property,
                                                                    kPersistenceProperty);
  if (persistence_property == nullptr) {
    return BKE_GamePersistenceIdentityState::Missing;
  }
  if (persistence_property->type != IDP_GROUP) {
    return BKE_GamePersistenceIdentityState::Invalid;
  }
  const IDProperty *uuid_property = IDP_GetPropertyFromGroup(persistence_property, kUuidProperty);
  if (uuid_property == nullptr) {
    return BKE_GamePersistenceIdentityState::Missing;
  }
  if (uuid_property->type != IDP_STRING || uuid_property->subtype != IDP_STRING_SUB_UTF8 ||
      uuid_property->data.pointer == nullptr || uuid_property->len != UUID_STRING_SIZE ||
      uuid_property->totallen < UUID_STRING_SIZE)
  {
    return BKE_GamePersistenceIdentityState::Invalid;
  }

  const char *text = IDP_string_get(uuid_property);
  if (text[UUID_STRING_SIZE - 1] != '\0' ||
      std::memchr(text, '\0', UUID_STRING_SIZE - 1) != nullptr ||
      !BLI_uuid_parse_string(&r_uuid, text) ||
      BLI_uuid_is_nil(r_uuid))
  {
    r_uuid = BLI_uuid_nil();
    return BKE_GamePersistenceIdentityState::Invalid;
  }
  char canonical[UUID_STRING_SIZE];
  BLI_uuid_format(canonical, r_uuid);
  if (std::strcmp(text, canonical) != 0) {
    r_uuid = BLI_uuid_nil();
    return BKE_GamePersistenceIdentityState::Invalid;
  }
  return BKE_GamePersistenceIdentityState::Valid;
}

bool BKE_game_persistence_identity_renew(ID &id)
{
  if (!is_editable_local_identity(id)) {
    return false;
  }
  return identity_store(id, BLI_uuid_generate_random());
}

bool BKE_game_persistence_identity_copy(const ID &source, ID &destination)
{
  bUUID uuid;
  if (BKE_game_persistence_identity_read(source, uuid) !=
      BKE_GamePersistenceIdentityState::Valid)
  {
    return false;
  }
  return identity_store(destination, uuid);
}

void BKE_game_persistence_identity_handle_copy(const ID &source,
                                               ID &destination,
                                               const int copy_flags)
{
  const bool is_internal_copy =
      (copy_flags &
       (LIB_ID_COPY_SET_COPIED_ON_WRITE | LIB_ID_CREATE_NO_MAIN | LIB_ID_CREATE_LOCAL)) != 0;
  /* Initial override creation and override refresh both use the local-data flag. The refresh path
   * copies from the linked reference into temporary local data, so library ownership alone cannot
   * distinguish it from authored duplication. */
  const bool is_override_transfer =
      (copy_flags & LIB_ID_COPY_NO_LIB_OVERRIDE_LOCAL_DATA_FLAG) != 0;
  const bool is_real_override = source.override_library != nullptr &&
                                destination.override_library != nullptr &&
                                (copy_flags & LIB_ID_COPY_NO_LIB_OVERRIDE) == 0;
  if (is_internal_copy || ID_IS_LINKED(&destination) || is_override_transfer || is_real_override) {
    return;
  }

  bUUID source_identity;
  if (BKE_game_persistence_identity_read(source, source_identity) ==
      BKE_GamePersistenceIdentityState::Valid)
  {
    BKE_game_persistence_identity_renew(destination);
  }
}

void BKE_game_persistence_identities_on_copybuffer_paste(Main &bmain)
{
  auto renew_new_local_identity = [](ID &id) {
    if ((id.tag & ID_TAG_PRE_EXISTING) != 0 || ID_IS_LINKED(&id)) {
      return;
    }
    bUUID uuid;
    if (BKE_game_persistence_identity_read(id, uuid) ==
        BKE_GamePersistenceIdentityState::Valid)
    {
      /* Clipboard paste creates a distinct authored ID but bypasses the regular Object/Scene copy
       * callbacks by round-tripping through a temporary blend file. */
      BKE_game_persistence_identity_renew(id);
    }
  };

  for (Object &object : bmain.objects) {
    renew_new_local_identity(object.id);
  }
  for (Scene &scene : bmain.scenes) {
    renew_new_local_identity(scene.id);
  }
}

bool BKE_game_persistence_ensure(Main &bmain, Scene *selected_scene, Object &object)
{
  if (!is_editable_local_identity(object.id)) {
    return false;
  }

  Scene *scene = find_checkpoint_scene(bmain, selected_scene, object);
  if (scene == nullptr) {
    return false;
  }

  bUUID scene_uuid;
  bUUID object_uuid;
  const BKE_GamePersistenceIdentityState scene_state =
      BKE_game_persistence_identity_read(scene->id, scene_uuid);
  const BKE_GamePersistenceIdentityState object_state =
      BKE_game_persistence_identity_read(object.id, object_uuid);
  if (scene_state == BKE_GamePersistenceIdentityState::Invalid ||
      object_state == BKE_GamePersistenceIdentityState::Invalid)
  {
    return false;
  }

  return identity_ensure(scene->id) && identity_ensure(object.id);
}
