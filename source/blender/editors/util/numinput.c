/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include "MEM_guardedalloc.h"

#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_string_cursor_utf8.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_unit.h"

#include "DNA_scene_types.h"

#include "WM_api.h"
#include "WM_types.h"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.h"
#endif

#include "ED_numinput.h"
#include "UI_interface.h"

/* Numeric input which isn't allowing full numeric editing. */
#define USE_FAKE_EDIT

/**
 * #NumInput.flag
 * (1 << 8) and below are reserved for public flags!
 */
enum {
  /** Enable full editing, with units and math operators support. */
  NUM_EDIT_FULL = (1 << 9),
#ifdef USE_FAKE_EDIT
  /** Fake edited state (temp, avoids issue with backspace). */
  NUM_FAKE_EDITED = (1 << 10),
#endif
};

/* NumInput.val_flag[] */
enum {
  /* (1 << 8) and below are reserved for public flags! */
  /** User has edited this value somehow. */
  NUM_EDITED = (1 << 9),
  /** Current expression for this value is invalid. */
  NUM_INVALID = (1 << 10),
#ifdef USE_FAKE_EDIT
  /** Current expression's result has to be negated. */
  NUM_NEGATE = (1 << 11),
  /** Current expression's result has to be inverted. */
  NUM_INVERSE = (1 << 12),
#endif
};

/* ************************** Functions *************************** */

/* ************************** NUMINPUT **************************** */

void initNumInput(NumInput *n)
{
  n->idx_max = 0;
  n->unit_sys = USER_UNIT_NONE;
  copy_vn_i(n->unit_type, NUM_MAX_ELEMENTS, B_UNIT_NONE);
  n->unit_use_radians = false;

  n->flag = 0;
  copy_vn_short(n->val_flag, NUM_MAX_ELEMENTS, 0);
  zero_v3(n->val);
  copy_vn_fl(n->val_org, NUM_MAX_ELEMENTS, 0.0f);
  copy_vn_fl(n->val_inc, NUM_MAX_ELEMENTS, 1.0f);

  n->idx = 0;
  n->str[0] = '\0';
  n->str_cur = 0;
}

void outputNumInput(NumInput *n, char *str, UnitSettings *unit_settings)
{
  short j;
  const int ln = NUM_STR_REP_LEN;
  int prec = 2; /* draw-only, and avoids too much issues with radian->degrees conversion. */

  for (j = 0; j <= n->idx_max; j++) {
    /* if AFFECTALL and no number typed and cursor not on number, use first number */
    const short i = (n->flag & NUM_AFFECT_ALL && n->idx != j && !(n->val_flag[j] & NUM_EDITED)) ?
                        0 :
                        j;

    /* Use scale_length if needed! */
    const float fac = (float)BKE_scene_unit_scale(unit_settings, n->unit_type[j], 1.0);

    if (n->val_flag[i] & NUM_EDITED) {
      /* Get the best precision, allows us to draw '10.0001' as '10' instead! */
      prec = UI_calc_float_precision(prec, (double)n->val[i]);
      if (i == n->idx) {
        const char *heading_exp = "", *trailing_exp = "";
        char before_cursor[NUM_STR_REP_LEN];
        char val[16];

#ifdef USE_FAKE_EDIT
        if (n->val_flag[i] & NUM_NEGATE) {
          heading_exp = (n->val_flag[i] & NUM_INVERSE) ? "-1/(" : "-(";
          trailing_exp = ")";
        }
        else if (n->val_flag[i] & NUM_INVERSE) {
          heading_exp = "1/(";
          trailing_exp = ")";
        }
#endif

        if (n->val_flag[i] & NUM_INVALID) {
          BLI_strncpy(val, "Invalid", sizeof(val));
        }
        else {
          BKE_unit_value_as_string_adaptive(val,
                                            sizeof(val),
                                            (double)(n->val[i] * fac),
                                            prec,
                                            n->unit_sys,
                                            n->unit_type[i],
                                            true,
                                            false);
        }

        /* +1 because of trailing '\0' */
        BLI_strncpy(before_cursor, n->str, n->str_cur + 1);
        BLI_snprintf(&str[j * ln],
                     ln,
                     "[%s%s|%s%s] = %s",
                     heading_exp,
                     before_cursor,
                     &n->str[n->str_cur],
                     trailing_exp,
                     val);
      }
      else {
        const char *cur = (i == n->idx) ? "|" : "";
        if (n->unit_use_radians && n->unit_type[i] == B_UNIT_ROTATION) {
          /* Radian exception... */
          BLI_snprintf(&str[j * ln], ln, "%s%.6gr%s", cur, n->val[i], cur);
        }
        else {
          char tstr[NUM_STR_REP_LEN];
          BKE_unit_value_as_string_adaptive(
              tstr, ln, (double)n->val[i], prec, n->unit_sys, n->unit_type[i], true, false);
          BLI_snprintf(&str[j * ln], ln, "%s%s%s", cur, tstr, cur);
        }
      }
    }
    else {
      const char *cur = (i == n->idx) ? "|" : "";
      BLI_snprintf(&str[j * ln], ln, "%sNONE%s", cur, cur);
    }
    /* We might have cut some multi-bytes utf8 chars
     * (e.g. trailing '°' of degrees values can become only 'A')... */
    BLI_str_utf8_invalid_strip(&str[j * ln], strlen(&str[j * ln]));
  }
}

bool hasNumInput(const NumInput *n)
{
  short i;

#ifdef USE_FAKE_EDIT
  if (n->flag & NUM_FAKE_EDITED) {
    return true;
  }
#endif

  for (i = 0; i <= n->idx_max; i++) {
    if (n->val_flag[i] & NUM_EDITED) {
      return true;
    }
  }

  return false;
}

bool applyNumInput(NumInput *n, float *vec)
{
  short i, j;
  float val;

  if (hasNumInput(n)) {
    for (j = 0; j <= n->idx_max; j++) {
#ifdef USE_FAKE_EDIT
      if (n->flag & NUM_FAKE_EDITED) {
        val = n->val[j];
      }
      else
#endif
      {
        /* if AFFECTALL and no number typed and cursor not on number, use first number */
        i = (n->flag & NUM_AFFECT_ALL && n->idx != j && !(n->val_flag[j] & NUM_EDITED)) ? 0 : j;
        val = (!(n->val_flag[i] & NUM_EDITED) && n->val_flag[i] & NUM_NULL_ONE) ? 1.0f : n->val[i];

        if (n->val_flag[i] & NUM_NO_NEGATIVE && val < 0.0f) {
          val = 0.0f;
        }
        if (n->val_flag[i] & NUM_NO_FRACTION && val != floorf(val)) {
          val = floorf(val + 0.5f);
          if (n->val_flag[i] & NUM_NO_ZERO && val == 0.0f) {
            val = 1.0f;
          }
        }
        else if (n->val_flag[i] & NUM_NO_ZERO && val == 0.0f) {
          val = 0.0001f;
        }
      }
      vec[j] = val;
    }
#ifdef USE_FAKE_EDIT
    n->flag &= ~NUM_FAKE_EDITED;
#endif
    return true;
  }

  /* Else, we set the 'org' values for numinput! */
  for (j = 0; j <= n->idx_max; j++) {
    n->val[j] = n->val_org[j] = vec[j];
  }
  return false;
}

static void value_to_editstr(NumInput *n, int idx)
{
  const int prec = 6; /* editing, higher precision needed. */
  n->str_cur = BKE_unit_value_as_string_adaptive(n->str,
                                                 NUM_STR_REP_LEN,
                                                 (double)n->val[idx],
                                                 prec,
                                                 n->unit_sys,
                                                 n->unit_type[idx],
                                                 true,
                                                 false);
}

static bool editstr_insert_at_cursor(NumInput *n, const char *buf, const int buf_len)
{
  int cur = n->str_cur;
  int len = strlen(&n->str[cur]) + 1; /* +1 for the trailing '\0'. */
  int n_cur = cur + buf_len;

  if (n_cur + len >= NUM_STR_REP_LEN) {
    return false;
  }

  memmove(&n->str[n_cur], &n->str[cur], len);
  memcpy(&n->str[cur], buf, sizeof(char) * buf_len);

  n->str_cur = n_cur;
  return true;
}

bool user_string_to_number(bContext *C,
                           const char *str,
                           const UnitSettings *unit,
                           int type,
                           double *r_value,
                           const bool use_single_line_error,
                           char **r_error)
{
#ifdef WITH_PYTHON
  struct BPy_RunErrInfo err_info = {
      .use_single_line_error = use_single_line_error,
      .r_string = r_error,
  };
  double unit_scale = BKE_scene_unit_scale(unit, type, 1.0);
  if (BKE_unit_string_contains_unit(str, type)) {
    char str_unit_convert[256];
    BLI_strncpy(str_unit_convert, str, sizeof(str_unit_convert));
    BKE_unit_replace_string(
        str_unit_convert, sizeof(str_unit_convert), str, unit_scale, unit->system, type);

    return BPY_run_string_as_number(C, NULL, str_unit_convert, &err_info, r_value);
  }

  int success = BPY_run_string_as_number(C, NULL, str, &err_info, r_value);
  *r_value = BKE_unit_apply_preferred_unit(unit, type, *r_value);
  *r_value /= unit_scale;
  return success;

#else
  UNUSED_VARS(C, unit, type, use_single_line_error, r_error);
  *r_value = atof(str);
  return true;
#endif
}

static bool editstr_is_simple_numinput(const char ascii)
{
  if (ascii >= '0' && ascii <= '9') {
    return true;
  }
  if (ascii == '.') {
    return true;
  }
  return false;
}

bool handleNumInput(bContext *C, NumInput *n, const wmEvent *event)
{
  const char *utf8_buf = NULL;
  const char event_ascii = WM_event_utf8_to_ascii(event);
  char ascii[2] = {'\0', '\0'};
  bool updated = false;
  short idx = n->idx, idx_max = n->idx_max;
  short dir = STRCUR_DIR_NEXT, mode = STRCUR_JUMP_NONE;
  int cur;

#ifdef USE_FAKE_EDIT
  if (U.flag & USER_FLAG_NUMINPUT_ADVANCED)
#endif
  {
    if (((event->modifier & (KM_CTRL | KM_ALT)) == 0) && (event_ascii != '\0') &&
        strchr("01234567890@%^&*-+/{}()[]<>.|", event_ascii)) {
      if (!(n->flag & NUM_EDIT_FULL)) {
        n->flag |= NUM_EDITED;
        n->flag |= NUM_EDIT_FULL;
        n->val_flag[idx] |= NUM_EDITED;
      }
    }
  }

#ifdef USE_FAKE_EDIT
  /* XXX Hack around keyboards without direct access to '=' nor '*'... */
  if (ELEM(event_ascii, '=', '*')) {
    if (!(n->flag & NUM_EDIT_FULL)) {
      n->flag |= NUM_EDIT_FULL;
      n->val_flag[idx] |= NUM_EDITED;
      return true;
    }
    if (event->modifier & KM_CTRL) {
      n->flag &= ~NUM_EDIT_FULL;
      return true;
    }
  }
#endif

  switch (event->type) {
    case EVT_MODAL_MAP:
      if (ELEM(event->val, NUM_MODAL_INCREMENT_UP, NUM_MODAL_INCREMENT_DOWN)) {
        n->val[idx] += (event->val == NUM_MODAL_INCREMENT_UP) ? n->val_inc[idx] : -n->val_inc[idx];
        value_to_editstr(n, idx);
        n->val_flag[idx] |= NUM_EDITED;
        updated = true;
      }
      else {
        /* might be a char too... */
        utf8_buf = event->utf8_buf;
        ascii[0] = event_ascii;
      }
      break;
    case EVT_BACKSPACEKEY:
      /* Part specific to backspace... */
      if (!(n->val_flag[idx] & NUM_EDITED)) {
        copy_v3_v3(n->val, n->val_org);
        n->val_flag[0] &= ~NUM_EDITED;
        n->val_flag[1] &= ~NUM_EDITED;
        n->val_flag[2] &= ~NUM_EDITED;
#ifdef USE_FAKE_EDIT
        n->flag |= NUM_FAKE_EDITED;
#else
        n->flag |= NUM_EDIT_FULL;
#endif
        updated = true;
        break;
      }
      else if ((event->modifier & KM_SHIFT) || !n->str[0]) {
        n->val[idx] = n->val_org[idx];
        n->val_flag[idx] &= ~NUM_EDITED;
        n->str[0] = '\0';
        n->str_cur = 0;
        updated = true;
        break;
      }
      /* Else, common behavior with DELKEY,
       * only difference is remove char(s) before/after the cursor. */
      dir = STRCUR_DIR_PREV;
      ATTR_FALLTHROUGH;
    case EVT_DELKEY:
      if ((n->val_flag[idx] & NUM_EDITED) && n->str[0]) {
        int t_cur = cur = n->str_cur;
        if (event->modifier & KM_CTRL) {
          mode = STRCUR_JUMP_DELIM;
        }
        BLI_str_cursor_step_utf8(n->str, strlen(n->str), &t_cur, dir, mode, true);
        if (t_cur != cur) {
          if (t_cur < cur) {
            SWAP(int, t_cur, cur);
            n->str_cur = cur;
          }
          /* +1 for trailing '\0'. */
          memmove(&n->str[cur], &n->str[t_cur], strlen(&n->str[t_cur]) + 1);
          updated = true;
        }
        if (!n->str[0]) {
          n->val[idx] = n->val_org[idx];
        }
      }
      else {
        return false;
      }
      break;
    case EVT_LEFTARROWKEY:
      dir = STRCUR_DIR_PREV;
      ATTR_FALLTHROUGH;
    case EVT_RIGHTARROWKEY:
      cur = n->str_cur;
      if (event->modifier & KM_CTRL) {
        mode = STRCUR_JUMP_DELIM;
      }
      BLI_str_cursor_step_utf8(n->str, strlen(n->str), &cur, dir, mode, true);
      if (cur != n->str_cur) {
        n->str_cur = cur;
        return true;
      }
      return false;
    case EVT_HOMEKEY:
      if (n->str[0]) {
        n->str_cur = 0;
        return true;
      }
      return false;
    case EVT_ENDKEY:
      if (n->str[0]) {
        n->str_cur = strlen(n->str);
        return true;
      }
      return false;
    case EVT_TABKEY:
#ifdef USE_FAKE_EDIT
      n->val_flag[idx] &= ~(NUM_NEGATE | NUM_INVERSE);
#endif

      idx = (idx + idx_max + ((event->modifier & KM_CTRL) ? 0 : 2)) % (idx_max + 1);
      n->idx = idx;
      if (n->val_flag[idx] & NUM_EDITED) {
        value_to_editstr(n, idx);
      }
      else {
        n->str[0] = '\0';
        n->str_cur = 0;
      }
      return true;
    case EVT_PADPERIOD:
    case EVT_PERIODKEY:
      /* Force number-pad "." since some OS's/countries generate a comma char, see: T37992 */
      ascii[0] = '.';
      utf8_buf = ascii;
      break;
#if 0
    /* Those keys are not directly accessible in all layouts,
     * preventing to generate matching events.
     * So we use a hack (ascii value) instead, see below.
     */
    case EQUALKEY:
    case PADASTERKEY:
      if (!(n->flag & NUM_EDIT_FULL)) {
        n->flag |= NUM_EDIT_FULL;
        n->val_flag[idx] |= NUM_EDITED;
        return true;
      }
      else if (event->modifier & KM_CTRL) {
        n->flag &= ~NUM_EDIT_FULL;
        return true;
      }
      break;
#endif

#ifdef USE_FAKE_EDIT
    case EVT_PADMINUS:
    case EVT_MINUSKEY:
      if ((event->modifier & KM_CTRL) || !(n->flag & NUM_EDIT_FULL)) {
        n->val_flag[idx] ^= NUM_NEGATE;
        updated = true;
      }
      break;
    case EVT_PADSLASHKEY:
    case EVT_SLASHKEY:
      if ((event->modifier & KM_CTRL) || !(n->flag & NUM_EDIT_FULL)) {
        n->val_flag[idx] ^= NUM_INVERSE;
        updated = true;
      }
      break;
#endif
    case EVT_CKEY:
      if (event->modifier & KM_CTRL) {
        /* Copy current `str` to the copy/paste buffer. */
        WM_clipboard_text_set(n->str, 0);
        updated = true;
      }
      break;
    case EVT_VKEY:
      if (event->modifier & KM_CTRL) {
        /* extract the first line from the clipboard */
        int pbuf_len;
        char *pbuf = WM_clipboard_text_get_firstline(false, &pbuf_len);

        if (pbuf) {
          const bool success = editstr_insert_at_cursor(n, pbuf, pbuf_len);

          MEM_freeN(pbuf);
          if (!success) {
            return false;
          }

          n->val_flag[idx] |= NUM_EDITED;
        }
        updated = true;
      }
      break;
    default:
      break;
  }

  if (!updated && !utf8_buf && event->utf8_buf[0]) {
    utf8_buf = event->utf8_buf;
    ascii[0] = event_ascii;
  }

  /* Up to this point, if we have a ctrl modifier, skip.
   * This allows to still access most of modals' shortcuts even in numinput mode.
   */
  if (!updated && (event->modifier & KM_CTRL)) {
    return false;
  }

  if ((!utf8_buf || !utf8_buf[0]) && ascii[0]) {
    /* Fallback to ascii. */
    utf8_buf = ascii;
  }

  if (utf8_buf && utf8_buf[0]) {
    if (!(n->flag & NUM_EDIT_FULL)) {
      /* In simple edit mode, we only keep a few chars as valid! */
      /* no need to decode unicode, ascii is first char only */
      if (!editstr_is_simple_numinput(utf8_buf[0])) {
        return false;
      }
    }

    if (!editstr_insert_at_cursor(n, utf8_buf, BLI_str_utf8_size(utf8_buf))) {
      return false;
    }

    n->val_flag[idx] |= NUM_EDITED;
  }
  else if (!updated) {
    return false;
  }

  /* At this point, our value has changed, try to interpret it with python
   * (if str is not empty!). */
  if (n->str[0]) {
    const float val_prev = n->val[idx];
    Scene *sce = CTX_data_scene(C);
    char *error = NULL;

    double val;
    int success = user_string_to_number(
        C, n->str, &sce->unit, n->unit_type[idx], &val, false, &error);

    if (error) {
      ReportList *reports = CTX_wm_reports(C);
      printf("%s\n", error);
      BKE_report(reports, RPT_ERROR, error);
      BKE_report(reports, RPT_ERROR, IFACE_("Numeric input evaluation"));
      MEM_freeN(error);
    }

    if (success) {
      n->val[idx] = (float)val;
      n->val_flag[idx] &= ~NUM_INVALID;
    }
    else {
      n->val_flag[idx] |= NUM_INVALID;
    }

#ifdef USE_FAKE_EDIT
    if (n->val_flag[idx] & NUM_NEGATE) {
      n->val[idx] = -n->val[idx];
    }
    if (n->val_flag[idx] & NUM_INVERSE) {
      val = n->val[idx];
      /* If we invert on radians when user is in degrees,
       * you get unexpected results... See T53463. */
      if (!n->unit_use_radians && n->unit_type[idx] == B_UNIT_ROTATION) {
        val = RAD2DEG(val);
      }
      val = 1.0 / val;
      if (!n->unit_use_radians && n->unit_type[idx] == B_UNIT_ROTATION) {
        val = DEG2RAD(val);
      }
      n->val[idx] = (float)val;
    }
#endif

    if (UNLIKELY(!isfinite(n->val[idx]))) {
      n->val[idx] = val_prev;
      n->val_flag[idx] |= NUM_INVALID;
    }
  }

  /* REDRAW SINCE NUMBERS HAVE CHANGED */
  return true;
}
