/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

#include "dupli_parent_finder.hh"

#include "BLI_utildefines.h"

#include <iostream>

namespace blender::io {

void DupliParentFinder::insert(const DupliObject *dupli_ob)
{
  dupli_set_.insert(dupli_ob->ob);

  PersistentID dupli_pid(dupli_ob);
  pid_to_dupli_[dupli_pid] = dupli_ob;
  instancer_pid_to_duplis_[dupli_pid.instancer_pid()].insert(dupli_ob);
}

bool DupliParentFinder::is_duplicated(const Object *object) const
{
  return dupli_set_.find(object) != dupli_set_.end();
}

const DupliObject *DupliParentFinder::find_suitable_export_parent(
    const DupliObject *dupli_ob) const
{
  if (dupli_ob->ob->parent != nullptr) {
    const DupliObject *parent = find_duplicated_parent(dupli_ob);
    if (parent != nullptr) {
      return parent;
    }
  }

  return find_instancer(dupli_ob);
}

const DupliObject *DupliParentFinder::find_duplicated_parent(const DupliObject *dupli_ob) const
{
  const PersistentID dupli_pid(dupli_ob);
  PersistentID parent_pid = dupli_pid.instancer_pid();

  const Object *parent_ob = dupli_ob->ob->parent;
  BLI_assert(parent_ob != nullptr);

  InstancerPIDToDuplisMap::const_iterator found = instancer_pid_to_duplis_.find(parent_pid);
  if (found == instancer_pid_to_duplis_.end()) {
    /* Unexpected, as there should be at least one entry here, for the dupli_ob itself. */
    return nullptr;
  }

  for (const DupliObject *potential_parent_dupli : found->second) {
    if (potential_parent_dupli->ob != parent_ob) {
      continue;
    }

    PersistentID potential_parent_pid(potential_parent_dupli);
    if (potential_parent_pid.is_from_same_instancer_as(dupli_pid)) {
      return potential_parent_dupli;
    }
  }
  return nullptr;
}

const DupliObject *DupliParentFinder::find_instancer(const DupliObject *dupli_ob) const
{
  PersistentID dupli_pid(dupli_ob);
  PersistentID parent_pid = dupli_pid.instancer_pid();

  PIDToDupliMap::const_iterator found = pid_to_dupli_.find(parent_pid);
  if (found == pid_to_dupli_.end()) {
    return nullptr;
  }

  const DupliObject *instancer_dupli = found->second;
  return instancer_dupli;
}

}  // namespace blender::io
