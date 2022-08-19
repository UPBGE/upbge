/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2004 Blender Foundation. All rights reserved. */

#pragma once

/** \file
 * \ingroup sequencer
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "DNA_scene_types.h"

struct BlendDataReader;
struct BlendExpander;
struct BlendLibReader;
struct BlendWriter;
struct Depsgraph;
struct Editing;
struct Scene;
struct Sequence;
struct SequenceLookup;
struct SequencerToolSettings;

/* RNA enums, just to be more readable */
enum {
  SEQ_SIDE_MOUSE = -1,
  SEQ_SIDE_NONE = 0,
  SEQ_SIDE_LEFT,
  SEQ_SIDE_RIGHT,
  SEQ_SIDE_BOTH,
  SEQ_SIDE_NO_CHANGE,
};

/* seq_dupli' flags */
#define SEQ_DUPE_UNIQUE_NAME (1 << 0)
#define SEQ_DUPE_ALL (1 << 3) /* otherwise only selected are copied */
#define SEQ_DUPE_IS_RECURSIVE_CALL (1 << 4)

struct SequencerToolSettings *SEQ_tool_settings_init(void);
struct SequencerToolSettings *SEQ_tool_settings_ensure(struct Scene *scene);
void SEQ_tool_settings_free(struct SequencerToolSettings *tool_settings);
eSeqImageFitMethod SEQ_tool_settings_fit_method_get(struct Scene *scene);
void SEQ_tool_settings_fit_method_set(struct Scene *scene, eSeqImageFitMethod fit_method);
short SEQ_tool_settings_snap_flag_get(struct Scene *scene);
short SEQ_tool_settings_snap_mode_get(struct Scene *scene);
int SEQ_tool_settings_snap_distance_get(struct Scene *scene);
eSeqOverlapMode SEQ_tool_settings_overlap_mode_get(struct Scene *scene);
int SEQ_tool_settings_pivot_point_get(struct Scene *scene);
struct SequencerToolSettings *SEQ_tool_settings_copy(struct SequencerToolSettings *tool_settings);
struct Editing *SEQ_editing_get(const struct Scene *scene);
struct Editing *SEQ_editing_ensure(struct Scene *scene);
void SEQ_editing_free(struct Scene *scene, bool do_id_user);
/**
 * Get seqbase that is being viewed currently. This can be main seqbase or meta strip seqbase
 *
 * \param ed: sequence editor data
 * \return pointer to active seqbase. returns NULL if ed is NULL
 */
struct ListBase *SEQ_active_seqbase_get(const struct Editing *ed);
/**
 * Set seqbase that is being viewed currently. This can be main seqbase or meta strip seqbase
 *
 * \param ed: sequence editor data
 * \param seqbase: ListBase with strips
 */
void SEQ_seqbase_active_set(struct Editing *ed, struct ListBase *seqbase);
struct Sequence *SEQ_sequence_alloc(ListBase *lb, int timeline_frame, int machine, int type);
void SEQ_sequence_free(struct Scene *scene, struct Sequence *seq);
/**
 * Get #MetaStack that corresponds to current level that is being viewed
 *
 * \return pointer to meta stack
 */
struct MetaStack *SEQ_meta_stack_active_get(const struct Editing *ed);
/**
 * Open Meta strip content for editing.
 *
 * \param ed: sequence editor data
 * \param seqm: meta sequence or NULL for top level view
 */
void SEQ_meta_stack_set(const struct Scene *scene, struct Sequence *dst_seq);
/**
 * Close last Meta strip open for editing.
 *
 * \param ed: sequence editor data
 */
struct Sequence *SEQ_meta_stack_pop(struct Editing *ed);
struct Sequence *SEQ_sequence_dupli_recursive(const struct Scene *scene_src,
                                              struct Scene *scene_dst,
                                              struct ListBase *new_seq_list,
                                              struct Sequence *seq,
                                              int dupe_flag);
void SEQ_sequence_base_dupli_recursive(const struct Scene *scene_src,
                                       struct Scene *scene_dst,
                                       struct ListBase *nseqbase,
                                       const struct ListBase *seqbase,
                                       int dupe_flag,
                                       int flag);
bool SEQ_valid_strip_channel(struct Sequence *seq);

/**
 * Read and Write functions for `.blend` file data.
 */
void SEQ_blend_write(struct BlendWriter *writer, struct ListBase *seqbase);
void SEQ_blend_read(struct BlendDataReader *reader, struct ListBase *seqbase);

void SEQ_blend_read_lib(struct BlendLibReader *reader,
                        struct Scene *scene,
                        struct ListBase *seqbase);

void SEQ_blend_read_expand(struct BlendExpander *expander, struct ListBase *seqbase);

/* Depsgraph update function. */

/**
 * Evaluate parts of sequences which needs to be done as a part of a dependency graph evaluation.
 * This does NOT include actual rendering of the strips, but rather makes them up-to-date for
 * animation playback and makes them ready for the sequencer's rendering pipeline to render them.
 */
void SEQ_eval_sequences(struct Depsgraph *depsgraph,
                        struct Scene *scene,
                        struct ListBase *seqbase);

/* Defined in sequence_lookup.c */

typedef enum eSequenceLookupTag {
  SEQ_LOOKUP_TAG_INVALID = (1 << 0),
} eSequenceLookupTag;

/**
 * Find a sequence with a given name.
 * If lookup hash doesn't exist, it will be created. If hash is tagged as invalid, it will be
 * rebuilt.
 *
 * \param scene: scene that owns lookup hash
 * \param key: Sequence name without SQ prefix (seq->name + 2)
 *
 * \return pointer to Sequence
 */
struct Sequence *SEQ_sequence_lookup_seq_by_name(const struct Scene *scene, const char *key);

/**
 * Free lookup hash data.
 *
 * \param scene: scene that owns lookup hash
 */
void SEQ_sequence_lookup_free(const struct Scene *scene);
/**
 * Find a sequence with a given name.
 *
 * \param scene: scene that owns lookup hash
 * \param tag: tag to set
 */
void SEQ_sequence_lookup_tag(const struct Scene *scene, eSequenceLookupTag tag);

#ifdef __cplusplus
}
#endif
