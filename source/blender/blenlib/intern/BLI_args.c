/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bli
 * \brief A general argument parsing module
 */

#include <ctype.h> /* for tolower */
#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_args.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

static char NO_DOCS[] = "NO DOCUMENTATION SPECIFIED";

struct bArgDoc;
typedef struct bArgDoc {
  struct bArgDoc *next, *prev;
  const char *short_arg;
  const char *long_arg;
  const char *documentation;
  bool done;
} bArgDoc;

typedef struct bAKey {
  const char *arg;
  uintptr_t pass; /* cast easier */
  int case_str;   /* case specific or not */
} bAKey;

typedef struct bArgument {
  bAKey *key;
  BA_ArgCallback func;
  void *data;
  bArgDoc *doc;
} bArgument;

struct bArgs {
  ListBase docs;
  GHash *items;
  int argc;
  const char **argv;
  int *passes;

  /* Only use when initializing arguments. */
  int current_pass;
};

static uint case_strhash(const void *ptr)
{
  const char *s = ptr;
  uint i = 0;
  unsigned char c;

  while ((c = tolower(*s++))) {
    i = i * 37 + c;
  }

  return i;
}

static uint keyhash(const void *ptr)
{
  const bAKey *k = ptr;
  return case_strhash(k->arg); /* ^ BLI_ghashutil_inthash((void *)k->pass); */
}

static bool keycmp(const void *a, const void *b)
{
  const bAKey *ka = a;
  const bAKey *kb = b;
  if (ka->pass == kb->pass || ka->pass == -1 || kb->pass == -1) { /* -1 is wildcard for pass */
    if (ka->case_str == 1 || kb->case_str == 1) {
      return (BLI_strcasecmp(ka->arg, kb->arg) != 0);
    }
    return (!STREQ(ka->arg, kb->arg));
  }
  return BLI_ghashutil_intcmp((const void *)ka->pass, (const void *)kb->pass);
}

static bArgument *lookUp(struct bArgs *ba, const char *arg, int pass, int case_str)
{
  bAKey key;

  key.case_str = case_str;
  key.pass = pass;
  key.arg = arg;

  return BLI_ghash_lookup(ba->items, &key);
}

bArgs *BLI_args_create(int argc, const char **argv)
{
  bArgs *ba = MEM_callocN(sizeof(bArgs), "bArgs");
  ba->passes = MEM_callocN(sizeof(int) * argc, "bArgs passes");
  ba->items = BLI_ghash_new(keyhash, keycmp, "bArgs passes gh");
  BLI_listbase_clear(&ba->docs);
  ba->argc = argc;
  ba->argv = argv;

  /* Must be initialized by #BLI_args_pass_set. */
  ba->current_pass = 0;

  return ba;
}

void BLI_args_destroy(struct bArgs *ba)
{
  BLI_ghash_free(ba->items, MEM_freeN, MEM_freeN);
  MEM_freeN(ba->passes);
  BLI_freelistN(&ba->docs);
  MEM_freeN(ba);
}

void BLI_args_pass_set(struct bArgs *ba, int current_pass)
{
  BLI_assert((current_pass != 0) && (current_pass >= -1));
  ba->current_pass = current_pass;
}

void BLI_args_print(struct bArgs *ba)
{
  int i;
  for (i = 0; i < ba->argc; i++) {
    printf("argv[%d] = %s\n", i, ba->argv[i]);
  }
}

static bArgDoc *internalDocs(struct bArgs *ba,
                             const char *short_arg,
                             const char *long_arg,
                             const char *doc)
{
  bArgDoc *d;

  d = MEM_callocN(sizeof(bArgDoc), "bArgDoc");

  if (doc == NULL) {
    doc = NO_DOCS;
  }

  d->short_arg = short_arg;
  d->long_arg = long_arg;
  d->documentation = doc;

  BLI_addtail(&ba->docs, d);

  return d;
}

static void internalAdd(
    struct bArgs *ba, const char *arg, int case_str, BA_ArgCallback cb, void *data, bArgDoc *d)
{
  const int pass = ba->current_pass;
  bArgument *a;
  bAKey *key;

  a = lookUp(ba, arg, pass, case_str);

  if (a) {
    printf("WARNING: conflicting argument\n");
    printf("\ttrying to add '%s' on pass %i, %scase sensitive\n",
           arg,
           pass,
           case_str == 1 ? "not " : "");
    printf("\tconflict with '%s' on pass %i, %scase sensitive\n\n",
           a->key->arg,
           (int)a->key->pass,
           a->key->case_str == 1 ? "not " : "");
  }

  a = MEM_callocN(sizeof(bArgument), "bArgument");
  key = MEM_callocN(sizeof(bAKey), "bAKey");

  key->arg = arg;
  key->pass = pass;
  key->case_str = case_str;

  a->key = key;
  a->func = cb;
  a->data = data;
  a->doc = d;

  BLI_ghash_insert(ba->items, key, a);
}

void BLI_args_add_case(struct bArgs *ba,
                       const char *short_arg,
                       int short_case,
                       const char *long_arg,
                       int long_case,
                       const char *doc,
                       BA_ArgCallback cb,
                       void *data)
{
  bArgDoc *d = internalDocs(ba, short_arg, long_arg, doc);

  if (short_arg) {
    internalAdd(ba, short_arg, short_case, cb, data, d);
  }

  if (long_arg) {
    internalAdd(ba, long_arg, long_case, cb, data, d);
  }
}

void BLI_args_add(struct bArgs *ba,
                  const char *short_arg,
                  const char *long_arg,
                  const char *doc,
                  BA_ArgCallback cb,
                  void *data)
{
  BLI_args_add_case(ba, short_arg, 0, long_arg, 0, doc, cb, data);
}

static void internalDocPrint(bArgDoc *d)
{
  if (d->short_arg && d->long_arg) {
    printf("%s or %s", d->short_arg, d->long_arg);
  }
  else if (d->short_arg) {
    printf("%s", d->short_arg);
  }
  else if (d->long_arg) {
    printf("%s", d->long_arg);
  }

  printf(" %s\n\n", d->documentation);
}

void BLI_args_print_arg_doc(struct bArgs *ba, const char *arg)
{
  bArgument *a = lookUp(ba, arg, -1, -1);

  if (a) {
    bArgDoc *d = a->doc;

    internalDocPrint(d);

    d->done = true;
  }
}

void BLI_args_print_other_doc(struct bArgs *ba)
{
  bArgDoc *d;

  for (d = ba->docs.first; d; d = d->next) {
    if (d->done == 0) {
      internalDocPrint(d);
    }
  }
}

bool BLI_args_has_other_doc(const struct bArgs *ba)
{
  for (const bArgDoc *d = ba->docs.first; d; d = d->next) {
    if (d->done == 0) {
      return true;
    }
  }
  return false;
}

void BLI_args_parse(struct bArgs *ba, int pass, BA_ArgCallback default_cb, void *default_data)
{
  BLI_assert((pass != 0) && (pass >= -1));
  int i = 0;

  for (i = 1; i < ba->argc; i++) { /* skip argv[0] */
    if (ba->passes[i] == 0) {
      /* -1 signal what side of the comparison it is */
      bArgument *a = lookUp(ba, ba->argv[i], pass, -1);
      BA_ArgCallback func = NULL;
      void *data = NULL;

      if (a) {
        func = a->func;
        data = a->data;
      }
      else {
        func = default_cb;
        data = default_data;
      }

      if (func) {
        int retval = func(ba->argc - i, ba->argv + i, data);

        if (retval >= 0) {
          int j;

          /* use extra arguments */
          for (j = 0; j <= retval; j++) {
            ba->passes[i + j] = pass;
          }
          i += retval;
        }
        else if (retval == -1) {
          if (a) {
            if (a->key->pass != -1) {
              ba->passes[i] = pass;
            }
          }
          break;
        }
      }
    }
  }
}
