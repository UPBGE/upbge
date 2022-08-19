/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Use a define instead of `#pragma once` because of `bmesh_iterators_inline.h` */
#ifndef __BLI_TASK_H__
#define __BLI_TASK_H__

#include <string.h> /* for memset() */

struct ListBase;

/** \file
 * \ingroup bli
 */

#include "BLI_threads.h"
#include "BLI_utildefines.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BLI_mempool;

/* -------------------------------------------------------------------- */
/** \name Task Scheduler
 *
 * Central scheduler that holds running threads ready to execute tasks.
 * A single queue holds the task from all pools.
 *
 * Initialize/exit must be called before/after any task pools are created/freed, and must
 * be called from the main threads. All other scheduler and pool functions are thread-safe.
 * \{ */

void BLI_task_scheduler_init(void);
void BLI_task_scheduler_exit(void);
int BLI_task_scheduler_num_threads(void);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task Pool
 *
 * Pool of tasks that will be executed by the central task scheduler. For each
 * pool, we can wait for all tasks to be done, or cancel them before they are
 * done.
 *
 * Running tasks may spawn new tasks.
 *
 * Pools may be nested, i.e. a thread running a task can create another task
 * pool with smaller tasks. When other threads are busy they will continue
 * working on their own tasks, if not they will join in, no new threads will
 * be launched.
 * \{ */

typedef enum eTaskPriority {
  TASK_PRIORITY_LOW,
  TASK_PRIORITY_HIGH,
} eTaskPriority;

typedef struct TaskPool TaskPool;
typedef void (*TaskRunFunction)(TaskPool *__restrict pool, void *taskdata);
typedef void (*TaskFreeFunction)(TaskPool *__restrict pool, void *taskdata);

/**
 * Regular task pool that immediately starts executing tasks as soon as they
 * are pushed, either on the current or another thread.
 */
TaskPool *BLI_task_pool_create(void *userdata, eTaskPriority priority);

/**
 * Background: always run tasks in a background thread, never immediately
 * execute them. For running background jobs.
 */
TaskPool *BLI_task_pool_create_background(void *userdata, eTaskPriority priority);

/**
 * Background Serial: run tasks one after the other in the background,
 * without parallelization between the tasks.
 */
TaskPool *BLI_task_pool_create_background_serial(void *userdata, eTaskPriority priority);

/**
 * Suspended: don't execute tasks until work_and_wait is called. This is slower
 * as threads can't immediately start working. But it can be used if the data
 * structures the threads operate on are not fully initialized until all tasks are created.
 */
TaskPool *BLI_task_pool_create_suspended(void *userdata, eTaskPriority priority);

/**
 * No threads: immediately executes tasks on the same thread. For debugging.
 */
TaskPool *BLI_task_pool_create_no_threads(void *userdata);

void BLI_task_pool_free(TaskPool *pool);

void BLI_task_pool_push(TaskPool *pool,
                        TaskRunFunction run,
                        void *taskdata,
                        bool free_taskdata,
                        TaskFreeFunction freedata);

/**
 * Work and wait until all tasks are done.
 */
void BLI_task_pool_work_and_wait(TaskPool *pool);
/**
 * Cancel all tasks, keep worker threads running.
 */
void BLI_task_pool_cancel(TaskPool *pool);

/**
 * For worker threads, test if current task pool canceled. this function may
 * only be called from worker threads and pool must be the task pool that the
 * thread is currently executing a task from.
 */
bool BLI_task_pool_current_canceled(TaskPool *pool);

/**
 * Optional `userdata` pointer to pass along to run function.
 */
void *BLI_task_pool_user_data(TaskPool *pool);

/**
 * Optional mutex to use from run function.
 */
ThreadMutex *BLI_task_pool_user_mutex(TaskPool *pool);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parallel for Routines
 * \{ */

/**
 * Per-thread specific data passed to the callback.
 */
typedef struct TaskParallelTLS {
  /**
   * Copy of user-specifier chunk, which is copied from original chunk to all worker threads.
   * This is similar to OpenMP's `firstprivate`.
   */
  void *userdata_chunk;
} TaskParallelTLS;

typedef void (*TaskParallelRangeFunc)(void *__restrict userdata,
                                      int iter,
                                      const TaskParallelTLS *__restrict tls);

typedef void (*TaskParallelInitFunc)(const void *__restrict userdata, void *__restrict chunk);

typedef void (*TaskParallelReduceFunc)(const void *__restrict userdata,
                                       void *__restrict chunk_join,
                                       void *__restrict chunk);

typedef void (*TaskParallelFreeFunc)(const void *__restrict userdata, void *__restrict chunk);

typedef struct TaskParallelSettings {
  /* Whether caller allows to do threading of the particular range.
   * Usually set by some equation, which forces threading off when threading
   * overhead becomes higher than speed benefit.
   * BLI_task_parallel_range() by itself will always use threading when range
   * is higher than a chunk size. As in, threading will always be performed.
   */
  bool use_threading;
  /* Each instance of looping chunks will get a copy of this data
   * (similar to OpenMP's firstprivate).
   */
  void *userdata_chunk;       /* Pointer to actual data. */
  size_t userdata_chunk_size; /* Size of that data. */
  /* Function called from calling thread once whole range have been
   * processed.
   */
  /* Function called to initialize user data chunk,
   * typically to allocate data, freed by `func_free`.
   */
  TaskParallelInitFunc func_init;
  /* Function called to join user data chunk into another, to reduce
   * the result to the original userdata_chunk memory.
   * The reduce functions should have no side effects, so that they
   * can be run on any thread. */
  TaskParallelReduceFunc func_reduce;
  /* Function called to free data created by TaskParallelRangeFunc. */
  TaskParallelFreeFunc func_free;
  /* Minimum allowed number of range iterators to be handled by a single
   * thread. This allows to achieve following:
   * - Reduce amount of threading overhead.
   * - Partially occupy thread pool with ranges which are computationally
   *   expensive, but which are smaller than amount of available threads.
   *   For example, it's possible to multi-thread [0 .. 64] range into 4
   *   thread which will be doing 16 iterators each.
   * This is a preferred way to tell scheduler when to start threading than
   * having a global use_threading switch based on just range size.
   */
  int min_iter_per_thread;
} TaskParallelSettings;

BLI_INLINE void BLI_parallel_range_settings_defaults(TaskParallelSettings *settings);

void BLI_task_parallel_range(int start,
                             int stop,
                             void *userdata,
                             TaskParallelRangeFunc func,
                             const TaskParallelSettings *settings);

/**
 * This data is shared between all tasks, its access needs thread lock or similar protection.
 */
typedef struct TaskParallelIteratorStateShared {
  /* Maximum amount of items to acquire at once. */
  int chunk_size;
  /* Next item to be acquired. */
  void *next_item;
  /* Index of the next item to be acquired. */
  int next_index;
  /* Indicates that end of iteration has been reached. */
  bool is_finished;
  /* Helper lock to protect access to this data in iterator getter callback,
   * can be ignored (if the callback implements its own protection system, using atomics e.g.).
   * Will be NULL when iterator is actually processed in a single thread. */
  SpinLock *spin_lock;
} TaskParallelIteratorStateShared;

typedef void (*TaskParallelIteratorIterFunc)(void *__restrict userdata,
                                             const TaskParallelTLS *__restrict tls,
                                             void **r_next_item,
                                             int *r_next_index,
                                             bool *r_do_abort);

typedef void (*TaskParallelIteratorFunc)(void *__restrict userdata,
                                         void *item,
                                         int index,
                                         const TaskParallelTLS *__restrict tls);

/**
 * This function allows to parallelize for loops using a generic iterator.
 *
 * \param userdata: Common userdata passed to all instances of \a func.
 * \param iter_func: Callback function used to generate chunks of items.
 * \param init_item: The initial item, if necessary (may be NULL if unused).
 * \param init_index: The initial index.
 * \param items_num: The total amount of items to iterate over
 *                   (if unknown, set it to a negative number).
 * \param func: Callback function.
 * \param settings: See public API doc of TaskParallelSettings for description of all settings.
 *
 * \note Static scheduling is only available when \a items_num is >= 0.
 */
void BLI_task_parallel_iterator(void *userdata,
                                TaskParallelIteratorIterFunc iter_func,
                                void *init_item,
                                int init_index,
                                int items_num,
                                TaskParallelIteratorFunc func,
                                const TaskParallelSettings *settings);

/**
 * This function allows to parallelize for loops over ListBase items.
 *
 * \param listbase: The double linked list to loop over.
 * \param userdata: Common userdata passed to all instances of \a func.
 * \param func: Callback function.
 * \param settings: See public API doc of ParallelRangeSettings for description of all settings.
 *
 * \note There is no static scheduling here,
 * since it would need another full loop over items to count them.
 */
void BLI_task_parallel_listbase(struct ListBase *listbase,
                                void *userdata,
                                TaskParallelIteratorFunc func,
                                const TaskParallelSettings *settings);

typedef struct MempoolIterData MempoolIterData;

typedef void (*TaskParallelMempoolFunc)(void *userdata,
                                        MempoolIterData *iter,
                                        const TaskParallelTLS *__restrict tls);
/**
 * This function allows to parallelize for loops over Mempool items.
 *
 * \param mempool: The iterable BLI_mempool to loop over.
 * \param userdata: Common userdata passed to all instances of \a func.
 * \param func: Callback function.
 * \param settings: See public API doc of TaskParallelSettings for description of all settings.
 *
 * \note There is no static scheduling here.
 */
void BLI_task_parallel_mempool(struct BLI_mempool *mempool,
                               void *userdata,
                               TaskParallelMempoolFunc func,
                               const TaskParallelSettings *settings);

/** TODO(sergey): Think of a better place for this. */
BLI_INLINE void BLI_parallel_range_settings_defaults(TaskParallelSettings *settings)
{
  memset(settings, 0, sizeof(*settings));
  settings->use_threading = true;
  /* Use default heuristic to define actual chunk size. */
  settings->min_iter_per_thread = 0;
}

BLI_INLINE void BLI_parallel_mempool_settings_defaults(TaskParallelSettings *settings)
{
  memset(settings, 0, sizeof(*settings));
  settings->use_threading = true;
}

/**
 * Don't use this, store any thread specific data in `tls->userdata_chunk` instead.
 * Only here for code to be removed.
 */
int BLI_task_parallel_thread_id(const TaskParallelTLS *tls);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task Graph Scheduling
 *
 * Task Graphs can be used to create a forest of directional trees and schedule work to any tree.
 * The nodes in the graph can be run in separate threads.
 *
 * \code{.unparsed}
 *     +---- [root] ----+
 *     |                |
 *     v                v
 * [node_1]    +---- [node_2] ----+
 *             |                  |
 *             v                  v
 *          [node_3]           [node_4]
 * \endcode
 *
 * \code{.c}
 * TaskGraph *task_graph = BLI_task_graph_create();
 * TaskNode *root = BLI_task_graph_node_create(task_graph, root_exec, NULL, NULL);
 * TaskNode *node_1 = BLI_task_graph_node_create(task_graph, node_exec, NULL, NULL);
 * TaskNode *node_2 = BLI_task_graph_node_create(task_graph, node_exec, NULL, NULL);
 * TaskNode *node_3 = BLI_task_graph_node_create(task_graph, node_exec, NULL, NULL);
 * TaskNode *node_4 = BLI_task_graph_node_create(task_graph, node_exec, NULL, NULL);
 *
 * BLI_task_graph_edge_create(root, node_1);
 * BLI_task_graph_edge_create(root, node_2);
 * BLI_task_graph_edge_create(node_2, node_3);
 * BLI_task_graph_edge_create(node_2, node_4);
 * \endcode
 *
 * Any node can be triggered to start a chain of tasks. Normally you would trigger a root node but
 * it is supported to start the chain of tasks anywhere in the forest or tree. When a node
 * completes, the execution flow is forwarded via the created edges.
 * When a child node has multiple parents the child node will be triggered once for each parent.
 *
 *    `BLI_task_graph_node_push_work(root);`
 *
 * In this example After `root` is finished, `node_1` and `node_2` will be started.
 * Only after `node_2` is finished `node_3` and `node_4` will be started.
 *
 * After scheduling work we need to wait until all the tasks have been finished.
 *
 *    `BLI_task_graph_work_and_wait();`
 *
 * When finished you can clean up all the resources by freeing the task_graph. Nodes are owned by
 * the graph and are freed task_data will only be freed if a free_func was given.
 *
 *    `BLI_task_graph_free(task_graph);`
 *
 * Work can enter a tree on any node. Normally this would be the root_node.
 * A `task_graph` can be reused, but the caller needs to make sure the task_data is reset.
 *
 * Task-Data
 * ---------
 *
 * Typically you want give a task data to work on.
 * Task data can be shared with other nodes, but be careful not to free the data multiple times.
 * Task data is freed when calling #BLI_task_graph_free.
 *
 * \code{.c}
 * MyData *task_data = MEM_callocN(sizeof(MyData), __func__);
 * TaskNode *root = BLI_task_graph_node_create(task_graph, root_exec, task_data, MEM_freeN);
 * TaskNode *node_1 = BLI_task_graph_node_create(task_graph, node_exec, task_data, NULL);
 * TaskNode *node_2 = BLI_task_graph_node_create(task_graph, node_exec, task_data, NULL);
 * TaskNode *node_3 = BLI_task_graph_node_create(task_graph, node_exec, task_data, NULL);
 * TaskNode *node_4 = BLI_task_graph_node_create(task_graph, node_exec, task_data, NULL);
 * \endcode
 * \{ */

struct TaskGraph;
struct TaskNode;

typedef void (*TaskGraphNodeRunFunction)(void *__restrict task_data);
typedef void (*TaskGraphNodeFreeFunction)(void *task_data);

struct TaskGraph *BLI_task_graph_create(void);
void BLI_task_graph_work_and_wait(struct TaskGraph *task_graph);
void BLI_task_graph_free(struct TaskGraph *task_graph);
struct TaskNode *BLI_task_graph_node_create(struct TaskGraph *task_graph,
                                            TaskGraphNodeRunFunction run,
                                            void *user_data,
                                            TaskGraphNodeFreeFunction free_func);
bool BLI_task_graph_node_push_work(struct TaskNode *task_node);
void BLI_task_graph_edge_create(struct TaskNode *from_node, struct TaskNode *to_node);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task Isolation
 *
 * Task isolation helps avoid unexpected task scheduling decisions that can lead to bugs if wrong
 * assumptions were made. Typically that happens when doing "nested threading", i.e. one thread
 * schedules a bunch of main-tasks and those spawn new sub-tasks.
 *
 * What can happen is that when a main-task waits for its sub-tasks to complete on other threads,
 * another main-task is scheduled within the already running main-task. Generally, this is good,
 * because it leads to better performance. However, sometimes code (often unintentionally) makes
 * the assumption that at most one main-task runs on a thread at a time.
 *
 * The bugs often show themselves in two ways:
 * - Deadlock, when a main-task holds a mutex while waiting for its sub-tasks to complete.
 * - Data corruption, when a main-task makes wrong assumptions about a thread-local variable.
 *
 * Task isolation can avoid these bugs by making sure that a main-task does not start executing
 * another main-task while waiting for its sub-tasks. More precisely, a function that runs in an
 * isolated region is only allowed to run sub-tasks that were spawned in the same isolated region.
 *
 * Unfortunately, incorrect use of task isolation can lead to deadlocks itself. This can happen
 * when threading primitives are used that separate spawning tasks from executing them. The problem
 * occurs when a task is spawned in one isolated region while the tasks are waited for in another
 * isolated region. In this setup, the thread that is waiting for the spawned tasks to complete
 * cannot run the tasks itself. On a single thread, that causes a deadlock already. When there are
 * multiple threads, another thread will typically run the task and avoid the deadlock. However, if
 * this situation happens on all threads at the same time, all threads will deadlock. This happened
 * in T88598.
 * \{ */

void BLI_task_isolate(void (*func)(void *userdata), void *userdata);

/** \} */

#ifdef __cplusplus
}
#endif

#endif
