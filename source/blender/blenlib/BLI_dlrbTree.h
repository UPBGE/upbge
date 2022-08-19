/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

#pragma once

/** \file
 * \ingroup bli
 *
 * Double-Linked Red-Black Tree Implementation:
 *
 * This is simply a Red-Black Tree implementation whose nodes can later
 * be arranged + retrieved as elements in a Double-Linked list (i.e. ListBase).
 * The Red-Black Tree implementation is based on the methods defined by Wikipedia.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ********************************************** */
/* Data Types and Type Defines */

/* -------------------------------------------------------------------- */
/** \name Base Structs
 * \{ */

/** Basic Layout for a Node. */
typedef struct DLRBT_Node {
  /* ListBase capabilities */
  struct DLRBT_Node *next, *prev;

  /* Tree Associativity settings */
  struct DLRBT_Node *left, *right;
  struct DLRBT_Node *parent;

  char tree_col;
  /* ... for nice alignment, next item should usually be a char too... */
} DLRBT_Node;

/** Red/Black defines for tree_col. */
typedef enum eDLRBT_Colors {
  DLRBT_BLACK = 0,
  DLRBT_RED,
} eDLRBT_Colors;

/* -------- */

/** The Tree Data. */
typedef struct DLRBT_Tree {
  /* ListBase capabilities */
  void *first, *last; /* these should be based on DLRBT_Node-s */

  /* Root Node */
  void *root; /* this should be based on DLRBT_Node-s */
} DLRBT_Tree;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callback Types
 * \{ */

/**
 * Return -1, 0, 1 for whether the given data is less than,
 * equal to, or greater than the given node.
 * \param node: <DLRBT_Node> the node to compare to.
 * \param data: pointer to the relevant data or values stored in the bit-pattern.
 * Dependent on the function.
 */
typedef short (*DLRBT_Comparator_FP)(void *node, void *data);

/**
 * Return a new node instance wrapping the given data
 * - data: Pointer to the relevant data to create a subclass of node from.
 */
typedef DLRBT_Node *(*DLRBT_NAlloc_FP)(void *data);

/**
 * Update an existing node instance accordingly to be in sync with the given data.
 * \param node: <DLRBT_Node> the node to update.
 * \param data: Pointer to the relevant data or values stored in the bit-pattern.
 * Dependent on the function.
 */
typedef void (*DLRBT_NUpdate_FP)(void *node, void *data);

/**
 * Free a node and the wrapped data.
 * \param node: <DLRBT_Node> the node to free.
 */
typedef void (*DLRBT_NFree_FP)(void *node);

/* ********************************************** */
/* Public API */

/** \} */

/* -------------------------------------------------------------------- */
/** \name ADT Management
 * \{ */

/**
 * Create a new tree, and initialize as necessary.
 */
DLRBT_Tree *BLI_dlrbTree_new(void);

/**
 * Initializes some given trees.
 * Just zero out the pointers used.
 */
void BLI_dlrbTree_init(DLRBT_Tree *tree);

/**
 * Free the given tree's data but not the tree itself.
 */
void BLI_dlrbTree_free(DLRBT_Tree *tree, DLRBT_NFree_FP free_cb);

/**
 * Make sure the tree's Double-Linked list representation is valid.
 */
void BLI_dlrbTree_linkedlist_sync(DLRBT_Tree *tree);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tree Searching Utilities
 * \{ */

/**
 * Find the node which matches or is the closest to the requested node.
 */
DLRBT_Node *BLI_dlrbTree_search(const DLRBT_Tree *tree,
                                DLRBT_Comparator_FP cmp_cb,
                                void *search_data);

/**
 * Find the node which exactly matches the required data.
 */
DLRBT_Node *BLI_dlrbTree_search_exact(const DLRBT_Tree *tree,
                                      DLRBT_Comparator_FP cmp_cb,
                                      void *search_data);

/**
 * Find the node which occurs immediately before the best matching node.
 */
DLRBT_Node *BLI_dlrbTree_search_prev(const DLRBT_Tree *tree,
                                     DLRBT_Comparator_FP cmp_cb,
                                     void *search_data);

/**
 * Find the node which occurs immediately after the best matching node.
 */
DLRBT_Node *BLI_dlrbTree_search_next(const DLRBT_Tree *tree,
                                     DLRBT_Comparator_FP cmp_cb,
                                     void *search_data);

/**
 * Check whether there is a node matching the requested node.
 */
short BLI_dlrbTree_contains(DLRBT_Tree *tree, DLRBT_Comparator_FP cmp_cb, void *search_data);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Operations (Managed)
 * \{ */

/**
 * These methods automate the process of adding/removing nodes from the BST,
 * using the supplied data and callbacks.
 */

/**
 * Add the given data to the tree, and return the node added.
 * \note for duplicates, the update_cb is called (if available),
 * and the existing node is returned.
 */
DLRBT_Node *BLI_dlrbTree_add(DLRBT_Tree *tree,
                             DLRBT_Comparator_FP cmp_cb,
                             DLRBT_NAlloc_FP new_cb,
                             DLRBT_NUpdate_FP update_cb,
                             void *data);

/* FIXME: this is not implemented yet. */
/**
 * Remove the given element from the tree and balance again.
 */
// void BLI_dlrbTree_remove(DLRBT_Tree *tree, DLRBT_Node *node);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Operations (Manual)
 *
 * These methods require custom code for creating BST nodes and adding them to the
 * tree in special ways, such that the node can then be balanced.
 *
 * It is recommended that these methods are only used where the other method is too cumbersome.
 * \{ */

/**
 * Balance the tree after the given node has been added to it
 * (using custom code, in the Binary Tree way).
 */
void BLI_dlrbTree_insert(DLRBT_Tree *tree, DLRBT_Node *node);

/** \} */

#ifdef __cplusplus
}
#endif
