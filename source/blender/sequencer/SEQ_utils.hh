/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "DNA_scene_types.h"

struct bSound;
struct ListBase;
struct Mask;
struct Scene;
struct Strip;
struct StripElem;

namespace blender::seq {

void sequence_base_unique_name_recursive(Scene *scene, ListBase *seqbasep, Strip *strip);
const char *sequence_give_name(const Strip *strip);
ListBase *get_seqbase_from_sequence(Strip *strip, ListBase **r_channels, int *r_offset);
const Strip *get_topmost_sequence(const Scene *scene, int frame);
/**
 * In cases where we don't know the sequence's listbase.
 */
ListBase *get_seqbase_by_seq(const Scene *scene, Strip *strip);
/**
 * Only use as last resort when the StripElem is available but no the Sequence.
 * (needed for RNA)
 */
Strip *sequence_from_strip_elem(ListBase *seqbase, StripElem *se);
Strip *get_sequence_by_name(ListBase *seqbase, const char *name, bool recursive);
Mask *active_mask_get(Scene *scene);
void alpha_mode_from_file_extension(Strip *strip);

/**
 * Check if an input referenced by this strip is valid (e.g. scene for a scene strip).
 * Note that this only checks data block references, for missing media referenced
 * by paths use #media_presence_is_missing.
 */
bool sequence_has_valid_data(const Strip *strip);

void set_scale_to_fit(const Strip *strip,
                      int image_width,
                      int image_height,
                      int preview_width,
                      int preview_height,
                      eSeqImageFitMethod fit_method);
/**
 * Ensure, that provided Sequence has unique name. If animation data exists for this Sequence, it
 * will be duplicated and mapped onto new name
 *
 * \param seq: Sequence which name will be ensured to be unique
 * \param scene: Scene in which name must be unique
 */
void ensure_unique_name(Strip *strip, Scene *scene);

void fontmap_clear();

/**
 * Check whether a sequence strip has missing media.
 * Results of the query for this strip will be cached into #MediaPresence cache. The cache
 * will be created on demand.
 *
 * \param scene: Scene to query.
 * \param seq: Sequencer strip.
 * \return True if media file is missing.
 */
bool media_presence_is_missing(Scene *scene, const Strip *strip);

/**
 * Set or change the missing media cache value for a given strip.
 */
void media_presence_set_missing(Scene *scene, const Strip *strip, bool missing);

/**
 * Invalidate media presence cache for the given strip.
 */
void media_presence_invalidate_strip(Scene *scene, const Strip *strip);

/**
 * Invalidate media presence cache for the given sound.
 */
void media_presence_invalidate_sound(Scene *scene, const bSound *sound);

/**
 * Free media presence cache, if it was created.
 */
void media_presence_free(Scene *scene);

}  // namespace blender::seq
