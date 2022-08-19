/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup ikplugin
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Depsgraph;
struct Object;
struct Scene;
struct bPoseChannel;

struct IKPlugin {
  void (*initialize_tree_func)(struct Depsgraph *depsgraph,
                               struct Scene *scene,
                               struct Object *ob,
                               float ctime);
  void (*execute_tree_func)(struct Depsgraph *depsgraph,
                            struct Scene *scene,
                            struct Object *ob,
                            struct bPoseChannel *pchan,
                            float ctime);
  void (*release_tree_func)(struct Scene *scene, struct Object *ob, float ctime);
  void (*remove_armature_func)(struct bPose *pose);
  void (*clear_cache)(struct bPose *pose);
  void (*update_param)(struct bPose *pose);
  void (*test_constraint)(struct Object *ob, struct bConstraint *cons);
};

typedef struct IKPlugin IKPlugin;

#ifdef __cplusplus
}
#endif
