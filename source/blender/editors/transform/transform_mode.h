/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edtransform
 * \brief transform modes used by different operators.
 */

#pragma once

struct AnimData;
struct LinkNode;
struct TransData;
struct TransDataContainer;
struct TransInfo;
struct bContext;
struct wmOperator;

/* header of TransDataEdgeSlideVert, TransDataEdgeSlideEdge */
typedef struct TransDataGenericSlideVert {
  struct BMVert *v;
  struct LinkNode **cd_loop_groups;
  float co_orig_3d[3];
} TransDataGenericSlideVert;

/* transform_mode.c */

eTfmMode transform_mode_really_used(struct bContext *C, eTfmMode mode);
bool transdata_check_local_center(const TransInfo *t, short around);
/**
 * Informs if the mode can be switched during modal.
 */
bool transform_mode_is_changeable(int mode);
void protectedTransBits(short protectflag, float vec[3]);
void protectedSizeBits(short protectflag, float size[3]);
void constraintTransLim(const TransInfo *t, TransData *td);
void constraintSizeLim(const TransInfo *t, TransData *td);
/**
 * Used by Transform Rotation and Transform Normal Rotation.
 */
void headerRotation(TransInfo *t, char *str, int str_size, float final);
/**
 * Applies values of rotation to `td->loc` and `td->ext->quat`
 * based on a rotation matrix (mat) and a pivot (center).
 *
 * Protected axis and other transform settings are taken into account.
 */
void ElementRotation_ex(const TransInfo *t,
                        const TransDataContainer *tc,
                        TransData *td,
                        const float mat[3][3],
                        const float *center);
void ElementRotation(const TransInfo *t,
                     const TransDataContainer *tc,
                     TransData *td,
                     const float mat[3][3],
                     short around);
void headerResize(TransInfo *t, const float vec[3], char *str, int str_size);
void ElementResize(const TransInfo *t,
                   const TransDataContainer *tc,
                   TransData *td,
                   const float mat[3][3]);
void transform_mode_init(TransInfo *t, struct wmOperator *op, int mode);
/**
 * When in modal and not set, initializes a default orientation for the mode.
 */
void transform_mode_default_modal_orientation_set(TransInfo *t, int type);

/* transform_mode_align.c */

void initAlign(TransInfo *t);

/* transform_mode_baketime.c */

void initBakeTime(TransInfo *t);

/* transform_mode_bbone_resize.c */

void initBoneSize(TransInfo *t);

/* transform_mode_bend.c */

void initBend(TransInfo *t);

/* transform_mode_boneenvelope.c */

void initBoneEnvelope(TransInfo *t);

/* transform_mode_boneroll.c */

void initBoneRoll(TransInfo *t);

/* transform_mode_curveshrinkfatten.c */

void initCurveShrinkFatten(TransInfo *t);

/* transform_mode_edge_bevelweight.c */

void initBevelWeight(TransInfo *t);

/* transform_mode_edge_crease.c */

void initEgdeCrease(TransInfo *t);
void initVertCrease(TransInfo *t);

/* transform_mode_edge_rotate_normal.c */

void initNormalRotation(TransInfo *t);

/* transform_mode_edge_seq_slide.c */

void initSeqSlide(TransInfo *t);

/* transform_mode_edge_slide.c */

void drawEdgeSlide(TransInfo *t);
void initEdgeSlide_ex(
    TransInfo *t, bool use_double_side, bool use_even, bool flipped, bool use_clamp);
void initEdgeSlide(TransInfo *t);

/* transform_mode_gpopacity.c */

void initGPOpacity(TransInfo *t);

/* transform_mode_gpshrinkfatten.c */

void initGPShrinkFatten(TransInfo *t);

/* transform_mode_maskshrinkfatten.c */

void initMaskShrinkFatten(TransInfo *t);

/* transform_mode_mirror.c */

void initMirror(TransInfo *t);

/* transform_mode_push_pull.c */

void initPushPull(TransInfo *t);

/* transform_mode_resize.c */

void initResize(TransInfo *t, float mouse_dir_constraint[3]);

/* transform_mode_rotate.c */

void initRotation(TransInfo *t);

/* transform_mode_shear.c */

void initShear(TransInfo *t);

/* transform_mode_shrink_fatten.c */

void initShrinkFatten(TransInfo *t);

/* transform_mode_skin_resize.c */

void initSkinResize(TransInfo *t);

/* transform_mode_tilt.c */

void initTilt(TransInfo *t);

/* transform_mode_timescale.c */

void initTimeScale(TransInfo *t);

/* transform_mode_timeslide.c */

void initTimeSlide(TransInfo *t);

/* transform_mode_timetranslate.c */

void initTimeTranslate(TransInfo *t);

/* transform_mode_tosphere.c */

void initToSphere(TransInfo *t);

/* transform_mode_trackball.c */

void initTrackball(TransInfo *t);

/* transform_mode_translate.c */

void initTranslation(TransInfo *t);

/* transform_mode_vert_slide.c */

void drawVertSlide(TransInfo *t);
void initVertSlide_ex(TransInfo *t, bool use_even, bool flipped, bool use_clamp);
void initVertSlide(TransInfo *t);
