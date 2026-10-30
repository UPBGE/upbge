/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_OverlayResource.cpp
 *  \ingroup ketsji
 */

#include "KX_OverlayResource.h"

#include <algorithm>

KX_OverlayResourceRegistry::Entry *KX_OverlayResourceRegistry::Find(
    blender::Collection *collection)
{
  if (collection == nullptr) {
    return nullptr;
  }
  const auto item = m_entries.find(collection);
  return item != m_entries.end() ? &item->second : nullptr;
}

const KX_OverlayResourceRegistry::Entry *KX_OverlayResourceRegistry::Find(
    const blender::Collection *collection) const
{
  if (collection == nullptr) {
    return nullptr;
  }
  const auto item = m_entries.find(const_cast<blender::Collection *>(collection));
  return item != m_entries.end() ? &item->second : nullptr;
}

bool KX_OverlayResourceRegistry::PresentationVisible(const Entry &entry) const
{
  if (entry.independent_enabled) {
    return true;
  }
  return std::any_of(entry.leases.begin(), entry.leases.end(), [](const Lease &lease) {
    return lease.visible;
  });
}

bool KX_OverlayResourceRegistry::Has(const blender::Collection *collection) const
{
  return Find(collection) != nullptr;
}

bool KX_OverlayResourceRegistry::IndependentEnabled(const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr && entry->independent_enabled;
}

bool KX_OverlayResourceRegistry::HasAnyLease(const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr && !entry->leases.empty();
}

bool KX_OverlayResourceRegistry::HasLease(const blender::Collection *collection,
                                          const MenuHandle handle) const
{
  const Entry *entry = Find(collection);
  if (entry == nullptr) {
    return false;
  }
  return std::any_of(
      entry->leases.begin(), entry->leases.end(), [handle](const Lease &lease) {
        return lease.handle == handle;
      });
}

bool KX_OverlayResourceRegistry::LeaseVisible(const blender::Collection *collection,
                                              const MenuHandle handle) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr &&
         std::any_of(entry->leases.begin(), entry->leases.end(), [handle](const Lease &lease) {
           return lease.handle == handle && lease.visible;
         });
}

bool KX_OverlayResourceRegistry::CreatorVisible(const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry == nullptr || entry->creator_visible;
}

bool KX_OverlayResourceRegistry::ShouldExist(const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr && (entry->independent_enabled || !entry->leases.empty());
}

bool KX_OverlayResourceRegistry::WouldExistWithoutLease(const blender::Collection *collection,
                                                        const MenuHandle handle) const
{
  const Entry *entry = Find(collection);
  if (entry == nullptr) {
    return false;
  }
  if (entry->independent_enabled) {
    return true;
  }
  return std::any_of(entry->leases.begin(), entry->leases.end(), [handle](const Lease &lease) {
    return lease.handle != handle;
  });
}

bool KX_OverlayResourceRegistry::ShouldRender(const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr && entry->creator_visible && PresentationVisible(*entry);
}

uint64_t KX_OverlayResourceRegistry::MembershipGeneration(
    const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr ? entry->membership_generation : 0;
}

uint64_t KX_OverlayResourceRegistry::InsertionSequence(
    const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr ? entry->insertion_sequence : 0;
}

bool KX_OverlayResourceRegistry::PresentationRequested(
    const blender::Collection *collection) const
{
  const Entry *entry = Find(collection);
  return entry != nullptr && PresentationVisible(*entry);
}

KX_OverlayCollectionResult KX_OverlayResourceRegistry::ValidateIndependentEnable(
    const blender::Collection *collection) const
{
  if (collection == nullptr) {
    return KX_OverlayCollectionResult::MissingCollection;
  }
  const Entry *entry = Find(collection);
  if (entry != nullptr && entry->independent_enabled) {
    return KX_OverlayCollectionResult::NoChange;
  }
  return KX_OverlayCollectionResult::Success;
}

KX_OverlayCollectionResult KX_OverlayResourceRegistry::ValidateIndependentDisable(
    const blender::Collection *collection) const
{
  if (collection == nullptr) {
    return KX_OverlayCollectionResult::MissingCollection;
  }
  const Entry *entry = Find(collection);
  if (entry == nullptr || !entry->independent_enabled) {
    return KX_OverlayCollectionResult::NoChange;
  }
  return KX_OverlayCollectionResult::Success;
}

void KX_OverlayResourceRegistry::SetEstablishedCamera(KX_Camera *camera)
{
  m_establishedCamera = camera;
}

void KX_OverlayResourceRegistry::Create(blender::Collection *collection)
{
  if (collection == nullptr || Has(collection)) {
    return;
  }
  Entry entry;
  entry.insertion_sequence = m_nextInsertionSequence++;
  m_entries.emplace(collection, entry);
  m_order.push_back(collection);
}

void KX_OverlayResourceRegistry::SetIndependentEnabled(blender::Collection *collection,
                                                       const bool enabled)
{
  Entry *entry = Find(collection);
  if (entry == nullptr) {
    return;
  }
  entry->independent_enabled = enabled;
}

void KX_OverlayResourceRegistry::AddLease(blender::Collection *collection,
                                          const MenuHandle handle,
                                          const bool visible)
{
  Entry *entry = Find(collection);
  if (entry == nullptr || handle == 0) {
    return;
  }
  for (Lease &lease : entry->leases) {
    if (lease.handle == handle) {
      lease.visible = visible;
      return;
    }
  }
  entry->leases.push_back(Lease{handle, visible});
}

void KX_OverlayResourceRegistry::RemoveLease(blender::Collection *collection,
                                             const MenuHandle handle)
{
  Entry *entry = Find(collection);
  if (entry == nullptr) {
    return;
  }
  entry->leases.erase(std::remove_if(entry->leases.begin(),
                                     entry->leases.end(),
                                     [handle](const Lease &lease) {
                                       return lease.handle == handle;
                                     }),
                      entry->leases.end());
}

void KX_OverlayResourceRegistry::SetLeaseVisible(blender::Collection *collection,
                                                 const MenuHandle handle,
                                                 const bool visible)
{
  Entry *entry = Find(collection);
  if (entry == nullptr) {
    return;
  }
  for (Lease &lease : entry->leases) {
    if (lease.handle == handle) {
      lease.visible = visible;
      return;
    }
  }
}

void KX_OverlayResourceRegistry::SetCreatorVisible(blender::Collection *collection,
                                                   const bool visible)
{
  Entry *entry = Find(collection);
  if (entry == nullptr) {
    return;
  }
  entry->creator_visible = visible;
}

void KX_OverlayResourceRegistry::NoteMembershipChanged(blender::Collection *collection)
{
  Entry *entry = Find(collection);
  if (entry == nullptr) {
    return;
  }
  entry->membership_generation++;
}

void KX_OverlayResourceRegistry::Erase(blender::Collection *collection)
{
  if (collection == nullptr) {
    return;
  }
  m_entries.erase(collection);
  m_order.erase(std::remove(m_order.begin(), m_order.end(), collection), m_order.end());
  if (m_entries.empty()) {
    m_establishedCamera = nullptr;
  }
}

void KX_OverlayResourceRegistry::Clear()
{
  m_entries.clear();
  m_order.clear();
  m_establishedCamera = nullptr;
}
