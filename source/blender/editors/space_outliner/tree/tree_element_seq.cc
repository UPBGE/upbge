/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_outliner_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"

#include "BLT_translation.h"

#include "../outliner_intern.hh"
#include "tree_element_seq.hh"

namespace blender::ed::outliner {

TreeElementSequence::TreeElementSequence(TreeElement &legacy_te, Sequence &sequence)
    : AbstractTreeElement(legacy_te), sequence_(sequence)
{
  BLI_assert(legacy_te.store_elem->type == TSE_SEQUENCE);

  /*
   * The idcode is a little hack, but the outliner
   * only check te->idcode if te->type is equal to zero,
   * so this is "safe".
   */
  legacy_te.idcode = sequence_.type;
  legacy_te.name = sequence_.name + 2;
}

bool TreeElementSequence::expandPoll(const SpaceOutliner & /*space_outliner*/) const
{
  return !(sequence_.type & SEQ_TYPE_EFFECT);
}

void TreeElementSequence::expand(SpaceOutliner &space_outliner) const
{
  /*
   * This work like the sequence.
   * If the sequence have a name (not default name)
   * show it, in other case put the filename.
   */

  if (sequence_.type == SEQ_TYPE_META) {
    LISTBASE_FOREACH (Sequence *, child, &sequence_.seqbase) {
      outliner_add_element(
          &space_outliner, &legacy_te_.subtree, child, &legacy_te_, TSE_SEQUENCE, 0);
    }
  }
  else {
    outliner_add_element(
        &space_outliner, &legacy_te_.subtree, sequence_.strip, &legacy_te_, TSE_SEQ_STRIP, 0);
  }
}

Sequence &TreeElementSequence::getSequence() const
{
  return sequence_;
}

/* -------------------------------------------------------------------- */
/* Strip */

TreeElementSequenceStrip::TreeElementSequenceStrip(TreeElement &legacy_te, Strip &strip)
    : AbstractTreeElement(legacy_te)
{
  BLI_assert(legacy_te.store_elem->type == TSE_SEQ_STRIP);

  if (strip.dir[0] != '\0') {
    legacy_te_.name = strip.dir;
  }
  else {
    legacy_te_.name = IFACE_("Strip None");
  }
}

/* -------------------------------------------------------------------- */
/* Strip Duplicate */

TreeElementSequenceStripDuplicate::TreeElementSequenceStripDuplicate(TreeElement &legacy_te,
                                                                     Sequence &sequence)
    : AbstractTreeElement(legacy_te), sequence_(sequence)
{
  BLI_assert(legacy_te.store_elem->type == TSE_SEQUENCE_DUP);

  legacy_te_.idcode = sequence.type;
  legacy_te_.name = sequence.strip->stripdata->name;
}

Sequence &TreeElementSequenceStripDuplicate::getSequence() const
{
  return sequence_;
}

}  // namespace blender::ed::outliner
