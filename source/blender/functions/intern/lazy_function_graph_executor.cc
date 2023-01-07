/* SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * This file implements the evaluation of a lazy-function graph. It's main objectives are:
 * - Only compute values that are actually used.
 * - Stay single threaded when nodes are executed quickly.
 * - Allow spreading the work over an arbitrary number of threads efficiently.
 *
 * This executor makes use of `FN_lazy_threading.hh` to enable multi-threading only when it seems
 * beneficial. It operates in two modes: single- and multi-threaded. The use of a task pool and
 * locks is avoided in single-threaded mode. Once multi-threading is enabled the executor starts
 * using both. It is not possible to switch back from multi-threaded to single-threaded mode.
 *
 * The multi-threading design implemented in this executor requires *no* main thread that
 * coordinates everything. Instead, one thread will trigger some initial work and then many threads
 * coordinate themselves in a distributed fashion. In an ideal situation, every thread ends up
 * processing a separate part of the graph which results in less communication overhead. The way
 * TBB schedules tasks helps with that: a thread will next process the task that it added to a task
 * pool just before.
 *
 * Communication between threads is synchronized by using a mutex in every node. When a thread
 * wants to access the state of a node, its mutex has to be locked first (with some documented
 * exceptions). The assumption here is that most nodes are only ever touched by a single thread and
 * therefore the lock contention is reduced the more nodes there are.
 *
 * Similar to how a #LazyFunction can be thought of as a state machine (see `FN_lazy_function.hh`),
 * each node can also be thought of as a state machine. The state of a node contains the evaluation
 * state of its inputs and outputs. Every time a node is executed, it has to advance its state in
 * some way (e.g. it requests a new input or computes a new output).
 *
 * When a node is executed it may send notifications to other nodes which may in turn schedule
 * those nodes. For example, when the current node has computed one of its outputs, then the
 * computed value is forwarded to all linked inputs, changing their node states in the process. If
 * this input was the last missing required input, the node will be scheduled that it is executed
 * next.
 *
 * When all tasks are completed, the executor gives back control to the caller which may later
 * provide new inputs to the graph which in turn leads to new nodes being scheduled and the process
 * starts again.
 */

#include <mutex>

#include "BLI_compute_context.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_function_ref.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_timeit.hh"

#include "FN_lazy_function_graph_executor.hh"

namespace blender::fn::lazy_function {

enum class NodeScheduleState {
  /**
   * Default state of every node.
   */
  NotScheduled,
  /**
   * The node has been added to the task pool or is otherwise scheduled to be executed in the
   * future.
   */
  Scheduled,
  /**
   * The node is currently running.
   */
  Running,
  /**
   * The node is running and has been rescheduled while running. In this case the node run again.
   * This state exists, because we don't want to add the node to the task pool twice, because then
   * the node might run twice at the same time, which is not allowed. Instead, once the node is
   * done running, it will reschedule itself.
   */
  RunningAndRescheduled,
};

struct InputState {
  /**
   * Value of this input socket. By default, the value is empty. When other nodes are done
   * computing their outputs, the computed values will be forwarded to linked input sockets. The
   * value will then live here until it is found that it is not needed anymore.
   *
   * If #was_ready_for_execution is true, access does not require holding the node lock.
   */
  void *value = nullptr;
  /**
   * How the node intends to use this input. By default, all inputs may be used. Based on which
   * outputs are used, a node can decide that an input will definitely be used or is never used.
   * This allows freeing values early and avoids unnecessary computations.
   */
  ValueUsage usage = ValueUsage::Maybe;
  /**
   * Set to true once #value is set and will stay true afterwards. Access during execution of a
   * node, does not require holding the node lock.
   */
  bool was_ready_for_execution = false;
};

struct OutputState {
  /**
   * Keeps track of how the output value is used. If a connected input becomes used, this output
   * has to become used as well. The output becomes unused when it is used by no input socket
   * anymore and it's not an output of the graph.
   */
  ValueUsage usage = ValueUsage::Maybe;
  /**
   * This is a copy of #usage that is done right before node execution starts. This is done so that
   * the node gets a consistent view of what outputs are used, even when this changes while the
   * node is running (the node might be reevaluated in that case). Access during execution of a
   * node, does not require holding the node lock.
   */
  ValueUsage usage_for_execution = ValueUsage::Maybe;
  /**
   * Number of linked sockets that might still use the value of this output.
   */
  int potential_target_sockets = 0;
  /**
   * Is set to true once the output has been computed and then stays true. Access does not require
   * holding the node lock.
   */
  bool has_been_computed = false;
  /**
   * Holds the output value for a short period of time while the node is initializing it and before
   * it's forwarded to input sockets. Access does not require holding the node lock.
   */
  void *value = nullptr;
};

struct NodeState {
  /**
   * Needs to be locked when any data in this state is accessed that is not explicitly marked as
   * not needing the lock.
   */
  mutable std::mutex mutex;
  /**
   * States of the individual input and output sockets. One can index into these arrays without
   * locking. However, to access data inside, a lock is needed unless noted otherwise.
   */
  MutableSpan<InputState> inputs;
  MutableSpan<OutputState> outputs;
  /**
   * Counts the number of inputs that still have to be provided to this node, until it should run
   * again. This is used as an optimization so that nodes are not scheduled unnecessarily in many
   * cases.
   */
  int missing_required_inputs = 0;
  /**
   * Is set to true once the node is done with its work, i.e. when all outputs that may be used
   * have been computed.
   */
  bool node_has_finished = false;
  /**
   * Set to true once the always required inputs have been requested.
   * This happens the first time the node is run.
   */
  bool always_used_inputs_requested = false;
  /**
   * Set to true when the storage and defaults have been initialized.
   * This happens the first time the node function is executed.
   */
  bool storage_and_defaults_initialized = false;
  /**
   * Nodes with side effects should always be executed when their required inputs have been
   * computed.
   */
  bool has_side_effects = false;
  /**
   * A node is always in one specific schedule state. This helps to ensure that the same node does
   * not run twice at the same time accidentally.
   */
  NodeScheduleState schedule_state = NodeScheduleState::NotScheduled;
  /**
   * Custom storage of the node.
   */
  void *storage = nullptr;
};

/**
 * Utility class that wraps a node whose state is locked. Having this is a separate class is useful
 * because it allows methods to communicate that they expect the node to be locked.
 */
struct LockedNode {
  /**
   * This is the node that is currently locked.
   */
  const Node &node;
  NodeState &node_state;

  /**
   * Used to delay notifying (and therefore locking) other nodes until the current node is not
   * locked anymore. This might not be strictly necessary to avoid deadlocks in the current code,
   * but is a good measure to avoid accidentally adding a deadlock later on. By not locking more
   * than one node per thread at a time, deadlocks are avoided.
   *
   * The notifications will be send right after the node is not locked anymore.
   */
  Vector<const OutputSocket *> delayed_required_outputs;
  Vector<const OutputSocket *> delayed_unused_outputs;

  LockedNode(const Node &node, NodeState &node_state) : node(node), node_state(node_state)
  {
  }
};

class Executor;
class GraphExecutorLFParams;

struct CurrentTask {
  /**
   * Mutex used to protect #scheduled_nodes when the executor uses multi-threading.
   */
  std::mutex mutex;
  /**
   * Nodes that have been scheduled to execute next.
   */
  Vector<const FunctionNode *> scheduled_nodes;
  /**
   * Makes it cheaper to check if there are any scheduled nodes because it avoids locking the
   * mutex.
   */
  std::atomic<bool> has_scheduled_nodes = false;
};

class Executor {
 private:
  const GraphExecutor &self_;
  /**
   * Remembers which inputs have been loaded from the caller already, to avoid loading them twice.
   * Atomics are used to make sure that every input is only retrieved once.
   */
  Array<std::atomic<uint8_t>> loaded_inputs_;
  /**
   * State of every node, indexed by #Node::index_in_graph.
   */
  Array<NodeState *> node_states_;
  /**
   * Parameters provided by the caller. This is always non-null, while a node is running.
   */
  Params *params_ = nullptr;
  const Context *context_ = nullptr;
  /**
   * Used to distribute work on separate nodes to separate threads.
   * If this is empty, the executor is in single threaded mode.
   */
  std::atomic<TaskPool *> task_pool_ = nullptr;
#ifdef FN_LAZY_FUNCTION_DEBUG_THREADS
  std::thread::id current_main_thread_;
#endif
  /**
   * A separate linear allocator for every thread. We could potentially reuse some memory, but that
   * doesn't seem worth it yet.
   */
  struct ThreadLocalData {
    LinearAllocator<> allocator;
  };
  std::unique_ptr<threading::EnumerableThreadSpecific<ThreadLocalData>> thread_locals_;
  LinearAllocator<> main_allocator_;
  /**
   * Set to false when the first execution ends.
   */
  bool is_first_execution_ = true;

  friend GraphExecutorLFParams;

 public:
  Executor(const GraphExecutor &self) : self_(self), loaded_inputs_(self.graph_inputs_.size())
  {
    /* The indices are necessary, because they are used as keys in #node_states_. */
    BLI_assert(self_.graph_.node_indices_are_valid());
  }

  ~Executor()
  {
    if (TaskPool *task_pool = task_pool_.load()) {
      BLI_task_pool_free(task_pool);
    }
    threading::parallel_for(node_states_.index_range(), 1024, [&](const IndexRange range) {
      for (const int node_index : range) {
        const Node &node = *self_.graph_.nodes()[node_index];
        NodeState &node_state = *node_states_[node_index];
        this->destruct_node_state(node, node_state);
      }
    });
  }

  /**
   * Main entry point to the execution of this graph.
   */
  void execute(Params &params, const Context &context)
  {
    params_ = &params;
    context_ = &context;
#ifdef FN_LAZY_FUNCTION_DEBUG_THREADS
    current_main_thread_ = std::this_thread::get_id();
#endif
    const auto deferred_func = [&]() {
      /* Make sure the pointers are not dangling, even when it shouldn't be accessed by anyone. */
      params_ = nullptr;
      context_ = nullptr;
      is_first_execution_ = false;
#ifdef FN_LAZY_FUNCTION_DEBUG_THREADS
      current_main_thread_ = {};
#endif
    };
    BLI_SCOPED_DEFER(deferred_func);

    CurrentTask current_task;
    if (is_first_execution_) {
      this->initialize_node_states();

      /* Initialize atomics to zero. */
      memset(static_cast<void *>(loaded_inputs_.data()), 0, loaded_inputs_.size() * sizeof(bool));

      this->set_always_unused_graph_inputs();
      this->set_defaulted_graph_outputs();

      /* Retrieve and tag side effect nodes. */
      Vector<const FunctionNode *> side_effect_nodes;
      if (self_.side_effect_provider_ != nullptr) {
        side_effect_nodes = self_.side_effect_provider_->get_nodes_with_side_effects(*context_);
        for (const FunctionNode *node : side_effect_nodes) {
          const int node_index = node->index_in_graph();
          NodeState &node_state = *node_states_[node_index];
          node_state.has_side_effects = true;
        }
      }

      this->initialize_static_value_usages(side_effect_nodes);
      this->schedule_side_effect_nodes(side_effect_nodes, current_task);
    }

    this->schedule_newly_requested_outputs(current_task);
    this->forward_newly_provided_inputs(current_task);

    this->run_task(current_task);

    if (TaskPool *task_pool = task_pool_.load()) {
      BLI_task_pool_work_and_wait(task_pool);
    }
  }

 private:
  void initialize_node_states()
  {
    Span<const Node *> nodes = self_.graph_.nodes();
    node_states_.reinitialize(nodes.size());

    auto construct_node_range = [&](const IndexRange range, LinearAllocator<> &allocator) {
      for (const int i : range) {
        const Node &node = *nodes[i];
        NodeState &node_state = *allocator.construct<NodeState>().release();
        node_states_[i] = &node_state;
        this->construct_initial_node_state(allocator, node, node_state);
      }
    };
    if (nodes.size() <= 256) {
      construct_node_range(nodes.index_range(), main_allocator_);
    }
    else {
      this->ensure_thread_locals();
      /* Construct all node states in parallel. */
      threading::parallel_for(nodes.index_range(), 256, [&](const IndexRange range) {
        LinearAllocator<> &allocator = thread_locals_->local().allocator;
        construct_node_range(range, allocator);
      });
    }
  }

  void construct_initial_node_state(LinearAllocator<> &allocator,
                                    const Node &node,
                                    NodeState &node_state)
  {
    const Span<const InputSocket *> node_inputs = node.inputs();
    const Span<const OutputSocket *> node_outputs = node.outputs();

    node_state.inputs = allocator.construct_array<InputState>(node_inputs.size());
    node_state.outputs = allocator.construct_array<OutputState>(node_outputs.size());
  }

  void destruct_node_state(const Node &node, NodeState &node_state)
  {
    if (node.is_function()) {
      const LazyFunction &fn = static_cast<const FunctionNode &>(node).function();
      if (node_state.storage != nullptr) {
        fn.destruct_storage(node_state.storage);
      }
    }
    for (const int i : node.inputs().index_range()) {
      InputState &input_state = node_state.inputs[i];
      const InputSocket &input_socket = node.input(i);
      this->destruct_input_value_if_exists(input_state, input_socket.type());
    }
    std::destroy_at(&node_state);
  }

  void schedule_newly_requested_outputs(CurrentTask &current_task)
  {
    for (const int graph_output_index : self_.graph_outputs_.index_range()) {
      if (params_->get_output_usage(graph_output_index) != ValueUsage::Used) {
        continue;
      }
      if (params_->output_was_set(graph_output_index)) {
        continue;
      }
      const InputSocket &socket = *self_.graph_outputs_[graph_output_index];
      const Node &node = socket.node();
      NodeState &node_state = *node_states_[node.index_in_graph()];
      this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
        this->set_input_required(locked_node, socket);
      });
    }
  }

  void set_defaulted_graph_outputs()
  {
    for (const int graph_output_index : self_.graph_outputs_.index_range()) {
      const InputSocket &socket = *self_.graph_outputs_[graph_output_index];
      if (socket.origin() != nullptr) {
        continue;
      }
      const CPPType &type = socket.type();
      const void *default_value = socket.default_value();
      BLI_assert(default_value != nullptr);

      if (self_.logger_ != nullptr) {
        self_.logger_->log_socket_value(socket, {type, default_value}, *context_);
      }

      void *output_ptr = params_->get_output_data_ptr(graph_output_index);
      type.copy_construct(default_value, output_ptr);
      params_->output_set(graph_output_index);
    }
  }

  void set_always_unused_graph_inputs()
  {
    for (const int i : self_.graph_inputs_.index_range()) {
      const OutputSocket &socket = *self_.graph_inputs_[i];
      const Node &node = socket.node();
      const NodeState &node_state = *node_states_[node.index_in_graph()];
      const OutputState &output_state = node_state.outputs[socket.index()];
      if (output_state.usage == ValueUsage::Unused) {
        params_->set_input_unused(i);
      }
    }
  }

  /**
   * Determines which nodes might be executed and which are unreachable. The set of reachable nodes
   * can dynamically depend on the side effect nodes.
   *
   * Most importantly, this function initializes `InputState.usage` and
   * `OutputState.potential_target_sockets`.
   */
  void initialize_static_value_usages(const Span<const FunctionNode *> side_effect_nodes)
  {
    const Span<const Node *> all_nodes = self_.graph_.nodes();

    /* Used for a search through all nodes that outputs depend on. */
    Stack<const Node *> reachable_nodes_to_check;
    Array<bool> reachable_node_flags(all_nodes.size(), false);

    /* Graph outputs are always reachable. */
    for (const InputSocket *socket : self_.graph_outputs_) {
      const Node &node = socket->node();
      const int node_index = node.index_in_graph();
      if (!reachable_node_flags[node_index]) {
        reachable_node_flags[node_index] = true;
        reachable_nodes_to_check.push(&node);
      }
    }

    /* Side effect nodes are always reachable. */
    for (const FunctionNode *node : side_effect_nodes) {
      const int node_index = node->index_in_graph();
      reachable_node_flags[node_index] = true;
      reachable_nodes_to_check.push(node);
    }

    /* Tag every node that reachable nodes depend on using depth-first-search. */
    while (!reachable_nodes_to_check.is_empty()) {
      const Node &node = *reachable_nodes_to_check.pop();
      for (const InputSocket *input_socket : node.inputs()) {
        const OutputSocket *origin_socket = input_socket->origin();
        if (origin_socket != nullptr) {
          const Node &origin_node = origin_socket->node();
          const int origin_node_index = origin_node.index_in_graph();
          if (!reachable_node_flags[origin_node_index]) {
            reachable_node_flags[origin_node_index] = true;
            reachable_nodes_to_check.push(&origin_node);
          }
        }
      }
    }

    for (const int node_index : reachable_node_flags.index_range()) {
      const Node &node = *all_nodes[node_index];
      NodeState &node_state = *node_states_[node_index];
      const bool node_is_reachable = reachable_node_flags[node_index];
      if (node_is_reachable) {
        for (const int output_index : node.outputs().index_range()) {
          const OutputSocket &output_socket = node.output(output_index);
          OutputState &output_state = node_state.outputs[output_index];
          int use_count = 0;
          for (const InputSocket *target_socket : output_socket.targets()) {
            const Node &target_node = target_socket->node();
            const bool target_is_reachable = reachable_node_flags[target_node.index_in_graph()];
            /* Only count targets that are reachable. */
            if (target_is_reachable) {
              use_count++;
            }
          }
          output_state.potential_target_sockets = use_count;
          if (use_count == 0) {
            output_state.usage = ValueUsage::Unused;
          }
        }
      }
      else {
        /* Inputs of unreachable nodes are unused. */
        for (InputState &input_state : node_state.inputs) {
          input_state.usage = ValueUsage::Unused;
        }
      }
    }
  }

  void schedule_side_effect_nodes(const Span<const FunctionNode *> side_effect_nodes,
                                  CurrentTask &current_task)
  {
    for (const FunctionNode *node : side_effect_nodes) {
      NodeState &node_state = *node_states_[node->index_in_graph()];
      this->with_locked_node(*node, node_state, current_task, [&](LockedNode &locked_node) {
        this->schedule_node(locked_node, current_task);
      });
    }
  }

  void forward_newly_provided_inputs(CurrentTask &current_task)
  {
    LinearAllocator<> &allocator = this->get_main_or_local_allocator();
    for (const int graph_input_index : self_.graph_inputs_.index_range()) {
      std::atomic<uint8_t> &was_loaded = loaded_inputs_[graph_input_index];
      if (was_loaded.load()) {
        continue;
      }
      void *input_data = params_->try_get_input_data_ptr(graph_input_index);
      if (input_data == nullptr) {
        continue;
      }
      if (was_loaded.fetch_or(1)) {
        /* The value was forwarded before. */
        continue;
      }
      this->forward_newly_provided_input(current_task, allocator, graph_input_index, input_data);
    }
  }

  void forward_newly_provided_input(CurrentTask &current_task,
                                    LinearAllocator<> &allocator,
                                    const int graph_input_index,
                                    void *input_data)
  {
    const OutputSocket &socket = *self_.graph_inputs_[graph_input_index];
    const CPPType &type = socket.type();
    void *buffer = allocator.allocate(type.size(), type.alignment());
    type.move_construct(input_data, buffer);
    this->forward_value_to_linked_inputs(socket, {type, buffer}, current_task);
  }

  void notify_output_required(const OutputSocket &socket, CurrentTask &current_task)
  {
    const Node &node = socket.node();
    const int index_in_node = socket.index();
    NodeState &node_state = *node_states_[node.index_in_graph()];
    OutputState &output_state = node_state.outputs[index_in_node];

    /* The notified output socket might be an input of the entire graph. In this case, notify the
     * caller that the input is required. */
    if (node.is_dummy()) {
      const int graph_input_index = self_.graph_inputs_.index_of(&socket);
      std::atomic<uint8_t> &was_loaded = loaded_inputs_[graph_input_index];
      if (was_loaded.load()) {
        return;
      }
      void *input_data = params_->try_get_input_data_ptr_or_request(graph_input_index);
      if (input_data == nullptr) {
        return;
      }
      if (was_loaded.fetch_or(1)) {
        /* The value was forwarded already. */
        return;
      }
      this->forward_newly_provided_input(
          current_task, this->get_main_or_local_allocator(), graph_input_index, input_data);
      return;
    }

    BLI_assert(node.is_function());
    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
      if (output_state.usage == ValueUsage::Used) {
        return;
      }
      output_state.usage = ValueUsage::Used;
      this->schedule_node(locked_node, current_task);
    });
  }

  void notify_output_unused(const OutputSocket &socket, CurrentTask &current_task)
  {
    const Node &node = socket.node();
    const int index_in_node = socket.index();
    NodeState &node_state = *node_states_[node.index_in_graph()];
    OutputState &output_state = node_state.outputs[index_in_node];

    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
      output_state.potential_target_sockets -= 1;
      if (output_state.potential_target_sockets == 0) {
        BLI_assert(output_state.usage != ValueUsage::Unused);
        if (output_state.usage == ValueUsage::Maybe) {
          output_state.usage = ValueUsage::Unused;
          if (node.is_dummy()) {
            const int graph_input_index = self_.graph_inputs_.index_of(&socket);
            params_->set_input_unused(graph_input_index);
          }
          else {
            this->schedule_node(locked_node, current_task);
          }
        }
      }
    });
  }

  void schedule_node(LockedNode &locked_node, CurrentTask &current_task)
  {
    BLI_assert(locked_node.node.is_function());
    switch (locked_node.node_state.schedule_state) {
      case NodeScheduleState::NotScheduled: {
        locked_node.node_state.schedule_state = NodeScheduleState::Scheduled;
        const FunctionNode &node = static_cast<const FunctionNode &>(locked_node.node);
        if (this->use_multi_threading()) {
          std::lock_guard lock{current_task.mutex};
          current_task.scheduled_nodes.append(&node);
        }
        else {
          current_task.scheduled_nodes.append(&node);
        }
        current_task.has_scheduled_nodes.store(true, std::memory_order_relaxed);
        break;
      }
      case NodeScheduleState::Scheduled: {
        break;
      }
      case NodeScheduleState::Running: {
        locked_node.node_state.schedule_state = NodeScheduleState::RunningAndRescheduled;
        break;
      }
      case NodeScheduleState::RunningAndRescheduled: {
        break;
      }
    }
  }

  void with_locked_node(const Node &node,
                        NodeState &node_state,
                        CurrentTask &current_task,
                        const FunctionRef<void(LockedNode &)> f)
  {
    BLI_assert(&node_state == node_states_[node.index_in_graph()]);

    LockedNode locked_node{node, node_state};
    if (this->use_multi_threading()) {
      std::lock_guard lock{node_state.mutex};
      threading::isolate_task([&]() { f(locked_node); });
    }
    else {
      f(locked_node);
    }

    this->send_output_required_notifications(locked_node.delayed_required_outputs, current_task);
    this->send_output_unused_notifications(locked_node.delayed_unused_outputs, current_task);
  }

  void send_output_required_notifications(const Span<const OutputSocket *> sockets,
                                          CurrentTask &current_task)
  {
    for (const OutputSocket *socket : sockets) {
      this->notify_output_required(*socket, current_task);
    }
  }

  void send_output_unused_notifications(const Span<const OutputSocket *> sockets,
                                        CurrentTask &current_task)
  {
    for (const OutputSocket *socket : sockets) {
      this->notify_output_unused(*socket, current_task);
    }
  }

  void run_task(CurrentTask &current_task)
  {
    while (!current_task.scheduled_nodes.is_empty()) {
      const FunctionNode &node = *current_task.scheduled_nodes.pop_last();
      if (current_task.scheduled_nodes.is_empty()) {
        current_task.has_scheduled_nodes.store(false, std::memory_order_relaxed);
      }
      this->run_node_task(node, current_task);
    }
  }

  void run_node_task(const FunctionNode &node, CurrentTask &current_task)
  {
    NodeState &node_state = *node_states_[node.index_in_graph()];
    LinearAllocator<> &allocator = this->get_main_or_local_allocator();
    const LazyFunction &fn = node.function();

    bool node_needs_execution = false;
    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
      BLI_assert(node_state.schedule_state == NodeScheduleState::Scheduled);
      node_state.schedule_state = NodeScheduleState::Running;

      if (node_state.node_has_finished) {
        return;
      }

      bool required_uncomputed_output_exists = false;
      for (OutputState &output_state : node_state.outputs) {
        output_state.usage_for_execution = output_state.usage;
        if (output_state.usage == ValueUsage::Used && !output_state.has_been_computed) {
          required_uncomputed_output_exists = true;
        }
      }
      if (!required_uncomputed_output_exists && !node_state.has_side_effects) {
        return;
      }

      if (!node_state.always_used_inputs_requested) {
        /* Request linked inputs that are always needed. */
        const Span<Input> fn_inputs = fn.inputs();
        for (const int input_index : fn_inputs.index_range()) {
          const Input &fn_input = fn_inputs[input_index];
          if (fn_input.usage == ValueUsage::Used) {
            const InputSocket &input_socket = node.input(input_index);
            if (input_socket.origin() != nullptr) {
              this->set_input_required(locked_node, input_socket);
            }
          }
        }

        node_state.always_used_inputs_requested = true;
      }

      for (const int input_index : node_state.inputs.index_range()) {
        InputState &input_state = node_state.inputs[input_index];
        if (input_state.was_ready_for_execution) {
          continue;
        }
        if (input_state.value != nullptr) {
          input_state.was_ready_for_execution = true;
          continue;
        }
        if (!fn.allow_missing_requested_inputs()) {
          if (input_state.usage == ValueUsage::Used) {
            return;
          }
        }
      }

      node_needs_execution = true;
    });

    if (node_needs_execution) {
      if (!node_state.storage_and_defaults_initialized) {
        /* Initialize storage. */
        node_state.storage = fn.init_storage(allocator);

        /* Load unlinked inputs. */
        for (const int input_index : node.inputs().index_range()) {
          const InputSocket &input_socket = node.input(input_index);
          if (input_socket.origin() != nullptr) {
            continue;
          }
          InputState &input_state = node_state.inputs[input_index];
          const CPPType &type = input_socket.type();
          const void *default_value = input_socket.default_value();
          BLI_assert(default_value != nullptr);
          if (self_.logger_ != nullptr) {
            self_.logger_->log_socket_value(input_socket, {type, default_value}, *context_);
          }
          BLI_assert(input_state.value == nullptr);
          input_state.value = allocator.allocate(type.size(), type.alignment());
          type.copy_construct(default_value, input_state.value);
          input_state.was_ready_for_execution = true;
        }

        node_state.storage_and_defaults_initialized = true;
      }

      /* Importantly, the node must not be locked when it is executed. That would result in locks
       * being hold very long in some cases and results in multiple locks being hold by the same
       * thread in the same graph which can lead to deadlocks. */
      this->execute_node(node, node_state, current_task);
    }

    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
#ifdef DEBUG
      if (node_needs_execution) {
        this->assert_expected_outputs_have_been_computed(locked_node);
      }
#endif
      this->finish_node_if_possible(locked_node);
      const bool reschedule_requested = node_state.schedule_state ==
                                        NodeScheduleState::RunningAndRescheduled;
      node_state.schedule_state = NodeScheduleState::NotScheduled;
      if (reschedule_requested && !node_state.node_has_finished) {
        this->schedule_node(locked_node, current_task);
      }
    });
  }

  void assert_expected_outputs_have_been_computed(LockedNode &locked_node)
  {
    const FunctionNode &node = static_cast<const FunctionNode &>(locked_node.node);
    const NodeState &node_state = locked_node.node_state;

    if (node_state.missing_required_inputs > 0) {
      return;
    }
    if (node_state.schedule_state == NodeScheduleState::RunningAndRescheduled) {
      return;
    }
    Vector<const OutputSocket *> missing_outputs;
    for (const int i : node_state.outputs.index_range()) {
      const OutputState &output_state = node_state.outputs[i];
      if (output_state.usage_for_execution == ValueUsage::Used) {
        if (!output_state.has_been_computed) {
          missing_outputs.append(&node.output(i));
        }
      }
    }
    if (!missing_outputs.is_empty()) {
      if (self_.logger_ != nullptr) {
        self_.logger_->dump_when_outputs_are_missing(node, missing_outputs, *context_);
      }
      BLI_assert_unreachable();
    }
  }

  void finish_node_if_possible(LockedNode &locked_node)
  {
    const Node &node = locked_node.node;
    NodeState &node_state = locked_node.node_state;

    if (node_state.node_has_finished) {
      /* Was finished already. */
      return;
    }
    /* If there are outputs that may still be used, the node is not done yet. */
    for (const OutputState &output_state : node_state.outputs) {
      if (output_state.usage != ValueUsage::Unused && !output_state.has_been_computed) {
        return;
      }
    }
    /* If the node is still waiting for inputs, it is not done yet. */
    for (const InputState &input_state : node_state.inputs) {
      if (input_state.usage == ValueUsage::Used && !input_state.was_ready_for_execution) {
        return;
      }
    }

    node_state.node_has_finished = true;

    for (const int input_index : node_state.inputs.index_range()) {
      const InputSocket &input_socket = node.input(input_index);
      InputState &input_state = node_state.inputs[input_index];
      if (input_state.usage == ValueUsage::Maybe) {
        this->set_input_unused(locked_node, input_socket);
      }
      else if (input_state.usage == ValueUsage::Used) {
        this->destruct_input_value_if_exists(input_state, input_socket.type());
      }
    }

    if (node_state.storage != nullptr) {
      if (node.is_function()) {
        const FunctionNode &fn_node = static_cast<const FunctionNode &>(node);
        fn_node.function().destruct_storage(node_state.storage);
      }
      node_state.storage = nullptr;
    }
  }

  void destruct_input_value_if_exists(InputState &input_state, const CPPType &type)
  {
    if (input_state.value != nullptr) {
      type.destruct(input_state.value);
      input_state.value = nullptr;
    }
  }

  void execute_node(const FunctionNode &node, NodeState &node_state, CurrentTask &current_task);

  void set_input_unused_during_execution(const Node &node,
                                         NodeState &node_state,
                                         const int input_index,
                                         CurrentTask &current_task)
  {
    const InputSocket &input_socket = node.input(input_index);
    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
      this->set_input_unused(locked_node, input_socket);
    });
  }

  void set_input_unused(LockedNode &locked_node, const InputSocket &input_socket)
  {
    NodeState &node_state = locked_node.node_state;
    const int input_index = input_socket.index();
    InputState &input_state = node_state.inputs[input_index];

    BLI_assert(input_state.usage != ValueUsage::Used);
    if (input_state.usage == ValueUsage::Unused) {
      return;
    }
    input_state.usage = ValueUsage::Unused;

    this->destruct_input_value_if_exists(input_state, input_socket.type());
    if (input_state.was_ready_for_execution) {
      return;
    }
    const OutputSocket *origin = input_socket.origin();
    if (origin != nullptr) {
      locked_node.delayed_unused_outputs.append(origin);
    }
  }

  void *set_input_required_during_execution(const Node &node,
                                            NodeState &node_state,
                                            const int input_index,
                                            CurrentTask &current_task)
  {
    const InputSocket &input_socket = node.input(input_index);
    void *result;
    this->with_locked_node(node, node_state, current_task, [&](LockedNode &locked_node) {
      result = this->set_input_required(locked_node, input_socket);
    });
    return result;
  }

  void *set_input_required(LockedNode &locked_node, const InputSocket &input_socket)
  {
    BLI_assert(&locked_node.node == &input_socket.node());
    NodeState &node_state = locked_node.node_state;
    const int input_index = input_socket.index();
    InputState &input_state = node_state.inputs[input_index];

    BLI_assert(input_state.usage != ValueUsage::Unused);

    if (input_state.value != nullptr) {
      input_state.was_ready_for_execution = true;
      return input_state.value;
    }
    if (input_state.usage == ValueUsage::Used) {
      return nullptr;
    }
    input_state.usage = ValueUsage::Used;
    node_state.missing_required_inputs += 1;

    const OutputSocket *origin_socket = input_socket.origin();
    /* Unlinked inputs are always loaded in advance. */
    BLI_assert(origin_socket != nullptr);
    locked_node.delayed_required_outputs.append(origin_socket);
    return nullptr;
  }

  void forward_value_to_linked_inputs(const OutputSocket &from_socket,
                                      GMutablePointer value_to_forward,
                                      CurrentTask &current_task)
  {
    BLI_assert(value_to_forward.get() != nullptr);
    LinearAllocator<> &allocator = this->get_main_or_local_allocator();
    const CPPType &type = *value_to_forward.type();

    if (self_.logger_ != nullptr) {
      self_.logger_->log_socket_value(from_socket, value_to_forward, *context_);
    }

    const Span<const InputSocket *> targets = from_socket.targets();
    for (const InputSocket *target_socket : targets) {
      const Node &target_node = target_socket->node();
      NodeState &node_state = *node_states_[target_node.index_in_graph()];
      const int input_index = target_socket->index();
      InputState &input_state = node_state.inputs[input_index];
      const bool is_last_target = target_socket == targets.last();
#ifdef DEBUG
      if (input_state.value != nullptr) {
        if (self_.logger_ != nullptr) {
          self_.logger_->dump_when_input_is_set_twice(*target_socket, from_socket, *context_);
        }
        BLI_assert_unreachable();
      }
#endif
      BLI_assert(!input_state.was_ready_for_execution);
      BLI_assert(target_socket->type() == type);
      BLI_assert(target_socket->origin() == &from_socket);

      if (self_.logger_ != nullptr) {
        self_.logger_->log_socket_value(*target_socket, value_to_forward, *context_);
      }
      if (target_node.is_dummy()) {
        /* Forward the value to the outside of the graph. */
        const int graph_output_index = self_.graph_outputs_.index_of_try(target_socket);
        if (graph_output_index != -1 &&
            params_->get_output_usage(graph_output_index) != ValueUsage::Unused) {
          void *dst_buffer = params_->get_output_data_ptr(graph_output_index);
          if (is_last_target) {
            type.move_construct(value_to_forward.get(), dst_buffer);
          }
          else {
            type.copy_construct(value_to_forward.get(), dst_buffer);
          }
          params_->output_set(graph_output_index);
        }
        continue;
      }
      this->with_locked_node(target_node, node_state, current_task, [&](LockedNode &locked_node) {
        if (input_state.usage == ValueUsage::Unused) {
          return;
        }
        if (is_last_target) {
          /* No need to make a copy if this is the last target. */
          this->forward_value_to_input(locked_node, input_state, value_to_forward, current_task);
          value_to_forward = {};
        }
        else {
          void *buffer = allocator.allocate(type.size(), type.alignment());
          type.copy_construct(value_to_forward.get(), buffer);
          this->forward_value_to_input(locked_node, input_state, {type, buffer}, current_task);
        }
      });
    }
    if (value_to_forward.get() != nullptr) {
      value_to_forward.destruct();
    }
  }

  void forward_value_to_input(LockedNode &locked_node,
                              InputState &input_state,
                              GMutablePointer value,
                              CurrentTask &current_task)
  {
    NodeState &node_state = locked_node.node_state;

    BLI_assert(input_state.value == nullptr);
    BLI_assert(!input_state.was_ready_for_execution);
    input_state.value = value.get();

    if (input_state.usage == ValueUsage::Used) {
      node_state.missing_required_inputs -= 1;
      if (node_state.missing_required_inputs == 0 ||
          (locked_node.node.is_function() && static_cast<const FunctionNode &>(locked_node.node)
                                                 .function()
                                                 .allow_missing_requested_inputs())) {
        this->schedule_node(locked_node, current_task);
      }
    }
  }

  bool use_multi_threading() const
  {
    return task_pool_.load() != nullptr;
  }

  bool try_enable_multi_threading()
  {
#ifndef WITH_TBB
    /* The non-TBB task pool has the property that it immediately executes tasks under some
     * circumstances. This is not supported here because tasks might be scheduled while another
     * node is in the middle of being executed on the same thread. */
    return false;
#endif
    if (this->use_multi_threading()) {
      return true;
    }
#ifdef FN_LAZY_FUNCTION_DEBUG_THREADS
    /* Only the current main thread is allowed to enabled multi-threading, because the executor is
     * still in single-threaded mode. */
    if (current_main_thread_ != std::this_thread::get_id()) {
      BLI_assert_unreachable();
    }
#endif
    /* Check of the caller supports multi-threading. */
    if (!params_->try_enable_multi_threading()) {
      return false;
    }
    /* Avoid using multiple threads when only one thread can be used anyway. */
    if (BLI_system_thread_count() <= 1) {
      return false;
    }
    this->ensure_thread_locals();
    task_pool_.store(BLI_task_pool_create(this, TASK_PRIORITY_HIGH));
    return true;
  }

  void ensure_thread_locals()
  {
#ifdef FN_LAZY_FUNCTION_DEBUG_THREADS
    if (current_main_thread_ != std::this_thread::get_id()) {
      BLI_assert_unreachable();
    }
#endif
    if (!thread_locals_) {
      thread_locals_ = std::make_unique<threading::EnumerableThreadSpecific<ThreadLocalData>>();
    }
  }

  /**
   * Allow other threads to steal all the nodes that are currently scheduled on this thread.
   */
  void move_scheduled_nodes_to_task_pool(CurrentTask &current_task)
  {
    BLI_assert(this->use_multi_threading());
    using FunctionNodeVector = Vector<const FunctionNode *>;
    FunctionNodeVector *nodes = MEM_new<FunctionNodeVector>(__func__);
    {
      std::lock_guard lock{current_task.mutex};
      if (current_task.scheduled_nodes.is_empty()) {
        return;
      }
      *nodes = std::move(current_task.scheduled_nodes);
      current_task.has_scheduled_nodes.store(false, std::memory_order_relaxed);
    }
    /* All nodes are pushed as a single task in the pool. This avoids unnecessary threading
     * overhead when the nodes are fast to compute. */
    BLI_task_pool_push(
        task_pool_.load(),
        [](TaskPool *pool, void *data) {
          Executor &executor = *static_cast<Executor *>(BLI_task_pool_user_data(pool));
          FunctionNodeVector &nodes = *static_cast<FunctionNodeVector *>(data);
          CurrentTask new_current_task;
          new_current_task.scheduled_nodes = std::move(nodes);
          new_current_task.has_scheduled_nodes.store(true, std::memory_order_relaxed);
          executor.run_task(new_current_task);
        },
        nodes,
        true,
        [](TaskPool * /*pool*/, void *data) {
          MEM_delete(static_cast<FunctionNodeVector *>(data));
        });
  }

  LinearAllocator<> &get_main_or_local_allocator()
  {
    if (this->use_multi_threading()) {
      return thread_locals_->local().allocator;
    }
    return main_allocator_;
  }
};

class GraphExecutorLFParams final : public Params {
 private:
  Executor &executor_;
  const Node &node_;
  NodeState &node_state_;
  CurrentTask &current_task_;

 public:
  GraphExecutorLFParams(const LazyFunction &fn,
                        Executor &executor,
                        const Node &node,
                        NodeState &node_state,
                        CurrentTask &current_task)
      : Params(fn, executor.use_multi_threading()),
        executor_(executor),
        node_(node),
        node_state_(node_state),
        current_task_(current_task)
  {
  }

 private:
  void *try_get_input_data_ptr_impl(const int index) const override
  {
    const InputState &input_state = node_state_.inputs[index];
    if (input_state.was_ready_for_execution) {
      return input_state.value;
    }
    return nullptr;
  }

  void *try_get_input_data_ptr_or_request_impl(const int index) override
  {
    const InputState &input_state = node_state_.inputs[index];
    if (input_state.was_ready_for_execution) {
      return input_state.value;
    }
    return executor_.set_input_required_during_execution(node_, node_state_, index, current_task_);
  }

  void *get_output_data_ptr_impl(const int index) override
  {
    OutputState &output_state = node_state_.outputs[index];
    BLI_assert(!output_state.has_been_computed);
    if (output_state.value == nullptr) {
      LinearAllocator<> &allocator = executor_.get_main_or_local_allocator();
      const CPPType &type = node_.output(index).type();
      output_state.value = allocator.allocate(type.size(), type.alignment());
    }
    return output_state.value;
  }

  void output_set_impl(const int index) override
  {
    OutputState &output_state = node_state_.outputs[index];
    BLI_assert(!output_state.has_been_computed);
    BLI_assert(output_state.value != nullptr);
    const OutputSocket &output_socket = node_.output(index);
    executor_.forward_value_to_linked_inputs(
        output_socket, {output_socket.type(), output_state.value}, current_task_);
    output_state.value = nullptr;
    output_state.has_been_computed = true;
  }

  bool output_was_set_impl(const int index) const override
  {
    const OutputState &output_state = node_state_.outputs[index];
    return output_state.has_been_computed;
  }

  ValueUsage get_output_usage_impl(const int index) const override
  {
    const OutputState &output_state = node_state_.outputs[index];
    return output_state.usage_for_execution;
  }

  void set_input_unused_impl(const int index) override
  {
    executor_.set_input_unused_during_execution(node_, node_state_, index, current_task_);
  }

  bool try_enable_multi_threading_impl() override
  {
    return executor_.try_enable_multi_threading();
  }
};

/**
 * Actually execute the node.
 *
 * Making this `inline` results in a simpler back-trace in release builds.
 */
inline void Executor::execute_node(const FunctionNode &node,
                                   NodeState &node_state,
                                   CurrentTask &current_task)
{
  const LazyFunction &fn = node.function();
  GraphExecutorLFParams node_params{fn, *this, node, node_state, current_task};
  BLI_assert(context_ != nullptr);
  Context fn_context = *context_;
  fn_context.storage = node_state.storage;

  if (self_.logger_ != nullptr) {
    self_.logger_->log_before_node_execute(node, node_params, fn_context);
  }

  /* This is run when the execution of the node calls `lazy_threading::send_hint` to indicate that
   * the execution will take a while. In this case, other tasks waiting on this thread should be
   * allowed to be picked up by another thread. */
  auto blocking_hint_fn = [&]() {
    if (!current_task.has_scheduled_nodes.load()) {
      return;
    }
    if (!this->try_enable_multi_threading()) {
      return;
    }
    this->move_scheduled_nodes_to_task_pool(current_task);
  };

  lazy_threading::HintReceiver blocking_hint_receiver{blocking_hint_fn};
  fn.execute(node_params, fn_context);

  if (self_.logger_ != nullptr) {
    self_.logger_->log_after_node_execute(node, node_params, fn_context);
  }
}

GraphExecutor::GraphExecutor(const Graph &graph,
                             const Span<const OutputSocket *> graph_inputs,
                             const Span<const InputSocket *> graph_outputs,
                             const Logger *logger,
                             const SideEffectProvider *side_effect_provider)
    : graph_(graph),
      graph_inputs_(graph_inputs),
      graph_outputs_(graph_outputs),
      logger_(logger),
      side_effect_provider_(side_effect_provider)
{
  /* The graph executor can handle partial execution when there are still missing inputs. */
  allow_missing_requested_inputs_ = true;

  for (const OutputSocket *socket : graph_inputs_) {
    BLI_assert(socket->node().is_dummy());
    inputs_.append({"In", socket->type(), ValueUsage::Maybe});
  }
  for (const InputSocket *socket : graph_outputs_) {
    BLI_assert(socket->node().is_dummy());
    outputs_.append({"Out", socket->type()});
  }
}

void GraphExecutor::execute_impl(Params &params, const Context &context) const
{
  Executor &executor = *static_cast<Executor *>(context.storage);
  executor.execute(params, context);
}

void *GraphExecutor::init_storage(LinearAllocator<> &allocator) const
{
  Executor &executor = *allocator.construct<Executor>(*this).release();
  return &executor;
}

void GraphExecutor::destruct_storage(void *storage) const
{
  std::destroy_at(static_cast<Executor *>(storage));
}

void GraphExecutorLogger::log_socket_value(const Socket &socket,
                                           const GPointer value,
                                           const Context &context) const
{
  UNUSED_VARS(socket, value, context);
}

void GraphExecutorLogger::log_before_node_execute(const FunctionNode &node,
                                                  const Params &params,
                                                  const Context &context) const
{
  UNUSED_VARS(node, params, context);
}

void GraphExecutorLogger::log_after_node_execute(const FunctionNode &node,
                                                 const Params &params,
                                                 const Context &context) const
{
  UNUSED_VARS(node, params, context);
}

Vector<const FunctionNode *> GraphExecutorSideEffectProvider::get_nodes_with_side_effects(
    const Context &context) const
{
  UNUSED_VARS(context);
  return {};
}

void GraphExecutorLogger::dump_when_outputs_are_missing(const FunctionNode &node,
                                                        Span<const OutputSocket *> missing_sockets,
                                                        const Context &context) const
{
  UNUSED_VARS(node, missing_sockets, context);
}

void GraphExecutorLogger::dump_when_input_is_set_twice(const InputSocket &target_socket,
                                                       const OutputSocket &from_socket,
                                                       const Context &context) const
{
  UNUSED_VARS(target_socket, from_socket, context);
}

}  // namespace blender::fn::lazy_function
