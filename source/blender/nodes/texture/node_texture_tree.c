/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup nodes
 */

#include <string.h>

#include "DNA_node_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"

#include "BLI_listbase.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_linestyle.h"
#include "BKE_node.h"
#include "BKE_paint.h"

#include "NOD_texture.h"
#include "node_common.h"
#include "node_exec.h"
#include "node_texture_util.h"
#include "node_util.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "RE_texture.h"

#include "UI_resources.h"

static void texture_get_from_context(const bContext *C,
                                     bNodeTreeType *UNUSED(treetype),
                                     bNodeTree **r_ntree,
                                     ID **r_id,
                                     ID **r_from)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *ob = OBACT(view_layer);
  Tex *tx = NULL;

  if (snode->texfrom == SNODE_TEX_BRUSH) {
    struct Brush *brush = NULL;

    if (ob && (ob->mode & OB_MODE_SCULPT)) {
      brush = BKE_paint_brush(&scene->toolsettings->sculpt->paint);
    }
    else {
      brush = BKE_paint_brush(&scene->toolsettings->imapaint.paint);
    }

    if (brush) {
      *r_from = (ID *)brush;
      tx = give_current_brush_texture(brush);
      if (tx) {
        *r_id = &tx->id;
        *r_ntree = tx->nodetree;
      }
    }
  }
  else if (snode->texfrom == SNODE_TEX_LINESTYLE) {
    FreestyleLineStyle *linestyle = BKE_linestyle_active_from_view_layer(view_layer);
    if (linestyle) {
      *r_from = (ID *)linestyle;
      tx = give_current_linestyle_texture(linestyle);
      if (tx) {
        *r_id = &tx->id;
        *r_ntree = tx->nodetree;
      }
    }
  }
}

static void foreach_nodeclass(Scene *UNUSED(scene), void *calldata, bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Input"));
  func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
  func(calldata, NODE_CLASS_OP_COLOR, N_("Color"));
  func(calldata, NODE_CLASS_PATTERN, N_("Patterns"));
  func(calldata, NODE_CLASS_TEXTURE, N_("Textures"));
  func(calldata, NODE_CLASS_CONVERTER, N_("Converter"));
  func(calldata, NODE_CLASS_DISTORT, N_("Distort"));
  func(calldata, NODE_CLASS_GROUP, N_("Group"));
  func(calldata, NODE_CLASS_INTERFACE, N_("Interface"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

/* XXX muting disabled in previews because of threading issues with the main execution
 * it works here, but disabled for consistency
 */
#if 1
static void localize(bNodeTree *localtree, bNodeTree *UNUSED(ntree))
{
  bNode *node, *node_next;

  /* replace muted nodes and reroute nodes by internal links */
  for (node = localtree->nodes.first; node; node = node_next) {
    node_next = node->next;

    if (node->flag & NODE_MUTED || node->type == NODE_REROUTE) {
      nodeInternalRelink(localtree, node);
      ntreeFreeLocalNode(localtree, node);
    }
  }
}
#else
static void localize(bNodeTree *UNUSED(localtree), bNodeTree *UNUSED(ntree))
{
}
#endif

static void update(bNodeTree *ntree)
{
  ntree_update_reroute_nodes(ntree);
}

static bool texture_node_tree_socket_type_valid(bNodeTreeType *UNUSED(ntreetype),
                                                bNodeSocketType *socket_type)
{
  return nodeIsStaticSocketType(socket_type) &&
         ELEM(socket_type->type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA);
}

bNodeTreeType *ntreeType_Texture;

void register_node_tree_type_tex(void)
{
  bNodeTreeType *tt = ntreeType_Texture = MEM_callocN(sizeof(bNodeTreeType),
                                                      "texture node tree type");

  tt->type = NTREE_TEXTURE;
  strcpy(tt->idname, "TextureNodeTree");
  strcpy(tt->group_idname, "TextureNodeGroup");
  strcpy(tt->ui_name, N_("Texture Node Editor"));
  tt->ui_icon = ICON_NODE_TEXTURE; /* Defined in `drawnode.c`. */
  strcpy(tt->ui_description, N_("Texture nodes"));

  tt->foreach_nodeclass = foreach_nodeclass;
  tt->update = update;
  tt->localize = localize;
  tt->get_from_context = texture_get_from_context;
  tt->valid_socket_type = texture_node_tree_socket_type_valid;

  tt->rna_ext.srna = &RNA_TextureNodeTree;

  ntreeTypeAdd(tt);
}

/**** Material/Texture trees ****/

bNodeThreadStack *ntreeGetThreadStack(bNodeTreeExec *exec, int thread)
{
  ListBase *lb = &exec->threadstack[thread];
  bNodeThreadStack *nts;

  for (nts = (bNodeThreadStack *)lb->first; nts; nts = nts->next) {
    if (!nts->used) {
      nts->used = true;
      break;
    }
  }

  if (!nts) {
    nts = MEM_callocN(sizeof(bNodeThreadStack), "bNodeThreadStack");
    nts->stack = (bNodeStack *)MEM_dupallocN(exec->stack);
    nts->used = true;
    BLI_addtail(lb, nts);
  }

  return nts;
}

void ntreeReleaseThreadStack(bNodeThreadStack *nts)
{
  nts->used = false;
}

bool ntreeExecThreadNodes(bNodeTreeExec *exec, bNodeThreadStack *nts, void *callerdata, int thread)
{
  bNodeStack *nsin[MAX_SOCKET] = {NULL};  /* arbitrary... watch this */
  bNodeStack *nsout[MAX_SOCKET] = {NULL}; /* arbitrary... watch this */
  bNodeExec *nodeexec;
  bNode *node;
  int n;

  /* nodes are presorted, so exec is in order of list */

  for (n = 0, nodeexec = exec->nodeexec; n < exec->totnodes; n++, nodeexec++) {
    node = nodeexec->node;
    if (node->need_exec) {
      node_get_stack(node, nts->stack, nsin, nsout);
      /* Handle muted nodes...
       * If the mute func is not set, assume the node should never be muted,
       * and hence execute it!
       */
      if (node->typeinfo->exec_fn && !(node->flag & NODE_MUTED)) {
        node->typeinfo->exec_fn(callerdata, thread, node, &nodeexec->data, nsin, nsout);
      }
    }
  }

  /* signal to that all went OK, for render */
  return true;
}

bNodeTreeExec *ntreeTexBeginExecTree_internal(bNodeExecContext *context,
                                              bNodeTree *ntree,
                                              bNodeInstanceKey parent_key)
{
  bNodeTreeExec *exec;
  bNode *node;

  /* common base initialization */
  exec = ntree_exec_begin(context, ntree, parent_key);

  /* allocate the thread stack listbase array */
  exec->threadstack = MEM_callocN(BLENDER_MAX_THREADS * sizeof(ListBase), "thread stack array");

  for (node = exec->nodetree->nodes.first; node; node = node->next) {
    node->need_exec = 1;
  }

  return exec;
}

bNodeTreeExec *ntreeTexBeginExecTree(bNodeTree *ntree)
{
  bNodeExecContext context;
  bNodeTreeExec *exec;

  /* XXX hack: prevent exec data from being generated twice.
   * this should be handled by the renderer!
   */
  if (ntree->execdata) {
    return ntree->execdata;
  }

  context.previews = ntree->previews;

  exec = ntreeTexBeginExecTree_internal(&context, ntree, NODE_INSTANCE_KEY_BASE);

  /* XXX this should not be necessary, but is still used for cmp/sha/tex nodes,
   * which only store the ntree pointer. Should be fixed at some point!
   */
  ntree->execdata = exec;

  return exec;
}

/* free texture delegates */
static void tex_free_delegates(bNodeTreeExec *exec)
{
  bNodeThreadStack *nts;
  bNodeStack *ns;
  int th, a;

  for (th = 0; th < BLENDER_MAX_THREADS; th++) {
    for (nts = exec->threadstack[th].first; nts; nts = nts->next) {
      for (ns = nts->stack, a = 0; a < exec->stacksize; a++, ns++) {
        if (ns->data && !ns->is_copy) {
          MEM_freeN(ns->data);
        }
      }
    }
  }
}

void ntreeTexEndExecTree_internal(bNodeTreeExec *exec)
{
  bNodeThreadStack *nts;
  int a;

  if (exec->threadstack) {
    tex_free_delegates(exec);

    for (a = 0; a < BLENDER_MAX_THREADS; a++) {
      for (nts = exec->threadstack[a].first; nts; nts = nts->next) {
        if (nts->stack) {
          MEM_freeN(nts->stack);
        }
      }
      BLI_freelistN(&exec->threadstack[a]);
    }

    MEM_freeN(exec->threadstack);
    exec->threadstack = NULL;
  }

  ntree_exec_end(exec);
}

void ntreeTexEndExecTree(bNodeTreeExec *exec)
{
  if (exec) {
    /* exec may get freed, so assign ntree */
    bNodeTree *ntree = exec->nodetree;
    ntreeTexEndExecTree_internal(exec);

    /* XXX clear nodetree backpointer to exec data, same problem as noted in ntreeBeginExecTree */
    ntree->execdata = NULL;
  }
}

int ntreeTexExecTree(bNodeTree *ntree,
                     TexResult *target,
                     const float co[3],
                     float dxt[3],
                     float dyt[3],
                     int osatex,
                     const short thread,
                     const Tex *UNUSED(tex),
                     short which_output,
                     int cfra,
                     int preview,
                     MTex *mtex)
{
  TexCallData data;
  int retval = TEX_INT;
  bNodeThreadStack *nts = NULL;
  bNodeTreeExec *exec = ntree->execdata;

  data.co = co;
  data.dxt = dxt;
  data.dyt = dyt;
  data.osatex = osatex;
  data.target = target;
  data.do_preview = preview;
  data.do_manage = true;
  data.thread = thread;
  data.which_output = which_output;
  data.cfra = cfra;
  data.mtex = mtex;

  /* ensure execdata is only initialized once */
  if (!exec) {
    BLI_thread_lock(LOCK_NODES);
    if (!ntree->execdata) {
      ntreeTexBeginExecTree(ntree);
    }
    BLI_thread_unlock(LOCK_NODES);

    exec = ntree->execdata;
  }

  nts = ntreeGetThreadStack(exec, thread);
  ntreeExecThreadNodes(exec, nts, &data, thread);
  ntreeReleaseThreadStack(nts);

  retval |= TEX_RGB;

  return retval;
}
