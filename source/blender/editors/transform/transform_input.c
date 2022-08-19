/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <math.h>
#include <stdlib.h>

#include "DNA_screen_types.h"

#include "BKE_context.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "WM_api.h"
#include "WM_types.h"

#include "transform.h"

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Callbacks for #MouseInput.apply
 * \{ */

/** Callback for #INPUT_VECTOR */
static void InputVector(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  convertViewVec(t, output, mval[0] - mi->imval[0], mval[1] - mi->imval[1]);
}

/** Callback for #INPUT_SPRING */
static void InputSpring(TransInfo *UNUSED(t),
                        MouseInput *mi,
                        const double mval[2],
                        float output[3])
{
  double dx, dy;
  float ratio;

  dx = ((double)mi->center[0] - mval[0]);
  dy = ((double)mi->center[1] - mval[1]);
  ratio = hypot(dx, dy) / (double)mi->factor;

  output[0] = ratio;
}

/** Callback for #INPUT_SPRING_FLIP */
static void InputSpringFlip(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  InputSpring(t, mi, mval, output);

  /* flip scale */
  /* values can become really big when zoomed in so use longs T26598. */
  if (((int64_t)((int)mi->center[0] - mval[0]) * (int64_t)((int)mi->center[0] - mi->imval[0]) +
       (int64_t)((int)mi->center[1] - mval[1]) * (int64_t)((int)mi->center[1] - mi->imval[1])) <
      0) {
    output[0] *= -1.0f;
  }
}

/** Callback for #INPUT_SPRING_DELTA */
static void InputSpringDelta(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  InputSpring(t, mi, mval, output);
  output[0] -= 1.0f;
}

/** Callback for #INPUT_TRACKBALL */
static void InputTrackBall(TransInfo *UNUSED(t),
                           MouseInput *mi,
                           const double mval[2],
                           float output[3])
{
  output[0] = (float)(mi->imval[1] - mval[1]);
  output[1] = (float)(mval[0] - mi->imval[0]);

  output[0] *= mi->factor;
  output[1] *= mi->factor;
}

/** Callback for #INPUT_HORIZONTAL_RATIO */
static void InputHorizontalRatio(TransInfo *t,
                                 MouseInput *mi,
                                 const double mval[2],
                                 float output[3])
{
  const int winx = t->region ? t->region->winx : 1;

  output[0] = ((mval[0] - mi->imval[0]) / winx) * 2.0f;
}

/** Callback for #INPUT_HORIZONTAL_ABSOLUTE */
static void InputHorizontalAbsolute(TransInfo *t,
                                    MouseInput *mi,
                                    const double mval[2],
                                    float output[3])
{
  float vec[3];

  InputVector(t, mi, mval, vec);
  project_v3_v3v3(vec, vec, t->viewinv[0]);

  output[0] = dot_v3v3(t->viewinv[0], vec) * 2.0f;
}

static void InputVerticalRatio(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  const int winy = t->region ? t->region->winy : 1;

  /* Dragging up increases (matching viewport zoom). */
  output[0] = ((mval[1] - mi->imval[1]) / winy) * 2.0f;
}

/** Callback for #INPUT_VERTICAL_ABSOLUTE */
static void InputVerticalAbsolute(TransInfo *t,
                                  MouseInput *mi,
                                  const double mval[2],
                                  float output[3])
{
  float vec[3];

  InputVector(t, mi, mval, vec);
  project_v3_v3v3(vec, vec, t->viewinv[1]);

  /* Dragging up increases (matching viewport zoom). */
  output[0] = dot_v3v3(t->viewinv[1], vec) * 2.0f;
}

/** Callback for #INPUT_CUSTOM_RATIO_FLIP */
static void InputCustomRatioFlip(TransInfo *UNUSED(t),
                                 MouseInput *mi,
                                 const double mval[2],
                                 float output[3])
{
  double length;
  double distance;
  double dx, dy;
  const int *data = mi->data;

  if (data) {
    int mdx, mdy;
    dx = data[2] - data[0];
    dy = data[3] - data[1];

    length = hypot(dx, dy);

    mdx = mval[0] - data[2];
    mdy = mval[1] - data[3];

    distance = (length != 0.0) ? (mdx * dx + mdy * dy) / length : 0.0;

    output[0] = (length != 0.0) ? (double)(distance / length) : 0.0;
  }
}

/** Callback for #INPUT_CUSTOM_RATIO */
static void InputCustomRatio(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  InputCustomRatioFlip(t, mi, mval, output);
  output[0] = -output[0];
}

struct InputAngle_Data {
  double angle;
  double mval_prev[2];
};

/** Callback for #INPUT_ANGLE */
static void InputAngle(TransInfo *UNUSED(t), MouseInput *mi, const double mval[2], float output[3])
{
  struct InputAngle_Data *data = mi->data;
  float dir_prev[2], dir_curr[2], mi_center[2];
  copy_v2_v2(mi_center, mi->center);

  sub_v2_v2v2(dir_prev, (const float[2]){UNPACK2(data->mval_prev)}, mi_center);
  sub_v2_v2v2(dir_curr, (const float[2]){UNPACK2(mval)}, mi_center);

  if (normalize_v2(dir_prev) && normalize_v2(dir_curr)) {
    float dphi = angle_normalized_v2v2(dir_prev, dir_curr);

    if (cross_v2v2(dir_prev, dir_curr) > 0.0f) {
      dphi = -dphi;
    }

    data->angle += ((double)dphi) * (mi->precision ? (double)mi->precision_factor : 1.0);

    data->mval_prev[0] = mval[0];
    data->mval_prev[1] = mval[1];
  }

  output[0] = data->angle;
}

static void InputAngleSpring(TransInfo *t, MouseInput *mi, const double mval[2], float output[3])
{
  float toutput[3];

  InputAngle(t, mi, mval, output);
  InputSpring(t, mi, mval, toutput);

  output[1] = toutput[0];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom 2D Start/End Coordinate API
 *
 * - #INPUT_CUSTOM_RATIO
 * - #INPUT_CUSTOM_RATIO_FLIP
 * \{ */

void setCustomPoints(TransInfo *UNUSED(t),
                     MouseInput *mi,
                     const int mval_start[2],
                     const int mval_end[2])
{
  int *data;

  mi->data = MEM_reallocN(mi->data, sizeof(int[4]));

  data = mi->data;

  data[0] = mval_start[0];
  data[1] = mval_start[1];
  data[2] = mval_end[0];
  data[3] = mval_end[1];
}

void setCustomPointsFromDirection(TransInfo *t, MouseInput *mi, const float dir[2])
{
  BLI_ASSERT_UNIT_V2(dir);
  const int win_axis = t->region ? ((abs((int)(t->region->winx * dir[0])) +
                                     abs((int)(t->region->winy * dir[1]))) /
                                    2) :
                                   1;
  const int mval_start[2] = {
      mi->imval[0] + dir[0] * win_axis,
      mi->imval[1] + dir[1] * win_axis,
  };
  const int mval_end[2] = {mi->imval[0], mi->imval[1]};
  setCustomPoints(t, mi, mval_start, mval_end);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Setup & Handle Mouse Input
 * \{ */

void initMouseInput(TransInfo *UNUSED(t),
                    MouseInput *mi,
                    const float center[2],
                    const int mval[2],
                    const bool precision)
{
  mi->factor = 0;
  mi->precision = precision;

  mi->center[0] = center[0];
  mi->center[1] = center[1];

  mi->imval[0] = mval[0];
  mi->imval[1] = mval[1];

  mi->post = NULL;
}

static void calcSpringFactor(MouseInput *mi)
{
  mi->factor = sqrtf(
      ((float)(mi->center[1] - mi->imval[1])) * ((float)(mi->center[1] - mi->imval[1])) +
      ((float)(mi->center[0] - mi->imval[0])) * ((float)(mi->center[0] - mi->imval[0])));

  if (mi->factor == 0.0f) {
    mi->factor = 1.0f; /* prevent Inf */
  }
}

void initMouseInputMode(TransInfo *t, MouseInput *mi, MouseInputMode mode)
{
  /* In case we allocate a new value. */
  void *mi_data_prev = mi->data;

  mi->use_virtual_mval = true;
  mi->precision_factor = 1.0f / 10.0f;

  switch (mode) {
    case INPUT_VECTOR:
      mi->apply = InputVector;
      t->helpline = HLP_NONE;
      break;
    case INPUT_SPRING:
      calcSpringFactor(mi);
      mi->apply = InputSpring;
      t->helpline = HLP_SPRING;
      break;
    case INPUT_SPRING_FLIP:
      calcSpringFactor(mi);
      mi->apply = InputSpringFlip;
      t->helpline = HLP_SPRING;
      break;
    case INPUT_SPRING_DELTA:
      calcSpringFactor(mi);
      mi->apply = InputSpringDelta;
      t->helpline = HLP_SPRING;
      break;
    case INPUT_ANGLE:
    case INPUT_ANGLE_SPRING: {
      struct InputAngle_Data *data;
      mi->use_virtual_mval = false;
      mi->precision_factor = 1.0f / 30.0f;
      data = MEM_callocN(sizeof(struct InputAngle_Data), "angle accumulator");
      data->mval_prev[0] = mi->imval[0];
      data->mval_prev[1] = mi->imval[1];
      mi->data = data;
      if (mode == INPUT_ANGLE) {
        mi->apply = InputAngle;
      }
      else {
        calcSpringFactor(mi);
        mi->apply = InputAngleSpring;
      }
      t->helpline = HLP_ANGLE;
      break;
    }
    case INPUT_TRACKBALL:
      mi->precision_factor = 1.0f / 30.0f;
      /* factor has to become setting or so */
      mi->factor = 0.01f;
      mi->apply = InputTrackBall;
      t->helpline = HLP_TRACKBALL;
      break;
    case INPUT_HORIZONTAL_RATIO:
      mi->apply = InputHorizontalRatio;
      t->helpline = HLP_HARROW;
      break;
    case INPUT_HORIZONTAL_ABSOLUTE:
      mi->apply = InputHorizontalAbsolute;
      t->helpline = HLP_HARROW;
      break;
    case INPUT_VERTICAL_RATIO:
      mi->apply = InputVerticalRatio;
      t->helpline = HLP_VARROW;
      break;
    case INPUT_VERTICAL_ABSOLUTE:
      mi->apply = InputVerticalAbsolute;
      t->helpline = HLP_VARROW;
      break;
    case INPUT_CUSTOM_RATIO:
      mi->apply = InputCustomRatio;
      t->helpline = HLP_CARROW;
      break;
    case INPUT_CUSTOM_RATIO_FLIP:
      mi->apply = InputCustomRatioFlip;
      t->helpline = HLP_CARROW;
      break;
    case INPUT_NONE:
    default:
      mi->apply = NULL;
      break;
  }

  /* setup for the mouse cursor: either set a custom one,
   * or hide it if it will be drawn with the helpline */
  wmWindow *win = CTX_wm_window(t->context);
  switch (t->helpline) {
    case HLP_NONE:
      /* INPUT_VECTOR, INPUT_CUSTOM_RATIO, INPUT_CUSTOM_RATIO_FLIP */
      if (t->flag & T_MODAL) {
        t->flag |= T_MODAL_CURSOR_SET;
        WM_cursor_modal_set(win, WM_CURSOR_NSEW_SCROLL);
      }
      break;
    case HLP_SPRING:
    case HLP_ANGLE:
    case HLP_TRACKBALL:
    case HLP_HARROW:
    case HLP_VARROW:
    case HLP_CARROW:
      if (t->flag & T_MODAL) {
        t->flag |= T_MODAL_CURSOR_SET;
        WM_cursor_modal_set(win, WM_CURSOR_NONE);
      }
      break;
    default:
      break;
  }

  /* if we've allocated new data, free the old data
   * less hassle than checking before every alloc above */
  if (mi_data_prev && (mi_data_prev != mi->data)) {
    MEM_freeN(mi_data_prev);
  }
}

void setInputPostFct(MouseInput *mi, void (*post)(struct TransInfo *t, float values[3]))
{
  mi->post = post;
}

void applyMouseInput(TransInfo *t, MouseInput *mi, const int mval[2], float output[3])
{
  double mval_db[2];

  if (mi->use_virtual_mval) {
    /* update accumulator */
    double mval_delta[2];

    mval_delta[0] = (mval[0] - mi->imval[0]) - mi->virtual_mval.prev[0];
    mval_delta[1] = (mval[1] - mi->imval[1]) - mi->virtual_mval.prev[1];

    mi->virtual_mval.prev[0] += mval_delta[0];
    mi->virtual_mval.prev[1] += mval_delta[1];

    if (mi->precision) {
      mval_delta[0] *= (double)mi->precision_factor;
      mval_delta[1] *= (double)mi->precision_factor;
    }

    mi->virtual_mval.accum[0] += mval_delta[0];
    mi->virtual_mval.accum[1] += mval_delta[1];

    mval_db[0] = mi->imval[0] + mi->virtual_mval.accum[0];
    mval_db[1] = mi->imval[1] + mi->virtual_mval.accum[1];
  }
  else {
    mval_db[0] = mval[0];
    mval_db[1] = mval[1];
  }

  if (mi->apply != NULL) {
    mi->apply(t, mi, mval_db, output);
  }

  if (mi->post) {
    mi->post(t, output);
  }
}

/** \} */
